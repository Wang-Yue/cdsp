#include "Public/volume.h"
#include "Engine/dsp_engine.h"

float cdsp_get_volume(const dsp_engine_t* engine) {
  return dsp_engine_get_fader_volume(engine, (fader_t)0);
}

void cdsp_set_volume(dsp_engine_t* engine, float db, bool instant) {
  dsp_engine_set_fader_volume(engine, (fader_t)0, db, instant);
}

bool cdsp_get_mute(const dsp_engine_t* engine) {
  return dsp_engine_is_fader_muted(engine, (fader_t)0);
}

void cdsp_set_mute(dsp_engine_t* engine, bool mute) {
  dsp_engine_set_fader_mute(engine, (fader_t)0, mute);
}

float cdsp_get_fader_volume(const dsp_engine_t* engine, uint32_t fader_idx) {
  return dsp_engine_get_fader_volume(engine, (fader_t)fader_idx);
}

void cdsp_set_fader_volume(dsp_engine_t* engine, uint32_t fader_idx, float db, bool instant) {
  dsp_engine_set_fader_volume(engine, (fader_t)fader_idx, db, instant);
}

bool cdsp_is_fader_muted(const dsp_engine_t* engine, uint32_t fader_idx) {
  return dsp_engine_is_fader_muted(engine, (fader_t)fader_idx);
}

void cdsp_set_fader_mute(dsp_engine_t* engine, uint32_t fader_idx, bool mute) {
  dsp_engine_set_fader_mute(engine, (fader_t)fader_idx, mute);
}
