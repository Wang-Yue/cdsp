#include "filter.h"

#include <stdlib.h>
#include <string.h>

#include "Logging/app_logger.h"
#include "biquad.h"
#include "biquad_combo.h"
#include "convolution.h"
#include "delay.h"
#include "diffeq.h"
#include "dither.h"
#include "gain.h"
#include "limiter.h"
#include "lookahead_limiter.h"
#include "loudness.h"
#include "volume.h"

static const logger_t g_logger = {"dsp.filter"};

typedef struct filter_vtable {
  void (*process)(void* instance, mutable_waveform_t waveform, size_t count);
  void (*transfer_state)(void* dest, const void* src);
  void (*free)(void* instance);
} filter_vtable_t;

struct filter {
  char name[64];               /**< The unique name of this filter instance. */
  filter_instance_type_t type; /**< The type of the filter instance. */
  const filter_vtable_t* vtable; /**< Virtual table for polymorphic dispatch. */
  void* instance; /**< Pointer to the concrete filter instance data. */
};

/* --- Static VTable Instances --- */

static const filter_vtable_t g_biquad_vtable = {
    .process = (void (*)(void*, mutable_waveform_t, size_t))biquad_filter_process,
    .transfer_state =
        (void (*)(void*, const void*))biquad_filter_transfer_state,
    .free = (void (*)(void*))biquad_filter_free};

static const filter_vtable_t g_biquad_combo_vtable = {
    .process =
        (void (*)(void*, mutable_waveform_t, size_t))biquad_combo_filter_process,
    .transfer_state =
        (void (*)(void*, const void*))biquad_combo_filter_transfer_state,
    .free = (void (*)(void*))biquad_combo_filter_free};

static const filter_vtable_t g_convolution_vtable = {
    .process =
        (void (*)(void*, mutable_waveform_t, size_t))convolution_filter_process,
    .transfer_state = NULL,
    .free = (void (*)(void*))convolution_filter_free};

static const filter_vtable_t g_delay_vtable = {
    .process = (void (*)(void*, mutable_waveform_t, size_t))delay_filter_process,
    .transfer_state = NULL,
    .free = (void (*)(void*))delay_filter_free};

static const filter_vtable_t g_diffeq_vtable = {
    .process = (void (*)(void*, mutable_waveform_t, size_t))diffeq_filter_process,
    .transfer_state = NULL,
    .free = (void (*)(void*))diffeq_filter_free};

static const filter_vtable_t g_dither_vtable = {
    .process = (void (*)(void*, mutable_waveform_t, size_t))dither_filter_process,
    .transfer_state = NULL,
    .free = (void (*)(void*))dither_filter_free};

static const filter_vtable_t g_gain_vtable = {
    .process = (void (*)(void*, mutable_waveform_t, size_t))gain_filter_process,
    .transfer_state = NULL,
    .free = (void (*)(void*))gain_filter_free};

static const filter_vtable_t g_limiter_vtable = {
    .process = (void (*)(void*, mutable_waveform_t, size_t))limiter_filter_process,
    .transfer_state = NULL,
    .free = (void (*)(void*))limiter_filter_free};

static const filter_vtable_t g_lookahead_limiter_vtable = {
    .process = (void (*)(void*, mutable_waveform_t,
                         size_t))lookahead_limiter_filter_process,
    .transfer_state = NULL,
    .free = (void (*)(void*))lookahead_limiter_filter_free};

static const filter_vtable_t g_loudness_vtable = {
    .process = (void (*)(void*, mutable_waveform_t, size_t))loudness_filter_process,
    .transfer_state =
        (void (*)(void*, const void*))loudness_filter_transfer_state,
    .free = (void (*)(void*))loudness_filter_free};

static const filter_vtable_t g_volume_vtable = {
    .process = (void (*)(void*, mutable_waveform_t, size_t))volume_filter_process,
    .transfer_state =
        (void (*)(void*, const void*))volume_filter_transfer_state,
    .free = (void (*)(void*))volume_filter_free};

filter_t* filter_create(const char* name, const filter_config_t* config,
                        int sample_rate, size_t chunk_size,
                        processing_parameters_t* proc_params,
                        config_error_t* err) {
  if (filter_config_validate(config, sample_rate, err) != 0) return NULL;
  filter_t* filter = (filter_t*)calloc(1, sizeof(filter_t));
  if (!filter) {
    logger_error(&g_logger, "Failed to allocate filter wrapper for '%s'",
                 name ? name : "unnamed");
    config_error_set(err, CONFIG_ERR_PARSE,
                     "Failed to allocate filter wrapper");
    return NULL;
  }
  if (name) {
    strncpy(filter->name, name, sizeof(filter->name) - 1);
    filter->name[sizeof(filter->name) - 1] = '\0';
  } else {
    strcpy(filter->name, "filter");
  }

  switch (config->type) {
    case FILTER_TYPE_BIQUAD:
      filter->type = FILTER_INSTANCE_BIQUAD;
      filter->vtable = &g_biquad_vtable;
      filter->instance = biquad_filter_create(name, &config->parameters.biquad,
                                              sample_rate, err);
      break;
    case FILTER_TYPE_BIQUAD_COMBO:
      filter->type = FILTER_INSTANCE_BIQUAD_COMBO;
      filter->vtable = &g_biquad_combo_vtable;
      filter->instance = biquad_combo_filter_create(
          name, &config->parameters.biquad_combo, sample_rate, err);
      break;
    case FILTER_TYPE_CONV:
      filter->type = FILTER_INSTANCE_CONVOLUTION;
      filter->vtable = &g_convolution_vtable;
      filter->instance = convolution_filter_create(
          name, &config->parameters.conv, chunk_size, err);
      break;
    case FILTER_TYPE_DELAY:
      filter->type = FILTER_INSTANCE_DELAY;
      filter->vtable = &g_delay_vtable;
      filter->instance = delay_filter_create(name, &config->parameters.delay,
                                             sample_rate, err);
      break;
    case FILTER_TYPE_DIFF_EQ:
      filter->type = FILTER_INSTANCE_DIFF_EQ;
      filter->vtable = &g_diffeq_vtable;
      filter->instance =
          diffeq_filter_create(name, &config->parameters.diff_eq, err);
      break;
    case FILTER_TYPE_DITHER:
      filter->type = FILTER_INSTANCE_DITHER;
      filter->vtable = &g_dither_vtable;
      filter->instance =
          dither_filter_create(name, &config->parameters.dither, err);
      break;
    case FILTER_TYPE_GAIN:
      filter->type = FILTER_INSTANCE_GAIN;
      filter->vtable = &g_gain_vtable;
      filter->instance =
          gain_filter_create(name, &config->parameters.gain, err);
      break;
    case FILTER_TYPE_LIMITER:
      filter->type = FILTER_INSTANCE_LIMITER;
      filter->vtable = &g_limiter_vtable;
      filter->instance =
          limiter_filter_create(name, &config->parameters.limiter, err);
      break;
    case FILTER_TYPE_LOOKAHEAD_LIMITER:
      filter->type = FILTER_INSTANCE_LOOKAHEAD_LIMITER;
      filter->vtable = &g_lookahead_limiter_vtable;
      filter->instance = lookahead_limiter_filter_create(
          name, &config->parameters.lookahead_limiter, sample_rate, chunk_size, err);
      break;
    case FILTER_TYPE_LOUDNESS:
      filter->type = FILTER_INSTANCE_LOUDNESS;
      filter->vtable = &g_loudness_vtable;
      filter->instance = loudness_filter_create(
          name, &config->parameters.loudness, sample_rate, proc_params, err);
      break;
    case FILTER_TYPE_VOLUME:
      filter->type = FILTER_INSTANCE_VOLUME;
      filter->vtable = &g_volume_vtable;
      filter->instance =
          volume_filter_create(name, &config->parameters.volume, sample_rate,
                               chunk_size, proc_params, err);
      break;
    default:
      logger_error(&g_logger, "Unknown filter type %d for '%s'", config->type,
                   filter->name);
      config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                       "Unknown filter type %d for '%s'", config->type,
                       filter->name);
      free(filter);
      return NULL;
  }

  if (!filter->instance) {
    logger_error(&g_logger, "Failed to instantiate filter '%s'", filter->name);
    free(filter);
    return NULL;
  }
  logger_debug(&g_logger, "Filter '%s' successfully created (type=%d)",
               filter->name, filter->type);
  return filter;
}

void filter_process(filter_t* filter, mutable_waveform_t waveform,
                    size_t count) {
  if (!filter || !waveform || count == 0 || !filter->instance || !filter->vtable) return;
  filter->vtable->process(filter->instance, waveform, count);
}

void filter_transfer_state(filter_t* dest, const filter_t* src) {
  if (!dest || !src || dest->type != src->type || !dest->instance ||
      !dest->vtable || !dest->vtable->transfer_state)
    return;
  dest->vtable->transfer_state(dest->instance, src->instance);
  logger_info(&g_logger, "Transferred filter state for '%s'",
              filter_get_name(dest));
}

const char* filter_get_name(const filter_t* filter) {
  return filter ? filter->name : "";
}

void filter_free(filter_t* filter) {
  if (!filter) return;
  if (filter->instance && filter->vtable && filter->vtable->free) {
    filter->vtable->free(filter->instance);
  }
  free(filter);
}

int filter_config_validate(const filter_config_t* filter, int sample_rate,
                           config_error_t* err) {
  if (!filter) return 0;
  switch (filter->type) {
    case FILTER_TYPE_GAIN:
      return gain_config_validate(&filter->parameters.gain, err);
    case FILTER_TYPE_VOLUME:
      return volume_config_validate(&filter->parameters.volume, err);
    case FILTER_TYPE_LOUDNESS:
      return loudness_config_validate(&filter->parameters.loudness, err);
    case FILTER_TYPE_BIQUAD:
      return biquad_config_validate(&filter->parameters.biquad, sample_rate,
                                     err);
    case FILTER_TYPE_CONV:
      return convolution_config_validate(&filter->parameters.conv, err);
    case FILTER_TYPE_DELAY:
      return delay_config_validate(&filter->parameters.delay, err);
    case FILTER_TYPE_BIQUAD_COMBO:
      return biquad_combo_config_validate(&filter->parameters.biquad_combo,
                                           sample_rate, err);
    case FILTER_TYPE_DIFF_EQ:
      return diffeq_config_validate(&filter->parameters.diff_eq, err);
    case FILTER_TYPE_DITHER:
      return dither_config_validate(&filter->parameters.dither, err);
    case FILTER_TYPE_LIMITER:
      return limiter_config_validate(&filter->parameters.limiter, err);
    case FILTER_TYPE_LOOKAHEAD_LIMITER:
      return lookahead_limiter_config_validate(
          &filter->parameters.lookahead_limiter, sample_rate, err);
  }
  return 0;
}
