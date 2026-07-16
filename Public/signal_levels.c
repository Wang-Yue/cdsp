#include "Public/signal_levels.h"
#include <stdlib.h>
#include <string.h>
#include "Engine/dsp_engine.h"

bool cdsp_get_vu_levels(const dsp_engine_t* engine, cdsp_vu_levels_t* out_vu) {
  if (!engine || !out_vu) return false;
  vu_levels_t vu = dsp_engine_get_vu_levels(engine);
  
  out_vu->playback_channels = vu.playback_channels;
  out_vu->capture_channels = vu.capture_channels;

  if (vu.playback_channels > 0) {
    out_vu->playback_rms = (double*)malloc(vu.playback_channels * sizeof(double));
    out_vu->playback_peak = (double*)malloc(vu.playback_channels * sizeof(double));
    if (out_vu->playback_rms && vu.playback_rms) {
      memcpy(out_vu->playback_rms, vu.playback_rms, vu.playback_channels * sizeof(double));
    }
    if (out_vu->playback_peak && vu.playback_peak) {
      memcpy(out_vu->playback_peak, vu.playback_peak, vu.playback_channels * sizeof(double));
    }
  } else {
    out_vu->playback_rms = NULL;
    out_vu->playback_peak = NULL;
  }

  if (vu.capture_channels > 0) {
    out_vu->capture_rms = (double*)malloc(vu.capture_channels * sizeof(double));
    out_vu->capture_peak = (double*)malloc(vu.capture_channels * sizeof(double));
    if (out_vu->capture_rms && vu.capture_rms) {
      memcpy(out_vu->capture_rms, vu.capture_rms, vu.capture_channels * sizeof(double));
    }
    if (out_vu->capture_peak && vu.capture_peak) {
      memcpy(out_vu->capture_peak, vu.capture_peak, vu.capture_channels * sizeof(double));
    }
  } else {
    out_vu->capture_rms = NULL;
    out_vu->capture_peak = NULL;
  }

  dsp_engine_free_vu_levels(&vu);
  return true;
}

void cdsp_free_vu_levels(cdsp_vu_levels_t* vu) {
  if (!vu) return;
  if (vu->playback_rms) free(vu->playback_rms);
  if (vu->playback_peak) free(vu->playback_peak);
  if (vu->capture_rms) free(vu->capture_rms);
  if (vu->capture_peak) free(vu->capture_peak);
  vu->playback_rms = NULL;
  vu->playback_peak = NULL;
  vu->capture_rms = NULL;
  vu->capture_peak = NULL;
}

bool cdsp_get_channel_labels(const dsp_engine_t* engine,
                             char*** out_playback_labels, size_t* out_playback_count,
                             char*** out_capture_labels, size_t* out_capture_count) {
  if (!engine) return false;
  const dsp_config_t* cfg = dsp_engine_get_active_config(engine);
  if (!cfg) return false;

  size_t pb_count = cfg->devices.playback.labels_count;
  size_t cap_count = cfg->devices.capture.labels_count;

  if (out_playback_labels && out_playback_count) {
    *out_playback_count = pb_count;
    if (pb_count > 0 && cfg->devices.playback.labels) {
      char** pb_arr = (char**)malloc(pb_count * sizeof(char*));
      for (size_t i = 0; i < pb_count; i++) {
        pb_arr[i] = cfg->devices.playback.labels[i] ? strdup(cfg->devices.playback.labels[i]) : NULL;
      }
      *out_playback_labels = pb_arr;
    } else {
      *out_playback_labels = NULL;
    }
  }

  if (out_capture_labels && out_capture_count) {
    *out_capture_count = cap_count;
    if (cap_count > 0 && cfg->devices.capture.labels) {
      char** cap_arr = (char**)malloc(cap_count * sizeof(char*));
      for (size_t i = 0; i < cap_count; i++) {
        cap_arr[i] = cfg->devices.capture.labels[i] ? strdup(cfg->devices.capture.labels[i]) : NULL;
      }
      *out_capture_labels = cap_arr;
    } else {
      *out_capture_labels = NULL;
    }
  }

  return true;
}

void cdsp_free_channel_labels(char** labels, size_t count) {
  if (!labels) return;
  for (size_t i = 0; i < count; i++) {
    if (labels[i]) free(labels[i]);
  }
  free(labels);
}
