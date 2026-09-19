#include "filters/convolution.h"

#include <complex.h>
#include <ctype.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/processing_parameters.h"
#include "audio/sample_conversion.h"
#include "config/config_error.h"
#include "config/config_gen.h"
#include "config/engine_config_types.h"
#include "fft/real_fft.h"
#include "filters/filter.h"
#include "utils/cdsp_memory.h"
#include "utils/cdsp_path.h"
#include "utils/double_helpers.h"
#include "wav/raw_reader.h"
#include "wav/wav_reader.h"

typedef struct conv_coeffs_s {
  char name[64];
  size_t chunk_size;
  size_t num_segments;
  size_t spec_len;
  size_t spec_stride;
  int ref_count;
  /// Build pass that created this entry. Upstream keeps its `ConvCoeffCache`
  /// alive only for the duration of one build/update pass, because a longer
  /// lived cache risks handing out coefficients from a stale config. The C
  /// port cannot drop the whole list at the end of a pass (entries are still
  /// referenced by the filters of the *previous*, still running pipeline), so
  /// entries are tagged instead and only reused within their own pass.
  uint64_t generation;
  complex_t *data;
  struct conv_coeffs_s *next;
} conv_coeffs_t;

struct convolution_filter {
  char name[64];
  size_t chunk_size;
  size_t num_segments;
  size_t spec_len;
  size_t spec_stride;
  real_fft_t *fft;
  conv_coeffs_t *coeffs;
  complex_t *hist_f;
  complex_t *temp_buf;
  size_t write_idx;
  double *overlap_buffer;
  double *time_buf;
  double *out_buf;
};

typedef struct convolution_filter convolution_filter_t;

#include "config/engine_config_types.h"
// Uniform-partitioned overlap-add FIR convolution.
// Stockham-style segmented overlap-add with one 2N-point real FFT per
// chunk and an N+1-bin spectrum-domain multiply-accumulate across the
// segment history.
//
//   - Uses direct FFTW3 r2c/c2r interleaved complex transforms without
//     redundant intermediate buffers or copy loops.
//   - Frequency-domain multiply-accumulate runs through vectorized
//     ARM NEON / AVX+FMA fused multiply-add kernels.
//   - Inverse FFT produces unscaled linear convolution sum due to
//     pre-scaling of coefficients by 1 / (2 * chunkSize).

static conv_coeffs_t *g_conv_coeffs_cache = NULL;
static uint64_t g_conv_cache_generation = 0;
static pthread_mutex_t g_conv_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

void convolution_coeff_cache_begin_build_pass(void) {
  pthread_mutex_lock(&g_conv_cache_mutex);
  g_conv_cache_generation++;
  pthread_mutex_unlock(&g_conv_cache_mutex);
}

static conv_coeffs_t *conv_coeffs_cache_find(const char *name,
                                             size_t chunk_size) {
  const char *lookup_name = name ? name : "convolution";
  pthread_mutex_lock(&g_conv_cache_mutex);
  for (conv_coeffs_t *curr = g_conv_coeffs_cache; curr != NULL;
       curr = curr->next) {
    if (curr->generation == g_conv_cache_generation &&
        strcmp(curr->name, lookup_name) == 0 &&
        curr->chunk_size == chunk_size) {
      curr->ref_count++;
      pthread_mutex_unlock(&g_conv_cache_mutex);
      return curr;
    }
  }
  pthread_mutex_unlock(&g_conv_cache_mutex);
  return NULL;
}

static conv_coeffs_t *conv_coeffs_create(const char *name, size_t chunk_size,
                                         size_t num_seg, size_t spec_len,
                                         const double *coeffs,
                                         size_t coeffs_count, real_fft_t *fft,
                                         size_t fft_len) {
  const char *lookup_name = name ? name : "convolution";
  pthread_mutex_lock(&g_conv_cache_mutex);
  for (conv_coeffs_t *curr = g_conv_coeffs_cache; curr != NULL;
       curr = curr->next) {
    if (curr->generation == g_conv_cache_generation &&
        strcmp(curr->name, lookup_name) == 0 &&
        curr->chunk_size == chunk_size) {
      curr->ref_count++;
      pthread_mutex_unlock(&g_conv_cache_mutex);
      return curr;
    }
  }
  pthread_mutex_unlock(&g_conv_cache_mutex);

  conv_coeffs_t *entry = (conv_coeffs_t *)calloc(1, sizeof(conv_coeffs_t));
  if (!entry) {
    return NULL;
  }
  strncpy(entry->name, lookup_name, sizeof(entry->name) - 1);
  entry->name[sizeof(entry->name) - 1] = '\0';
  entry->chunk_size = chunk_size;
  entry->num_segments = num_seg;
  entry->spec_len = spec_len;
  size_t spec_stride = (spec_len + 3) & ~3;
  entry->spec_stride = spec_stride;
  entry->ref_count = 1;

  entry->data = (complex_t *)cdsp_aligned_alloc(64, num_seg * spec_stride *
                                                        sizeof(complex_t));
  if (!entry->data) {
    free(entry);
    return NULL;
  }

  double *scratch = (double *)cdsp_aligned_alloc(64, fft_len * sizeof(double));
  if (!scratch) {
    cdsp_aligned_free(entry->data);
    free(entry);
    return NULL;
  }

  double inv_scale = 1.0 / (double)fft_len;

  for (size_t s = 0; s < num_seg; s++) {
    memset(scratch, 0, fft_len * sizeof(double));
    size_t offset = s * chunk_size;
    size_t copy_len = (coeffs_count > offset) ? (coeffs_count - offset) : 0;
    if (copy_len > chunk_size)
      copy_len = chunk_size;
    if (copy_len > 0 && coeffs) {
      for (size_t k = 0; k < copy_len; k++) {
        scratch[k] = coeffs[offset + k] * inv_scale;
      }
    }
    real_fft_forward(fft, scratch, entry->data + s * spec_stride);
  }
  cdsp_aligned_free(scratch);

  pthread_mutex_lock(&g_conv_cache_mutex);
  entry->generation = g_conv_cache_generation;
  entry->next = g_conv_coeffs_cache;
  g_conv_coeffs_cache = entry;
  pthread_mutex_unlock(&g_conv_cache_mutex);
  return entry;
}

static void conv_coeffs_release(conv_coeffs_t *entry) {
  if (!entry)
    return;
  pthread_mutex_lock(&g_conv_cache_mutex);
  entry->ref_count--;
  if (entry->ref_count <= 0) {
    if (g_conv_coeffs_cache == entry) {
      g_conv_coeffs_cache = entry->next;
    } else {
      conv_coeffs_t *prev = g_conv_coeffs_cache;
      while (prev && prev->next != entry) {
        prev = prev->next;
      }
      if (prev) {
        prev->next = entry->next;
      }
    }
    pthread_mutex_unlock(&g_conv_cache_mutex);
    if (entry->data) {
      cdsp_aligned_free(entry->data);
    }
    free(entry);
    return;
  }
  pthread_mutex_unlock(&g_conv_cache_mutex);
}

/**
 * @brief Free the convolution filter instance and its associated resources.
 *
 * @param filter The convolution filter instance to free.
 */
static void convolution_filter_free(void *instance) {
  convolution_filter_t *filter = (convolution_filter_t *)instance;
  if (!filter)
    return;
  if (filter->coeffs) {
    conv_coeffs_release(filter->coeffs);
    filter->coeffs = NULL;
  }
  if (filter->hist_f)
    cdsp_aligned_free(filter->hist_f);
  if (filter->temp_buf)
    cdsp_aligned_free(filter->temp_buf);
  if (filter->fft)
    real_fft_free(filter->fft);
  if (filter->overlap_buffer)
    free(filter->overlap_buffer);
  if (filter->time_buf)
    cdsp_aligned_free(filter->time_buf);
  if (filter->out_buf)
    cdsp_aligned_free(filter->out_buf);
  free(filter);
}

/**
 * @brief Validates convolution filter parameters.
 *
 * @param config High-level filter configuration.
 * @param sample_rate The sample rate.
 * @param err Pointer to a config error struct to populate on failure.
 * @return 0 on success, -1 on failure.
 */
static int convolution_config_validate(const filter_config_t *config,
                                       int sample_rate, config_error_t *err) {
  (void)sample_rate;
  if (!config || config->type != FILTER_TYPE_CONV)
    return -1;
  const conv_config_t *params = &config->parameters.conv;
  if (!params)
    return 0;
  switch (params->type) {
  case CONV_TYPE_VALUES:
    if (!params->values || params->values_count == 0) {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                       "Conv 'values' must be non-empty");
      return -1;
    }
    for (size_t i = 0; i < params->values_count; i++) {
      if (!isfinite(params->values[i])) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Non-finite coefficient at index %zu", i);
        config_error_set(err, CONFIG_ERR_INVALID_FILTER, "%s", msg);
        return -1;
      }
    }
    break;
  case CONV_TYPE_WAV: {
    if (params->filename[0] == '\0') {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                       "Conv filter missing filename");
      return -1;
    }
    if (params->channel < 0) {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                       "Conv 'channel' must be non-negative");
      return -1;
    }
    FILE *f = cdsp_fopen(params->filename, "rb");
    if (!f) {
      char msg[512];
      snprintf(msg, sizeof(msg),
               "Conv file '%s' cannot be opened or does not exist",
               params->filename);
      config_error_set(err, CONFIG_ERR_INVALID_FILTER, msg);
      return -1;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fclose(f);
    if (fsize <= 0) {
      char msg[512];
      snprintf(msg, sizeof(msg), "Conv file '%s' is empty or invalid",
               params->filename);
      config_error_set(err, CONFIG_ERR_INVALID_FILTER, msg);
      return -1;
    }
    char err_msg[512] = {0};
    size_t count = 0;
    double *probe = wav_read_channel_samples(params->filename, params->channel,
                                             &count, err_msg, sizeof(err_msg));
    if (!probe || count == 0) {
      free(probe);
      if (err_msg[0] != '\0') {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER, "%s", err_msg);
      } else {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "Conv coefficients could not be read from '%s' "
                         "(unsupported encoding, channel out of range, or no "
                         "usable samples)",
                         params->filename);
      }
      return -1;
    }
    for (size_t i = 0; i < count; i++) {
      if (!isfinite(probe[i])) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "Non-finite coefficient at index %zu in '%s'", i,
                 params->filename);
        config_error_set(err, CONFIG_ERR_INVALID_FILTER, "%s", msg);
        free(probe);
        return -1;
      }
    }
    free(probe);
    break;
  }
  case CONV_TYPE_RAW: {
    if (params->filename[0] == '\0') {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                       "Conv filter missing filename");
      return -1;
    }
    FILE *f = cdsp_fopen(params->filename, "rb");
    if (!f) {
      char msg[512];
      snprintf(msg, sizeof(msg),
               "Conv file '%s' cannot be opened or does not exist",
               params->filename);
      config_error_set(err, CONFIG_ERR_INVALID_FILTER, msg);
      return -1;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fclose(f);
    if (fsize <= 0) {
      char msg[512];
      snprintf(msg, sizeof(msg), "Conv file '%s' is empty or invalid",
               params->filename);
      config_error_set(err, CONFIG_ERR_INVALID_FILTER, msg);
      return -1;
    }
    char err_msg[512] = {0};
    size_t count = 0;
    double *probe = raw_read_samples(
        params->filename, params->format,
        params->skip_bytes_lines > 0 ? (size_t)params->skip_bytes_lines : 0,
        params->read_bytes_lines > 0 ? (size_t)params->read_bytes_lines : 0,
        &count, err_msg, sizeof(err_msg));
    if (!probe || count == 0) {
      free(probe);
      if (err_msg[0] != '\0') {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER, "%s", err_msg);
      } else {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "Conv coefficients could not be read from '%s' "
                         "(unsupported encoding, channel out of range, or no "
                         "usable samples)",
                         params->filename);
      }
      return -1;
    }
    for (size_t i = 0; i < count; i++) {
      if (!isfinite(probe[i])) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "Non-finite coefficient at index %zu in '%s'", i,
                 params->filename);
        config_error_set(err, CONFIG_ERR_INVALID_FILTER, "%s", msg);
        free(probe);
        return -1;
      }
    }
    free(probe);
    break;
  }
  case CONV_TYPE_DUMMY:
    if (params->length <= 0) {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                       "Conv 'dummy' length must be > 0");
      return -1;
    }
    break;
  }
  return 0;
}

/**
 * @brief Build a convolution filter from raw IR samples.
 *
 * Resolve the parameters to a flat IR buffer. Only called from the
 * control plane (filter creation / hot-swap), never from
 * convolution_filter_process.
 *
 * @param name The name of the filter.
 * @param config High-level filter configuration.
 * @param sample_rate The sample rate.
 * @param chunk_size Per-call block length N. Must match the
 *                   validFrames the pipeline will hand to process.
 * @param proc_params Processing parameters.
 * @param err Pointer to a config error struct to populate on failure.
 * @return A pointer to the created convolution filter, or NULL on failure.
 */
static void *convolution_filter_create(const char *name,
                                       const filter_config_t *config,
                                       int sample_rate, size_t chunk_size,
                                       processing_parameters_t *proc_params,
                                       config_error_t *err) {
  (void)sample_rate;
  (void)proc_params;
  if (!config || config->type != FILTER_TYPE_CONV)
    return NULL;
  const conv_config_t *params = &config->parameters.conv;
  if (convolution_config_validate(config, 0, err) != 0)
    return NULL;
  if (chunk_size == 0) {
    config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                     "Convolution chunk_size must be positive");
    return NULL;
  }
  convolution_filter_t *filter =
      (convolution_filter_t *)calloc(1, sizeof(convolution_filter_t));
  if (!filter) {
    config_error_set(err, CONFIG_ERR_PARSE,
                     "Failed to allocate convolution filter wrapper");
    return NULL;
  }
  if (name) {
    strncpy(filter->name, name, sizeof(filter->name) - 1);
    filter->name[sizeof(filter->name) - 1] = '\0';
  } else {
    strcpy(filter->name, "convolution");
  }
  filter->chunk_size = chunk_size;
  size_t fft_len = 2 * chunk_size;
  size_t spec_len = fft_len / 2 + 1;
  filter->spec_len = spec_len;

  // Check cache for shared coefficients
  conv_coeffs_t *cached_coeffs =
      conv_coeffs_cache_find(filter->name, chunk_size);
  if (cached_coeffs) {
    filter->coeffs = cached_coeffs;
    filter->num_segments = cached_coeffs->num_segments;
  }

  const double *coeffs = NULL;
  size_t coeffs_count = 0;
  double *dummy_coeffs = NULL;

  if (!filter->coeffs) {
    char err_msg[512] = {0};
    if (params->type == CONV_TYPE_VALUES) {
      coeffs = params->values;
      coeffs_count = params->values_count;
    } else if (params->type == CONV_TYPE_DUMMY) {
      size_t len = params->length > 0 ? (size_t)params->length : 1;
      dummy_coeffs = (double *)calloc(len, sizeof(double));
      if (!dummy_coeffs) {
        goto fail;
      }
      dummy_coeffs[0] = 1.0;
      coeffs = dummy_coeffs;
      coeffs_count = len;
    } else if (params->type == CONV_TYPE_WAV) {
      size_t count = 0;
      dummy_coeffs = wav_read_channel_samples(params->filename, params->channel,
                                              &count, err_msg, sizeof(err_msg));
      coeffs = dummy_coeffs;
      coeffs_count = count;
    } else if (params->type == CONV_TYPE_RAW) {
      size_t count = 0;
      dummy_coeffs = raw_read_samples(
          params->filename, params->format,
          params->skip_bytes_lines > 0 ? (size_t)params->skip_bytes_lines : 0,
          params->read_bytes_lines > 0 ? (size_t)params->read_bytes_lines : 0,
          &count, err_msg, sizeof(err_msg));
      coeffs = dummy_coeffs;
      coeffs_count = count;
    }

    if (!coeffs && err_msg[0] != '\0') {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER, "Conv filter '%s': %s",
                       filter->name, err_msg);
      goto fail;
    }
    if (!coeffs || coeffs_count == 0) {
      filter->num_segments = 1;
    } else {
      filter->num_segments = (coeffs_count + chunk_size - 1) / chunk_size;
      if (filter->num_segments == 0)
        filter->num_segments = 1;
    }
  }

  size_t num_seg = filter->num_segments;
  size_t spec_stride = (spec_len + 3) & ~3;
  filter->spec_stride = spec_stride;
  filter->hist_f = (complex_t *)cdsp_aligned_alloc(64, num_seg * spec_stride *
                                                           sizeof(complex_t));
  if (!filter->hist_f) {
    goto fail;
  }
  memset(filter->hist_f, 0, num_seg * spec_stride * sizeof(complex_t));

  filter->temp_buf =
      (complex_t *)cdsp_aligned_alloc(64, spec_len * sizeof(complex_t));
  filter->time_buf = (double *)cdsp_aligned_alloc(64, fft_len * sizeof(double));
  filter->out_buf = (double *)cdsp_aligned_alloc(64, fft_len * sizeof(double));
  filter->overlap_buffer = (double *)calloc(chunk_size, sizeof(double));
  filter->write_idx = 0;

  if (!filter->temp_buf || !filter->time_buf || !filter->out_buf ||
      !filter->overlap_buffer) {
    config_error_set(err, CONFIG_ERR_PARSE,
                     "Failed to allocate convolution scratch buffers");
    goto fail;
  }

  filter->fft = real_fft_create(fft_len, err);
  if (!filter->fft) {
    config_error_set(err, CONFIG_ERR_PARSE,
                     "Failed to create real FFT context");
    goto fail;
  }

  // Clear buffers that might have been overwritten during FFT planning
  memset(filter->hist_f, 0, num_seg * spec_stride * sizeof(complex_t));
  memset(filter->temp_buf, 0, spec_len * sizeof(complex_t));
  memset(filter->time_buf, 0, fft_len * sizeof(double));
  memset(filter->out_buf, 0, fft_len * sizeof(double));

  if (!filter->coeffs) {
    filter->coeffs =
        conv_coeffs_create(filter->name, chunk_size, num_seg, spec_len, coeffs,
                           coeffs_count, filter->fft, fft_len);
    if (!filter->coeffs) {
      goto fail;
    }
  }

  if (dummy_coeffs) {
    free(dummy_coeffs);
    dummy_coeffs = NULL;
  }

  return filter;

fail:
  if (err && err->type == CONFIG_ERR_NONE) {
    config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                     "Failed to initialize convolution filter '%s' (check IR "
                     "values or file format/existence)",
                     name ? name : "");
  }
  if (dummy_coeffs)
    free(dummy_coeffs);
  convolution_filter_free(filter);
  return NULL;
}

/**
 * @brief Processes one chunk of audio data using partitioned overlap-add
 * convolution.
 *
 * This function performs the following steps:
 * 1. Copies the input block to the time buffer and pads with zeros.
 * 2. Computes the forward FFT of the padded block directly into the history
 * buffer.
 * 3. Performs vectorized frequency-domain multiply-accumulate with partitioned
 * IR segments.
 * 4. Computes the inverse FFT of the accumulated spectrum directly.
 * 5. Reconstructs the output block using overlap-add.
 *
 * @param filter Pointer to the convolution filter.
 * @param waveform In-place buffer containing the input block, which will be
 * overwritten with the output.
 */
static void process_chunk_internal(convolution_filter_t *filter,
                                   mutable_waveform_t waveform, size_t len) {
  if (!filter || filter->num_segments == 0 || !filter->coeffs || len == 0)
    return;
  size_t cs = filter->chunk_size;
  size_t spec_len = filter->spec_len;
  size_t spec_stride = filter->spec_stride;
  size_t num_seg = filter->num_segments;
  size_t widx = filter->write_idx;

  // 1. Stage the block in time_buf; zero the remainder up to 2 * cs (the FFT
  // zero-pad).
  memcpy(filter->time_buf, waveform, len * sizeof(double));
  if (2 * cs > len) {
    memset(filter->time_buf + len, 0, (2 * cs - len) * sizeof(double));
  }

  // 2. Advance the history index and FFT the block directly into that slot.
  complex_t *dest_slot = filter->hist_f + widx * spec_stride;
  real_fft_forward(filter->fft, filter->time_buf, dest_slot);

  // 3. Spectrum-domain multiply-accumulate across the segment history.
  //    seg=0 pairs the newest input with coeff[0]; seg=k pairs the input from
  //    `k` blocks ago with coeff[k].
  const complex_t *coeffs_data = filter->coeffs->data;
  size_t hidx0 = widx;
  dsp_ops_complex_multiply_interleaved(filter->hist_f + hidx0 * spec_stride,
                                       coeffs_data, filter->temp_buf, spec_len);

  for (size_t s = 1; s < num_seg; s++) {
    size_t hidx = (widx + num_seg - s) % num_seg;
    dsp_ops_complex_fma_interleaved(filter->temp_buf,
                                    filter->hist_f + hidx * spec_stride,
                                    coeffs_data + s * spec_stride, spec_len);
  }

  // 4. Inverse FFT directly into out_buf.
  real_fft_inverse(filter->fft, filter->temp_buf, filter->out_buf);

  // 5. Overlap-add output: out[i] = ifft[i] + overlap_prev[i] for
  //    i in 0..<len; overlap_next = ifft[cs..2*cs].
  for (size_t i = 0; i < len; i++) {
    waveform[i] = filter->out_buf[i] + filter->overlap_buffer[i];
  }
  memcpy(filter->overlap_buffer, filter->out_buf + cs, cs * sizeof(double));

  filter->write_idx = (widx + 1) % num_seg;
}

/// Process audio block in-place. Full blocks are convolved directly;
/// any trailing partial block is zero-padded and convolved immediately
/// (matching upstream CamillaDSP) rather than deferred with stale buffer
/// contents.
static void convolution_filter_process(void *instance,
                                       mutable_waveform_t waveform,
                                       size_t count) {
  convolution_filter_t *filter = (convolution_filter_t *)instance;
  if (!filter || !waveform || count == 0)
    return;
  size_t cs = filter->chunk_size;
  size_t i = 0;

  // Process any full blocks in-place directly from/to the waveform
  while (i + cs <= count) {
    process_chunk_internal(filter, waveform + i, cs);
    i += cs;
  }

  // Zero-pad and process any remaining partial block immediately
  size_t rem = count - i;
  if (rem > 0) {
    process_chunk_internal(filter, waveform + i, rem);
  }
}

static void convolution_filter_transfer_state(void *dest_ptr,
                                              const void *src_ptr) {
  convolution_filter_t *dest = (convolution_filter_t *)dest_ptr;
  const convolution_filter_t *src = (const convolution_filter_t *)src_ptr;
  if (!dest || !src || dest == src)
    return;

  if (dest->chunk_size == src->chunk_size) {
    // Copy overlap buffer whenever chunk sizes match, even if segment count
    // changes
    memcpy(dest->overlap_buffer, src->overlap_buffer,
           dest->chunk_size * sizeof(double));

    // History segments only line up if segment count also matches
    if (dest->num_segments == src->num_segments) {
      size_t num_seg = dest->num_segments;
      size_t spec_stride = dest->spec_stride;

      // Copy history segments in a single contiguous block
      memcpy(dest->hist_f, src->hist_f,
             num_seg * spec_stride * sizeof(complex_t));
      dest->write_idx = src->write_idx;
    }
  }
}

const filter_vtable_t g_convolution_vtable = {
    .validate = convolution_config_validate,
    .create = convolution_filter_create,
    .process = convolution_filter_process,
    .transfer_state = convolution_filter_transfer_state,
    .free = convolution_filter_free};
