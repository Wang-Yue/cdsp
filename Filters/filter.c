#include "Filters/filter.h"

#include <stdlib.h>
#include <string.h>

#include "Audio/processing_parameters.h"
#include "Config/filter_config_types.h"
#include "Filters/biquad.h"
#include "Filters/biquad_combo.h"
#include "Filters/clipper.h"
#include "Filters/convolution.h"
#include "Filters/delay.h"
#include "Filters/diffeq.h"
#include "Filters/dither.h"
#include "Filters/gain.h"
#include "Filters/lookahead_limiter.h"
#include "Filters/loudness.h"
#include "Filters/volume.h"
#include "Logging/app_logger.h"
#include "Utils/double_helpers.h"

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
    case FILTER_TYPE_CLIPPER:
      return &g_clipper_vtable;
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
    case FILTER_TYPE_CLIPPER:
      return FILTER_INSTANCE_CLIPPER;
    case FILTER_TYPE_LOOKAHEAD_LIMITER:
      return FILTER_INSTANCE_LOOKAHEAD_LIMITER;
    case FILTER_TYPE_LOUDNESS:
      return FILTER_INSTANCE_LOUDNESS;
    case FILTER_TYPE_VOLUME:
      return FILTER_INSTANCE_VOLUME;
    case FILTER_TYPE_INVALID:
      break;
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
    logger_error(&g_logger, "Unknown filter type %s for '%s'",
                 filter_type_to_string(config->type), name ? name : "unnamed");
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

  logger_debug(&g_logger, "Filter '%s' successfully created (type=%s)",
               filter->name, filter_type_to_string(config->type));
  return filter;
}

void filter_process(filter_t* filter, mutable_waveform_t waveform,
                    size_t count) {
  if (!filter || !waveform || count == 0 || !filter->instance ||
      !filter->vtable)
    return;
  filter->vtable->process(filter->instance, waveform, count);
}

static const char* filter_instance_type_to_string(filter_instance_type_t type) {
  switch (type) {
    case FILTER_INSTANCE_BIQUAD:
      return "Biquad";
    case FILTER_INSTANCE_BIQUAD_COMBO:
      return "BiquadCombo";
    case FILTER_INSTANCE_CONVOLUTION:
      return "Conv";
    case FILTER_INSTANCE_DELAY:
      return "Delay";
    case FILTER_INSTANCE_DIFF_EQ:
      return "DiffEq";
    case FILTER_INSTANCE_DITHER:
      return "Dither";
    case FILTER_INSTANCE_GAIN:
      return "Gain";
    case FILTER_INSTANCE_CLIPPER:
      return "Clipper";
    case FILTER_INSTANCE_LOOKAHEAD_LIMITER:
      return "Limiter";
    case FILTER_INSTANCE_LOUDNESS:
      return "Loudness";
    case FILTER_INSTANCE_VOLUME:
      return "Volume";
    default:
      return "Unknown";
  }
}

void filter_transfer_state(filter_t* dest, const filter_t* src) {
  if (!dest || !src) return;
  if (dest->type != src->type) {
    logger_debug(&g_logger, "Filter '%s' type changed (%s -> %s), state reset",
                 filter_get_name(dest),
                 filter_instance_type_to_string(src->type),
                 filter_instance_type_to_string(dest->type));
    return;
  }
  if (!dest->instance || !dest->vtable) return;
  if (!dest->vtable->transfer_state) {
    logger_debug(
        &g_logger, "Filter '%s' (type=%s) is stateless, no state to transfer",
        filter_get_name(dest), filter_instance_type_to_string(dest->type));
    return;
  }
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

bool filter_config_is_biquad(const filter_config_t* config) {
  if (!config) return false;
  return config->type == FILTER_TYPE_BIQUAD ||
         config->type == FILTER_TYPE_BIQUAD_COMBO;
}

size_t filter_get_biquad_stage_count(const filter_t* filter) {
  if (!filter || !filter->instance) return 0;
  if (filter->type == FILTER_INSTANCE_BIQUAD) {
    return 1;
  }
  if (filter->type == FILTER_INSTANCE_BIQUAD_COMBO) {
    return biquad_combo_get_stage_count(filter->instance);
  }
  return 0;
}

size_t filter_get_biquad_stages(const filter_t* filter,
                                biquad_filter_t** out_stages,
                                size_t max_stages) {
  if (!filter || !filter->instance || !out_stages || max_stages == 0) return 0;
  if (filter->type == FILTER_INSTANCE_BIQUAD) {
    out_stages[0] = (biquad_filter_t*)filter->instance;
    return 1;
  }
  if (filter->type == FILTER_INSTANCE_BIQUAD_COMBO) {
    return biquad_combo_get_stages(filter->instance, out_stages, max_stages);
  }
  return 0;
}
