#include "Engine/dsp_engine.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Audio/audio_chunk.h"
#include "Audio/audio_history_buffer.h"
#include "Audio/processing_parameters.h"
#include "Audio/spectrum_analyzer.h"
#include "Backend/audio_backend_registry.h"
#include "Config/config_diff.h"
#include "Config/config_error.h"
#include "Config/configuration.h"
#include "Config/engine_config_types.h"
#include "Config/log_level.h"
#include "Engine/dsp_session.h"
#include "Engine/engine_state_manager.h"
#include "Logging/app_logger.h"
#include "Pipeline/config_loader.h"

// Ref: engine_state_management.md - Section 1.3: Controller Level
// (dsp_engine_t) & Section 1.6: Mutex Isolation (state_mutex as Level 1
// Top-Level Controller Lock)
struct dsp_engine_impl {
  /** Active session and historical stop reason. */
  struct {
    dsp_session_t* active;
    processing_stop_reason_t last_stop_reason;
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

  // Ref: engine_state_management.md - Section 3.1: Startup & Initialization
  // Flow Step 1 Configuration Change Decision Tree: If devices match, perform
  // non-blocking pipeline hot-reload via dsp_session_reload_config. If devices
  // differ, fall back to full session teardown and rebuild.
  if (impl->session.active && dsp_session_get_state(impl->session.active) !=
                                  PROCESSING_STATE_INACTIVE) {
    const dsp_config_t* cur_cfg = dsp_session_get_config(impl->session.active);
    if (cur_cfg && devices_config_equal(&cur_cfg->devices, &config->devices)) {
      audio_backend_error_t berr = {0};
      if (dsp_session_reload_config(impl->session.active, config, &berr)) {
        return true;
      } else {
        impl->session.last_stop_reason = dsp_session_stop_and_free(
            impl->session.active,
            (processing_stop_reason_t){.type = STOP_REASON_NONE});
        impl->session.active = NULL;
        if (err) *err = berr;
        return false;
      }
    }
  }

  if (impl->session.active) {
    impl->session.last_stop_reason = dsp_session_stop_and_free(
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

  // Ref: engine_state_management.md - Section 1.7.1: Lifecycle & Ownership
  // Contract Matrix & Section 3.1: Startup & Initialization Flow (Step 3
  // Pre-seeded Fader Sync). Ownership of config transfers to dsp_session_t on
  // creation success.
  dsp_session_t* session = dsp_session_create_and_start(
      config, engine_on_chunk_captured_callback, impl->buffers.capture,
      engine_on_chunk_processed_callback, impl->buffers.playback,
      impl->state_mgr, err);
  if (!session) {
    return false;
  }

  impl->session.active = session;
  impl->session.last_stop_reason.type = STOP_REASON_NONE;
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
      if (cerr.type == CONFIG_ERR_PARSE) {
        err->type = AUDIO_BACKEND_ERR_CONFIG_READ;
      } else {
        err->type = AUDIO_BACKEND_ERR_CONFIG_PARSE;
      }
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
    // Ref: engine_state_management.md - Section 1.7.1: Cleanup on Builder
    // Failure If builder fails before core->current_config assignment, caller
    // frees parsed config.
    dsp_config_free(parsed);
  }
  return success;
}

static bool dsp_engine_set_config_json(void* ctx, const char* json_str,
                                       audio_backend_error_t* out_err) {
  if (!ctx) return false;
  dsp_engine_impl_t* impl = (dsp_engine_impl_t*)ctx;
  // Ref: engine_state_management.md - Section 3.1: Startup & Initialization
  // Flow Step 1: Set config_in_progress to true. Status queries check this
  // atomic flag to return STARTING immediately without blocking on the
  // state_mutex.
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
  // Ref: engine_state_management.md - Section 3.6: Immediate Abort Teardown
  // Step 3: Controller teardown stops backend devices, joins terminated
  // threads, and cleans up session resources under the controller lock.
  pthread_mutex_lock(&impl->state_mutex);
  if (impl->session.active) {
    dsp_session_collect_garbage(impl->session.active);
    processing_stop_reason_t reason = {.type = STOP_REASON_NONE};
    dsp_session_is_stop_requested(impl->session.active, &reason);
    processing_stop_reason_t final_reason =
        dsp_session_stop_and_free(impl->session.active, reason);
    impl->session.last_stop_reason = final_reason;
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
    } else {
      res.stop_reason = impl->session.last_stop_reason;
    }
  } else {
    res.state = PROCESSING_STATE_INACTIVE;
    res.stop_reason = impl->session.last_stop_reason;
  }
  return res;
}

static bool dsp_engine_get_status(void* ctx, state_update_t* out_status) {
  if (!ctx || !out_status) return false;
  dsp_engine_impl_t* impl = (dsp_engine_impl_t*)ctx;
  // Ref: engine_state_management.md - Section 1.5: Atomic Variables
  // (config.in_progress) & Section 3.1: Startup & Initialization Flow Step 1:
  // Lock-free status query optimization:
  // Since configuration reloads hold state_mutex for a relatively long duration
  // (while rebuilding the DSP pipeline), checking the atomic in_progress flag
  // first allows status queries (e.g., from HTTP/WebSocket servers) to return
  // STARTING instantly without blocking the caller on the mutex.
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
  processing_parameters_t* params =
      impl->session.active
          ? dsp_session_get_processing_params(impl->session.active)
          : NULL;
  double measured =
      params ? processing_parameters_get_measured_capture_rate(params) : 0.0;
  int rate = (measured > 0.0) ? (int)(measured + 0.5) : 0;
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

static bool dsp_engine_get_vu_levels(void* ctx, vu_levels_t* out_vu) {
  if (!ctx || !out_vu) return false;
  dsp_engine_impl_t* impl = (dsp_engine_impl_t*)ctx;
  pthread_mutex_lock(&impl->state_mutex);
  processing_parameters_t* p =
      dsp_session_get_processing_params(impl->session.active);
  if (!p) {
    pthread_mutex_unlock(&impl->state_mutex);
    out_vu->playback_channels = 0;
    out_vu->capture_channels = 0;
    return false;
  }
  dsp_session_collect_garbage(impl->session.active);

  size_t pb_ch = processing_parameters_get_playback_channels(p);
  size_t cap_ch = processing_parameters_get_capture_channels(p);
  out_vu->playback_channels = pb_ch;
  out_vu->capture_channels = cap_ch;

  if (out_vu->playback_rms && pb_ch > 0) {
    processing_parameters_get_playback_signal_rms(p, out_vu->playback_rms,
                                                  pb_ch);
  }
  if (out_vu->playback_peak && pb_ch > 0) {
    processing_parameters_get_playback_signal_peak(p, out_vu->playback_peak,
                                                   pb_ch);
  }
  if (out_vu->capture_rms && cap_ch > 0) {
    processing_parameters_get_capture_signal_rms(p, out_vu->capture_rms,
                                                 cap_ch);
  }
  if (out_vu->capture_peak && cap_ch > 0) {
    processing_parameters_get_capture_signal_peak(p, out_vu->capture_peak,
                                                  cap_ch);
  }

  pthread_mutex_unlock(&impl->state_mutex);
  return true;
}

static bool dsp_engine_get_signal_levels_since(void* ctx, bool is_capture,
                                               bool is_rms, uint64_t since_ms,
                                               float* out_levels,
                                               size_t* out_channels) {
  if (!ctx) return false;
  dsp_engine_impl_t* impl = (dsp_engine_impl_t*)ctx;
  pthread_mutex_lock(&impl->state_mutex);
  processing_parameters_t* p =
      dsp_session_get_processing_params(impl->session.active);
  if (!p) {
    pthread_mutex_unlock(&impl->state_mutex);
    if (out_channels) *out_channels = 0;
    return false;
  }
  size_t ch = is_capture ? processing_parameters_get_capture_channels(p)
                         : processing_parameters_get_playback_channels(p);
  if (out_channels) *out_channels = ch;
  if (out_levels && ch > 0) {
    if (is_capture) {
      if (is_rms) {
        processing_parameters_get_capture_signal_rms_since(p, since_ms,
                                                           out_levels, ch);
      } else {
        processing_parameters_get_capture_signal_peak_since(p, since_ms,
                                                            out_levels, ch);
      }
    } else {
      if (is_rms) {
        processing_parameters_get_playback_signal_rms_since(p, since_ms,
                                                            out_levels, ch);
      } else {
        processing_parameters_get_playback_signal_peak_since(p, since_ms,
                                                             out_levels, ch);
      }
    }
  }
  pthread_mutex_unlock(&impl->state_mutex);
  return true;
}

static bool dsp_engine_get_spectrum(void* ctx, bool is_capture,
                                    const size_t* channel, float min_freq,
                                    float max_freq, uint32_t n_bins,
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
  size_t samplerate = (is_capture && core_cfg->devices.has_capture_samplerate)
                          ? core_cfg->devices.capture_samplerate
                          : core_cfg->devices.samplerate;
  size_t buf_channels = audio_history_buffer_get_channels(buf);

  if (channel && *channel >= buf_channels) {
    pthread_mutex_unlock(&impl->state_mutex);
    return false;
  }

  spectrum_result_t res;
  spectrum_status_t status =
      spectrum_analyzer_compute(impl->buffers.spectrum, buf, channel, min_freq,
                                max_freq, (size_t)n_bins, samplerate, &res);
  pthread_mutex_unlock(&impl->state_mutex);

  if (status != 0) return false;
  out_spec->count = res.count;
  if (out_spec->frequencies && out_spec->magnitudes) {
    memcpy(out_spec->frequencies, res.frequencies, res.count * sizeof(float));
    memcpy(out_spec->magnitudes, res.magnitudes, res.count * sizeof(float));
  }
  return true;
}

static bool dsp_engine_get_samples(void* ctx, bool is_capture, size_t n_frames,
                                   audio_samples_t* out_samples,
                                   audio_backend_error_t* err) {
  if (!ctx || !out_samples) return false;
  dsp_engine_impl_t* impl = (dsp_engine_impl_t*)ctx;
  pthread_mutex_lock(&impl->state_mutex);
  if (!impl->session.active) {
    pthread_mutex_unlock(&impl->state_mutex);
    if (err) {
      err->type = AUDIO_BACKEND_ERR_ENGINE_NOT_RUNNING;
      snprintf(err->message, sizeof(err->message), "Engine not running");
    }
    return false;
  }
  audio_history_buffer_t* buf =
      is_capture ? impl->buffers.capture : impl->buffers.playback;
  if (!buf) {
    pthread_mutex_unlock(&impl->state_mutex);
    if (err) {
      err->type = AUDIO_BACKEND_ERR_BUFFER_EMPTY;
      snprintf(err->message, sizeof(err->message), "Buffer empty");
    }
    return false;
  }

  size_t ch_count = audio_history_buffer_get_channels(buf);
  if (ch_count == 0) {
    pthread_mutex_unlock(&impl->state_mutex);
    if (err) {
      err->type = AUDIO_BACKEND_ERR_BUFFER_EMPTY;
      snprintf(err->message, sizeof(err->message), "No channels");
    }
    return false;
  }

  size_t n = n_frames;
  if (n > AUDIO_HISTORY_BUFFER_CAPACITY) n = AUDIO_HISTORY_BUFFER_CAPACITY;

  if (out_samples->channels && n > 0) {
    for (size_t ch = 0; ch < ch_count; ch++) {
      if (out_samples->channels[ch]) {
        bool enough = false;
        audio_history_buffer_status_t status = audio_history_buffer_read_latest(
            buf, out_samples->channels[ch], n, &ch, &enough);
        if (status != AUDIO_HISTORY_BUFFER_OK || !enough) {
          pthread_mutex_unlock(&impl->state_mutex);
          if (err) {
            err->type = AUDIO_BACKEND_ERR_BUFFER_EMPTY;
            snprintf(err->message, sizeof(err->message), "Buffer empty");
          }
          return false;
        }
      }
    }
  }

  out_samples->channels_count = ch_count;
  out_samples->frames = n;

  pthread_mutex_unlock(&impl->state_mutex);
  return true;
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

// Ref: engine_state_management.md - Section 3.4: Watchdog Stall & Recovery Flow
// Step 1 (Unified Main-Thread Watchdog) & Section 1.7.1 (Garbage Collection via
// dsp_session_collect_garbage)
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
  bool stop_needed =
      impl->session.active
          ? dsp_session_is_stop_requested(impl->session.active, &stop_reason)
          : false;
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

  pthread_mutex_init(&impl->state_mutex, NULL);
  impl->session.last_stop_reason.type = STOP_REASON_NONE;
  impl->config.active_json = NULL;
  impl->config.previous_json = NULL;
  atomic_init(&impl->config.in_progress, false);

  impl->buffers.spectrum = spectrum_analyzer_create();
  impl->buffers.capture = audio_history_buffer_create();
  impl->buffers.playback = audio_history_buffer_create();
  impl->state_mgr = engine_state_manager_create();

  if (!impl->buffers.spectrum || !impl->buffers.capture ||
      !impl->buffers.playback || !impl->state_mgr) {
    dsp_engine_free_impl(impl);
    return NULL;
  }

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
  impl->iface.get_signal_levels_since = dsp_engine_get_signal_levels_since;
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
