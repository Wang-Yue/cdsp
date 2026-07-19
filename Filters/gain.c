#include "gain.h"

#include "filter.h"

struct gain_filter {
  char name[64];
  double linear_gain;
  bool muted;
};

typedef struct gain_filter gain_filter_t;

#include <stdlib.h>
#include <string.h>

/**
 * @brief Validates gain filter parameters.
 *
 * @param config Pointer to the gain parameters configuration to validate.
 * @param sample_rate The sample rate.
 * @param err Pointer to a config error struct to populate on failure.
 * @return 0 on success, -1 on failure.
 */
#include <math.h>

static int gain_config_validate(const filter_config_t* config, int sample_rate,
                                config_error_t* err) {
  (void)sample_rate;
  if (!config || config->type != FILTER_TYPE_GAIN) return -1;
  const gain_config_t* params = &config->parameters.gain;
  if (!params || !params->has_gain) {
    config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                     "gain is a required parameter for the Gain filter");
    return -1;
  }
  if (!isfinite(params->gain)) {
    config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                     "gain must be a finite number");
    return -1;
  }
  if (params->scale == GAIN_SCALE_LINEAR) {
    if (params->gain < -10.0 || params->gain > 10.0) {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                       "linear gain must be in [-10, 10], got %g",
                       params->gain);
      return -1;
    }
  } else {
    if (params->gain < -150.0 || params->gain > 150.0) {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                       "gain must be in [-150, 150] dB, got %g", params->gain);
      return -1;
    }
  }
  return 0;
}

/**
 * @brief Create a gain filter.
 *
 * @param name The name of the filter.
 * @param config Pointer to the filter configuration.
 * @param sample_rate The sample rate.
 * @param chunk_size Maximum number of frames per processing chunk.
 * @param proc_params Processing parameters.
 * @param err Optional pointer to receive configuration error detail on failure.
 * @return Pointer to the allocated gain_filter_t, or NULL on failure.
 */
static void* gain_filter_create(const char* name, const filter_config_t* config,
                                int sample_rate, size_t chunk_size,
                                processing_parameters_t* proc_params,
                                config_error_t* err) {
  (void)sample_rate;
  (void)chunk_size;
  (void)proc_params;
  if (!config || config->type != FILTER_TYPE_GAIN) return NULL;
  const gain_config_t* params = &config->parameters.gain;
  if (gain_config_validate(config, 0, err) != 0) return NULL;
  gain_filter_t* filter = (gain_filter_t*)calloc(1, sizeof(gain_filter_t));
  if (!filter) return NULL;
  if (name) {
    strncpy(filter->name, name, sizeof(filter->name) - 1);
    filter->name[sizeof(filter->name) - 1] = '\0';
  } else {
    strcpy(filter->name, "gain");
  }
  filter->muted = params->mute;
  double computed_gain = (params->scale == GAIN_SCALE_LINEAR)
                             ? params->gain
                             : double_from_db(params->gain);

  // Apply phase inversion if configured.
  if (params->inverted) {
    computed_gain *= -1.0;
  }
  filter->linear_gain = computed_gain;
  return filter;
}

/**
 * @brief Process a waveform buffer in-place by applying gain.
 *
 * @param filter Pointer to the gain filter instance.
 * @param waveform The waveform data to process.
 * @param count The number of samples to process.
 */
static void gain_filter_process(void* instance,
                                mutable_waveform_t waveform, size_t count) {
  gain_filter_t* filter = (gain_filter_t*)instance;
  if (!filter || !waveform || count == 0) return;
  if (filter->muted) {
    // If muted, we clear the buffer to output silence.
    dsp_ops_clear(waveform, count);
  } else if (filter->linear_gain != 1.0) {
    // Apply linear scaling factor.
    dsp_ops_scalar_multiply(waveform, filter->linear_gain, count);
  }
}

double gain_filter_process_single(gain_filter_t* filter, double sample) {
  if (!filter || filter->muted) return 0.0;
  return sample * filter->linear_gain;
}

/**
 * @brief Free the gain filter instance.
 *
 * @param filter Pointer to the gain filter instance to free.
 */
static void gain_filter_free(void* instance) {
  gain_filter_t* filter = (gain_filter_t*)instance;
  if (filter) free(filter);
}

const filter_vtable_t g_gain_vtable = {
    .validate = gain_config_validate,
    .create = gain_filter_create,
    .process = gain_filter_process,
    .transfer_state = NULL,
    .free = gain_filter_free};
