#include "Public/processing.h"
#include <math.h>
#include <stdlib.h>
#include "Engine/dsp_engine.h"

cdsp_processing_state_t cdsp_get_state(const dsp_engine_t* engine) {
  if (!engine) return CDSP_PROCESSING_STATE_INACTIVE;
  state_update_t status = dsp_engine_get_status(engine);
  return (cdsp_processing_state_t)status.state;
}

void cdsp_get_stop_reason(const dsp_engine_t* engine, cdsp_stop_reason_t* out_reason) {
  if (!out_reason) return;
  if (!engine) {
    out_reason->type = CDSP_STOP_REASON_NONE;
    out_reason->message[0] = '\0';
    out_reason->format_change_rate = 0;
    return;
  }
  state_update_t status = dsp_engine_get_status(engine);
  out_reason->type = (cdsp_stop_reason_type_t)status.stop_reason.type;
  strncpy(out_reason->message, status.stop_reason.message, sizeof(out_reason->message) - 1);
  out_reason->message[sizeof(out_reason->message) - 1] = '\0';
  out_reason->format_change_rate = status.stop_reason.format_change_rate;
}

int cdsp_get_capture_rate(const dsp_engine_t* engine) {
  if (!engine) return 0;
  state_update_t status = dsp_engine_get_status(engine);
  if (status.state != PROCESSING_STATE_RUNNING) {
    return 0;
  }
  dsp_engine_interface_t* iface = dsp_engine_get_interface((dsp_engine_t*)engine);
  return iface && iface->get_active_samplerate ? iface->get_active_samplerate(iface->ctx) : 0;
}

double cdsp_get_signal_range(const dsp_engine_t* engine) {
  if (!engine) return 0.0;
  vu_levels_t vu = dsp_engine_get_vu_levels(engine);
  size_t count = vu.playback_channels;
  if (count == 0 || !vu.playback_peak) {
    dsp_engine_free_vu_levels(&vu);
    return 0.0;
  }
  double max_peak = -1000.0;
  for (size_t i = 0; i < count; i++) {
    double pk = vu.playback_peak[i];
    if (pk > max_peak) max_peak = pk;
  }
  dsp_engine_free_vu_levels(&vu);
  return 2.0 * pow(10.0, max_peak / 20.0);
}

bool cdsp_get_processing_status(const dsp_engine_t* engine,
                                double* out_rate_adjust,
                                double* out_buffer_level,
                                uint64_t* out_clipped_samples,
                                double* out_processing_load,
                                double* out_resampler_load) {
  if (!engine) return false;
  dsp_engine_interface_t* iface = dsp_engine_get_interface((dsp_engine_t*)engine);
  return iface && iface->get_processing_status &&
         iface->get_processing_status(iface->ctx, out_rate_adjust, out_buffer_level,
                                      out_clipped_samples, out_processing_load,
                                      out_resampler_load);
}

void cdsp_reset_clipped_samples(dsp_engine_t* engine) {
  if (!engine) return;
  dsp_engine_interface_t* iface = dsp_engine_get_interface(engine);
  if (iface && iface->reset_clipped_samples) {
    iface->reset_clipped_samples(iface->ctx);
  }
}

const char* cdsp_get_state_file_path(const dsp_engine_t* engine) {
  if (!engine) return NULL;
  dsp_engine_interface_t* iface = dsp_engine_get_interface((dsp_engine_t*)engine);
  return iface && iface->get_state_file ? iface->get_state_file(iface->ctx) : NULL;
}

bool cdsp_is_state_dirty(const dsp_engine_t* engine) {
  if (!engine) return false;
  dsp_engine_interface_t* iface = dsp_engine_get_interface((dsp_engine_t*)engine);
  return iface && iface->is_state_dirty ? iface->is_state_dirty(iface->ctx) : false;
}
