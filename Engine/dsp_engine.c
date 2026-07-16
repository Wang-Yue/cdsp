#include "Engine/dsp_engine.h"

#include "Audio/audio_history_buffer.h"
#include "Engine/dsp_session.h"
#include "Engine/engine_state_manager.h"

struct dsp_engine_impl {
  /** Pointer to the active DSP session. */
  dsp_session_t* session;
  /** Spectrum analyzer instance. */
  spectrum_analyzer_t* spectrum;
  /** History buffer for captured audio. */
  audio_history_buffer_t* capture_buffer;
  /** History buffer for playback audio. */
  audio_history_buffer_t* playback_buffer;
  /** State manager for volume/mute and path persistence. */
  engine_state_manager_t* state_mgr;
  /** Reason for the last processing stop. */
  processing_stop_reason_t last_stop_reason;
  /** True if last stop reason is valid. */
  bool has_last_stop_reason;
  /** Mutex for protecting state variables. */
  pthread_mutex_t state_mutex;
  /** JSON representation of the active configuration. */
  char* active_config_json;
  /** JSON representation of the previous configuration. */
  char* previous_config_json;
  /** Self-contained interface function pointer table. */
  dsp_engine_t iface;
  /** True if configuration or reload is in progress. */
  _Atomic bool config_in_progress;
};

typedef struct dsp_engine_impl dsp_engine_impl_t;

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "Backend/audio_backend_registry.h"
#include "Config/config_diff.h"
#include "Logging/app_logger.h"
#include "Pipeline/config_loader.h"
#include "Pipeline/state_file.h"

static const logger_t g_logger = {"dsp.engine"};

static void engine_on_chunk_captured_callback(void* ctx,
                                              const audio_chunk_t* chunk) {
  audio_history_buffer_t* buf = (audio_history_buffer_t*)ctx;
  if (buf && chunk) audio_history_buffer_append(buf, chunk);
}

static void engine_on_chunk_processed_callback(void* ctx,
                                               const audio_chunk_t* chunk) {
  audio_history_buffer_t* buf = (audio_history_buffer_t*)ctx;
  if (buf && chunk) audio_history_buffer_append(buf, chunk);
}

static bool dsp_engine_set_config_struct_locked(dsp_engine_impl_t* impl,
                                                dsp_config_t* config,
                                                audio_backend_error_t* err) {
  if (!impl || !config) return false;
  if (impl->session) {
    dsp_session_collect_garbage(impl->session);
  }

  if (impl->session &&
      dsp_session_get_state(impl->session) != PROCESSING_STATE_INACTIVE) {
    const dsp_config_t* cur_cfg = dsp_session_get_config(impl->session);
    if (cur_cfg && devices_config_equal(&cur_cfg->devices, &config->devices)) {
      audio_backend_error_t berr;
      if (dsp_session_reload_config(impl->session, config, &berr)) {
        return true;
      } else {
        dsp_session_stop_and_free(impl->session, (processing_stop_reason_t){
                                                     .type = STOP_REASON_NONE});
        impl->session = NULL;
        if (err) *err = berr;
        return false;
      }
    }
  }

  if (impl->session) {
    dsp_session_stop_and_free(
        impl->session, (processing_stop_reason_t){.type = STOP_REASON_NONE});
    impl->session = NULL;
  }

  audio_history_buffer_reset(
      impl->capture_buffer,
      capture_device_config_get_channels(&config->devices.capture));
  audio_history_buffer_reset(
      impl->playback_buffer,
      playback_device_config_get_channels(&config->devices.playback));

  dsp_session_t* session = dsp_session_create_and_start(
      config, engine_on_chunk_captured_callback, impl->capture_buffer,
      engine_on_chunk_processed_callback, impl->playback_buffer, err);
  if (!session) {
    return false;
  }

  processing_parameters_t* session_params =
      dsp_session_get_processing_params(session);
  engine_state_manager_sync_to_processing_parameters(impl->state_mgr,
                                                     session_params);

  impl->session = session;
  impl->has_last_stop_reason = false;
  return true;
}

#ifdef CDSP_TEST
bool dsp_engine_set_config_struct(dsp_engine_t* engine, dsp_config_t* config,
                                  audio_backend_error_t* err) {
  if (!engine || !engine->ctx) return false;
  dsp_engine_impl_t* impl = (dsp_engine_impl_t*)engine->ctx;
  atomic_store(&impl->config_in_progress, true);
  pthread_mutex_lock(&impl->state_mutex);
  bool res = dsp_engine_set_config_struct_locked(impl, config, err);
  pthread_mutex_unlock(&impl->state_mutex);
  atomic_store(&impl->config_in_progress, false);
  return res;
}
#endif

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
    if (impl->previous_config_json) {
      free(impl->previous_config_json);
    }
    impl->previous_config_json = impl->active_config_json;
    impl->active_config_json = strdup(json);
  }
  return success;
}

#ifdef CDSP_TEST
bool dsp_engine_set_config(dsp_engine_t* engine, const char* json,
                           audio_backend_error_t* err) {
  if (!engine || !engine->ctx) return false;
  dsp_engine_impl_t* impl = (dsp_engine_impl_t*)engine->ctx;
  atomic_store(&impl->config_in_progress, true);
  pthread_mutex_lock(&impl->state_mutex);
  bool res = dsp_engine_set_config_locked(impl, json, err);
  pthread_mutex_unlock(&impl->state_mutex);
  atomic_store(&impl->config_in_progress, false);
  return res;
}
#endif

static void dsp_engine_stop(dsp_engine_impl_t* impl) {
  if (!impl) return;
  pthread_mutex_lock(&impl->state_mutex);
  if (impl->session) {
    dsp_session_collect_garbage(impl->session);
    processing_stop_reason_t reason = {.type = STOP_REASON_NONE};
    dsp_session_is_stop_requested(impl->session, &reason);
    if (reason.type != STOP_REASON_NONE) {
      impl->last_stop_reason = reason;
      impl->has_last_stop_reason = true;
    }
    dsp_session_stop_and_free(impl->session, reason);
    impl->session = NULL;
  }
  pthread_mutex_unlock(&impl->state_mutex);
}

static void dsp_engine_set_fader_volume(dsp_engine_impl_t* impl, fader_t fader,
                                        float db, bool instant) {
  if (!impl || fader < 0 || fader >= FADER_COUNT) return;
  pthread_mutex_lock(&impl->state_mutex);
  engine_state_manager_set_fader_volume(impl->state_mgr, fader, db);

  processing_parameters_t* p = dsp_session_get_processing_params(impl->session);
  if (p) {
    processing_parameters_set_target_volume_for_fader(p, (double)db, fader);
    if (instant) {
      processing_parameters_set_current_volume_for_fader(p, (double)db, fader);
    }
  }
  pthread_mutex_unlock(&impl->state_mutex);
}

static void dsp_engine_set_fader_mute(dsp_engine_impl_t* impl, fader_t fader,
                                      bool mute) {
  if (!impl || fader < 0 || fader >= FADER_COUNT) return;
  pthread_mutex_lock(&impl->state_mutex);
  engine_state_manager_set_fader_mute(impl->state_mgr, fader, mute);

  processing_parameters_t* p = dsp_session_get_processing_params(impl->session);
  if (p) {
    processing_parameters_set_muted_for_fader(p, mute, fader);
  }
  pthread_mutex_unlock(&impl->state_mutex);
}

static float dsp_engine_get_fader_volume(const dsp_engine_impl_t* impl,
                                         fader_t fader) {
  return impl ? engine_state_manager_get_fader_volume(impl->state_mgr, fader)
              : 0.0f;
}

static bool dsp_engine_is_fader_muted(const dsp_engine_impl_t* impl,
                                      fader_t fader) {
  return impl ? engine_state_manager_is_fader_muted(impl->state_mgr, fader)
              : false;
}

static state_update_t dsp_engine_get_status_locked(dsp_engine_impl_t* impl) {
  state_update_t res = {0};
  if (!impl) return res;
  if (impl->session) {
    dsp_session_collect_garbage(impl->session);
    res.state = dsp_session_get_state(impl->session);
    processing_stop_reason_t r = dsp_session_get_stop_reason(impl->session);
    if (r.type != STOP_REASON_NONE) {
      res.stop_reason = r;
    } else if (impl->has_last_stop_reason) {
      res.stop_reason = impl->last_stop_reason;
    } else {
      res.stop_reason.type = STOP_REASON_NONE;
    }
  } else {
    res.state = PROCESSING_STATE_INACTIVE;
    if (impl->has_last_stop_reason) {
      res.stop_reason = impl->last_stop_reason;
    } else {
      res.stop_reason.type = STOP_REASON_NONE;
    }
  }
  return res;
}

static state_update_t dsp_engine_get_status(dsp_engine_impl_t* impl) {
  if (!impl) {
    state_update_t res = {.state = PROCESSING_STATE_INACTIVE,
                          .stop_reason = {.type = STOP_REASON_NONE}};
    return res;
  }
  if (atomic_load(&impl->config_in_progress)) {
    state_update_t res = {.state = PROCESSING_STATE_STARTING,
                          .stop_reason = {.type = STOP_REASON_NONE}};
    return res;
  }
  pthread_mutex_lock(&impl->state_mutex);
  if (atomic_load(&impl->config_in_progress)) {
    pthread_mutex_unlock(&impl->state_mutex);
    state_update_t res = {.state = PROCESSING_STATE_STARTING,
                          .stop_reason = {.type = STOP_REASON_NONE}};
    return res;
  }
  state_update_t res = dsp_engine_get_status_locked(impl);
  pthread_mutex_unlock(&impl->state_mutex);
  return res;
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
  processing_parameters_t* p = dsp_session_get_processing_params(impl->session);
  if (!p) return res;
  dsp_session_collect_garbage(impl->session);
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

static vu_levels_t dsp_engine_get_vu_levels(dsp_engine_impl_t* impl) {
  if (!impl) {
    vu_levels_t res = {0};
    return res;
  }
  pthread_mutex_lock(&impl->state_mutex);
  vu_levels_t res = dsp_engine_get_vu_levels_locked(impl);
  pthread_mutex_unlock(&impl->state_mutex);
  return res;
}

static spectrum_status_t dsp_engine_get_spectrum_locked(
    dsp_engine_impl_t* impl, bool is_capture, int channel, double min_freq,
    double max_freq, size_t n_bins, spectrum_result_t* out_result) {
  if (!impl || !impl->session || !impl->spectrum) return SPECTRUM_ERROR_EMPTY;
  const dsp_config_t* core_cfg = dsp_session_get_config(impl->session);
  if (!core_cfg) return SPECTRUM_ERROR_EMPTY;
  audio_history_buffer_t* buf =
      is_capture ? impl->capture_buffer : impl->playback_buffer;
  size_t samplerate = core_cfg->devices.samplerate;
  return spectrum_analyzer_compute(impl->spectrum, buf, channel, min_freq,
                                   max_freq, n_bins, samplerate, out_result);
}

static spectrum_status_t dsp_engine_get_spectrum(
    dsp_engine_impl_t* impl, bool is_capture, int channel, double min_freq,
    double max_freq, size_t n_bins, spectrum_result_t* out_result) {
  if (!impl) return SPECTRUM_ERROR_EMPTY;
  pthread_mutex_lock(&impl->state_mutex);
  spectrum_status_t res = dsp_engine_get_spectrum_locked(
      impl, is_capture, channel, min_freq, max_freq, n_bins, out_result);
  pthread_mutex_unlock(&impl->state_mutex);
  return res;
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

static audio_samples_t* dsp_engine_get_samples_locked(
    dsp_engine_impl_t* impl, bool is_capture, size_t n_frames,
    audio_backend_error_t* err) {
  if (!impl || !impl->session) {
    if (err) {
      err->type = AUDIO_BACKEND_ERR_ENGINE_NOT_RUNNING;
      snprintf(err->message, sizeof(err->message), "Engine not running");
    }
    return NULL;
  }
  audio_history_buffer_t* buf =
      is_capture ? impl->capture_buffer : impl->playback_buffer;
  if (!audio_history_buffer_has_data(buf)) {
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
    if (err) {
      err->type = AUDIO_BACKEND_ERR_BUFFER_EMPTY;
      snprintf(err->message, sizeof(err->message), "No channels");
    }
    return NULL;
  }

  audio_samples_t* res = (audio_samples_t*)calloc(1, sizeof(audio_samples_t));
  if (!res) return NULL;
  res->channels_count = ch_count;
  res->frames = n;
  res->channels = (double**)calloc(ch_count, sizeof(double*));
  if (!res->channels) {
    free(res);
    return NULL;
  }

  float* tmp = (float*)calloc(n, sizeof(float));
  for (size_t ch = 0; ch < ch_count; ch++) {
    res->channels[ch] = (double*)calloc(n, sizeof(double));
    bool enough = false;
    audio_history_buffer_status_t status =
        audio_history_buffer_read_latest(buf, tmp, n, (int)ch, &enough);
    if (status != AUDIO_HISTORY_BUFFER_OK) {
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
  free(tmp);
  return res;
}

static audio_samples_t* dsp_engine_get_samples(dsp_engine_impl_t* impl,
                                               bool is_capture, size_t n_frames,
                                               audio_backend_error_t* err) {
  if (!impl) return NULL;
  pthread_mutex_lock(&impl->state_mutex);
  audio_samples_t* res =
      dsp_engine_get_samples_locked(impl, is_capture, n_frames, err);
  pthread_mutex_unlock(&impl->state_mutex);
  return res;
}

static void dsp_engine_set_log_level(log_level_t level) {
  app_logger_set_level(level);
}

static int dsp_engine_get_available_devices(const char* backend, bool input,
                                            audio_device_t* out_devices,
                                            int max_devices) {
  return audio_backend_registry_get_available_devices(backend, input,
                                                      out_devices, max_devices);
}

static audio_device_descriptor_t* dsp_engine_get_device_capabilities(
    const char* backend, const char* device, bool is_capture,
    device_error_t* err) {
  return audio_backend_registry_get_device_capabilities(backend, device,
                                                        is_capture, err);
}

#ifdef CDSP_TEST
const dsp_config_t* dsp_engine_get_active_config(dsp_engine_t* engine) {
  if (!engine || !engine->ctx) return NULL;
  dsp_engine_impl_t* impl = (dsp_engine_impl_t*)engine->ctx;
  pthread_mutex_lock(&impl->state_mutex);
  const dsp_config_t* res = dsp_session_get_config(impl->session);
  pthread_mutex_unlock(&impl->state_mutex);
  return res;
}
#endif

static bool dsp_engine_check_stop_requested(
    dsp_engine_impl_t* impl, processing_stop_reason_t* out_reason) {
  if (!impl || !impl->session) return false;
  return dsp_session_is_stop_requested(impl->session, out_reason);
}

static void dsp_engine_set_state_file(dsp_engine_impl_t* impl,
                                      const char* path) {
  if (impl) engine_state_manager_set_state_file(impl->state_mgr, path);
}

static const char* dsp_engine_get_state_file(const dsp_engine_impl_t* impl) {
  return impl ? engine_state_manager_get_state_file(impl->state_mgr) : NULL;
}

static bool dsp_engine_is_state_dirty(const dsp_engine_impl_t* impl) {
  return impl ? engine_state_manager_is_dirty(impl->state_mgr) : false;
}

static void dsp_engine_set_config_path(dsp_engine_impl_t* impl,
                                       const char* path) {
  if (impl) engine_state_manager_set_config_path(impl->state_mgr, path);
}

static char* dsp_engine_get_config_path(const dsp_engine_impl_t* impl) {
  return impl ? engine_state_manager_get_config_path(impl->state_mgr) : NULL;
}

static bool iface_get_status(void* ctx, state_update_t* out_status) {
  if (!ctx || !out_status) return false;
  *out_status = dsp_engine_get_status((dsp_engine_impl_t*)ctx);
  return true;
}

static int iface_get_active_samplerate(void* ctx) {
  if (!ctx) return 0;
  dsp_engine_impl_t* engine = (dsp_engine_impl_t*)ctx;
  pthread_mutex_lock(&engine->state_mutex);
  const dsp_config_t* cfg = dsp_session_get_config(engine->session);
  int rate = cfg ? cfg->devices.samplerate : 0;
  pthread_mutex_unlock(&engine->state_mutex);
  return rate;
}

static bool iface_get_processing_status(void* ctx, double* out_rate_adjust,
                                        double* out_buffer_level,
                                        uint64_t* out_clipped_samples,
                                        double* out_processing_load,
                                        double* out_resampler_load) {
  if (!ctx) return false;
  dsp_engine_impl_t* engine = (dsp_engine_impl_t*)ctx;
  pthread_mutex_lock(&engine->state_mutex);
  processing_parameters_t* p =
      dsp_session_get_processing_params(engine->session);
  if (!p) {
    pthread_mutex_unlock(&engine->state_mutex);
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
  pthread_mutex_unlock(&engine->state_mutex);
  return true;
}

static void iface_reset_clipped_samples(void* ctx) {
  if (!ctx) return;
  dsp_engine_impl_t* engine = (dsp_engine_impl_t*)ctx;
  pthread_mutex_lock(&engine->state_mutex);
  processing_parameters_t* p =
      dsp_session_get_processing_params(engine->session);
  if (p) {
    processing_parameters_reset_clipped_samples(p);
  }
  pthread_mutex_unlock(&engine->state_mutex);
}

static bool iface_get_active_config_json(void* ctx, char** out_json) {
  if (!ctx || !out_json) return false;
  dsp_engine_impl_t* engine = (dsp_engine_impl_t*)ctx;
  pthread_mutex_lock(&engine->state_mutex);
  if (engine->active_config_json) {
    *out_json = strdup(engine->active_config_json);
    pthread_mutex_unlock(&engine->state_mutex);
    return true;
  }
  pthread_mutex_unlock(&engine->state_mutex);
  *out_json = NULL;
  return false;
}

static bool iface_get_previous_config_json(void* ctx, char** out_json) {
  if (!ctx || !out_json) return false;
  dsp_engine_impl_t* engine = (dsp_engine_impl_t*)ctx;
  pthread_mutex_lock(&engine->state_mutex);
  if (engine->previous_config_json) {
    *out_json = strdup(engine->previous_config_json);
    pthread_mutex_unlock(&engine->state_mutex);
    return true;
  }
  pthread_mutex_unlock(&engine->state_mutex);
  *out_json = NULL;
  return false;
}

static bool iface_get_vu_levels(void* ctx, vu_levels_t* out_vu) {
  if (!ctx || !out_vu) return false;
  *out_vu = dsp_engine_get_vu_levels((dsp_engine_impl_t*)ctx);
  return true;
}

static bool iface_get_available_devices(void* ctx, const char* backend,
                                        bool is_input,
                                        audio_device_t** out_devices,
                                        size_t* out_count) {
  if (!ctx || !out_devices || !out_count) return false;
  audio_device_t* devs = (audio_device_t*)calloc(32, sizeof(audio_device_t));
  if (!devs) return false;
  int n = dsp_engine_get_available_devices(backend, is_input, devs, 32);
  *out_devices = devs;
  *out_count = (size_t)n;
  return true;
}

static bool iface_get_device_capabilities(void* ctx, const char* backend,
                                          const char* device, bool is_capture,
                                          audio_device_descriptor_t** out_desc,
                                          device_error_t* out_err) {
  if (!ctx || !out_desc) return false;
  *out_desc =
      dsp_engine_get_device_capabilities(backend, device, is_capture, out_err);
  return *out_desc != NULL;
}

static bool iface_get_spectrum(void* ctx, bool is_capture, uint32_t channel,
                               double min_freq, double max_freq,
                               uint32_t n_bins, spectrum_t* out_spec) {
  if (!ctx || !out_spec) return false;
  spectrum_result_t res;
  if (dsp_engine_get_spectrum((dsp_engine_impl_t*)ctx, is_capture, (int)channel,
                              min_freq, max_freq, (size_t)n_bins, &res) != 0)
    return false;
  out_spec->count = res.count;
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
  return true;
}

static bool iface_set_config_json(void* ctx, const char* json_str,
                                  audio_backend_error_t* out_err) {
  if (!ctx) return false;
  dsp_engine_impl_t* impl = (dsp_engine_impl_t*)ctx;
  atomic_store(&impl->config_in_progress, true);
  pthread_mutex_lock(&impl->state_mutex);
  bool res = dsp_engine_set_config_locked(impl, json_str, out_err);
  pthread_mutex_unlock(&impl->state_mutex);
  atomic_store(&impl->config_in_progress, false);
  return res;
}

static void iface_stop(void* ctx) {
  if (ctx) dsp_engine_stop((dsp_engine_impl_t*)ctx);
}

static float iface_get_fader_volume(void* ctx, fader_t fader) {
  return ctx ? dsp_engine_get_fader_volume((dsp_engine_impl_t*)ctx, fader)
             : 0.0f;
}

static bool iface_is_fader_muted(void* ctx, fader_t fader) {
  return ctx ? dsp_engine_is_fader_muted((dsp_engine_impl_t*)ctx, fader)
             : false;
}

static void iface_set_fader_volume(void* ctx, fader_t fader, float db,
                                   bool instant) {
  if (ctx)
    dsp_engine_set_fader_volume((dsp_engine_impl_t*)ctx, fader, db, instant);
}

static void iface_set_fader_mute(void* ctx, fader_t fader, bool mute) {
  if (ctx) dsp_engine_set_fader_mute((dsp_engine_impl_t*)ctx, fader, mute);
}

static const char* iface_get_state_file(void* ctx) {
  return ctx ? dsp_engine_get_state_file((dsp_engine_impl_t*)ctx) : NULL;
}

static void iface_set_state_file(void* ctx, const char* path) {
  if (ctx) dsp_engine_set_state_file((dsp_engine_impl_t*)ctx, path);
}

static bool iface_is_state_dirty(void* ctx) {
  return ctx ? dsp_engine_is_state_dirty((dsp_engine_impl_t*)ctx) : false;
}

static char* iface_get_config_path(void* ctx) {
  return ctx ? dsp_engine_get_config_path((dsp_engine_impl_t*)ctx) : NULL;
}

static void iface_set_config_path(void* ctx, const char* path) {
  if (ctx) dsp_engine_set_config_path((dsp_engine_impl_t*)ctx, path);
}

static void iface_set_log_level(void* ctx, log_level_t level) {
  (void)ctx;
  dsp_engine_set_log_level(level);
}

static audio_samples_t* iface_get_samples(void* ctx, bool is_capture,
                                          size_t n_frames,
                                          audio_backend_error_t* err) {
  return ctx ? dsp_engine_get_samples((dsp_engine_impl_t*)ctx, is_capture,
                                      n_frames, err)
             : NULL;
}

static void iface_free(void* ctx) {
  if (!ctx) return;
  dsp_engine_impl_t* impl = (dsp_engine_impl_t*)ctx;
  dsp_engine_stop(impl);
  if (impl->spectrum) spectrum_analyzer_free(impl->spectrum);
  if (impl->capture_buffer) audio_history_buffer_free(impl->capture_buffer);
  if (impl->playback_buffer) audio_history_buffer_free(impl->playback_buffer);
  if (impl->state_mgr) engine_state_manager_free(impl->state_mgr);
  pthread_mutex_destroy(&impl->state_mutex);
  if (impl->active_config_json) free(impl->active_config_json);
  if (impl->previous_config_json) free(impl->previous_config_json);
  free(impl);
}

static void iface_poll(void* ctx) {
  if (!ctx) return;
  dsp_engine_impl_t* impl = (dsp_engine_impl_t*)ctx;
  pthread_mutex_lock(&impl->state_mutex);
  if (impl->session) {
    dsp_session_collect_garbage(impl->session);
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

  impl->spectrum = spectrum_analyzer_create();
  impl->capture_buffer = audio_history_buffer_create();
  impl->playback_buffer = audio_history_buffer_create();
  impl->state_mgr = engine_state_manager_create();

  if (!impl->spectrum || !impl->capture_buffer || !impl->playback_buffer ||
      !impl->state_mgr) {
    iface_free(impl);
    return NULL;
  }

  impl->has_last_stop_reason = false;
  pthread_mutex_init(&impl->state_mutex, NULL);
  impl->active_config_json = NULL;
  impl->previous_config_json = NULL;
  atomic_init(&impl->config_in_progress, false);

  // Initialize self-contained interface table
  impl->iface.ctx = impl;
  impl->iface.free = iface_free;
  impl->iface.poll = iface_poll;
  impl->iface.get_status = iface_get_status;
  impl->iface.get_active_samplerate = iface_get_active_samplerate;
  impl->iface.get_processing_status = iface_get_processing_status;
  impl->iface.reset_clipped_samples = iface_reset_clipped_samples;
  impl->iface.get_active_config_json = iface_get_active_config_json;
  impl->iface.get_previous_config_json = iface_get_previous_config_json;
  impl->iface.get_vu_levels = iface_get_vu_levels;
  impl->iface.get_available_devices = iface_get_available_devices;
  impl->iface.get_device_capabilities = iface_get_device_capabilities;
  impl->iface.get_spectrum = iface_get_spectrum;
  impl->iface.set_config_json = iface_set_config_json;
  impl->iface.stop = iface_stop;
  impl->iface.get_fader_volume = iface_get_fader_volume;
  impl->iface.is_fader_muted = iface_is_fader_muted;
  impl->iface.set_fader_volume = iface_set_fader_volume;
  impl->iface.set_fader_mute = iface_set_fader_mute;
  impl->iface.get_samples = iface_get_samples;
  impl->iface.get_state_file = iface_get_state_file;
  impl->iface.set_state_file = iface_set_state_file;
  impl->iface.is_state_dirty = iface_is_state_dirty;
  impl->iface.get_config_path = iface_get_config_path;
  impl->iface.set_config_path = iface_set_config_path;
  impl->iface.set_log_level = iface_set_log_level;

  return &impl->iface;
}
