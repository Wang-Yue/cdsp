#include "cdsp/fader.h"

#include <math.h>

#include "cdsp/cdsp_pub_types.h"
#include "config/config_gen.h"
#include "engine/dsp_engine.h"

static inline float clamp_volume_db(float db) {
  if (isnan(db))
    return -150.0f;
  if (db > 50.0f)
    return 50.0f;
  if (db < -150.0f)
    return -150.0f;
  return db;
}

float cdsp_get_volume(const dsp_engine_t *engine) {
  return engine && engine->get_fader_volume
             ? engine->get_fader_volume(engine->ctx, (fader_t)0)
             : 0.0f;
}

void cdsp_set_volume(dsp_engine_t *engine, float db, bool instant) {
  if (engine && engine->set_fader_volume) {
    engine->set_fader_volume(engine->ctx, (fader_t)0, clamp_volume_db(db),
                             instant);
  }
}

bool cdsp_get_mute(const dsp_engine_t *engine) {
  return engine && engine->get_fader_mute
             ? engine->get_fader_mute(engine->ctx, (fader_t)0)
             : false;
}

void cdsp_set_mute(dsp_engine_t *engine, bool mute) {
  if (engine && engine->set_fader_mute) {
    engine->set_fader_mute(engine->ctx, (fader_t)0, mute);
  }
}

float cdsp_get_fader_volume(const dsp_engine_t *engine, cdsp_fader_t fader) {
  return engine && engine->get_fader_volume
             ? engine->get_fader_volume(engine->ctx, (fader_t)fader)
             : 0.0f;
}

void cdsp_set_fader_volume(dsp_engine_t *engine, cdsp_fader_t fader, float db,
                           bool instant) {
  if (engine && engine->set_fader_volume) {
    engine->set_fader_volume(engine->ctx, (fader_t)fader, clamp_volume_db(db),
                             instant);
  }
}

float cdsp_adjust_fader_volume(dsp_engine_t *engine, cdsp_fader_t fader,
                               float delta) {
  float current = cdsp_get_fader_volume(engine, fader);
  float new_vol = clamp_volume_db(current + delta);
  cdsp_set_fader_volume(engine, fader, new_vol, false);
  return new_vol;
}

float cdsp_adjust_fader_volume_clamped(dsp_engine_t *engine, cdsp_fader_t fader,
                                       float delta, float min_db,
                                       float max_db) {
  float current = cdsp_get_fader_volume(engine, fader);
  if (isnan(delta) || max_db < min_db) {
    return current;
  }
  float new_vol = current + delta;
  if (new_vol < min_db)
    new_vol = min_db;
  if (new_vol > max_db)
    new_vol = max_db;
  cdsp_set_fader_volume(engine, fader, new_vol, false);
  return new_vol;
}

float cdsp_adjust_volume(dsp_engine_t *engine, float delta) {
  return cdsp_adjust_fader_volume(engine, (cdsp_fader_t)0, delta);
}

bool cdsp_get_fader_mute(const dsp_engine_t *engine, cdsp_fader_t fader) {
  return engine && engine->get_fader_mute
             ? engine->get_fader_mute(engine->ctx, (fader_t)fader)
             : false;
}

void cdsp_set_fader_mute(dsp_engine_t *engine, cdsp_fader_t fader, bool mute) {
  if (engine && engine->set_fader_mute) {
    engine->set_fader_mute(engine->ctx, (fader_t)fader, mute);
  }
}
