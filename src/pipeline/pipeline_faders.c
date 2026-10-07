#include "pipeline/pipeline_faders.h"

#include <limits.h>
#include <math.h>
#include <string.h>

#include "config/config_gen.h"
#include "utils/double_helpers.h"

/// Settings of an Aux fader that no Volume filter uses. With nothing to ramp,
/// its level follows the target directly (upstream UNUSED_AUX_FADER).
static const fader_settings_t k_unused_aux_fader = {.ramp_time_ms = 0.0,
                                                    .limit = 50.0};

bool pipeline_faders_collect_settings(const dsp_config_t *config,
                                      fader_settings_t out[FADER_COUNT],
                                      config_error_t *err) {
  const char *set_by[FADER_COUNT] = {NULL};
  for (size_t f = 0; f < FADER_COUNT; f++)
    out[f] = k_unused_aux_fader;
  if (!config)
    return true;
  out[0].ramp_time_ms = config->devices.has_volume_ramp_time_ms
                            ? config->devices.volume_ramp_time_ms
                            : 400.0;
  out[0].limit =
      config->devices.has_volume_limit ? config->devices.volume_limit : 50.0;

  if (!config->pipeline || config->pipeline_count == 0 || !config->filters ||
      config->filters_count == 0) {
    return true;
  }

  bool ok = true;
  for (size_t i = 0; i < config->pipeline_count; i++) {
    const pipeline_step_config_t *step = &config->pipeline[i];
    if (step->bypassed || step->type != PIPELINE_STEP_TYPE_FILTER)
      continue;
    if (step->has_channels && step->channels_count == 0)
      continue;
    for (size_t j = 0; j < step->names_count; j++) {
      const char *name = step->names ? step->names[j] : NULL;
      if (!name || name[0] == '\0')
        continue;
      const filter_config_t *filt = dsp_config_get_filter(config, name);
      if (!filt || filt->type != FILTER_TYPE_VOLUME)
        continue;
      const volume_config_t *vol = &filt->parameters.volume;
      int idx = (int)vol->fader;
      // Upstream Volume filters can only use Aux1..Aux4; the Main fader's
      // settings always come from the devices section.
      if (idx < 1 || idx >= FADER_COUNT)
        continue;
      fader_settings_t these = {
          .ramp_time_ms = vol->has_ramp_time_ms ? vol->ramp_time_ms : 400.0,
          .limit = vol->has_limit ? vol->limit : 50.0};
      if (!set_by[idx]) {
        out[idx] = these;
        set_by[idx] = name;
      } else if (ok && (out[idx].ramp_time_ms != these.ramp_time_ms ||
                        out[idx].limit != these.limit)) {
        ok = false;
        if (err) {
          config_error_set(
              err, CONFIG_ERR_INVALID_FILTER,
              "Volume filters '%s' and '%s' use the same fader %s, but have "
              "different ramp_time_ms or limit",
              set_by[idx], name, volume_fader_to_string(vol->fader));
        }
      }
    }
  }
  return ok;
}

static int ramp_time_in_chunks(double ramp_time_ms, size_t chunk_size,
                               int sample_rate) {
  if (chunk_size == 0 || sample_rate <= 0 || !(ramp_time_ms > 0.0))
    return 0;
  double chunk_ms = 1000.0 * (double)chunk_size / (double)sample_rate;
  double chunks = round(ramp_time_ms / chunk_ms);
  if (!(chunks > 0.0))
    return 0;
  return chunks > (double)INT_MAX ? INT_MAX : (int)chunks;
}

static inline double settled_level(const fader_ramp_t *f) {
  return f->mute ? FADER_MUTE_LEVEL_DB : f->target_db;
}

static inline double settled_gain(const fader_ramp_t *f) {
  return f->mute ? 0.0 : double_from_db(f->target_db);
}

static inline void set_constant(fader_level_t *level, double level_db,
                                double gain) {
  level->start_db = level_db;
  level->end_db = level_db;
  level->gain = gain;
  level->ramping = false;
}

static void seed_from_params(pipeline_faders_t *faders) {
  for (int i = 0; i < FADER_COUNT; i++) {
    fader_ramp_t *f = &faders->faders[i];
    // Every way a pipeline starts running is preceded upstream by
    // sync_volumes_to_target() (engine start, structural reload), and a
    // parameter-only reload carries the ramp state over instead (see
    // pipeline_faders_transfer()). Seeding from the target is therefore the
    // same as upstream's seeding from the synced current volume, without
    // writing to the shared parameters from the control thread.
    double target = faders->params
                        ? processing_parameters_get_target_volume_for_fader(
                              faders->params, (fader_t)i)
                        : 0.0;
    if (!isfinite(target))
      target = FADER_MUTE_LEVEL_DB;
    f->target_db = target < f->limit ? target : f->limit;
    f->mute = faders->params ? processing_parameters_is_muted_for_fader(
                                   faders->params, (fader_t)i)
                             : false;
    f->current_db = settled_level(f);
    f->ramp_start = f->current_db;
    f->ramp_step = 0;
    set_constant(&faders->levels.faders[i], f->current_db, settled_gain(f));
  }
  // Start in sync with the shared counter, so the first chunk is not mistaken
  // for a resume after a pause.
  faders->last_pause_count =
      faders->params ? processing_parameters_get_pause_count(faders->params)
                     : 0;
}

void pipeline_faders_init(pipeline_faders_t *faders,
                          const fader_settings_t settings[FADER_COUNT],
                          processing_parameters_t *params, size_t chunk_size,
                          int sample_rate) {
  if (!faders)
    return;
  memset(faders, 0, sizeof(*faders));
  faders->params = params;
  faders->chunk_size = chunk_size;
  faders->sample_rate = sample_rate;
  for (int i = 0; i < FADER_COUNT; i++) {
    const fader_settings_t *s = settings ? &settings[i] : &k_unused_aux_fader;
    faders->faders[i].ramp_chunks =
        ramp_time_in_chunks(s->ramp_time_ms, chunk_size, sample_rate);
    faders->faders[i].limit = s->limit;
  }
  seed_from_params(faders);
}

void pipeline_faders_reseed(pipeline_faders_t *faders) {
  if (!faders)
    return;
  seed_from_params(faders);
}

void pipeline_faders_transfer(pipeline_faders_t *dest,
                              const pipeline_faders_t *src) {
  if (!dest || !src || dest == src)
    return;
  for (int i = 0; i < FADER_COUNT; i++) {
    fader_ramp_t *d = &dest->faders[i];
    const fader_ramp_t *s = &src->faders[i];
    // dest keeps the ramp time and limit it was built with; the rest of the
    // state carries over.
    d->target_db = s->target_db;
    d->mute = s->mute;
    d->current_db = s->current_db;
    d->ramp_start = s->ramp_start;
    d->ramp_step = s->ramp_step;
    if (d->ramp_chunks != s->ramp_chunks && d->ramp_step > 0) {
      // Carry on from where the ramp is now, at the new speed.
      if (d->ramp_chunks > 0) {
        d->ramp_start = d->current_db;
        d->ramp_step = 1;
      } else {
        d->current_db = settled_level(d);
        d->ramp_step = 0;
      }
    }
    // The next chunk sees the target above the new limit, and ramps down to
    // it from here.
    if (d->current_db > d->limit)
      d->current_db = d->limit;
  }
  dest->levels = src->levels;
  dest->last_pause_count = src->last_pause_count;
}

void pipeline_faders_prepare_chunk(pipeline_faders_t *faders) {
  if (!faders || !faders->params)
    return;
  processing_parameters_t *params = faders->params;
  // Did audio flow stop between the previous chunk and this one? If so, any
  // volume change seen now was made while paused, and ramping it would fade
  // in from a level that is no longer relevant.
  uint64_t pause_count = processing_parameters_get_pause_count(params);
  bool resumed_after_pause = pause_count != faders->last_pause_count;
  faders->last_pause_count = pause_count;

  for (int idx = 0; idx < FADER_COUNT; idx++) {
    fader_ramp_t *f = &faders->faders[idx];
    fader_level_t *level = &faders->levels.faders[idx];
    bool shared_mute =
        processing_parameters_is_muted_for_fader(params, (fader_t)idx);
    double shared_target =
        processing_parameters_get_target_volume_for_fader(params, (fader_t)idx);
    if (!isfinite(shared_target)) {
      // Never let a non-finite target reach the gain: treat it as silence.
      shared_target = FADER_MUTE_LEVEL_DB;
    }
    double target_db = shared_target < f->limit ? shared_target : f->limit;

    if (fabs(target_db - f->target_db) > 0.01 || f->mute != shared_mute) {
      f->target_db = target_db;
      f->mute = shared_mute;
      if (f->ramp_chunks > 0 && !resumed_after_pause) {
        f->ramp_start = f->current_db;
        f->ramp_step = 1;
      } else {
        f->current_db = settled_level(f);
        f->ramp_step = 0;
      }
    }

    if (f->ramp_step == 0) {
      set_constant(level, f->current_db, settled_gain(f));
    } else {
      double range =
          (settled_level(f) - f->ramp_start) / (double)f->ramp_chunks;
      double start_db = f->ramp_start + range * (double)(f->ramp_step - 1);
      // The last step lands exactly on the settled level, so that the
      // constant gain that follows continues without a step.
      double end_db = (f->ramp_step >= f->ramp_chunks)
                          ? settled_level(f)
                          : f->ramp_start + range * (double)f->ramp_step;
      level->start_db = start_db;
      level->end_db = end_db;
      level->ramping = true;
      f->current_db = end_db;
      f->ramp_step++;
      if (f->ramp_step > f->ramp_chunks)
        f->ramp_step = 0;
    }
    processing_parameters_set_current_volume_for_fader(params, f->current_db,
                                                       (fader_t)idx);
  }
}
