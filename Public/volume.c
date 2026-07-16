#include "Public/volume.h"

#include "Engine/dsp_engine.h"

float cdsp_get_volume(const dsp_engine_t* engine) {
  return engine && engine->get_fader_volume
             ? engine->get_fader_volume(engine->ctx, (fader_t)0)
             : 0.0f;
}

void cdsp_set_volume(dsp_engine_t* engine, float db, bool instant) {
  if (engine && engine->set_fader_volume) {
    engine->set_fader_volume(engine->ctx, (fader_t)0, db, instant);
  }
}

bool cdsp_get_mute(const dsp_engine_t* engine) {
  return engine && engine->is_fader_muted
             ? engine->is_fader_muted(engine->ctx, (fader_t)0)
             : false;
}

void cdsp_set_mute(dsp_engine_t* engine, bool mute) {
  if (engine && engine->set_fader_mute) {
    engine->set_fader_mute(engine->ctx, (fader_t)0, mute);
  }
}

float cdsp_get_fader_volume(const dsp_engine_t* engine, cdsp_fader_t fader) {
  return engine && engine->get_fader_volume
             ? engine->get_fader_volume(engine->ctx, (fader_t)fader)
             : 0.0f;
}

void cdsp_set_fader_volume(dsp_engine_t* engine, cdsp_fader_t fader, float db,
                           bool instant) {
  if (engine && engine->set_fader_volume) {
    engine->set_fader_volume(engine->ctx, (fader_t)fader, db, instant);
  }
}

bool cdsp_is_fader_muted(const dsp_engine_t* engine, cdsp_fader_t fader) {
  return engine && engine->is_fader_muted
             ? engine->is_fader_muted(engine->ctx, (fader_t)fader)
             : false;
}

void cdsp_set_fader_mute(dsp_engine_t* engine, cdsp_fader_t fader, bool mute) {
  if (engine && engine->set_fader_mute) {
    engine->set_fader_mute(engine->ctx, (fader_t)fader, mute);
  }
}
