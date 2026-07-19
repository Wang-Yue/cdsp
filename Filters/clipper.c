#include "clipper.h"

#include "filter.h"

struct clipper_filter {
  char name[64];
  double clip_limit;
  bool soft_clip;
};

typedef struct clipper_filter clipper_filter_t;

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef ENABLE_ACCELERATE
#include <Accelerate/Accelerate.h>
#endif

/**
 * @brief Free the clipper filter instance.
 *
 * @param filter Pointer to the clipper filter instance to free.
 */
static void clipper_filter_free(void* instance) {
  clipper_filter_t* filter = (clipper_filter_t*)instance;
  if (filter) free(filter);
}

/**
 * @brief Validates clipper filter parameters.
 *
 * @param config Pointer to the clipper parameters to validate.
 * @param sample_rate The audio sample rate.
 * @param err Pointer to a config error struct to populate on failure.
 * @return 0 on success, -1 on failure.
 */
static int clipper_config_validate(const filter_config_t* config,
                                   int sample_rate, config_error_t* err) {
  (void)sample_rate;
  if (!config || config->type != FILTER_TYPE_CLIPPER) return -1;
  const clipper_config_t* params = &config->parameters.clipper;
  if (!params) return 0;
  if (!isfinite(params->clip_limit) || params->clip_limit < -120.0 ||
      params->clip_limit > 20.0) {
    config_error_set(
        err, CONFIG_ERR_INVALID_FILTER,
        "Clipper clip_limit must be between -120.0 dB and 20.0 dB, got %g",
        params->clip_limit);
    return -1;
  }
  return 0;
}

/**
 * @brief Create a clipper filter.
 *
 * @param name The name of the filter.
 * @param config Pointer to the filter configuration.
 * @param sample_rate The audio sample rate.
 * @param chunk_size Maximum number of frames per processing chunk.
 * @param proc_params Processing parameters.
 * @param err Optional pointer to receive configuration error detail on failure.
 * @return Pointer to the allocated clipper_filter_t, or NULL on failure.
 */
static void* clipper_filter_create(const char* name,
                                   const filter_config_t* config,
                                   int sample_rate, size_t chunk_size,
                                   processing_parameters_t* proc_params,
                                   config_error_t* err) {
  (void)sample_rate;
  (void)chunk_size;
  (void)proc_params;
  if (!config || config->type != FILTER_TYPE_CLIPPER) return NULL;
  const clipper_config_t* params = &config->parameters.clipper;
  if (clipper_config_validate(config, 0, err) != 0) return NULL;
  clipper_filter_t* filter =
      (clipper_filter_t*)calloc(1, sizeof(clipper_filter_t));
  if (!filter) return NULL;
  if (name) {
    strncpy(filter->name, name, sizeof(filter->name) - 1);
    filter->name[sizeof(filter->name) - 1] = '\0';
  } else {
    strcpy(filter->name, "clipper");
  }
  double limit_db = params ? params->clip_limit : 0.0;
  double limit = double_from_db(limit_db);
  if (limit <= 0.0 || !isfinite(limit)) {
    clipper_filter_free(filter);
    return NULL;
  }
  filter->clip_limit = limit;
  filter->soft_clip = params ? params->soft_clip : false;
  return filter;
}

/**
 * @brief Process a waveform buffer in-place by applying clipping.
 *
 * @param filter Pointer to the clipper filter instance.
 * @param waveform The waveform data to process.
 * @param count The number of samples to process.
 */
static void clipper_filter_process(void* instance, mutable_waveform_t waveform,
                                   size_t count) {
  clipper_filter_t* filter = (clipper_filter_t*)instance;
  if (!filter || !waveform || count == 0) return;
  if (filter->soft_clip) {
    double inv_limit = 1.0 / filter->clip_limit;
    for (size_t i = 0; i < count; i++) {
      // Scale waveform to range relative to clip limit.
      double scaled = waveform[i] * inv_limit;

      // Clamp scaled value to [-1.5, 1.5]. The soft clipping function
      // f(x) = x - x^3 / 6.75 is designed for this range, mapping it
      // smoothly to [-1.0, 1.0] with a flat slope at the boundaries.
      if (scaled < -1.5)
        scaled = -1.5;
      else if (scaled > 1.5)
        scaled = 1.5;

      // Apply the cubic soft-clipping polynomial: f(x) = x - x^3 / 6.75
      // and scale it back to the original clip limit.
      waveform[i] =
          (scaled - (scaled * scaled * scaled) / 6.75) * filter->clip_limit;
    }
  } else {
    double low_limit = -filter->clip_limit;
    double high_limit = filter->clip_limit;
#ifdef ENABLE_ACCELERATE
    vDSP_vclipD(waveform, 1, &low_limit, &high_limit, waveform, 1, count);
#else
    for (size_t i = 0; i < count; i++) {
      if (waveform[i] < low_limit)
        waveform[i] = low_limit;
      else if (waveform[i] > high_limit)
        waveform[i] = high_limit;
    }
#endif
  }
}

const filter_vtable_t g_clipper_vtable = {.validate = clipper_config_validate,
                                          .create = clipper_filter_create,
                                          .process = clipper_filter_process,
                                          .transfer_state = NULL,
                                          .free = clipper_filter_free};
