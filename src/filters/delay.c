#include "filters/delay.h"

#include "audio/processing_parameters.h"
#include "config/config_error.h"
#include "config/config_gen.h"
#include "filters/biquad.h"
#include "filters/filter.h"
#include "utils/double_helpers.h"

struct delay_filter {
  char name[64];
  double *queue;
  size_t queue_count;
  size_t read_index;
  biquad_filter_t *biquad;
  biquad_config_t bq_params;
  bool has_coeffs;
};

typedef struct delay_filter delay_filter_t;

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Calculates integer delay and coefficients for fractional delay.
 *
 * If subsample is enabled, this function designs a Thiran allpass filter
 * to approximate the fractional part of the delay.
 *
 * @param delay_samples The total target delay in samples.
 * @param subsample True to enable fractional delay using a Thiran allpass
 * filter.
 * @param[out] out_integer_delay Pointer to store the computed integer delay
 * part.
 * @param[out] out_coeffs Pointer to store the computed biquad coefficients for
 * the fractional part.
 * @param[out] out_has_coeffs Pointer to store a boolean indicating if
 * coefficients were written.
 */
static void build_delay(double delay_samples, bool subsample,
                        int *out_integer_delay, biquad_config_t *out_params,
                        bool *out_has_coeffs) {
  *out_has_coeffs = false;
  out_params->type = BIQUAD_TYPE_FREE;
  if (!isfinite(delay_samples) || delay_samples < 0.0) {
    *out_integer_delay = 0;
    return;
  }
  if (delay_samples > 100000000.0) {
    delay_samples = 100000000.0;
  }
  if (subsample) {
    // If the delay is very small, we can't design a stable Thiran filter.
    if (delay_samples < 0.1) {
      *out_integer_delay = 0;
      return;
    }
    // For small delays between 0.1 and 1.1, design a 1st order Thiran allpass
    // filter.
    if (delay_samples < 1.1) {
      double coeff = (1.0 - delay_samples) / (1.0 + delay_samples);
      // 1st order Thiran allpass: coeffs a1 = coeff, b0 = coeff, b1 = 1.0, b2 =
      // 0.0, a2 = 0.0
      out_params->b0 = coeff;
      out_params->b1 = 1.0;
      out_params->b2 = 0.0;
      out_params->a1 = coeff;
      out_params->a2 = 0.0;
      *out_integer_delay = 0;
      *out_has_coeffs = true;
      return;
    }

    // For delays >= 1.1, split the delay into integer and fractional parts.
    double samples = floor(delay_samples);
    double fraction = delay_samples - samples;
    // Shift delay by 1 sample to allow Thiran filter design range to be stable.
    samples -= 1.0;
    fraction += 1.0;
    // Ensure the fraction is in the range [1.1, 2.1) to avoid stability issues
    // near the boundaries.
    if (fraction < 1.1) {
      samples -= 1.0;
      fraction += 1.0;
    }
    // 2nd order Thiran allpass design.
    double coeff1 = 2.0 * (2.0 - fraction) / (1.0 + fraction);
    double coeff2 = ((2.0 - fraction) / (2.0 + fraction)) *
                    ((1.0 - fraction) / (1.0 + fraction));
    out_params->b0 = coeff2;
    out_params->b1 = coeff1;
    out_params->b2 = 1.0;
    out_params->a1 = coeff1;
    out_params->a2 = coeff2;
    *out_integer_delay = (int)samples;
    *out_has_coeffs = true;
  } else {
    // If subsample is disabled, round to the nearest integer sample.
    *out_integer_delay = (int)round(delay_samples);
  }
}

/**
 * @brief Free the delay filter instance and its associated resources.
 *
 * @param filter The delay filter instance to free.
 */
static void delay_filter_free(void *instance) {
  delay_filter_t *filter = (delay_filter_t *)instance;
  if (!filter)
    return;
  if (filter->queue)
    free(filter->queue);
  if (filter->biquad)
    g_biquad_vtable.free(filter->biquad);
  free(filter);
}

/**
 * @brief Computes equivalent sample delay count given a delay duration, unit,
 * and sample rate.
 *
 * @param delay The delay value.
 * @param unit The delay time/distance unit.
 * @param sample_rate The audio sample rate in Hz.
 * @return The delay in samples.
 */
static double compute_delay_samples(double delay, delay_unit_t unit,
                                    int sample_rate) {
  switch (unit) {
  case DELAY_UNIT_MS:
    return delay / 1000.0 * (double)sample_rate;
  case DELAY_UNIT_US:
    return delay / 1000000.0 * (double)sample_rate;
  case DELAY_UNIT_S:
    return delay * (double)sample_rate;
  case DELAY_UNIT_SAMPLES:
    return delay;
  case DELAY_UNIT_MM:
    // Compute delay using speed of sound in air (approx. 343 m/s)
    return delay / 1000.0 * (double)sample_rate / 343.0;
  case DELAY_UNIT_INVALID:
    return 0.0;
  }
  CDSP_UNREACHABLE();
  return 0.0;
}

/**
 * @brief Validates delay filter parameters.
 *
 * @param config Pointer to the filter configuration to validate.
 * @param sample_rate The audio sample rate.
 * @param err Pointer to a config error struct to populate on failure.
 * @return 0 on success, -1 on failure.
 */
static int delay_config_validate(const filter_config_t *config, int sample_rate,
                                 config_error_t *err) {
  (void)sample_rate;
  if (!config || config->type != FILTER_TYPE_DELAY)
    return -1;
  const delay_config_t *params = &config->parameters.delay;
  if (!params)
    return 0;
  if (!isfinite(params->delay)) {
    if (err) {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                       "Delay must be a finite number");
    }
    return -1;
  }
  if (params->delay < 0.0) {
    if (err) {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                       "Delay cannot be negative");
    }
    return -1;
  }
  return 0;
}

/**
 * @brief Create a new delay filter.
 *
 * Builds the subsample biquad allpass filter if fractional delay is requested.
 *
 * @param name The name of the filter.
 * @param config The filter configuration defining delay value, unit, etc.
 * @param sample_rate The audio sample rate in Hz.
 * @param chunk_size Maximum number of frames per processing chunk.
 * @param proc_params Processing parameters.
 * @param err Pointer to a config error struct to populate on failure.
 * @return A pointer to the created delay filter, or NULL on failure.
 */
static void *delay_filter_create(const char *name,
                                 const filter_config_t *config, int sample_rate,
                                 size_t chunk_size,
                                 processing_parameters_t *proc_params,
                                 config_error_t *err) {
  (void)chunk_size;
  (void)proc_params;
  if (!config || config->type != FILTER_TYPE_DELAY)
    return NULL;
  const delay_config_t *params = &config->parameters.delay;
  if (delay_config_validate(config, sample_rate, err) != 0)
    return NULL;
  delay_filter_t *filter = (delay_filter_t *)calloc(1, sizeof(delay_filter_t));
  if (!filter) {
    config_error_set(err, CONFIG_ERR_PARSE,
                     "Failed to allocate delay filter wrapper");
    return NULL;
  }
  if (name) {
    strncpy(filter->name, name, sizeof(filter->name) - 1);
    filter->name[sizeof(filter->name) - 1] = '\0';
  } else {
    strcpy(filter->name, "delay");
  }

  double delay = params ? params->delay : 0.0;
  delay_unit_t unit = params ? params->delay_unit : DELAY_UNIT_MS;
  bool subsample = params ? params->subsample : false;

  double delay_samples = compute_delay_samples(delay, unit, sample_rate);

  int integer_delay = 0;
  biquad_config_t bq_params = {0};
  bool has_coeffs = false;
  build_delay(delay_samples, subsample, &integer_delay, &bq_params,
              &has_coeffs);

  if (integer_delay > 0) {
    filter->queue = (double *)calloc(integer_delay, sizeof(double));
    if (!filter->queue) {
      config_error_set(err, CONFIG_ERR_PARSE,
                       "Failed to allocate delay line buffer of length %d",
                       integer_delay);
      delay_filter_free(filter);
      return NULL;
    }
    filter->queue_count = integer_delay;
  } else {
    filter->queue = NULL;
    filter->queue_count = 0;
  }
  filter->read_index = 0;
  if (has_coeffs) {
    filter_config_t bq_cfg = {.type = FILTER_TYPE_BIQUAD,
                              .parameters.biquad = bq_params};
    filter->biquad = (biquad_filter_t *)g_biquad_vtable.create(
        "delay_biquad", &bq_cfg, sample_rate, 0, NULL, err);
    if (!filter->biquad) {
      delay_filter_free(filter);
      return NULL;
    }
  } else {
    filter->biquad = NULL;
  }
  filter->bq_params = bq_params;
  filter->has_coeffs = has_coeffs;
  return filter;
}

/**
 * @brief Process a block of samples in-place.
 *
 * @param filter The delay filter instance.
 * @param waveform The input/output waveform buffer.
 * @param count The number of samples to process.
 */
static void delay_filter_process(void *instance, mutable_waveform_t waveform,
                                 size_t count) {
  delay_filter_t *filter = (delay_filter_t *)instance;
  if (!filter || !waveform || count == 0)
    return;
  // Apply integer delay using the circular buffer.
  if (filter->queue && filter->queue_count > 0) {
    size_t ri = filter->read_index;
    size_t qc = filter->queue_count;
    double *q = filter->queue;
    for (size_t i = 0; i < count; i++) {
      double delayed = q[ri];
      q[ri] = waveform[i];   // Write current sample to buffer
      waveform[i] = delayed; // Output delayed sample
      ri++;
      if (ri >= qc)
        ri = 0;
    }
    filter->read_index = ri;
  }
  // Apply fractional delay filter (Thiran allpass) if configured.
  if (filter->biquad) {
    g_biquad_vtable.process(filter->biquad, waveform, count);
  }
}

double delay_filter_process_single(delay_filter_t *filter, double sample) {
  if (!filter)
    return sample;
  double out = sample;
  // Apply integer delay using the circular buffer.
  if (filter->queue && filter->queue_count > 0) {
    double delayed = filter->queue[filter->read_index];
    filter->queue[filter->read_index] =
        sample;    // Write current sample to buffer
    out = delayed; // Output delayed sample
    filter->read_index++;
    if (filter->read_index >= filter->queue_count)
      filter->read_index = 0;
  }
  // Apply fractional delay filter (Thiran allpass) if configured.
  if (filter->biquad) {
    out = biquad_filter_process_single(filter->biquad, out);
  }
  return out;
}

/**
 * @brief Transfers the delay line and the subsample allpass state from src to
 * dest.
 *
 * Resize invariant: the queue holds the last `queue_count` input samples and is
 * read back purely by age, so its contents only mean anything while the delay
 * length is unchanged. When the length changes, every retained sample would be
 * replayed at the wrong time, so nothing is carried — a freshly created filter
 * already has a zeroed queue, which reads as silence for the duration of the
 * new delay. The subsample allpass is dropped along with it, because its state
 * is the tail of that same discarded signal.
 *
 * @param dest_ptr Pointer to the destination delay filter instance.
 * @param src_ptr Pointer to the source delay filter instance.
 */
static void delay_filter_transfer_state(void *dest_ptr, const void *src_ptr) {
  delay_filter_t *dest = (delay_filter_t *)dest_ptr;
  const delay_filter_t *src = (const delay_filter_t *)src_ptr;
  if (!dest || !src || dest == src)
    return;

  if (dest->queue_count != src->queue_count)
    return;

  // Only transfer subsample biquad state if both instances have active biquads
  // and their filter coefficients match. A change in fractional delay changes
  // the allpass transfer function and its z-state cannot be carried over.
  if (dest->has_coeffs && src->has_coeffs && dest->biquad && src->biquad) {
    if (dest->bq_params.a1 == src->bq_params.a1 &&
        dest->bq_params.a2 == src->bq_params.a2 &&
        dest->bq_params.b0 == src->bq_params.b0 &&
        dest->bq_params.b1 == src->bq_params.b1 &&
        dest->bq_params.b2 == src->bq_params.b2) {
      g_biquad_vtable.transfer_state(dest->biquad, src->biquad);
    }
  }

  if (dest->queue && src->queue && dest->queue_count > 0) {
    memcpy(dest->queue, src->queue, dest->queue_count * sizeof(double));
    dest->read_index = src->read_index;
  }
}

const filter_vtable_t g_delay_vtable = {.validate = delay_config_validate,
                                        .create = delay_filter_create,
                                        .process = delay_filter_process,
                                        .transfer_state =
                                            delay_filter_transfer_state,
                                        .free = delay_filter_free};
