#include "Public/signal_levels.h"

#include <stdlib.h>
#include <string.h>

#include "Config/cJSON.h"
#include "Engine/dsp_engine.h"

bool cdsp_get_vu_levels(const dsp_engine_t* engine, cdsp_vu_levels_t* out_vu) {
  if (!engine || !out_vu || !engine->get_vu_levels) return false;

  vu_levels_t vu = {0};
  if (!engine->get_vu_levels(engine->ctx, &vu)) return false;

  out_vu->playback_channels = vu.playback_channels;
  out_vu->capture_channels = vu.capture_channels;

  if (vu.playback_channels > 0) {
    out_vu->playback_rms =
        (double*)malloc(vu.playback_channels * sizeof(double));
    out_vu->playback_peak =
        (double*)malloc(vu.playback_channels * sizeof(double));
    if (out_vu->playback_rms && vu.playback_rms) {
      memcpy(out_vu->playback_rms, vu.playback_rms,
             vu.playback_channels * sizeof(double));
    }
    if (out_vu->playback_peak && vu.playback_peak) {
      memcpy(out_vu->playback_peak, vu.playback_peak,
             vu.playback_channels * sizeof(double));
    }
  } else {
    out_vu->playback_rms = NULL;
    out_vu->playback_peak = NULL;
  }

  if (vu.capture_channels > 0) {
    out_vu->capture_rms = (double*)malloc(vu.capture_channels * sizeof(double));
    out_vu->capture_peak =
        (double*)malloc(vu.capture_channels * sizeof(double));
    if (out_vu->capture_rms && vu.capture_rms) {
      memcpy(out_vu->capture_rms, vu.capture_rms,
             vu.capture_channels * sizeof(double));
    }
    if (out_vu->capture_peak && vu.capture_peak) {
      memcpy(out_vu->capture_peak, vu.capture_peak,
             vu.capture_channels * sizeof(double));
    }
  } else {
    out_vu->capture_rms = NULL;
    out_vu->capture_peak = NULL;
  }

  if (vu.playback_rms) free(vu.playback_rms);
  if (vu.playback_peak) free(vu.playback_peak);
  if (vu.capture_rms) free(vu.capture_rms);
  if (vu.capture_peak) free(vu.capture_peak);
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

static void get_labels_from_json(cJSON* devices, const char* dir_key,
                                 char*** out_labels, size_t* out_count) {
  if (!out_labels || !out_count) return;
  *out_labels = NULL;
  *out_count = 0;
  if (!devices) return;
  cJSON* dir = cJSON_GetObjectItem(devices, dir_key);
  if (!dir) return;
  cJSON* labels = cJSON_GetObjectItem(dir, "channel_labels");
  if (labels && cJSON_IsArray(labels)) {
    size_t count = cJSON_GetArraySize(labels);
    if (count > 0) {
      char** arr = (char**)malloc(count * sizeof(char*));
      for (size_t i = 0; i < count; i++) {
        cJSON* item = cJSON_GetArrayItem(labels, i);
        arr[i] = (item && cJSON_IsString(item) && item->valuestring)
                     ? strdup(item->valuestring)
                     : NULL;
      }
      *out_labels = arr;
      *out_count = count;
    }
  }
}

bool cdsp_get_channel_labels(const dsp_engine_t* engine,
                             char*** out_playback_labels,
                             size_t* out_playback_count,
                             char*** out_capture_labels,
                             size_t* out_capture_count) {
  if (!engine || !engine->get_active_config_json) return false;

  char* active_json = NULL;
  if (!engine->get_active_config_json(engine->ctx, &active_json) ||
      !active_json)
    return false;

  cJSON* root = cJSON_Parse(active_json);
  free(active_json);
  if (!root) return false;

  cJSON* devices = cJSON_GetObjectItem(root, "devices");
  if (devices) {
    get_labels_from_json(devices, "playback", out_playback_labels,
                         out_playback_count);
    get_labels_from_json(devices, "capture", out_capture_labels,
                         out_capture_count);
  }

  cJSON_Delete(root);
  return true;
}

void cdsp_free_channel_labels(char** labels, size_t count) {
  if (!labels) return;
  for (size_t i = 0; i < count; i++) {
    if (labels[i]) free(labels[i]);
  }
  free(labels);
}
