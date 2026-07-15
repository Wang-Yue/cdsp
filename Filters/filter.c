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

static const filter_vtable_t* filter_vtable_from_type(filter_type_t type) {
  switch (type) {
    case FILTER_TYPE_BIQUAD:
      return &g_biquad_vtable;
    case FILTER_TYPE_BIQUAD_COMBO:
      return &g_biquad_combo_vtable;
    case FILTER_TYPE_CONV:
      return &g_convolution_vtable;
    case FILTER_TYPE_DELAY:
      return &g_delay_vtable;
    case FILTER_TYPE_DIFF_EQ:
      return &g_diffeq_vtable;
    case FILTER_TYPE_DITHER:
      return &g_dither_vtable;
    case FILTER_TYPE_GAIN:
      return &g_gain_vtable;
    case FILTER_TYPE_LIMITER:
      return &g_limiter_vtable;
    case FILTER_TYPE_LOOKAHEAD_LIMITER:
      return &g_lookahead_limiter_vtable;
    case FILTER_TYPE_LOUDNESS:
      return &g_loudness_vtable;
    case FILTER_TYPE_VOLUME:
      return &g_volume_vtable;
    default:
      return NULL;
  }
}

static filter_instance_type_t filter_instance_type_from_config(
    filter_type_t type) {
  switch (type) {
    case FILTER_TYPE_BIQUAD:
      return FILTER_INSTANCE_BIQUAD;
    case FILTER_TYPE_BIQUAD_COMBO:
      return FILTER_INSTANCE_BIQUAD_COMBO;
    case FILTER_TYPE_CONV:
      return FILTER_INSTANCE_CONVOLUTION;
    case FILTER_TYPE_DELAY:
      return FILTER_INSTANCE_DELAY;
    case FILTER_TYPE_DIFF_EQ:
      return FILTER_INSTANCE_DIFF_EQ;
    case FILTER_TYPE_DITHER:
      return FILTER_INSTANCE_DITHER;
    case FILTER_TYPE_GAIN:
      return FILTER_INSTANCE_GAIN;
    case FILTER_TYPE_LIMITER:
      return FILTER_INSTANCE_LIMITER;
    case FILTER_TYPE_LOOKAHEAD_LIMITER:
      return FILTER_INSTANCE_LOOKAHEAD_LIMITER;
    case FILTER_TYPE_LOUDNESS:
      return FILTER_INSTANCE_LOUDNESS;
    case FILTER_TYPE_VOLUME:
      return FILTER_INSTANCE_VOLUME;
  }
  return FILTER_INSTANCE_BIQUAD;
}

filter_t* filter_create(const char* name, const filter_config_t* config,
                        int sample_rate, size_t chunk_size,
                        processing_parameters_t* proc_params,
                        config_error_t* err) {
  if (filter_config_validate(config, sample_rate, err) != 0) return NULL;
  const filter_vtable_t* vtable = filter_vtable_from_type(config->type);
  if (!vtable) {
    logger_error(&g_logger, "Unknown filter type %d for '%s'", config->type,
                 name ? name : "unnamed");
    config_error_set(err, CONFIG_ERR_INVALID_FILTER, "Unknown filter type");
    return NULL;
  }

  void* instance =
      vtable->create(name, config, sample_rate, chunk_size, proc_params, err);
  if (!instance) {
    logger_error(&g_logger, "Failed to instantiate filter '%s'",
                 name ? name : "unnamed");
    return NULL;
  }

  filter_t* filter = (filter_t*)calloc(1, sizeof(filter_t));
  if (!filter) {
    vtable->free(instance);
    return NULL;
  }

  if (name) {
    strncpy(filter->name, name, sizeof(filter->name) - 1);
  } else {
    strcpy(filter->name, "filter");
  }
  filter->type = filter_instance_type_from_config(config->type);
  filter->vtable = vtable;
  filter->instance = instance;

  logger_debug(&g_logger, "Filter '%s' successfully created (type=%d)",
               filter->name, filter->type);
  return filter;
}

void filter_process(filter_t* filter, mutable_waveform_t waveform,
                    size_t count) {
  if (!filter || !waveform || count == 0 || !filter->instance ||
      !filter->vtable)
    return;
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
  const filter_vtable_t* vtable = filter_vtable_from_type(filter->type);
  if (vtable && vtable->validate) {
    return vtable->validate(filter, sample_rate, err);
  }
  return 0;
}
