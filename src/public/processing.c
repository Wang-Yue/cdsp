#include "cdsp/processing.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cdsp/cdsp_pub_types.h"
#include "config/engine_config_types.h"
#include "engine/dsp_engine.h"
#include "utils/double_helpers.h"

cdsp_processing_state_t cdsp_get_state(const dsp_engine_t *engine) {
  if (engine && engine->get_status) {
    state_update_t status = {0};
    if (engine->get_status(engine->ctx, &status)) {
      return (cdsp_processing_state_t)status.state;
    }
  }
  return CDSP_PROCESSING_STATE_INACTIVE;
}

void cdsp_get_stop_reason(const dsp_engine_t *engine,
                          cdsp_stop_reason_t *out_reason) {
  if (!out_reason)
    return;
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

int cdsp_get_capture_rate(const dsp_engine_t *engine) {
  if (engine && engine->get_capture_rate) {
    return engine->get_capture_rate(engine->ctx);
  }
  return 0;
}

double cdsp_get_signal_range(const dsp_engine_t *engine) {
  if (engine && engine->get_signal_range) {
    return engine->get_signal_range(engine->ctx);
  }
  if (engine && engine->get_vu_levels) {
    vu_levels_t vu_query = {0};
    if (engine->get_vu_levels(engine->ctx, &vu_query)) {
      size_t cap_ch = vu_query.capture_channels;
      if (cap_ch == 0)
        return 0.0;

      // Preallocate generous headroom (at least 4096 channels) to prevent
      // TOCTOU heap overflow if a concurrent reload increases channels between
      // calls.
      size_t alloc_count = cap_ch < 4096 ? 4096 : (cap_ch * 2);
      float *pk_buf = (float *)malloc(alloc_count * sizeof(float));
      if (!pk_buf)
        return 0.0;

      vu_levels_t vu = {0};
      vu.capture_peak = pk_buf;

      if (engine->get_vu_levels(engine->ctx, &vu)) {
        size_t actual_ch = vu.capture_channels;
        size_t scan_count = actual_ch < alloc_count ? actual_ch : alloc_count;
        double max_peak = -INFINITY;
        for (size_t i = 0; i < scan_count; i++) {
          double pk = (double)pk_buf[i];
          if (pk > max_peak)
            max_peak = pk;
        }
        free(pk_buf);
        if (!isfinite(max_peak))
          return 0.0;
        return 2.0 * double_from_db(max_peak);
      }
      free(pk_buf);
    }
  }
  return 0.0;
}

bool cdsp_get_processing_status(const dsp_engine_t *engine,
                                double *out_rate_adjust,
                                double *out_buffer_level,
                                uint64_t *out_clipped_samples,
                                double *out_processing_load,
                                double *out_resampler_load) {
  return engine && engine->get_processing_status &&
         engine->get_processing_status(engine->ctx, out_rate_adjust,
                                       out_buffer_level, out_clipped_samples,
                                       out_processing_load, out_resampler_load);
}

void cdsp_reset_clipped_samples(dsp_engine_t *engine) {
  if (engine && engine->reset_clipped_samples) {
    engine->reset_clipped_samples(engine->ctx);
  }
}

const char *cdsp_get_state_file_path(const dsp_engine_t *engine) {
  return engine && engine->get_state_file_path
             ? engine->get_state_file_path(engine->ctx)
             : NULL;
}

void cdsp_set_state_file_path(dsp_engine_t *engine, const char *path) {
  if (engine && engine->set_state_file_path) {
    engine->set_state_file_path(engine->ctx, path);
  }
}

bool cdsp_get_state_file_updated(const dsp_engine_t *engine) {
  return engine && engine->get_state_file_updated
             ? engine->get_state_file_updated(engine->ctx)
             : true;
}

bool cdsp_get_samples(dsp_engine_t *engine, bool is_capture, size_t n_frames,
                      cdsp_audio_samples_t *out_samples,
                      cdsp_backend_error_t *out_err) {
  if (!engine || !engine->get_samples || !out_samples)
    return false;

  audio_backend_error_t raw_err = {0};
  audio_samples_t raw_samples = {
      .channels = out_samples->channels,
      .channels_count = out_samples->channels_count,
  };
  bool ok = engine->get_samples(engine->ctx, is_capture, n_frames, &raw_samples,
                                &raw_err);
  if (!ok) {
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
    return false;
  }
  out_samples->channels_count = raw_samples.channels_count;
  out_samples->frames = raw_samples.frames;
  return true;
}
