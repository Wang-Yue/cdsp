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
  char name[128];
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
  /// Number of samples current_ramp_gains was laid out for.
  size_t ramp_frames;
  /// Centrally advanced fader levels; NULL for a self-driven filter.
  const fader_levels_t *levels;
  /// Bound mode: what the latest volume_filter_load_levels() found.
  bool bound_ramping;
  double bound_gain;
};

typedef struct volume_filter volume_filter_t;

/// Lay the dB ramp start_db -> end_db out over @p frames samples, like
/// upstream Volume::fill_ramp(start_db, end_db, frames).
static void fill_ramp_db(volume_filter_t *filter, double start_db,
                         double end_db, size_t frames) {
  if (!filter->current_ramp_gains)
    return;
  if (frames > filter->chunk_size)
    frames = filter->chunk_size;
  if (frames == 0)
    return;
  double stepsize = (end_db - start_db) / (double)frames;
  for (size_t val = 0; val < frames; val++) {
    double db_val = start_db + (double)val * stepsize;
    filter->current_ramp_gains[val] = double_from_db(db_val);
  }
  filter->ramp_frames = frames;
}

static void fill_ramp(volume_filter_t *filter, size_t frames) {
  if (filter->chunk_size == 0 || filter->ramptime_in_chunks <= 0 ||
      !filter->current_ramp_gains)
    return;
  double target_vol = filter->mute ? -100.0 : filter->target_volume;
  double ramprange =
      (target_vol - filter->ramp_start) / (double)filter->ramptime_in_chunks;
  double start_db =
      filter->ramp_start + ramprange * ((double)filter->ramp_step - 1.0);
  double end_db =
      (filter->ramp_step >= filter->ramptime_in_chunks)
          ? target_vol
          : (filter->ramp_start + ramprange * (double)filter->ramp_step);
  // Divide by the number of samples actually processed, not the configured
  // chunk size, so a shorter chunk still ends exactly on end_db.
  fill_ramp_db(filter, start_db, end_db, frames);
}

/// Bound mode: read this chunk's level of the filter's fader and, when it is
/// ramping, lay the ramp out over @p frames samples.
static void volume_filter_load_levels(volume_filter_t *filter, size_t frames) {
  int idx = (int)filter->fader;
  if (idx < 0 || idx >= FADER_COUNT)
    idx = 0;
  const fader_level_t *level = &filter->levels->faders[idx];
  filter->bound_ramping = level->ramping;
  if (level->ramping) {
    fill_ramp_db(filter, level->start_db, level->end_db, frames);
  } else {
    filter->bound_gain = level->gain;
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
    // Upstream stores ramp_time_ms as FiniteF32, so NaN/Inf never get here.
    if (!isfinite(params->ramp_time_ms)) {
      if (err) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "Ramp time must be a finite number");
      }
      return -1;
    }
    if (params->ramp_time_ms < 0.0) {
      if (err) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "Ramp time cannot be negative");
      }
      return -1;
    }
  }
  if (params->has_limit) {
    if (!isfinite(params->limit)) {
      if (err) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "Volume limit must be a finite number");
      }
      return -1;
    }
    if (params->limit > 50.0) {
      if (err) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "Volume limit cannot be above +50 dB");
      }
      return -1;
    }
    if (params->limit < -150.0) {
      if (err) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "Volume limit cannot be less than -150 dB");
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

  // Initialize state from shared parameters (only used by a self-driven
  // filter; a filter bound to fader levels keeps no ramp state).
  // Upstream CamillaDSP seeds from current_volume(fader), clamped to
  // volume_limit.
  double initial_vol = 0.0;
  if (proc_params) {
    double cur = processing_parameters_get_current_volume_for_fader(
        proc_params, filter->fader);
    double target = processing_parameters_get_target_volume_for_fader(
        proc_params, filter->fader);
    if (processing_parameters_get_pause_count(proc_params) == 0 && cur == 0.0 &&
        target != 0.0) {
      initial_vol =
          target < filter->volume_limit ? target : filter->volume_limit;
      // Publish the limited level, never one above volume_limit.
      processing_parameters_set_current_volume_for_fader(
          proc_params, initial_vol, filter->fader);
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
  filter->bound_gain = 1.0;

  return filter;
}

void volume_filter_prepare_frames(volume_filter_t *filter, size_t frames) {
  if (!filter)
    return;
  if (filter->levels) {
    volume_filter_load_levels(filter, frames);
    return;
  }
  if (!filter->processing_parameters)
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
    fill_ramp(filter, frames);
  }
}

void volume_filter_prepare_chunk(volume_filter_t *filter) {
  if (!filter)
    return;
  volume_filter_prepare_frames(filter, filter->chunk_size);
}

static void apply_constant_gain(mutable_waveform_t waveform, double gain,
                                size_t count) {
  if (gain == 1.0) {
    // Unity gain: no-op
  } else if (gain == 0.0) {
    dsp_ops_clear(waveform, count);
  } else {
    dsp_ops_scalar_multiply(waveform, gain, count);
  }
}

static void volume_filter_process(void *instance, mutable_waveform_t waveform,
                                  size_t count) {
  volume_filter_t *filter = (volume_filter_t *)instance;
  if (!filter || !waveform || count == 0)
    return;
  if (filter->levels) {
    // Pure reader of the centrally advanced fader (upstream Volume).
    if (!filter->externally_driven) {
      volume_filter_load_levels(filter, count);
    } else if (filter->bound_ramping && filter->ramp_frames != count) {
      volume_filter_load_levels(filter, count);
    }
    if (!filter->bound_ramping) {
      apply_constant_gain(waveform, filter->bound_gain, count);
    } else if (filter->current_ramp_gains) {
      size_t limit = count < filter->ramp_frames ? count : filter->ramp_frames;
      dsp_ops_multiply(filter->current_ramp_gains, waveform, limit);
    }
    return;
  }
  if (!filter->externally_driven) {
    volume_filter_prepare_frames(filter, count);
  }
  if (filter->ramp_step == 0) {
    apply_constant_gain(waveform, filter->target_linear_gain, count);
  } else if (filter->current_ramp_gains) {
    if (filter->ramp_frames != count) {
      // Prepared for a different length: lay the same step out over count.
      fill_ramp(filter, count);
    }
    size_t limit = count < filter->ramp_frames ? count : filter->ramp_frames;
    dsp_ops_multiply(filter->current_ramp_gains, waveform, limit);
  }
  if (!filter->externally_driven) {
    volume_filter_advance_ramp(filter);
  }
}

void volume_filter_advance_ramp(volume_filter_t *filter) {
  if (!filter || filter->levels)
    return;
  if (filter->ramp_step > 0) {
    double target_vol = filter->mute ? -100.0 : filter->target_volume;
    double ramprange =
        (target_vol - filter->ramp_start) / (double)filter->ramptime_in_chunks;
    double end_db =
        (filter->ramp_step >= filter->ramptime_in_chunks)
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

void volume_filter_bind_fader_levels(volume_filter_t *filter,
                                     const fader_levels_t *levels) {
  if (!filter)
    return;
  filter->levels = levels;
  filter->bound_ramping = false;
  filter->bound_gain = 1.0;
  filter->ramp_frames = 0;
}

static void volume_filter_transfer_state(void *dest_ptr, const void *src_ptr) {
  volume_filter_t *dest = (volume_filter_t *)dest_ptr;
  const volume_filter_t *src = (const volume_filter_t *)src_ptr;
  if (!dest || !src || dest == src)
    return;
  // A bound filter is stateless: its fader's ramp lives in the pipeline's
  // fader bank, which pipeline_transfer_state() carries over. Like upstream
  // Volume::update_parameters, a changed `fader` simply takes effect.
  if (dest->levels)
    return;
  // Self-driven filters: state belongs to one fader. Copying it to a filter
  // on another fader would start that fader from the wrong level and publish
  // the wrong current volume for it.
  if (dest->fader != src->fader)
    return;

  dest->target_volume = src->target_volume;
  dest->target_linear_gain = src->target_linear_gain;
  dest->mute = src->mute;
  dest->current_volume = src->current_volume;
  dest->ramp_start = src->ramp_start;
  dest->ramp_step = src->ramp_step;
  dest->last_pause_count = src->last_pause_count;

  // Upstream Faders::update_parameters: on a changed ramp time mid-ramp,
  // carry on from where the ramp is now at the new speed, or settle at once
  // when ramping is now disabled.
  if (dest->ramptime_in_chunks != src->ramptime_in_chunks &&
      dest->ramp_step > 0) {
    if (dest->ramptime_in_chunks > 0) {
      dest->ramp_start = dest->current_volume;
      dest->ramp_step = 1;
    } else {
      dest->current_volume = dest->mute ? -100.0 : dest->target_volume;
      dest->ramp_step = 0;
    }
  }
  // The next chunk sees the target above the new limit, and ramps down to it
  // from here.
  if (dest->current_volume > dest->volume_limit) {
    dest->current_volume = dest->volume_limit;
  }
  if (dest->ramp_step > 0) {
    fill_ramp(dest, dest->chunk_size);
  }
}

const filter_vtable_t g_volume_vtable = {.validate = volume_config_validate,
                                         .create = volume_filter_create,
                                         .process = volume_filter_process,
                                         .transfer_state =
                                             volume_filter_transfer_state,
                                         .free = volume_filter_free};
