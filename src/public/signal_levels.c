#include "cdsp/signal_levels.h"

#include <stdlib.h>
#include <string.h>

#include "cdsp/cdsp_pub_types.h"
#include "config/configuration.h"
#include "config/engine_config_types.h"
#include "engine/dsp_engine.h"

bool cdsp_get_vu_levels(const dsp_engine_t *engine, cdsp_vu_levels_t *out_vu) {
  if (!engine || !out_vu || !engine->get_vu_levels)
    return false;

  vu_levels_t vu = {
      .playback_rms = out_vu->playback_rms,
      .playback_peak = out_vu->playback_peak,
      .capture_rms = out_vu->capture_rms,
      .capture_peak = out_vu->capture_peak,
  };
  bool ok = engine->get_vu_levels(engine->ctx, &vu);
  if (ok) {
    out_vu->playback_channels = vu.playback_channels;
    out_vu->capture_channels = vu.capture_channels;
  }
  return ok;
}

uint64_t cdsp_get_chunk_generation(const dsp_engine_t *engine,
                                   bool is_capture) {
  if (!engine || !engine->get_chunk_generation)
    return 0;
  return engine->get_chunk_generation(engine->ctx, is_capture);
}

bool cdsp_get_signal_levels_since(const dsp_engine_t *engine, bool is_capture,
                                  bool is_rms, uint64_t since_ms,
                                  float *out_levels, size_t *out_channels) {
  if (!engine || !engine->get_signal_levels_since)
    return false;
  return engine->get_signal_levels_since(engine->ctx, is_capture, is_rms,
                                         since_ms, out_levels, out_channels);
}

static void copy_labels(char **src_labels, size_t count, bool has_labels,
                        char ***out_labels, size_t *out_count) {
  if (!out_labels || !out_count)
    return;
  *out_labels = NULL;
  *out_count = 0;
  if (!has_labels)
    return;
  if (count > 0 && src_labels) {
    char **arr = (char **)malloc(count * sizeof(char *));
    if (!arr)
      return;
    for (size_t i = 0; i < count; i++) {
      arr[i] = src_labels[i] ? strdup(src_labels[i]) : NULL;
    }
    *out_labels = arr;
    *out_count = count;
  } else {
    *out_labels = (char **)malloc(sizeof(char *));
    *out_count = 0;
  }
}

bool cdsp_get_channel_labels(const dsp_engine_t *engine,
                             char ***out_playback_labels,
                             size_t *out_playback_count,
                             char ***out_capture_labels,
                             size_t *out_capture_count) {
  if (!engine || !engine->get_active_config_json)
    return false;

  char *active_json = NULL;
  if (!engine->get_active_config_json(engine->ctx, &active_json) ||
      !active_json)
    return false;

  dsp_config_t *cfg = NULL;
  config_error_t cerr = {0};
  if (dsp_config_parse_json_no_validate(active_json, &cfg, &cerr) != 0 ||
      !cfg) {
    free(active_json);
    return false;
  }
  free(active_json);

  // Capture labels
  copy_labels(cfg->devices.capture.labels, cfg->devices.capture.labels_count,
              cfg->devices.capture.has_labels, out_capture_labels,
              out_capture_count);

  // Playback labels: search pipeline in reverse for the last mixer
  bool mixer_found = false;
  if (cfg->pipeline && cfg->pipeline_count > 0) {
    for (ssize_t i = (ssize_t)cfg->pipeline_count - 1; i >= 0; i--) {
      if (cfg->pipeline[i].type == PIPELINE_STEP_TYPE_MIXER &&
          cfg->pipeline[i].has_name) {
        mixer_config_t *mixer =
            dsp_config_get_mixer(cfg, cfg->pipeline[i].name);
        if (mixer) {
          copy_labels(mixer->labels, mixer->labels_count, mixer->has_labels,
                      out_playback_labels, out_playback_count);
          mixer_found = true;
          break;
        }
      }
    }
  }

  // If no mixer found in pipeline, fallback to capture labels
  if (!mixer_found) {
    copy_labels(cfg->devices.capture.labels, cfg->devices.capture.labels_count,
                cfg->devices.capture.has_labels, out_playback_labels,
                out_playback_count);
  }

  dsp_config_free(cfg);
  return true;
}

void cdsp_free_channel_labels(char **labels, size_t count) {
  if (!labels)
    return;
  for (size_t i = 0; i < count; i++) {
    if (labels[i])
      free(labels[i]);
  }
  free(labels);
}

bool cdsp_get_global_peaks(const dsp_engine_t *engine, bool is_capture,
                           float *out_peaks, size_t *out_channels) {
  if (!engine || !engine->get_global_peaks)
    return false;
  return engine->get_global_peaks(engine->ctx, is_capture, out_peaks,
                                  out_channels);
}

void cdsp_reset_global_peaks(dsp_engine_t *engine) {
  if (!engine || !engine->reset_global_peaks)
    return;
  engine->reset_global_peaks(engine->ctx);
}
