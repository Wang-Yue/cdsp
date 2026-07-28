#include "lookahead_limiter.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "delay.h"
#include "filter.h"

/* =========================================================================
 * Core Lookahead Gain Implementation Structure & Helpers
 * ========================================================================= */

typedef struct lookahead_gain {
  double limit;
  int attack_samples;
  double release_coeff;
  double* history;
  size_t history_capacity;
  size_t history_read_idx;
  size_t history_write_idx;
  double release_gain;
  double* gain;
  size_t gain_capacity;
  size_t gain_len;
} lookahead_gain_t;

static inline double lookahead_window_get(const lookahead_gain_t* lg,
                                          size_t index,
                                          const double* detection) {
  if (index < (size_t)lg->attack_samples) {
    size_t lookahead_start = lg->history_capacity - lg->attack_samples;
    size_t real_idx =
        (lg->history_read_idx + lookahead_start + index) % lg->history_capacity;
    return lg->history[real_idx];
  } else {
    return detection[index - lg->attack_samples];
  }
}

static inline void history_push(lookahead_gain_t* lg, double sample) {
  lg->history[lg->history_write_idx] = sample;
  lg->history_write_idx = (lg->history_write_idx + 1) % lg->history_capacity;
  lg->history_read_idx = (lg->history_read_idx + 1) % lg->history_capacity;
}

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

static void calculate_envelope(lookahead_gain_t* lg, const double* detection,
                               size_t len) {
  if (len > lg->gain_capacity) {
    double* new_gain = (double*)realloc(lg->gain, len * sizeof(double));
    if (new_gain) {
      lg->gain = new_gain;
      lg->gain_capacity = len;
    } else {
      len = lg->gain_capacity;
    }
  }

  double peak = 1.0;
  int samples_since_peak = lg->attack_samples + 1;

  for (int i = (int)(lg->attack_samples + len) - 1; i >= 0; i--) {
    double input_sample = lookahead_window_get(lg, i, detection);
    double amplitude = fabs(input_sample);

    double gain = amplitude > lg->limit ? (lg->limit / amplitude) : 1.0;

    double ramp_gain = 1.0;
    if (samples_since_peak <= lg->attack_samples) {
      double ramp = (double)(lg->attack_samples - samples_since_peak) /
                    (double)(lg->attack_samples > 1 ? lg->attack_samples : 1);
      ramp_gain = 1.0 - (ramp * (1.0 - peak));
      samples_since_peak++;
    }

    if (gain < ramp_gain) {
      peak = gain;
      samples_since_peak = 1;
    } else {
      gain = ramp_gain;
    }

    if (i < (int)len) {
      lg->gain[i] = gain;
    }
  }

  for (size_t i = 0; i < len; i++) {
    if (lg->release_gain <= 1e-12 || !isfinite(lg->release_gain)) {
      lg->release_gain = 1e-12;
    }
    lg->release_gain = pow(lg->release_gain, lg->release_coeff);
    if (lg->release_gain > 1.0) lg->release_gain = 1.0;

    if (lg->gain[i] < lg->release_gain) {
      lg->release_gain = lg->gain[i];
    } else {
      lg->gain[i] = lg->release_gain;
    }
  }
  lg->gain_len = len;
}

/* =========================================================================
 * Common VTable Operations (shared by both Lookahead Gain and Limiter)
 * ========================================================================= */

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
  if (!config || config->type != FILTER_TYPE_LOOKAHEAD_LIMITER) return -1;
  const lookahead_limiter_config_t* params =
      &config->parameters.lookahead_limiter;
  if (!params) return 0;

  if (!isfinite(params->limit)) {
    if (err) {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                       "Limit must be a finite decibel value.");
    }
    return -1;
  }

  if (params->attack < 0.0) {
    if (err) {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                       "Attack time must be greater than or equal to 0.");
    }
    return -1;
  }

  double attack_samples = round(
      compute_time_samples(params->attack, params->attack_unit, sample_rate));
  if (attack_samples > (double)sample_rate) {
    if (err) {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                       "Lookahead limiter attack time must be less than or "
                       "equal to 1 second.");
    }
    return -1;
  }

  if (params->release < 0.0) {
    if (err) {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                       "Release time must be greater than or equal to 0.");
    }
    return -1;
  }

  return 0;
}

/**
 * @brief Create and initialize a lookahead gain instance.
 *
 * @param name Unique name for the filter (unused).
 * @param config Configuration specifying filter type and parameters.
 * @param sample_rate Audio sample rate in Hz.
 * @param chunk_size Block processing chunk size in samples.
 * @param proc_params Processing parameters (unused).
 * @param err Pointer to a config error struct to populate on failure.
 * @return Pointer to newly allocated lookahead_gain_t wrapper on success, or
 * NULL.
 */
static void* lookahead_gain_create_common(const char* name,
                                          const filter_config_t* config,
                                          int sample_rate, size_t chunk_size,
                                          processing_parameters_t* proc_params,
                                          config_error_t* err) {
  (void)name;
  (void)proc_params;
  if (!config || config->type != FILTER_TYPE_LOOKAHEAD_LIMITER) return NULL;
  const lookahead_limiter_config_t* params =
      &config->parameters.lookahead_limiter;
  if (lookahead_limiter_config_validate(config, sample_rate, err) != 0)
    return NULL;

  double limit;
  int attack_samples;
  double release_coeff;
  configure(params, sample_rate, &limit, &attack_samples, &release_coeff);

  lookahead_gain_t* lg = (lookahead_gain_t*)calloc(1, sizeof(lookahead_gain_t));
  if (!lg) return NULL;

  size_t history_len =
      (size_t)sample_rate > chunk_size ? (size_t)sample_rate : chunk_size;

  lg->limit = limit;
  lg->attack_samples = attack_samples;
  lg->release_coeff = release_coeff;
  lg->release_gain = 1.0;
  lg->history_capacity = history_len;
  lg->history = (double*)calloc(history_len, sizeof(double));
  lg->history_read_idx = 0;
  lg->history_write_idx = 0;

  size_t out_cap = chunk_size > 8192 ? chunk_size : 8192;
  lg->gain_capacity = out_cap;
  lg->gain = (double*)calloc(out_cap, sizeof(double));

  if (!lg->history || !lg->gain) {
    if (lg->history) free(lg->history);
    if (lg->gain) free(lg->gain);
    free(lg);
    return NULL;
  }

  return lg;
}

/**
 * @brief Free lookahead gain resources.
 *
 * @param instance Pointer to the lookahead gain instance to free.
 */
static void lookahead_gain_free_common(void* instance) {
  lookahead_gain_t* lg = (lookahead_gain_t*)instance;
  if (!lg) return;
  if (lg->history) free(lg->history);
  if (lg->gain) free(lg->gain);
  free(lg);
}

/**
 * @brief Transfers running envelope states and circular history buffers from
 * src to dest.
 *
 * @param dest_ptr Pointer to destination lookahead gain instance.
 * @param src_ptr Pointer to source lookahead gain instance.
 */
static void lookahead_gain_transfer_state_common(void* dest_ptr,
                                                 const void* src_ptr) {
  lookahead_gain_t* dest = (lookahead_gain_t*)dest_ptr;
  const lookahead_gain_t* src = (const lookahead_gain_t*)src_ptr;
  if (!dest || !src || dest == src) return;

  dest->release_gain = src->release_gain;

  if (dest->history && dest->history_capacity > 0 && src->history &&
      src->history_capacity > 0) {
    size_t dest_cap = dest->history_capacity;
    size_t src_cap = src->history_capacity;
    size_t copy_len = dest_cap < src_cap ? dest_cap : src_cap;

    memset(dest->history, 0, dest_cap * sizeof(double));

    size_t src_start_idx = src->history_write_idx;
    if (src_cap > copy_len) {
      src_start_idx = (src->history_write_idx + src_cap - copy_len) % src_cap;
    }
    size_t dest_start_idx = dest_cap - copy_len;

    for (size_t i = 0; i < copy_len; i++) {
      size_t src_idx = (src_start_idx + i) % src_cap;
      size_t dest_idx = dest_start_idx + i;
      dest->history[dest_idx] = src->history[src_idx];
    }
    dest->history_read_idx = 0;
    dest->history_write_idx = 0;
  }
}

/* =========================================================================
 * VTable 1: Lookahead Gain (Envelope Tracker)
 * ========================================================================= */

/**
 * @brief Compute lookahead gain envelope for peak detection signal in-place.
 *
 * @param instance Pointer to lookahead gain instance.
 * @param waveform Array containing peak detection samples, overwritten with
 * computed envelope.
 * @param count Length of peak detection array.
 */
static void lookahead_gain_process_envelope(void* instance,
                                            mutable_waveform_t waveform,
                                            size_t count) {
  lookahead_gain_t* lg = (lookahead_gain_t*)instance;
  if (!lg || !waveform || count == 0) return;

  calculate_envelope(lg, waveform, count);

  for (size_t i = 0; i < count; i++) {
    history_push(lg, waveform[i]);
  }

  memcpy(waveform, lg->gain, count * sizeof(double));
}

const filter_vtable_t g_lookahead_gain_vtable = {
    .validate = lookahead_limiter_config_validate,
    .create = lookahead_gain_create_common,
    .process = lookahead_gain_process_envelope,
    .transfer_state = lookahead_gain_transfer_state_common,
    .free = lookahead_gain_free_common};

/* =========================================================================
 * VTable 2: Lookahead Limiter (Full In-Place Waveform Limiter)
 * ========================================================================= */

/**
 * @brief Perform in-place audio limiting of a single channel.
 *
 * @param instance Pointer to lookahead gain instance.
 * @param waveform Array containing audio samples to limit in-place.
 * @param count Length of waveform array.
 */
static void lookahead_limiter_process_waveform(void* instance,
                                               mutable_waveform_t waveform,
                                               size_t count) {
  lookahead_gain_t* lg = (lookahead_gain_t*)instance;
  if (!lg || !waveform || count == 0) return;

  calculate_envelope(lg, waveform, count);

  size_t lookahead_start = lg->history_capacity - lg->attack_samples;
  for (size_t i = 0; i < count; i++) {
    double input_sample;
    if (i < (size_t)lg->attack_samples) {
      input_sample = lg->history[(lg->history_read_idx + lookahead_start + i) %
                                 lg->history_capacity];
    } else {
      input_sample = waveform[i - lg->attack_samples];
    }
    lg->gain[i] *= input_sample;
  }

  for (size_t i = 0; i < count; i++) {
    history_push(lg, waveform[i]);
  }

  memcpy(waveform, lg->gain, count * sizeof(double));
}

const filter_vtable_t g_lookahead_limiter_vtable = {
    .validate = lookahead_limiter_config_validate,
    .create = lookahead_gain_create_common,
    .process = lookahead_limiter_process_waveform,
    .transfer_state = lookahead_gain_transfer_state_common,
    .free = lookahead_gain_free_common};
