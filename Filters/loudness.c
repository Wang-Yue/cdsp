#include "loudness.h"

#include "biquad.h"
#include "filter.h"

struct loudness_filter {
  char name[64];
  int sample_rate;
  loudness_config_t params;
  biquad_filter_t* low_shelf_filter;
  biquad_filter_t* high_shelf_filter;
  double last_volume;
  bool is_processing_active;
  double midband_attenuation_db;
  processing_parameters_t* processing_parameters;
};

typedef struct loudness_filter loudness_filter_t;

#include <math.h>
#include <stdlib.h>
#include <string.h>

// RME ADI-2 DAC Loudness Curves
// https://www.rme-audio.de/downloads/adi2dac_e.pdf

/**
 * @brief Recomputes the low-shelf and high-shelf filter coefficients based on
 * the current volume.
 *
 * Implements the RME ADI-2 DAC Loudness Curve logic:
 * - A reference volume level is compared against the current volume.
 * - The difference determines a boost factor between 0.0 (no boost) and 1.0
 * (maximum configured boost).
 * - Low and high frequencies are boosted proportionally.
 * - Optionally, midband attenuation can be used instead of frequency boosts to
 * prevent clipping.
 *
 * @param filter The loudness filter instance.
 * @param volume The current volume in dB.
 */
static void recompute_shelves(loudness_filter_t* filter, double volume) {
  double ref = filter->params.has_reference_level
                   ? filter->params.reference_level
                   : -25.0;

  // Calculate boost factor. 1.0 boost factor corresponds to 20 dB or more
  // attenuation below the reference level.
  double diff = (ref - volume) / 20.0;
  double boost_factor = diff < 0.0 ? 0.0 : (diff > 1.0 ? 1.0 : diff);

  filter->is_processing_active = boost_factor > 0.001;

  // Calculate target low and high boosts.
  double low_boost =
      (filter->params.has_low_boost ? filter->params.low_boost : 10.0) *
      boost_factor;
  double high_boost =
      (filter->params.has_high_boost ? filter->params.high_boost : 10.0) *
      boost_factor;

  // If attenuate_mid is enabled, we lower the overall gain (attenuate midband)
  // by the maximum boost instead of boosting the shelves. This keeps the peak
  // gain at 0 dB, preventing digital clipping.
  if (filter->params.attenuate_mid) {
    double max_boost = low_boost > high_boost ? low_boost : high_boost;
    filter->midband_attenuation_db = -max_boost;
  } else {
    filter->midband_attenuation_db = 0.0;
  }

  // Low shelf at 70 Hz, 12 dB/oct slope
  // Update coefficients in-place to preserve biquad delay-line state (no
  // clicks)
  filter_config_t lp_cfg = {
      .type = FILTER_TYPE_BIQUAD,
      .parameters.biquad = {.type = BIQUAD_TYPE_LOWSHELF,
                            .freq = 70.0,
                            .gain = low_boost,
                            .slope = 12.0,
                            .steepness_type = STEEPNESS_TYPE_SLOPE}};
  biquad_filter_update_parameters(filter->low_shelf_filter, &lp_cfg,
                                  filter->sample_rate);

  // High shelf at 3500 Hz, 12 dB/oct slope
  filter_config_t hp_cfg = {
      .type = FILTER_TYPE_BIQUAD,
      .parameters.biquad = {.type = BIQUAD_TYPE_HIGHSHELF,
                            .freq = 3500.0,
                            .gain = high_boost,
                            .slope = 12.0,
                            .steepness_type = STEEPNESS_TYPE_SLOPE}};
  biquad_filter_update_parameters(filter->high_shelf_filter, &hp_cfg,
                                  filter->sample_rate);
}

/**
 * @brief Frees the resources allocated for the loudness filter instance.
 *
 * @param filter Pointer to the loudness filter instance to free.
 */
static void loudness_filter_free(void* instance) {
  loudness_filter_t* filter = (loudness_filter_t*)instance;
  if (!filter) return;
  if (filter->low_shelf_filter) g_biquad_vtable.free(filter->low_shelf_filter);
  if (filter->high_shelf_filter)
    g_biquad_vtable.free(filter->high_shelf_filter);
  free(filter);
}

/**
 * @brief Validates loudness filter parameters.
 *
 * @param config Pointer to the loudness parameters configuration to validate.
 * @param sample_rate The sample rate.
 * @param err Pointer to a config error struct to populate on failure.
 * @return 0 on success, -1 on failure.
 */
static int loudness_config_validate(const filter_config_t* config,
                                    int sample_rate, config_error_t* err) {
  (void)sample_rate;
  if (!config || config->type != FILTER_TYPE_LOUDNESS) return -1;
  const loudness_config_t* params = &config->parameters.loudness;
  if (!params) return 0;
  if (!params->has_reference_level) {
    if (err)
      config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                       "Loudness filter requires 'reference_level'");
    return -1;
  }
  if (params->reference_level > 20.0) {
    if (err) {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                       "Reference level must be less than 20");
    }
    return -1;
  }
  if (params->reference_level < -100.0) {
    if (err) {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                       "Reference level must be higher than -100");
    }
    return -1;
  }

  double high_boost = params->has_high_boost ? params->high_boost : 10.0;
  double low_boost = params->has_low_boost ? params->low_boost : 10.0;

  if (high_boost < 0.0) {
    if (err) {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                       "High boost cannot be less than 0");
    }
    return -1;
  }
  if (high_boost > 20.0) {
    if (err) {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                       "High boost cannot be larger than 20");
    }
    return -1;
  }
  if (low_boost < 0.0) {
    if (err) {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                       "Low boost cannot be less than 0");
    }
    return -1;
  }
  if (low_boost > 20.0) {
    if (err) {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                       "Low boost cannot be larger than 20");
    }
    return -1;
  }
  return 0;
}

/**
 * @brief Creates a new loudness filter instance.
 *
 * @param name The name of the filter.
 * @param config Pointer to the loudness filter configuration.
 * @param sample_rate The sample rate of the audio processing path.
 * @param chunk_size Maximum number of frames per processing chunk.
 * @param proc_params Pointer to shared processing parameters.
 * @param err Pointer to a config error struct to populate on failure.
 * @return A pointer to the newly created loudness_filter_t instance, or NULL on
 * failure.
 */
static void* loudness_filter_create(const char* name,
                                    const filter_config_t* config,
                                    int sample_rate, size_t chunk_size,
                                    processing_parameters_t* proc_params,
                                    config_error_t* err) {
  (void)chunk_size;
  if (!config || config->type != FILTER_TYPE_LOUDNESS) return NULL;
  const loudness_config_t* params = &config->parameters.loudness;
  if (loudness_config_validate(config, sample_rate, err) != 0) return NULL;
  loudness_filter_t* filter =
      (loudness_filter_t*)calloc(1, sizeof(loudness_filter_t));
  if (!filter) {
    config_error_set(err, CONFIG_ERR_PARSE,
                     "Failed to allocate loudness filter wrapper");
    return NULL;
  }
  if (name) {
    strncpy(filter->name, name, sizeof(filter->name) - 1);
    filter->name[sizeof(filter->name) - 1] = '\0';
  } else {
    strcpy(filter->name, "loudness");
  }
  filter->sample_rate = sample_rate;
  if (params) {
    filter->params = *params;
  } else {
    filter->params = (loudness_config_t){0};
  }
  filter->processing_parameters = proc_params;
  filter->low_shelf_filter = (biquad_filter_t*)g_biquad_vtable.create(
      "loudness_ls", NULL, sample_rate, 0, NULL, err);
  filter->high_shelf_filter = (biquad_filter_t*)g_biquad_vtable.create(
      "loudness_hs", NULL, sample_rate, 0, NULL, err);
  if (!filter->low_shelf_filter || !filter->high_shelf_filter) {
    loudness_filter_free(filter);
    return NULL;
  }

  double init_vol = processing_parameters_get_current_volume_for_fader(
      filter->processing_parameters, filter->params.fader);
  filter->last_volume = init_vol;
  recompute_shelves(filter, init_vol);

  return filter;
}

/**
 * @brief Processes a slice of waveform using the loudness filter.
 *
 * @param filter Pointer to the loudness filter instance.
 * @param waveform The waveform containing the samples to be processed.
 * @param count The number of samples to process.
 */
static void loudness_filter_process(void* instance, mutable_waveform_t waveform,
                                    size_t count) {
  loudness_filter_t* filter = (loudness_filter_t*)instance;
  if (!filter || !waveform || count == 0) return;
  if (!filter->processing_parameters) return;

  double current_vol = processing_parameters_get_current_volume_for_fader(
      filter->processing_parameters, filter->params.fader);

  // Recompute filter coefficients only if the volume has changed significantly.
  if (fabs(current_vol - filter->last_volume) > 0.01) {
    filter->last_volume = current_vol;
    recompute_shelves(filter, current_vol);
  }

  // If the volume is high enough that loudness compensation is not needed,
  // bypass processing.
  if (!filter->is_processing_active) return;

  // Apply high-shelf and low-shelf filters.
  g_biquad_vtable.process(filter->high_shelf_filter, waveform, count);
  g_biquad_vtable.process(filter->low_shelf_filter, waveform, count);

  // Apply midband attenuation if enabled to simulate bass/treble boost
  // without exceeding 0 dBFS peak gain.
  if (filter->params.attenuate_mid &&
      fabs(filter->midband_attenuation_db) > 0.001) {
    double factor = double_from_db(filter->midband_attenuation_db);
    dsp_ops_scalar_multiply(waveform, factor, count);
  }
}

/**
 * @brief Transfers nested shelf filter states and last volume level from src to
 * dest.
 *
 * @param dest The destination loudness filter instance.
 * @param src The source loudness filter instance.
 */
static void loudness_filter_transfer_state(void* dest_ptr,
                                           const void* src_ptr) {
  loudness_filter_t* dest = (loudness_filter_t*)dest_ptr;
  const loudness_filter_t* src = (const loudness_filter_t*)src_ptr;
  if (!dest || !src || dest == src) return;
  g_biquad_vtable.transfer_state(dest->low_shelf_filter, src->low_shelf_filter);
  g_biquad_vtable.transfer_state(dest->high_shelf_filter,
                                 src->high_shelf_filter);
  dest->last_volume = src->last_volume;
  recompute_shelves(dest, dest->last_volume);
}


const filter_vtable_t g_loudness_vtable = {
    .validate = loudness_config_validate,
    .create = loudness_filter_create,
    .process = loudness_filter_process,
    .transfer_state = loudness_filter_transfer_state,
    .free = loudness_filter_free};
