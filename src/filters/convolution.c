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
#include "audio/sample_format.h"
#include "config/config_error.h"
#include "config/config_gen.h"
#include "fft/real_fft.h"
#include "filters/filter.h"
#include "utils/cdsp_memory.h"
#include "utils/cdsp_path.h"
#include "utils/double_helpers.h"
#include "wav/raw_reader.h"
#include "wav/wav_reader.h"

typedef struct conv_coeffs_s {
  /// Identity of the impulse response: full (untruncated) filter name plus
  /// every parameter that determines the coefficients (see
  /// conv_coeffs_key_build). Upstream keys its pass-scoped `ConvCoeffCache` by
  /// name alone because a fresh cache is built for every pass from one config;
  /// this process-wide cache is also reachable by direct filter_create callers
  /// that never open a build pass, so the name alone is not a safe key.
  unsigned char *key;
  size_t key_len;
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
  char name[128];
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

// MARK: - Coefficient cache key

typedef struct {
  unsigned char *buf;
  size_t len;
  size_t cap;
  bool failed;
} conv_key_builder_t;

static void conv_key_append(conv_key_builder_t *b, const void *data,
                            size_t len) {
  if (b->failed || len == 0)
    return;
  if (b->len + len > b->cap) {
    size_t new_cap = b->cap ? b->cap : 256;
    while (new_cap < b->len + len)
      new_cap *= 2;
    unsigned char *nb = (unsigned char *)realloc(b->buf, new_cap);
    if (!nb) {
      b->failed = true;
      return;
    }
    b->buf = nb;
    b->cap = new_cap;
  }
  memcpy(b->buf + b->len, data, len);
  b->len += len;
}

static void conv_key_append_str(conv_key_builder_t *b, const char *s) {
  // Include the terminator so adjacent strings cannot alias.
  conv_key_append(b, s, strlen(s) + 1);
}

/**
 * @brief Serializes everything that determines a filter's coefficients.
 *
 * Two filters may share pre-transformed coefficients only if they have the
 * same full name and identical coefficient parameters. Using the full name
 * (not the 63-char filter_t copy) and the parameters fixes both the stale
 * reuse by direct filter_create callers (all at generation 0) and the
 * collision of long names sharing a 63-char prefix.
 *
 * @return malloc'd key (caller frees), or NULL on allocation failure.
 */
static unsigned char *conv_coeffs_key_build(const char *name,
                                            const conv_config_t *params,
                                            size_t *out_len) {
  conv_key_builder_t b = {0};
  int type = (int)params->type;
  conv_key_append_str(&b, name);
  conv_key_append(&b, &type, sizeof(type));
  switch (params->type) {
  case CONV_TYPE_VALUES: {
    size_t n = params->values_count;
    conv_key_append(&b, &n, sizeof(n));
    if (params->values && n > 0)
      conv_key_append(&b, params->values, n * sizeof(double));
    break;
  }
  case CONV_TYPE_WAV: {
    int channel = params->channel;
    conv_key_append_str(&b, params->filename);
    conv_key_append(&b, &channel, sizeof(channel));
    break;
  }
  case CONV_TYPE_RAW: {
    size_t skip = params->skip_bytes_lines;
    size_t read = params->read_bytes_lines;
    conv_key_append_str(&b, params->filename);
    conv_key_append_str(&b, params->format);
    conv_key_append(&b, &skip, sizeof(skip));
    conv_key_append(&b, &read, sizeof(read));
    break;
  }
  case CONV_TYPE_DUMMY: {
    int length = params->length;
    conv_key_append(&b, &length, sizeof(length));
    break;
  }
  }
  if (b.failed) {
    free(b.buf);
    return NULL;
  }
  *out_len = b.len;
  return b.buf;
}

static bool conv_coeffs_matches(const conv_coeffs_t *entry,
                                const unsigned char *key, size_t key_len,
                                size_t chunk_size) {
  return entry->generation == g_conv_cache_generation &&
         entry->chunk_size == chunk_size && entry->key_len == key_len &&
         memcmp(entry->key, key, key_len) == 0;
}

static conv_coeffs_t *conv_coeffs_cache_find(const unsigned char *key,
                                             size_t key_len,
                                             size_t chunk_size) {
  pthread_mutex_lock(&g_conv_cache_mutex);
  for (conv_coeffs_t *curr = g_conv_coeffs_cache; curr != NULL;
       curr = curr->next) {
    if (conv_coeffs_matches(curr, key, key_len, chunk_size)) {
      curr->ref_count++;
      pthread_mutex_unlock(&g_conv_cache_mutex);
      return curr;
    }
  }
  pthread_mutex_unlock(&g_conv_cache_mutex);
  return NULL;
}

/**
 * @brief Transforms @p coeffs into a new (or concurrently created) cache entry.
 *
 * Takes ownership of @p key in all cases.
 */
static conv_coeffs_t *conv_coeffs_create(unsigned char *key, size_t key_len,
                                         size_t chunk_size, size_t num_seg,
                                         size_t spec_len, const double *coeffs,
                                         size_t coeffs_count, real_fft_t *fft,
                                         size_t fft_len) {
  pthread_mutex_lock(&g_conv_cache_mutex);
  for (conv_coeffs_t *curr = g_conv_coeffs_cache; curr != NULL;
       curr = curr->next) {
    // A concurrent builder may have created the same entry since our lookup.
    // Only adopt it if its geometry matches what this filter was sized for.
    if (conv_coeffs_matches(curr, key, key_len, chunk_size) &&
        curr->num_segments == num_seg) {
      curr->ref_count++;
      pthread_mutex_unlock(&g_conv_cache_mutex);
      free(key);
      return curr;
    }
  }
  pthread_mutex_unlock(&g_conv_cache_mutex);

  conv_coeffs_t *entry = (conv_coeffs_t *)calloc(1, sizeof(conv_coeffs_t));
  if (!entry) {
    free(key);
    return NULL;
  }
  entry->key = key;
  entry->key_len = key_len;
  entry->chunk_size = chunk_size;
  entry->num_segments = num_seg;
  entry->spec_len = spec_len;
  size_t spec_stride = (spec_len + 3) & ~3;
  entry->spec_stride = spec_stride;
  entry->ref_count = 1;

  entry->data = (complex_t *)cdsp_aligned_alloc(64, num_seg * spec_stride *
                                                        sizeof(complex_t));
  if (!entry->data) {
    free(entry->key);
    free(entry);
    return NULL;
  }

  double *scratch = (double *)cdsp_aligned_alloc(64, fft_len * sizeof(double));
  if (!scratch) {
    cdsp_aligned_free(entry->data);
    free(entry->key);
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
    free(entry->key);
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

// MARK: - Parameter validation and coefficient loading

/**
 * @brief Checks the conv parameters that need no file I/O.
 */
static int conv_validate_params(const conv_config_t *params,
                                config_error_t *err) {
  switch (params->type) {
  case CONV_TYPE_VALUES:
    if (!params->values || params->values_count == 0) {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                       "Conv 'values' must be non-empty");
      return -1;
    }
    for (size_t i = 0; i < params->values_count; i++) {
      if (!isfinite(params->values[i])) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "Non-finite coefficient at index %zu", i);
        return -1;
      }
    }
    break;
  case CONV_TYPE_WAV:
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
    break;
  case CONV_TYPE_RAW:
    if (params->filename[0] == '\0') {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                       "Conv filter missing filename");
      return -1;
    }
    break;
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
 * @brief Reads and checks the impulse response of a WAV / RAW conv filter.
 *
 * Shared by validation and creation so the file read that actually builds
 * the filter gets the same checks (existence, non-empty, all finite) as the
 * validation probe.
 *
 * @return malloc'd coefficients (caller frees) with *out_count > 0, or NULL
 *         with @p err set.
 */
static double *conv_read_file_coeffs(const conv_config_t *params,
                                     size_t *out_count, config_error_t *err) {
  *out_count = 0;
  FILE *f = cdsp_fopen(params->filename, "rb");
  if (!f) {
    config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                     "Conv file '%s' cannot be opened or does not exist",
                     params->filename);
    return NULL;
  }
  // 64-bit seek/tell: plain long ftell() fails for files >= 2 GiB on LLP64
  // Windows and 32-bit targets, which the RF64-aware readers support.
  int64_t fsize = -1;
  if (cdsp_fseek64(f, 0, SEEK_END) == 0)
    fsize = cdsp_ftell64(f);
  fclose(f);
  if (fsize <= 0) {
    config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                     "Conv file '%s' is empty or invalid", params->filename);
    return NULL;
  }

  char err_msg[512] = {0};
  size_t count = 0;
  double *coeffs = NULL;
  if (params->type == CONV_TYPE_WAV) {
    coeffs = wav_read_channel_samples(params->filename, params->channel, &count,
                                      err_msg, sizeof(err_msg));
  } else {
    coeffs = raw_read_samples(
        params->filename, params->format, params->skip_bytes_lines,
        params->read_bytes_lines, &count, err_msg, sizeof(err_msg));
  }
  if (!coeffs || count == 0) {
    free(coeffs);
    if (err_msg[0] != '\0') {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER, "%s", err_msg);
    } else {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                       "Conv coefficients could not be read from '%s' "
                       "(unsupported encoding, channel out of range, or no "
                       "usable samples)",
                       params->filename);
    }
    return NULL;
  }
  for (size_t i = 0; i < count; i++) {
    if (!isfinite(coeffs[i])) {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                       "Non-finite coefficient at index %zu in '%s'", i,
                       params->filename);
      free(coeffs);
      return NULL;
    }
  }
  *out_count = count;
  return coeffs;
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
  if (conv_validate_params(params, err) != 0)
    return -1;
  if (params->type == CONV_TYPE_WAV || params->type == CONV_TYPE_RAW) {
    size_t count = 0;
    double *probe = conv_read_file_coeffs(params, &count, err);
    if (!probe)
      return -1;
    free(probe);
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
 * The coefficient cache is consulted before any file I/O, so channels 1..N-1
 * of a step reuse channel 0's transformed coefficients without re-reading the
 * file; only a cache miss reads (and fully re-checks) the impulse response.
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
  if (conv_validate_params(params, err) != 0)
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
  const char *full_name = name ? name : "convolution";
  strncpy(filter->name, full_name, sizeof(filter->name) - 1);
  filter->name[sizeof(filter->name) - 1] = '\0';
  filter->chunk_size = chunk_size;
  size_t fft_len = 2 * chunk_size;
  size_t spec_len = fft_len / 2 + 1;
  filter->spec_len = spec_len;

  const double *coeffs = NULL;
  size_t coeffs_count = 0;
  double *owned_coeffs = NULL;
  size_t key_len = 0;
  unsigned char *key = conv_coeffs_key_build(full_name, params, &key_len);
  if (!key) {
    config_error_set(err, CONFIG_ERR_PARSE,
                     "Failed to allocate convolution cache key");
    goto fail;
  }

  // Check cache for shared coefficients
  conv_coeffs_t *cached_coeffs =
      conv_coeffs_cache_find(key, key_len, chunk_size);
  if (cached_coeffs) {
    filter->coeffs = cached_coeffs;
    filter->num_segments = cached_coeffs->num_segments;
    free(key);
    key = NULL;
  } else {
    if (params->type == CONV_TYPE_VALUES) {
      coeffs = params->values;
      coeffs_count = params->values_count;
    } else if (params->type == CONV_TYPE_DUMMY) {
      size_t len = (size_t)params->length;
      owned_coeffs = (double *)calloc(len, sizeof(double));
      if (!owned_coeffs) {
        config_error_set(err, CONFIG_ERR_PARSE,
                         "Failed to allocate dummy convolution coefficients");
        goto fail;
      }
      owned_coeffs[0] = 1.0;
      coeffs = owned_coeffs;
      coeffs_count = len;
    } else {
      owned_coeffs = conv_read_file_coeffs(params, &coeffs_count, err);
      if (!owned_coeffs)
        goto fail;
      coeffs = owned_coeffs;
    }
    // Never build a filter from no coefficients (e.g. an allocation failure
    // inside a reader that left no message): that would silently mute the
    // channel instead of failing the build.
    if (!coeffs || coeffs_count == 0) {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                       "Conv filter '%s' has no coefficients", full_name);
      goto fail;
    }
    filter->num_segments = (coeffs_count + chunk_size - 1) / chunk_size;
  }

  size_t num_seg = filter->num_segments;
  size_t spec_stride = (spec_len + 3) & ~3;
  filter->spec_stride = spec_stride;
  filter->hist_f = (complex_t *)cdsp_aligned_alloc(64, num_seg * spec_stride *
                                                           sizeof(complex_t));
  if (!filter->hist_f) {
    config_error_set(err, CONFIG_ERR_PARSE,
                     "Failed to allocate convolution history buffer");
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

  // cdsp_aligned_alloc does not zero; start the scratch buffers clean.
  memset(filter->temp_buf, 0, spec_len * sizeof(complex_t));
  memset(filter->time_buf, 0, fft_len * sizeof(double));
  memset(filter->out_buf, 0, fft_len * sizeof(double));

  if (!filter->coeffs) {
    filter->coeffs =
        conv_coeffs_create(key, key_len, chunk_size, num_seg, spec_len, coeffs,
                           coeffs_count, filter->fft, fft_len);
    key = NULL; // ownership passed to conv_coeffs_create
    if (!filter->coeffs) {
      config_error_set(err, CONFIG_ERR_PARSE,
                       "Failed to allocate convolution coefficients");
      goto fail;
    }
  }

  free(owned_coeffs);
  return filter;

fail:
  if (err && err->type == CONFIG_ERR_NONE) {
    config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                     "Failed to initialize convolution filter '%s' (check IR "
                     "values or file format/existence)",
                     name ? name : "");
  }
  free(key);
  free(owned_coeffs);
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
 * @param instance Pointer to the convolution filter.
 * @param waveform In-place buffer containing the input block, which will be
 *                 overwritten with the output.
 * @param count Number of samples in the input block.
 */
static void convolution_filter_process(void *instance,
                                       mutable_waveform_t waveform,
                                       size_t count) {
  convolution_filter_t *filter = (convolution_filter_t *)instance;
  if (!filter || !waveform || count == 0 || filter->num_segments == 0 ||
      !filter->coeffs)
    return;
  size_t cs = filter->chunk_size;
  size_t len = count < cs ? count : cs;
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

  // 2. FFT the block directly into the current history slot (write_idx is
  //    advanced at the end, after the multiply-accumulate).
  complex_t *dest_slot = filter->hist_f + widx * spec_stride;
  real_fft_forward(filter->fft, filter->time_buf, dest_slot);

  // 3. Spectrum-domain multiply-accumulate across the segment history.
  //    seg=0 pairs the newest input with coeff[0]; seg=k pairs the input from
  //    `k` blocks ago with coeff[k].
  const complex_t *coeffs_data = filter->coeffs->data;
  dsp_ops_complex_multiply_interleaved(filter->hist_f + widx * spec_stride,
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
