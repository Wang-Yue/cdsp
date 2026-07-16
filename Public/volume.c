#include "Public/volume.h"
#include "Engine/dsp_engine.h"

float cdsp_get_volume(const dsp_engine_t* engine) {
  if (!engine) return 0.0f;
  dsp_engine_interface_t* iface = dsp_engine_get_interface((dsp_engine_t*)engine);
  return iface && iface->get_fader_volume ? iface->get_fader_volume(iface->ctx, (fader_t)0) : 0.0f;
}

void cdsp_set_volume(dsp_engine_t* engine, float db, bool instant) {
  if (!engine) return;
  dsp_engine_interface_t* iface = dsp_engine_get_interface(engine);
  if (iface && iface->set_fader_volume) {
    iface->set_fader_volume(iface->ctx, (fader_t)0, db, instant);
  }
}

bool cdsp_get_mute(const dsp_engine_t* engine) {
  if (!engine) return false;
  dsp_engine_interface_t* iface = dsp_engine_get_interface((dsp_engine_t*)engine);
  return iface && iface->is_fader_muted ? iface->is_fader_muted(iface->ctx, (fader_t)0) : false;
}

void cdsp_set_mute(dsp_engine_t* engine, bool mute) {
  if (!engine) return;
  dsp_engine_interface_t* iface = dsp_engine_get_interface(engine);
  if (iface && iface->set_fader_mute) {
    iface->set_fader_mute(iface->ctx, (fader_t)0, mute);
  }
}

float cdsp_get_fader_volume(const dsp_engine_t* engine, cdsp_fader_t fader) {
  if (!engine) return 0.0f;
  dsp_engine_interface_t* iface = dsp_engine_get_interface((dsp_engine_t*)engine);
  return iface && iface->get_fader_volume ? iface->get_fader_volume(iface->ctx, (fader_t)fader) : 0.0f;
}

void cdsp_set_fader_volume(dsp_engine_t* engine, cdsp_fader_t fader, float db, bool instant) {
  if (!engine) return;
  dsp_engine_interface_t* iface = dsp_engine_get_interface(engine);
  if (iface && iface->set_fader_volume) {
    iface->set_fader_volume(iface->ctx, (fader_t)fader, db, instant);
  }
}

bool cdsp_is_fader_muted(const dsp_engine_t* engine, cdsp_fader_t fader) {
  if (!engine) return false;
  dsp_engine_interface_t* iface = dsp_engine_get_interface((dsp_engine_t*)engine);
  return iface && iface->is_fader_muted ? iface->is_fader_muted(iface->ctx, (fader_t)fader) : false;
}

void cdsp_set_fader_mute(dsp_engine_t* engine, cdsp_fader_t fader, bool mute) {
  if (!engine) return;
  dsp_engine_interface_t* iface = dsp_engine_get_interface(engine);
  if (iface && iface->set_fader_mute) {
    iface->set_fader_mute(iface->ctx, (fader_t)fader, mute);
  }
}
