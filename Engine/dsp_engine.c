#include "Engine/dsp_engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "Audio/audio_history_buffer.h"
#include "Backend/audio_backend_registry.h"
#include "Config/config_diff.h"
#include "Engine/dsp_session.h"
#include "Engine/engine_state_manager.h"
#include "Logging/app_logger.h"
#include "Pipeline/config_loader.h"
#include "Pipeline/state_file.h"

struct dsp_engine_impl {
  /** Active session and historical stop reason. */
  struct {
    dsp_session_t* active;
    processing_stop_reason_t last_stop_reason;
    bool has_last_stop_reason;
  } session;

  /** Audio history buffers and spectrum analyzer. */
  struct {
    audio_history_buffer_t* capture;
    audio_history_buffer_t* playback;
    spectrum_analyzer_t* spectrum;
  } buffers;

  /** Active and previous configuration JSON payloads. */
  struct {
    char* active_json;
    char* previous_json;
    _Atomic bool in_progress;
  } config;

  /** State manager for volume/mute and path persistence. */
  engine_state_manager_t* state_mgr;
  /** Mutex for protecting state variables. */
  pthread_mutex_t state_mutex;
  /** Self-contained interface function pointer table. */
  dsp_engine_t iface;
};

typedef struct dsp_engine_impl dsp_engine_impl_t;

static const logger_t g_logger = {"dsp.engine"};

/**
 * @brief Internal callback invoked when an audio chunk is captured from the
 * input device. Appends captured audio frames into the capture history buffer.
 * @param ctx Pointer to the capture audio_history_buffer_t instance.
 * @param chunk Pointer to the captured audio_chunk_t data structure.
 */
static void engine_on_chunk_captured_callback(void* ctx,
                                              const audio_chunk_t* chunk) {
  audio_history_buffer_t* buf = (audio_history_buffer_t*)ctx;
  if (buf && chunk) audio_history_buffer_append(buf, chunk);
}

/**
 * @brief Internal callback invoked when an audio chunk finishes DSP processing
 * before playback. Appends processed audio frames into the playback history
 * buffer.
 * @param ctx Pointer to the playback audio_history_buffer_t instance.
 * @param chunk Pointer to the processed audio_chunk_t data structure.
 */
static void engine_on_chunk_processed_callback(void* ctx,
                                               const audio_chunk_t* chunk) {
  audio_history_buffer_t* buf = (audio_history_buffer_t*)ctx;
  if (buf && chunk) audio_history_buffer_append(buf, chunk);
}

/**
 * @brief Internal helper to apply a parsed dsp_config_t structure while holding
 * state mutex lock.
 * @param impl Pointer to concrete engine implementation state.
 * @param config Parsed configuration object pointer.
 * @param err Output error details on configuration failure.
 * @return true on successful application, false otherwise.
 */
static bool dsp_engine_set_config_struct_locked(dsp_engine_impl_t* impl,
                                                dsp_config_t* config,
                                                audio_backend_error_t* err) {
  if (!impl || !config) return false;
  if (impl->session.active) {
    dsp_session_collect_garbage(impl->session.active);
  }

  if (impl->session.active && dsp_session_get_state(impl->session.active) !=
                                  PROCESSING_STATE_INACTIVE) {
    const dsp_config_t* cur_cfg = dsp_session_get_config(impl->session.active);
    if (cur_cfg && devices_config_equal(&cur_cfg->devices, &config->devices)) {
      audio_backend_error_t berr = {0};
      if (dsp_session_reload_config(impl->session.active, config, &berr)) {
        return true;
      } else {
        dsp_session_stop_and_free(
            impl->session.active,
            (processing_stop_reason_t){.type = STOP_REASON_NONE});
        impl->session.active = NULL;
        if (err) *err = berr;
        return false;
      }
    }
  }

  if (impl->session.active) {
    dsp_session_stop_and_free(
        impl->session.active,
        (processing_stop_reason_t){.type = STOP_REASON_NONE});
    impl->session.active = NULL;
  }

  audio_history_buffer_reset(
      impl->buffers.capture,
      capture_device_config_get_channels(&config->devices.capture));
  audio_history_buffer_reset(
      impl->buffers.playback,
      playback_device_config_get_channels(&config->devices.playback));

  dsp_session_t* session = dsp_session_create_and_start(
      config, engine_on_chunk_captured_callback, impl->buffers.capture,
      engine_on_chunk_processed_callback, impl->buffers.playback, err);
  if (!session) {
    return false;
  }

  processing_parameters_t* session_params =
      dsp_session_get_processing_params(session);
  engine_state_manager_sync_to_processing_parameters(impl->state_mgr,
                                                     session_params);

  impl->session.active = session;
  impl->session.has_last_stop_reason = false;
  return true;
}

/**
 * @brief Internal helper to parse and apply a JSON config string while holding
 * state mutex lock.
 * @param impl Pointer to concrete engine implementation state.
 * @param json Null-terminated JSON configuration payload.
 * @param err Output error details on parse or application failure.
 * @return true on success, false otherwise.
 */
static bool dsp_engine_set_config_locked(dsp_engine_impl_t* impl,
                                         const char* json,
                                         audio_backend_error_t* err) {
  if (!impl || !json) return false;
  logger_info_str(&g_logger, "Set config:", json);

  dsp_config_t* parsed = NULL;
  config_error_t cerr = {0};
  if (config_loader_parse(json, &parsed, &cerr) != 0 || !parsed) {
    if (err) {
      err->type = AUDIO_BACKEND_ERR_CONFIG_PARSE;
      strncpy(err->message, cerr.message, sizeof(err->message) - 1);
      err->message[sizeof(err->message) - 1] = '\0';
    }
    return false;
  }
  bool success = dsp_engine_set_config_struct_locked(impl, parsed, err);
  if (success) {
    if (impl->config.previous_json) {
      free(impl->config.previous_json);
    }
    impl->config.previous_json = impl->config.active_json;
    impl->config.active_json = strdup(json);
  } else {
    dsp_config_free(parsed);
  }
  return success;
}

static bool dsp_engine_set_config_json(void* ctx, const char* json_str,
                                       audio_backend_error_t* out_err) {
  if (!ctx) return false;
  dsp_engine_impl_t* impl = (dsp_engine_impl_t*)ctx;
  atomic_store(&impl->config.in_progress, true);
  pthread_mutex_lock(&impl->state_mutex);
  bool res = dsp_engine_set_config_locked(impl, json_str, out_err);
  pthread_mutex_unlock(&impl->state_mutex);
  atomic_store(&impl->config.in_progress, false);
  return res;
}

static void dsp_engine_stop(void* ctx) {
  if (!ctx) return;
  dsp_engine_impl_t* impl = (dsp_engine_impl_t*)ctx;
  pthread_mutex_lock(&impl->state_mutex);
  if (impl->session.active) {
    dsp_session_collect_garbage(impl->session.active);
    processing_stop_reason_t reason = {.type = STOP_REASON_NONE};
    dsp_session_is_stop_requested(impl->session.active, &reason);
    processing_stop_reason_t final_reason =
        dsp_session_stop_and_free(impl->session.active, reason);
    if (final_reason.type != STOP_REASON_NONE) {
      impl->session.last_stop_reason = final_reason;
      impl->session.has_last_stop_reason = true;
    }
    impl->session.active = NULL;
  }
  pthread_mutex_unlock(&impl->state_mutex);
}

static void dsp_engine_set_fader_volume(void* ctx, fader_t fader, float db,
                                        bool instant) {
  if (!ctx || fader < 0 || fader >= FADER_COUNT) return;
  dsp_engine_impl_t* impl = (dsp_engine_impl_t*)ctx;
  pthread_mutex_lock(&impl->state_mutex);
  engine_state_manager_set_fader_volume(impl->state_mgr, fader, db);

  processing_parameters_t* p =
      dsp_session_get_processing_params(impl->session.active);
  if (p) {
    processing_parameters_set_target_volume_for_fader(p, (double)db, fader);
    if (instant) {
      processing_parameters_set_current_volume_for_fader(p, (double)db, fader);
    }
  }
  pthread_mutex_unlock(&impl->state_mutex);
}

static void dsp_engine_set_fader_mute(void* ctx, fader_t fader, bool mute) {
  if (!ctx || fader < 0 || fader >= FADER_COUNT) return;
  dsp_engine_impl_t* impl = (dsp_engine_impl_t*)ctx;
  pthread_mutex_lock(&impl->state_mutex);
  engine_state_manager_set_fader_mute(impl->state_mgr, fader, mute);

  processing_parameters_t* p =
      dsp_session_get_processing_params(impl->session.active);
  if (p) {
    processing_parameters_set_muted_for_fader(p, mute, fader);
  }
  pthread_mutex_unlock(&impl->state_mutex);
}

static float dsp_engine_get_fader_volume(void* ctx, fader_t fader) {
  dsp_engine_impl_t* impl = (dsp_engine_impl_t*)ctx;
  return impl ? engine_state_manager_get_fader_volume(impl->state_mgr, fader)
              : 0.0f;
}

static bool dsp_engine_get_fader_mute(void* ctx, fader_t fader) {
  dsp_engine_impl_t* impl = (dsp_engine_impl_t*)ctx;
  return impl ? engine_state_manager_is_fader_muted(impl->state_mgr, fader)
              : false;
}

static state_update_t dsp_engine_get_status_locked(dsp_engine_impl_t* impl) {
  state_update_t res = {0};
  if (!impl) return res;
  if (impl->session.active) {
    dsp_session_collect_garbage(impl->session.active);
    res.state = dsp_session_get_state(impl->session.active);
    processing_stop_reason_t r =
        dsp_session_get_stop_reason(impl->session.active);
    if (r.type != STOP_REASON_NONE) {
      res.stop_reason = r;
    } else if (impl->session.has_last_stop_reason) {
      res.stop_reason = impl->session.last_stop_reason;
    } else {
      res.stop_reason.type = STOP_REASON_NONE;
    }
  } else {
    res.state = PROCESSING_STATE_INACTIVE;
    if (impl->session.has_last_stop_reason) {
      res.stop_reason = impl->session.last_stop_reason;
    } else {
      res.stop_reason.type = STOP_REASON_NONE;
    }
  }
  return res;
}

static bool dsp_engine_get_status(void* ctx, state_update_t* out_status) {
  if (!ctx || !out_status) return false;
  dsp_engine_impl_t* impl = (dsp_engine_impl_t*)ctx;
  if (atomic_load(&impl->config.in_progress)) {
    *out_status = (state_update_t){.state = PROCESSING_STATE_STARTING,
                                   .stop_reason = {.type = STOP_REASON_NONE}};
    return true;
  }
  pthread_mutex_lock(&impl->state_mutex);
  if (atomic_load(&impl->config.in_progress)) {
    pthread_mutex_unlock(&impl->state_mutex);
    *out_status = (state_update_t){.state = PROCESSING_STATE_STARTING,
                                   .stop_reason = {.type = STOP_REASON_NONE}};
    return true;
  }
  *out_status = dsp_engine_get_status_locked(impl);
  pthread_mutex_unlock(&impl->state_mutex);
  return true;
}

static processing_state_t dsp_engine_get_state(void* ctx) {
  state_update_t status = {0};
  if (dsp_engine_get_status(ctx, &status)) {
    return status.state;
  }
  return PROCESSING_STATE_INACTIVE;
}

static bool dsp_engine_get_stop_reason(void* ctx,
                                       processing_stop_reason_t* out_reason) {
  if (!out_reason) return false;
  state_update_t status = {0};
  if (dsp_engine_get_status(ctx, &status)) {
    *out_reason = status.stop_reason;
    return true;
  }
  *out_reason = (processing_stop_reason_t){.type = STOP_REASON_NONE};
  return false;
}

static int dsp_engine_get_capture_rate(void* ctx) {
  if (!ctx) return 0;
  dsp_engine_impl_t* impl = (dsp_engine_impl_t*)ctx;
  pthread_mutex_lock(&impl->state_mutex);
  const dsp_config_t* cfg = dsp_session_get_config(impl->session.active);
  int rate = cfg ? cfg->devices.samplerate : 0;
  pthread_mutex_unlock(&impl->state_mutex);
  return rate;
}

static bool dsp_engine_get_processing_status(void* ctx, double* out_rate_adjust,
                                             double* out_buffer_level,
                                             uint64_t* out_clipped_samples,
                                             double* out_processing_load,
                                             double* out_resampler_load) {
  if (!ctx) return false;
  dsp_engine_impl_t* impl = (dsp_engine_impl_t*)ctx;
  pthread_mutex_lock(&impl->state_mutex);
  processing_parameters_t* p =
      dsp_session_get_processing_params(impl->session.active);
  if (!p) {
    pthread_mutex_unlock(&impl->state_mutex);
    return false;
  }
  if (out_rate_adjust)
    *out_rate_adjust = processing_parameters_get_rate_adjust(p);
  if (out_buffer_level)
    *out_buffer_level = processing_parameters_get_buffer_level(p);
  if (out_clipped_samples)
    *out_clipped_samples = processing_parameters_get_clipped_samples(p);
  if (out_processing_load)
    *out_processing_load = processing_parameters_get_processing_load(p);
  if (out_resampler_load)
    *out_resampler_load = processing_parameters_get_resampler_load(p);
  pthread_mutex_unlock(&impl->state_mutex);
  return true;
}

static void dsp_engine_reset_clipped_samples(void* ctx) {
  if (!ctx) return;
  dsp_engine_impl_t* impl = (dsp_engine_impl_t*)ctx;
  pthread_mutex_lock(&impl->state_mutex);
  processing_parameters_t* p =
      dsp_session_get_processing_params(impl->session.active);
  if (p) {
    processing_parameters_reset_clipped_samples(p);
  }
  pthread_mutex_unlock(&impl->state_mutex);
}

static bool dsp_engine_get_active_config_json(void* ctx, char** out_json) {
  if (!ctx || !out_json) return false;
  dsp_engine_impl_t* impl = (dsp_engine_impl_t*)ctx;
  pthread_mutex_lock(&impl->state_mutex);
  if (impl->config.active_json) {
    *out_json = strdup(impl->config.active_json);
    pthread_mutex_unlock(&impl->state_mutex);
    return true;
  }
  pthread_mutex_unlock(&impl->state_mutex);
  *out_json = NULL;
  return false;
}

static bool dsp_engine_get_previous_config_json(void* ctx, char** out_json) {
  if (!ctx || !out_json) return false;
  dsp_engine_impl_t* impl = (dsp_engine_impl_t*)ctx;
  pthread_mutex_lock(&impl->state_mutex);
  if (impl->config.previous_json) {
    *out_json = strdup(impl->config.previous_json);
    pthread_mutex_unlock(&impl->state_mutex);
    return true;
  }
  pthread_mutex_unlock(&impl->state_mutex);
  *out_json = NULL;
  return false;
}

static void dsp_engine_free_vu_levels(vu_levels_t* levels) {
  if (!levels) return;
  if (levels->playback_rms) {
    free(levels->playback_rms);
    levels->playback_rms = NULL;
  }
  if (levels->playback_peak) {
    free(levels->playback_peak);
    levels->playback_peak = NULL;
  }
  if (levels->capture_rms) {
    free(levels->capture_rms);
    levels->capture_rms = NULL;
  }
  if (levels->capture_peak) {
    free(levels->capture_peak);
    levels->capture_peak = NULL;
  }
  levels->playback_channels = 0;
  levels->capture_channels = 0;
}

static vu_levels_t dsp_engine_get_vu_levels_locked(dsp_engine_impl_t* impl) {
  vu_levels_t res = {0};
  if (!impl) return res;
  processing_parameters_t* p =
      dsp_session_get_processing_params(impl->session.active);
  if (!p) return res;
  dsp_session_collect_garbage(impl->session.active);
  res.playback_channels = processing_parameters_get_playback_channels(p);
  res.capture_channels = processing_parameters_get_capture_channels(p);
  if (res.playback_channels > 0) {
    res.playback_rms = (double*)calloc(res.playback_channels, sizeof(double));
    res.playback_peak = (double*)calloc(res.playback_channels, sizeof(double));
    if (!res.playback_rms || !res.playback_peak) {
      dsp_engine_free_vu_levels(&res);
      return (vu_levels_t){0};
    }
    processing_parameters_get_playback_signal_rms(p, res.playback_rms,
                                                  res.playback_channels);
    processing_parameters_get_playback_signal_peak(p, res.playback_peak,
                                                   res.playback_channels);
  }
  if (res.capture_channels > 0) {
    res.capture_rms = (double*)calloc(res.capture_channels, sizeof(double));
    res.capture_peak = (double*)calloc(res.capture_channels, sizeof(double));
    if (!res.capture_rms || !res.capture_peak) {
      dsp_engine_free_vu_levels(&res);
      return (vu_levels_t){0};
    }
    processing_parameters_get_capture_signal_rms(p, res.capture_rms,
                                                 res.capture_channels);
    processing_parameters_get_capture_signal_peak(p, res.capture_peak,
                                                  res.capture_channels);
  }
  return res;
}

static bool dsp_engine_get_vu_levels(void* ctx, vu_levels_t* out_vu) {
  if (!ctx || !out_vu) return false;
  dsp_engine_impl_t* impl = (dsp_engine_impl_t*)ctx;
  pthread_mutex_lock(&impl->state_mutex);
  *out_vu = dsp_engine_get_vu_levels_locked(impl);
  pthread_mutex_unlock(&impl->state_mutex);
  return true;
}

static bool dsp_engine_get_spectrum(void* ctx, bool is_capture,
                                    uint32_t channel, double min_freq,
                                    double max_freq, uint32_t n_bins,
                                    spectrum_t* out_spec) {
  if (!ctx || !out_spec) return false;
  dsp_engine_impl_t* impl = (dsp_engine_impl_t*)ctx;
  pthread_mutex_lock(&impl->state_mutex);
  if (!impl->session.active || !impl->buffers.spectrum) {
    pthread_mutex_unlock(&impl->state_mutex);
    return false;
  }
  const dsp_config_t* core_cfg = dsp_session_get_config(impl->session.active);
  if (!core_cfg) {
    pthread_mutex_unlock(&impl->state_mutex);
    return false;
  }
  audio_history_buffer_t* buf =
      is_capture ? impl->buffers.capture : impl->buffers.playback;
  size_t samplerate = core_cfg->devices.samplerate;
  size_t buf_channels = audio_history_buffer_get_channels(buf);

  if (channel != (uint32_t)-1 && (size_t)channel >= buf_channels) {
    pthread_mutex_unlock(&impl->state_mutex);
    return false;
  }

  spectrum_result_t res;
  spectrum_status_t status = spectrum_analyzer_compute(
      impl->buffers.spectrum, buf, (int)channel, min_freq, max_freq,
      (size_t)n_bins, samplerate, &res);
  pthread_mutex_unlock(&impl->state_mutex);

  if (status != 0) return false;
  out_spec->count = res.count;
  if (res.count > 0) {
    out_spec->frequencies = (double*)calloc(res.count, sizeof(double));
    out_spec->magnitudes = (double*)calloc(res.count, sizeof(double));
    if (!out_spec->frequencies || !out_spec->magnitudes) {
      if (out_spec->frequencies) free(out_spec->frequencies);
      if (out_spec->magnitudes) free(out_spec->magnitudes);
      out_spec->frequencies = NULL;
      out_spec->magnitudes = NULL;
      out_spec->count = 0;
      return false;
    }
    for (size_t i = 0; i < res.count; i++) {
      out_spec->frequencies[i] = (double)res.frequencies[i];
      out_spec->magnitudes[i] = (double)res.magnitudes[i];
    }
  } else {
    out_spec->frequencies = NULL;
    out_spec->magnitudes = NULL;
  }
  return true;
}

static void dsp_engine_free_samples(audio_samples_t* samples) {
  if (!samples) return;
  if (samples->channels) {
    for (size_t ch = 0; ch < samples->channels_count; ch++) {
      free(samples->channels[ch]);
    }
    free(samples->channels);
  }
  free(samples);
}

static audio_samples_t* dsp_engine_get_samples(void* ctx, bool is_capture,
                                               size_t n_frames,
                                               audio_backend_error_t* err) {
  if (!ctx) return NULL;
  dsp_engine_impl_t* impl = (dsp_engine_impl_t*)ctx;
  pthread_mutex_lock(&impl->state_mutex);
  if (!impl->session.active) {
    pthread_mutex_unlock(&impl->state_mutex);
    if (err) {
      err->type = AUDIO_BACKEND_ERR_ENGINE_NOT_RUNNING;
      snprintf(err->message, sizeof(err->message), "Engine not running");
    }
    return NULL;
  }
  audio_history_buffer_t* buf =
      is_capture ? impl->buffers.capture : impl->buffers.playback;
  if (!audio_history_buffer_has_data(buf)) {
    pthread_mutex_unlock(&impl->state_mutex);
    if (err) {
      err->type = AUDIO_BACKEND_ERR_BUFFER_EMPTY;
      snprintf(err->message, sizeof(err->message), "Buffer empty");
    }
    return NULL;
  }

  size_t n = n_frames;
  if (n > AUDIO_HISTORY_BUFFER_CAPACITY) n = AUDIO_HISTORY_BUFFER_CAPACITY;
  size_t ch_count = audio_history_buffer_get_channels(buf);
  if (ch_count == 0) {
    pthread_mutex_unlock(&impl->state_mutex);
    if (err) {
      err->type = AUDIO_BACKEND_ERR_BUFFER_EMPTY;
      snprintf(err->message, sizeof(err->message), "No channels");
    }
    return NULL;
  }

  audio_samples_t* res = (audio_samples_t*)calloc(1, sizeof(audio_samples_t));
  if (!res) {
    pthread_mutex_unlock(&impl->state_mutex);
    return NULL;
  }
  res->channels_count = ch_count;
  res->frames = n;
  res->channels = (double**)calloc(ch_count, sizeof(double*));
  if (!res->channels) {
    pthread_mutex_unlock(&impl->state_mutex);
    free(res);
    return NULL;
  }

  float* tmp = (float*)calloc(n, sizeof(float));
  if (!tmp) {
    pthread_mutex_unlock(&impl->state_mutex);
    dsp_engine_free_samples(res);
    if (err) {
      err->type = AUDIO_BACKEND_ERR_COMMAND_SEND;
      snprintf(err->message, sizeof(err->message), "Out of memory");
    }
    return NULL;
  }

  for (size_t ch = 0; ch < ch_count; ch++) {
    res->channels[ch] = (double*)calloc(n, sizeof(double));
    if (!res->channels[ch]) {
      pthread_mutex_unlock(&impl->state_mutex);
      free(tmp);
      dsp_engine_free_samples(res);
      if (err) {
        err->type = AUDIO_BACKEND_ERR_COMMAND_SEND;
        snprintf(err->message, sizeof(err->message), "Out of memory");
      }
      return NULL;
    }
    bool enough = false;
    audio_history_buffer_status_t status =
        audio_history_buffer_read_latest(buf, tmp, n, (int)ch, &enough);
    if (status != AUDIO_HISTORY_BUFFER_OK) {
      pthread_mutex_unlock(&impl->state_mutex);
      free(tmp);
      dsp_engine_free_samples(res);
      if (err) {
        err->type = AUDIO_BACKEND_ERR_BUFFER_EMPTY;
        snprintf(err->message, sizeof(err->message), "Failed to read buffer");
      }
      return NULL;
    }
    for (size_t i = 0; i < n; i++) {
      res->channels[ch][i] = (double)tmp[i];
    }
  }
  pthread_mutex_unlock(&impl->state_mutex);
  free(tmp);
  return res;
}

static void dsp_engine_set_log_level(void* ctx, log_level_t level) {
  (void)ctx;
  app_logger_set_level(level);
}

static bool dsp_engine_get_available_devices(void* ctx, const char* backend,
                                             bool is_input,
                                             audio_device_t** out_devices,
                                             size_t* out_count) {
  (void)ctx;
  if (!out_devices || !out_count) return false;
  audio_device_t* devs = (audio_device_t*)calloc(32, sizeof(audio_device_t));
  if (!devs) return false;
  int n =
      audio_backend_registry_get_available_devices(backend, is_input, devs, 32);
  if (n < 0) {
    free(devs);
    *out_devices = NULL;
    *out_count = 0;
    return false;
  }
  *out_devices = devs;
  *out_count = (size_t)n;
  return true;
}

static bool dsp_engine_get_device_capabilities(
    void* ctx, const char* backend, const char* device, bool is_capture,
    audio_device_descriptor_t** out_desc, device_error_t* err) {
  (void)ctx;
  if (!out_desc) return false;
  *out_desc = audio_backend_registry_get_device_capabilities(backend, device,
                                                             is_capture, err);
  return *out_desc != NULL;
}

static bool dsp_engine_check_stop_requested(
    dsp_engine_impl_t* impl, processing_stop_reason_t* out_reason) {
  if (!impl || !impl->session.active) return false;
  return dsp_session_is_stop_requested(impl->session.active, out_reason);
}

static void dsp_engine_set_state_file_path(void* ctx, const char* path) {
  dsp_engine_impl_t* impl = (dsp_engine_impl_t*)ctx;
  if (impl) engine_state_manager_set_state_file(impl->state_mgr, path);
}

static const char* dsp_engine_get_state_file_path(void* ctx) {
  dsp_engine_impl_t* impl = (dsp_engine_impl_t*)ctx;
  return impl ? engine_state_manager_get_state_file(impl->state_mgr) : NULL;
}

static bool dsp_engine_get_state_file_updated(void* ctx) {
  dsp_engine_impl_t* impl = (dsp_engine_impl_t*)ctx;
  return impl ? !engine_state_manager_is_dirty(impl->state_mgr) : true;
}

static void dsp_engine_set_config_file_path(void* ctx, const char* path) {
  dsp_engine_impl_t* impl = (dsp_engine_impl_t*)ctx;
  if (impl) engine_state_manager_set_config_path(impl->state_mgr, path);
}

static char* dsp_engine_get_config_file_path(void* ctx) {
  dsp_engine_impl_t* impl = (dsp_engine_impl_t*)ctx;
  return impl ? engine_state_manager_get_config_path(impl->state_mgr) : NULL;
}

static void dsp_engine_free_impl(void* ctx) {
  if (!ctx) return;
  dsp_engine_impl_t* impl = (dsp_engine_impl_t*)ctx;
  dsp_engine_stop(impl);
  if (impl->buffers.spectrum) spectrum_analyzer_free(impl->buffers.spectrum);
  if (impl->buffers.capture) audio_history_buffer_free(impl->buffers.capture);
  if (impl->buffers.playback) audio_history_buffer_free(impl->buffers.playback);
  if (impl->state_mgr) engine_state_manager_free(impl->state_mgr);
  pthread_mutex_destroy(&impl->state_mutex);
  if (impl->config.active_json) free(impl->config.active_json);
  if (impl->config.previous_json) free(impl->config.previous_json);
  free(impl);
}

static void dsp_engine_poll_impl(void* ctx) {
  if (!ctx) return;
  dsp_engine_impl_t* impl = (dsp_engine_impl_t*)ctx;
  pthread_mutex_lock(&impl->state_mutex);
  if (impl->session.active) {
    dsp_session_collect_garbage(impl->session.active);
  }
  pthread_mutex_unlock(&impl->state_mutex);

  pthread_mutex_lock(&impl->state_mutex);
  processing_stop_reason_t stop_reason;
  bool stop_needed = dsp_engine_check_stop_requested(impl, &stop_reason);
  pthread_mutex_unlock(&impl->state_mutex);

  if (stop_needed) {
    dsp_engine_stop(impl);
  }

  engine_state_manager_save_if_needed(impl->state_mgr);
}

dsp_engine_t* dsp_engine_create(void) {
  dsp_engine_impl_t* impl =
      (dsp_engine_impl_t*)calloc(1, sizeof(dsp_engine_impl_t));
  if (!impl) return NULL;

  impl->buffers.spectrum = spectrum_analyzer_create();
  impl->buffers.capture = audio_history_buffer_create();
  impl->buffers.playback = audio_history_buffer_create();
  impl->state_mgr = engine_state_manager_create();

  if (!impl->buffers.spectrum || !impl->buffers.capture ||
      !impl->buffers.playback || !impl->state_mgr) {
    dsp_engine_free_impl(impl);
    return NULL;
  }

  impl->session.has_last_stop_reason = false;
  pthread_mutex_init(&impl->state_mutex, NULL);
  impl->config.active_json = NULL;
  impl->config.previous_json = NULL;
  atomic_init(&impl->config.in_progress, false);

  impl->iface.ctx = impl;
  impl->iface.free = dsp_engine_free_impl;
  impl->iface.poll = dsp_engine_poll_impl;
  impl->iface.get_status = dsp_engine_get_status;
  impl->iface.get_state = dsp_engine_get_state;
  impl->iface.get_stop_reason = dsp_engine_get_stop_reason;
  impl->iface.get_capture_rate = dsp_engine_get_capture_rate;
  impl->iface.get_processing_status = dsp_engine_get_processing_status;
  impl->iface.reset_clipped_samples = dsp_engine_reset_clipped_samples;
  impl->iface.get_active_config_json = dsp_engine_get_active_config_json;
  impl->iface.get_previous_config_json = dsp_engine_get_previous_config_json;
  impl->iface.get_vu_levels = dsp_engine_get_vu_levels;
  impl->iface.get_available_devices = dsp_engine_get_available_devices;
  impl->iface.get_device_capabilities = dsp_engine_get_device_capabilities;
  impl->iface.get_spectrum = dsp_engine_get_spectrum;
  impl->iface.set_config_json = dsp_engine_set_config_json;
  impl->iface.stop = dsp_engine_stop;
  impl->iface.get_fader_volume = dsp_engine_get_fader_volume;
  impl->iface.get_fader_mute = dsp_engine_get_fader_mute;
  impl->iface.set_fader_volume = dsp_engine_set_fader_volume;
  impl->iface.set_fader_mute = dsp_engine_set_fader_mute;
  impl->iface.get_samples = dsp_engine_get_samples;
  impl->iface.get_state_file_path = dsp_engine_get_state_file_path;
  impl->iface.set_state_file_path = dsp_engine_set_state_file_path;
  impl->iface.get_state_file_updated = dsp_engine_get_state_file_updated;
  impl->iface.get_config_file_path = dsp_engine_get_config_file_path;
  impl->iface.set_config_file_path = dsp_engine_set_config_file_path;
  impl->iface.set_log_level = dsp_engine_set_log_level;

  return &impl->iface;
}
