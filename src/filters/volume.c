#include "filters/volume.h"

#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "audio/processing_parameters.h"
#include "config/config_error.h"
#include "config/config_gen.h"
#include "filters/filter.h"
#include "utils/double_helpers.h"

struct volume_filter {
  char name[64];
  fader_t fader;
  double volume_limit;
  size_t chunk_size;
  int ramptime_in_chunks;
  uint64_t last_pause_count;
  double current_volume;
  double target_volume;
  double target_linear_gain;
  bool mute;
  double ramp_start;
  int ramp_step;
  double *current_ramp_gains;
  processing_parameters_t *processing_parameters;
  bool externally_driven;
};

typedef struct volume_filter volume_filter_t;

static void fill_ramp(volume_filter_t *filter) {
  if (filter->chunk_size == 0 || filter->ramptime_in_chunks <= 0 ||
      !filter->current_ramp_gains)
    return;
  double target_vol = filter->mute ? -100.0 : filter->target_volume;
  double ramprange =
      (target_vol - filter->ramp_start) / (double)filter->ramptime_in_chunks;
  double start_db =
      filter->ramp_start + ramprange * ((double)filter->ramp_step - 1.0);
  double end_db = (filter->ramp_step >= filter->ramptime_in_chunks)
                      ? target_vol
                      : (filter->ramp_start + ramprange * (double)filter->ramp_step);
  double stepsize = (end_db - start_db) / (double)filter->chunk_size;
  for (size_t val = 0; val < filter->chunk_size; val++) {
    double db_val = start_db + (double)val * stepsize;
    filter->current_ramp_gains[val] = double_from_db(db_val);
  }
}

static void volume_filter_free(void *instance) {
  volume_filter_t *filter = (volume_filter_t *)instance;
  if (!filter)
    return;
  if (filter->current_ramp_gains) {
    free(filter->current_ramp_gains);
    filter->current_ramp_gains = NULL;
  }
  free(filter);
}

static int volume_config_validate(const filter_config_t *config,
                                  int sample_rate, config_error_t *err) {
  (void)sample_rate;
  if (!config || config->type != FILTER_TYPE_VOLUME)
    return -1;
  const volume_config_t *params = &config->parameters.volume;
  if (!params)
    return 0;
  if (params->has_ramp_time_ms) {
    if (params->ramp_time_ms < 0.0) {
      if (err) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "Ramp time cannot be negative");
      }
      return -1;
    }
  }
  return 0;
}

static void *volume_filter_create(const char *name,
                                  const filter_config_t *config,
                                  int sample_rate, size_t chunk_size,
                                  processing_parameters_t *proc_params,
                                  config_error_t *err) {
  if (!config || config->type != FILTER_TYPE_VOLUME)
    return NULL;
  const volume_config_t *params = &config->parameters.volume;
  if (volume_config_validate(config, sample_rate, err) != 0)
    return NULL;
  if (sample_rate <= 0) {
    config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                     "VolumeFilter: sample_rate must be positive");
    return NULL;
  }
  if (chunk_size == 0) {
    config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                     "VolumeFilter: chunk_size must be positive");
    return NULL;
  }

  volume_filter_t *filter =
      (volume_filter_t *)calloc(1, sizeof(volume_filter_t));
  if (!filter) {
    config_error_set(err, CONFIG_ERR_PARSE,
                     "Failed to allocate volume filter wrapper");
    return NULL;
  }
  if (name) {
    strncpy(filter->name, name, sizeof(filter->name) - 1);
    filter->name[sizeof(filter->name) - 1] = '\0';
  } else {
    strcpy(filter->name, "volume");
  }
  filter->fader = params ? params->fader : FADER_MAIN;
  double ramp_time_ms =
      (params && params->has_ramp_time_ms) ? params->ramp_time_ms : 400.0;
  filter->volume_limit = (params && params->has_limit) ? params->limit : 50.0;
  filter->chunk_size = chunk_size;
  filter->processing_parameters = proc_params;

  double chunk_duration_ms = 1000.0 * (double)chunk_size / (double)sample_rate;
  if (chunk_duration_ms > 0.0) {
    double rc = round(ramp_time_ms / chunk_duration_ms);
    filter->ramptime_in_chunks =
        (rc > (double)INT_MAX) ? INT_MAX : (rc < 0.0 ? 0 : (int)rc);
  } else {
    filter->ramptime_in_chunks = 0;
  }
  filter->last_pause_count =
      proc_params ? processing_parameters_get_pause_count(proc_params) : 0ULL;
  filter->current_ramp_gains =
      (double *)calloc(chunk_size > 0 ? chunk_size : 1, sizeof(double));
  if (!filter->current_ramp_gains) {
    config_error_set(err, CONFIG_ERR_PARSE,
                     "Failed to allocate volume fader ramp gains array");
    free(filter);
    return NULL;
  }

  // Initialize state from shared parameters.
  // Upstream CamillaDSP seeds from current_volume(fader), clamped to volume_limit.
  double initial_vol = 0.0;
  if (proc_params) {
    double cur = processing_parameters_get_current_volume_for_fader(
        proc_params, filter->fader);
    double target = processing_parameters_get_target_volume_for_fader(
        proc_params, filter->fader);
    if (processing_parameters_get_pause_count(proc_params) == 0 && cur == 0.0 && target != 0.0) {
      initial_vol = target;
      processing_parameters_set_current_volume_for_fader(proc_params, target,
                                                         filter->fader);
    } else {
      initial_vol = cur;
    }
  }
  if (initial_vol > filter->volume_limit) {
    initial_vol = filter->volume_limit;
  }
  bool initial_mute =
      proc_params
          ? processing_parameters_is_muted_for_fader(proc_params, filter->fader)
          : false;

  double current_vol_with_mute = initial_mute ? -100.0 : initial_vol;
  filter->target_volume = initial_vol;
  filter->mute = initial_mute;
  filter->current_volume = current_vol_with_mute;
  filter->target_linear_gain =
      initial_mute ? 0.0 : double_from_db(current_vol_with_mute);
  filter->ramp_start = initial_vol;
  filter->ramp_step = 0;

  return filter;
}

void volume_filter_prepare_chunk(volume_filter_t *filter) {
  if (!filter || !filter->processing_parameters)
    return;
  double shared_vol = processing_parameters_get_target_volume_for_fader(
      filter->processing_parameters, filter->fader);
  bool shared_mute = processing_parameters_is_muted_for_fader(
      filter->processing_parameters, filter->fader);
  double target_vol =
      shared_vol < filter->volume_limit ? shared_vol : filter->volume_limit;

  uint64_t pause_count =
      processing_parameters_get_pause_count(filter->processing_parameters);
  bool resumed_after_pause = (pause_count != filter->last_pause_count);
  filter->last_pause_count = pause_count;

  if (fabs(target_vol - filter->target_volume) > 0.01 ||
      filter->mute != shared_mute) {
    if (filter->ramptime_in_chunks > 0 && !resumed_after_pause) {
      filter->ramp_start = filter->current_volume;
      filter->ramp_step = 1;
    } else {
      filter->current_volume = shared_mute ? -100.0 : target_vol;
      filter->ramp_step = 0;
    }
    filter->target_volume = target_vol;
    filter->target_linear_gain = shared_mute ? 0.0 : double_from_db(target_vol);
    filter->mute = shared_mute;
  }

  if (filter->ramp_step > 0 &&
      filter->ramp_step <= filter->ramptime_in_chunks) {
    fill_ramp(filter);
  }
}

static void volume_filter_process(void *instance, mutable_waveform_t waveform,
                                  size_t count) {
  volume_filter_t *filter = (volume_filter_t *)instance;
  if (!filter || !waveform || count == 0)
    return;
  if (!filter->externally_driven) {
    volume_filter_prepare_chunk(filter);
  }
  if (filter->ramp_step == 0) {
    if (filter->target_linear_gain == 1.0) {
      // Unity gain: no-op
    } else if (filter->target_linear_gain == 0.0) {
      dsp_ops_clear(waveform, count);
    } else {
      dsp_ops_scalar_multiply(waveform, filter->target_linear_gain, count);
    }
  } else if (filter->current_ramp_gains) {
    size_t limit = count < filter->chunk_size ? count : filter->chunk_size;
    dsp_ops_multiply(filter->current_ramp_gains, waveform, limit);
  }
  if (!filter->externally_driven) {
    volume_filter_advance_ramp(filter);
  }
}

void volume_filter_advance_ramp(volume_filter_t *filter) {
  if (!filter)
    return;
  if (filter->ramp_step > 0) {
    double target_vol = filter->mute ? -100.0 : filter->target_volume;
    double ramprange =
        (target_vol - filter->ramp_start) / (double)filter->ramptime_in_chunks;
    double end_db = (filter->ramp_step >= filter->ramptime_in_chunks)
                        ? target_vol
                        : (filter->ramp_start + ramprange * (double)filter->ramp_step);
    filter->current_volume = end_db;
    filter->ramp_step++;
    if (filter->ramp_step > filter->ramptime_in_chunks) {
      filter->ramp_step = 0;
      filter->current_volume = target_vol;
    }
  } else {
    filter->current_volume = filter->mute ? -100.0 : filter->target_volume;
  }
  if (filter->processing_parameters) {
    processing_parameters_set_current_volume_for_fader(
        filter->processing_parameters, filter->current_volume, filter->fader);
  }
}

void volume_filter_set_externally_driven(volume_filter_t *filter,
                                         bool externally_driven) {
  if (!filter)
    return;
  filter->externally_driven = externally_driven;
}

static void volume_filter_transfer_state(void *dest_ptr, const void *src_ptr) {
  volume_filter_t *dest = (volume_filter_t *)dest_ptr;
  const volume_filter_t *src = (const volume_filter_t *)src_ptr;
  if (!dest || !src || dest == src)
    return;
  dest->current_volume = src->current_volume;
  if (dest->current_volume > dest->volume_limit) {
    dest->current_volume = dest->volume_limit;
  }
  dest->target_volume = src->target_volume;
  dest->target_linear_gain = src->target_linear_gain;
  dest->mute = src->mute;
  dest->ramp_start = src->ramp_start;
  dest->last_pause_count = src->last_pause_count;

  if (src->ramp_step > 0 && src->ramptime_in_chunks > 0 &&
      dest->ramptime_in_chunks > 0) {
    double progress = (double)src->ramp_step / (double)src->ramptime_in_chunks;
    dest->ramp_step = (int)round(progress * dest->ramptime_in_chunks);
    if (dest->ramp_step < 1) {
      dest->ramp_step = 1;
    } else if (dest->ramp_step > dest->ramptime_in_chunks) {
      dest->ramp_step = dest->ramptime_in_chunks;
    }
  } else {
    dest->ramp_step = 0;
  }

  if (dest->chunk_size == src->chunk_size && dest->current_ramp_gains &&
      src->current_ramp_gains) {
    memcpy(dest->current_ramp_gains, src->current_ramp_gains,
           dest->chunk_size * sizeof(double));
  }
}

const filter_vtable_t g_volume_vtable = {.validate = volume_config_validate,
                                         .create = volume_filter_create,
                                         .process = volume_filter_process,
                                         .transfer_state =
                                             volume_filter_transfer_state,
                                         .free = volume_filter_free};
