#include "lookahead_limiter.h"

#include "delay.h"
#include "filter.h"

struct lookahead_limiter_filter {
  char name[64];
  double limit;
  int attack_samples;
  double release_coeff;
  double* lookahead_data;
  size_t lookahead_capacity;
  size_t lookahead_read_index;
  size_t lookahead_write_index;
  double release_gain;
  double* output_buffer;
  size_t output_buffer_capacity;
};

typedef struct lookahead_limiter_filter lookahead_limiter_filter_t;

#include <math.h>
#include <stdlib.h>
#include <string.h>

static double compute_time_samples(double value, time_unit_t unit,
                                   int sample_rate) {
  switch (unit) {
    case TIME_UNIT_US:
      return value / 1000000.0 * (double)sample_rate;
    case TIME_UNIT_MS:
      return value / 1000.0 * (double)sample_rate;
    case TIME_UNIT_S:
      return value * (double)sample_rate;
    case TIME_UNIT_SAMPLES:
      return value;
  }
  return 0.0;
}

/**
 * @brief Parses parameters and computes internal filter settings.
 *
 * @param params User-provided filter parameters.
 * @param sample_rate The sample rate.
 * @param out_limit Output pointer for the computed linear gain limit.
 * @param out_attack_samples Output pointer for attack time in samples.
 * @param out_release_coeff Output pointer for exponential release coefficient.
 */
static void configure(const lookahead_limiter_config_t* params, int sample_rate,
                      double* out_limit, int* out_attack_samples,
                      double* out_release_coeff) {
  double limit_db = params ? params->limit : 0.0;
  *out_limit = double_from_db(limit_db);
  time_unit_t attack_unit = params ? params->attack_unit : TIME_UNIT_MS;
  time_unit_t release_unit = params ? params->release_unit : TIME_UNIT_MS;
  double attack = params ? params->attack : 0.0;
  double release = params ? params->release : 0.0;
  *out_attack_samples =
      (int)round(compute_time_samples(attack, attack_unit, sample_rate));
  double release_samples =
      compute_time_samples(release, release_unit, sample_rate);
  if (release_samples > 0.0) {
    *out_release_coeff = exp(-1.0 / release_samples);
  } else {
    *out_release_coeff = 0.0;
  }
}

/**
 * @brief Pushes a sample into the lookahead circular buffer, overwriting the
 * oldest sample.
 *
 * @param filter The limiter filter instance.
 * @param sample The input sample value.
 */
static inline void push_overwrite(lookahead_limiter_filter_t* filter,
                                  double sample) {
  filter->lookahead_data[filter->lookahead_write_index] = sample;
  filter->lookahead_write_index =
      (filter->lookahead_write_index + 1) % filter->lookahead_capacity;
  filter->lookahead_read_index =
      (filter->lookahead_read_index + 1) % filter->lookahead_capacity;
}

/**
 * @brief Retrieves a sample from the circular lookahead buffer at an offset
 * from read index.
 *
 * @param filter The limiter filter instance.
 * @param idx Offset relative to the current read index.
 * @return The sample value.
 */
static inline double get_occupied(lookahead_limiter_filter_t* filter,
                                  size_t idx) {
  size_t real_idx =
      (filter->lookahead_read_index + idx) % filter->lookahead_capacity;
  return filter->lookahead_data[real_idx];
}

/**
 * @brief Free the lookahead limiter filter instance.
 *
 * @param filter Pointer to the lookahead limiter filter instance to free.
 */
static void lookahead_limiter_filter_free(void* instance) {
  lookahead_limiter_filter_t* filter = (lookahead_limiter_filter_t*)instance;
  if (!filter) return;
  if (filter->lookahead_data) free(filter->lookahead_data);
  if (filter->output_buffer) free(filter->output_buffer);
  free(filter);
}

/**
 * @brief Validates lookahead limiter filter parameters.
 *
 * @param config Pointer to the filter configuration to validate.
 * @param sample_rate The sample rate in Hz.
 * @param err Pointer to a config error structure to populate on failure.
 * @return 0 on success, -1 on failure.
 */
static int lookahead_limiter_config_validate(const filter_config_t* config,
                                             int sample_rate,
                                             config_error_t* err) {
  if (sample_rate <= 0) {
    config_error_set(
        err, CONFIG_ERR_INVALID_FILTER,
        "Lookahead Limiter: sample_rate must be greater than 0, got %d",
        sample_rate);
    return -1;
  }
  if (!config || config->type != FILTER_TYPE_LOOKAHEAD_LIMITER) return -1;
  const lookahead_limiter_config_t* params =
      &config->parameters.lookahead_limiter;
  if (!params) return 0;
  if (!isfinite(params->limit)) {
    config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                     "Lookahead Limiter limit must be finite, got %g",
                     params->limit);
    return -1;
  }
  if (!isfinite(params->attack)) {
    config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                     "Lookahead Limiter: attack must be finite");
    return -1;
  }
  if (params->attack < 0.0) {
    config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                     "Lookahead Limiter: attack cannot be negative, got %g",
                     params->attack);
    return -1;
  }
  if (!isfinite(params->release)) {
    config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                     "Lookahead Limiter: release must be finite");
    return -1;
  }
  if (params->release < 0.0) {
    config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                     "Lookahead Limiter: release cannot be negative, got %g",
                     params->release);
    return -1;
  }
  double attack_samples =
      compute_time_samples(params->attack, params->attack_unit, sample_rate);
  if (attack_samples > (double)sample_rate) {
    config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                     "Lookahead Limiter: attack time cannot be longer than 1 "
                     "second, got %g samples",
                     attack_samples);
    return -1;
  }
  return 0;
}

/**
 * @brief Create a lookahead limiter filter.
 *
 * @param name The name of the filter.
 * @param config Pointer to the filter configuration.
 * @param sample_rate The sample rate in Hz.
 * @param chunk_size The processing chunk size.
 * @param proc_params Processing parameters.
 * @param err Optional pointer to receive configuration error detail on failure.
 * @return Pointer to the allocated lookahead_limiter_filter_t, or NULL on
 * failure.
 */
static void* lookahead_limiter_filter_create(
    const char* name, const filter_config_t* config, int sample_rate,
    size_t chunk_size, processing_parameters_t* proc_params,
    config_error_t* err) {
  (void)proc_params;
  if (!config || config->type != FILTER_TYPE_LOOKAHEAD_LIMITER) return NULL;
  const lookahead_limiter_config_t* params =
      &config->parameters.lookahead_limiter;
  if (lookahead_limiter_config_validate(config, sample_rate, err) != 0)
    return NULL;
  lookahead_limiter_filter_t* filter = (lookahead_limiter_filter_t*)calloc(
      1, sizeof(lookahead_limiter_filter_t));
  if (!filter) return NULL;
  if (name) {
    strncpy(filter->name, name, sizeof(filter->name) - 1);
    filter->name[sizeof(filter->name) - 1] = '\0';
  } else {
    strcpy(filter->name, "lookahead_limiter");
  }

  double limit;
  int attack_samples;
  double release_coeff;
  configure(params, sample_rate, &limit, &attack_samples, &release_coeff);

  if (limit <= 0.0 || !isfinite(limit) || attack_samples < 0) {
    lookahead_limiter_filter_free(filter);
    return NULL;
  }

  size_t lookahead_len =
      (size_t)sample_rate > chunk_size ? (size_t)sample_rate : chunk_size;

  if ((size_t)attack_samples >= lookahead_len) {
    lookahead_limiter_filter_free(filter);
    return NULL;
  }

  filter->limit = limit;
  filter->attack_samples = attack_samples;
  filter->release_coeff = release_coeff;
  filter->release_gain = 1.0;
  filter->lookahead_capacity = lookahead_len;
  filter->lookahead_data = (double*)calloc(lookahead_len, sizeof(double));
  if (!filter->lookahead_data) {
    lookahead_limiter_filter_free(filter);
    return NULL;
  }
  filter->lookahead_read_index = 0;
  filter->lookahead_write_index = 0;

  // Pre-allocated output buffer to avoid heap allocation on the hot path
  size_t out_cap = chunk_size > 8192 ? chunk_size : 8192;
  filter->output_buffer_capacity = out_cap;
  filter->output_buffer = (double*)calloc(out_cap, sizeof(double));
  if (!filter->output_buffer) {
    lookahead_limiter_filter_free(filter);
    return NULL;
  }

  return filter;
}

/**
 * @brief Processes a slice of the waveform.
 *
 * This function implements a two-pass lookahead limiter algorithm:
 * 1. A backward pass that scans future samples (including the lookahead delay
 * buffer) and calculates the required gain reduction to prevent clipping,
 * applying a linear ramp-down (attack) leading up to any peak.
 * 2. A forward pass that applies an exponential release to the gain reduction
 * envelope.
 * 3. Finally, it applies the computed gain envelope to the delayed input
 * samples.
 *
 * @param filter The limiter filter instance.
 * @param waveform The current block of audio samples to process.
 * @param len The length of the slice (must be <= output_buffer_capacity).
 */
static void process_slice(lookahead_limiter_filter_t* filter,
                          mutable_waveform_t waveform, size_t len) {
  // The oldest sample in the lookahead buffer (delay line) starts at this
  // offset relative to the capacity.
  size_t lookahead_start = filter->lookahead_capacity - filter->attack_samples;
  double peak = 1.0;
  int samples_since_peak = filter->attack_samples + 1;

  // Backward pass: Scan from the future (end of the new waveform block)
  // to the past (oldest samples in the delay line).
  // This calculates the necessary gain reduction to smoothly anticipate peaks.
  for (int i = (int)(filter->attack_samples + len) - 1; i >= 0; i--) {
    double input_sample;
    if (i < filter->attack_samples) {
      // Access samples in the lookahead delay line (older inputs).
      input_sample = get_occupied(filter, lookahead_start + i);
    } else {
      // Access new input samples from the current block.
      input_sample = waveform[i - filter->attack_samples];
    }

    double amplitude = fabs(input_sample);
    // If the sample exceeds the limit, compute the required attenuation factor.
    double gain = amplitude > filter->limit ? (filter->limit / amplitude) : 1.0;

    // Smoothly ramp down the gain leading up to a peak (going backward, this
    // looks like ramping up to 1.0 from the peak).
    double ramp_gain = 1.0;
    if (samples_since_peak <= filter->attack_samples) {
      double ramp =
          (double)(filter->attack_samples - samples_since_peak) /
          (double)(filter->attack_samples > 1 ? filter->attack_samples : 1);
      ramp_gain = 1.0 - (ramp * (1.0 - peak));
      samples_since_peak++;
    }

    // If the current sample requires more attenuation than the ramp,
    // establish a new peak.
    if (gain < ramp_gain) {
      peak = gain;
      samples_since_peak = 1;
    } else {
      gain = ramp_gain;
    }

    // We only output gain values for the current block (length len).
    if (i < (int)len) {
      filter->output_buffer[i] = gain;
    }
  }

  // Forward pass: Apply exponential release to the gain envelope.
  for (size_t i = 0; i < len; i++) {
    // Release gain exponentially towards 1.0 using pow() in the log/dB domain.
    if (filter->release_gain <= 1e-12 || !isfinite(filter->release_gain)) {
      filter->release_gain = 1e-12;
    }
    filter->release_gain = pow(filter->release_gain, filter->release_coeff);
    if (filter->release_gain > 1.0) filter->release_gain = 1.0;

    if (filter->output_buffer[i] < filter->release_gain) {
      // Instantaneous gain reduction if the peak requires it.
      filter->release_gain = filter->output_buffer[i];
    } else {
      // Apply the slower release gain.
      filter->output_buffer[i] = filter->release_gain;
    }
  }

  // Apply gain reduction: Multiply the delayed input samples by the computed
  // gain.
  for (size_t i = 0; i < len; i++) {
    double input_sample;
    if (i < (size_t)filter->attack_samples) {
      input_sample = get_occupied(filter, lookahead_start + i);
    } else {
      input_sample = waveform[i - filter->attack_samples];
    }
    filter->output_buffer[i] *= input_sample;
  }

  // Update lookahead buffer and Output:
  // Push the current raw input into the lookahead delay buffer,
  // and write the computed limited samples to the output waveform.
  for (size_t i = 0; i < len; i++) {
    push_overwrite(filter, waveform[i]);
    waveform[i] = filter->output_buffer[i];
  }
}

/**
 * @brief Process a waveform buffer in-place by applying lookahead limiting.
 *
 * @param filter Pointer to the lookahead limiter filter instance.
 * @param waveform The waveform data to process.
 * @param count The number of samples to process.
 */
static void lookahead_limiter_filter_process(void* instance,
                                             mutable_waveform_t waveform,
                                             size_t count) {
  lookahead_limiter_filter_t* filter = (lookahead_limiter_filter_t*)instance;
  if (!filter || !waveform || count == 0) return;
  size_t processed = 0;
  while (processed < count) {
    size_t slice = count - processed;
    if (slice > filter->output_buffer_capacity) {
      slice = filter->output_buffer_capacity;
    }
    process_slice(filter, waveform + processed, slice);
    processed += slice;
  }
}

static void lookahead_limiter_filter_transfer_state(
    void* dest_ptr, const void* src_ptr) {
  lookahead_limiter_filter_t* dest = (lookahead_limiter_filter_t*)dest_ptr;
  const lookahead_limiter_filter_t* src = (const lookahead_limiter_filter_t*)src_ptr;
  if (!dest || !src || dest == src) return;

  dest->release_gain = src->release_gain;

  if (dest->lookahead_data && dest->lookahead_capacity > 0 &&
      src->lookahead_data && src->lookahead_capacity > 0) {
    size_t dest_cap = dest->lookahead_capacity;
    size_t src_cap = src->lookahead_capacity;
    size_t copy_len = dest_cap < src_cap ? dest_cap : src_cap;

    memset(dest->lookahead_data, 0, dest_cap * sizeof(double));

    size_t src_start_idx = src->lookahead_write_index;
    if (src_cap > copy_len) {
      src_start_idx =
          (src->lookahead_write_index + src_cap - copy_len) % src_cap;
    }
    size_t dest_start_idx = dest_cap - copy_len;

    for (size_t i = 0; i < copy_len; i++) {
      size_t src_idx = (src_start_idx + i) % src_cap;
      size_t dest_idx = dest_start_idx + i;
      dest->lookahead_data[dest_idx] = src->lookahead_data[src_idx];
    }
    dest->lookahead_read_index = 0;
    dest->lookahead_write_index = 0;
  }
}

const filter_vtable_t g_lookahead_limiter_vtable = {
    .validate = lookahead_limiter_config_validate,
    .create = lookahead_limiter_filter_create,
    .process = lookahead_limiter_filter_process,
    .transfer_state = lookahead_limiter_filter_transfer_state,
    .free = lookahead_limiter_filter_free};
