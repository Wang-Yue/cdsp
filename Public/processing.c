#include "Public/processing.h"

#include <math.h>
#include <stdlib.h>

#include "Engine/dsp_engine.h"

cdsp_processing_state_t cdsp_get_state(const dsp_engine_t* engine) {
  if (engine && engine->get_status) {
    state_update_t status = {0};
    if (engine->get_status(engine->ctx, &status)) {
      return (cdsp_processing_state_t)status.state;
    }
  }
  return CDSP_PROCESSING_STATE_INACTIVE;
}

void cdsp_get_stop_reason(const dsp_engine_t* engine,
                          cdsp_stop_reason_t* out_reason) {
  if (!out_reason) return;
  out_reason->type = CDSP_STOP_REASON_NONE;
  out_reason->message[0] = '\0';
  out_reason->format_change_rate = 0;

  if (engine && engine->get_status) {
    state_update_t status = {0};
    if (engine->get_status(engine->ctx, &status)) {
      out_reason->type = (cdsp_stop_reason_type_t)status.stop_reason.type;
      strncpy(out_reason->message, status.stop_reason.message,
              sizeof(out_reason->message) - 1);
      out_reason->message[sizeof(out_reason->message) - 1] = '\0';
      out_reason->format_change_rate = status.stop_reason.format_change_rate;
    }
  }
}

int cdsp_get_capture_rate(const dsp_engine_t* engine) {
  if (engine && engine->get_status && engine->get_capture_rate) {
    state_update_t status = {0};
    if (engine->get_status(engine->ctx, &status) &&
        status.state == PROCESSING_STATE_RUNNING) {
      return engine->get_capture_rate(engine->ctx);
    }
  }
  return 0;
}

double cdsp_get_signal_range(const dsp_engine_t* engine) {
  if (engine && engine->get_vu_levels) {
    vu_levels_t vu = {0};
    if (engine->get_vu_levels(engine->ctx, &vu)) {
      size_t count = vu.playback_channels;
      if (count == 0 || !vu.playback_peak) {
        if (vu.playback_peak) free(vu.playback_peak);
        if (vu.playback_rms) free(vu.playback_rms);
        if (vu.capture_peak) free(vu.capture_peak);
        if (vu.capture_rms) free(vu.capture_rms);
        return 0.0;
      }
      double max_peak = -1000.0;
      for (size_t i = 0; i < count; i++) {
        double pk = vu.playback_peak[i];
        if (pk > max_peak) max_peak = pk;
      }
      if (vu.playback_peak) free(vu.playback_peak);
      if (vu.playback_rms) free(vu.playback_rms);
      if (vu.capture_peak) free(vu.capture_peak);
      if (vu.capture_rms) free(vu.capture_rms);
      return 2.0 * pow(10.0, max_peak / 20.0);
    }
  }
  return 0.0;
}

bool cdsp_get_processing_status(const dsp_engine_t* engine,
                                double* out_rate_adjust,
                                double* out_buffer_level,
                                uint64_t* out_clipped_samples,
                                double* out_processing_load,
                                double* out_resampler_load) {
  return engine && engine->get_processing_status &&
         engine->get_processing_status(engine->ctx, out_rate_adjust,
                                       out_buffer_level, out_clipped_samples,
                                       out_processing_load, out_resampler_load);
}

void cdsp_reset_clipped_samples(dsp_engine_t* engine) {
  if (engine && engine->reset_clipped_samples) {
    engine->reset_clipped_samples(engine->ctx);
  }
}

const char* cdsp_get_state_file_path(const dsp_engine_t* engine) {
  return engine && engine->get_state_file_path
             ? engine->get_state_file_path(engine->ctx)
             : NULL;
}

bool cdsp_get_state_file_updated(const dsp_engine_t* engine) {
  return engine && engine->get_state_file_updated
             ? engine->get_state_file_updated(engine->ctx)
             : true;
}

cdsp_audio_samples_t* cdsp_get_samples(dsp_engine_t* engine, bool is_capture,
                                       size_t n_frames,
                                       cdsp_backend_error_t* out_err) {
  if (!engine || !engine->get_samples) return NULL;

  audio_backend_error_t raw_err = {0};
  audio_samples_t* raw_samples =
      engine->get_samples(engine->ctx, is_capture, n_frames, &raw_err);
  if (!raw_samples) {
    if (out_err) {
      switch (raw_err.type) {
        case AUDIO_BACKEND_ERR_CONFIG_PARSE:
          out_err->type = CDSP_BACKEND_ERR_CONFIG_PARSE;
          break;
        case AUDIO_BACKEND_ERR_DEVICE_NOT_FOUND:
          out_err->type = CDSP_BACKEND_ERR_DEVICE_NOT_FOUND;
          break;
        case AUDIO_BACKEND_ERR_DEVICE_BUSY:
          out_err->type = CDSP_BACKEND_ERR_DEVICE_BUSY;
          break;
        default:
          out_err->type = CDSP_BACKEND_ERR_UNKNOWN;
          break;
      }
      strncpy(out_err->message, raw_err.message, sizeof(out_err->message) - 1);
      out_err->message[sizeof(out_err->message) - 1] = '\0';
    }
    return NULL;
  }
  return (cdsp_audio_samples_t*)raw_samples;
}

void cdsp_free_samples(cdsp_audio_samples_t* samples) {
  if (!samples) return;
  if (samples->channels) {
    for (size_t ch = 0; ch < samples->channels_count; ch++) {
      free(samples->channels[ch]);
    }
    free(samples->channels);
  }
  free(samples);
}
