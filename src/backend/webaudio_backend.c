/**
 * @file webaudio_backend.c
 * @brief WebAudio backend implementation.
 */

#if defined(ENABLE_WEBAUDIO)

#include "backend/webaudio_backend.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "audio/audio_chunk.h"
#include "audio/sample_format.h"
#include "backend/backend_buffer.h"
#include "config/config_gen.h"
#include "logging/app_logger.h"

static const logger_t g_logger = {"dsp.backend.webaudio"};

typedef struct webaudio_capture {
  int sample_rate;
  size_t channels;
  int chunk_size;
  backend_buffer_t *buffer;
  processing_parameters_t *params;
} webaudio_capture_t;

typedef struct webaudio_playback {
  int sample_rate;
  size_t channels;
  int chunk_size;
  size_t target_level;
  backend_buffer_t *buffer;
  processing_parameters_t *params;
} webaudio_playback_t;

// MARK: - Capture Backend Implementation

static bool webaudio_capture_open(void *ctx, backend_error_t *err) {
  webaudio_capture_t *capture = (webaudio_capture_t *)ctx;
  if (!capture)
    return false;

  size_t cap_min_frames = (size_t)ceil((double)capture->sample_rate * 0.025);
  size_t cap_frames_needed = (size_t)(4 * capture->chunk_size);
  if (cap_frames_needed < cap_min_frames)
    cap_frames_needed = cap_min_frames;

  capture->buffer = backend_buffer_create(
      cap_frames_needed, BINARY_SAMPLE_FORMAT_F32_LE, capture->channels,
      capture->sample_rate, true /* is_planar */, capture->params);

  if (!capture->buffer) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to allocate WebAudio capture buffer");
    return false;
  }

  backend_buffer_set_state(capture->buffer, BACKEND_STREAM_RUNNING);
  logger_info(&g_logger, "Opened WebAudio capture: rate=%d, channels=%zu",
              capture->sample_rate, capture->channels);
  return true;
}

static bool webaudio_capture_read(void *ctx, size_t frames,
                                  audio_chunk_t *chunk, backend_error_t *err) {
  webaudio_capture_t *capture = (webaudio_capture_t *)ctx;
  if (!capture || !capture->buffer)
    return false;
  return backend_buffer_read_chunk(capture->buffer, frames, chunk, err);
}

static void webaudio_capture_close(void *ctx) {
  webaudio_capture_t *capture = (webaudio_capture_t *)ctx;
  if (!capture)
    return;
  if (capture->buffer) {
    backend_buffer_set_state(capture->buffer, BACKEND_STREAM_STOPPED);
    backend_buffer_free(capture->buffer);
    capture->buffer = NULL;
  }
}

static bool webaudio_capture_get_pending_rate_change(void *ctx,
                                                     double *out_rate) {
  webaudio_capture_t *capture = (webaudio_capture_t *)ctx;
  if (!capture || !capture->buffer)
    return false;
  if (backend_buffer_has_pending_rate_change(capture->buffer)) {
    backend_buffer_set_pending_rate_change(capture->buffer, false);
    if (out_rate)
      *out_rate = (double)capture->sample_rate;
    return true;
  }
  return false;
}

static bool webaudio_capture_pitch_control_supported(void *ctx) {
  (void)ctx;
  return false;
}

static void webaudio_capture_set_pitch(void *ctx, double multiplier) {
  (void)ctx;
  (void)multiplier;
}

static bool webaudio_capture_wait_for_data(void *ctx, uint32_t timeout_ms) {
  webaudio_capture_t *capture = (webaudio_capture_t *)ctx;
  if (!capture || !capture->buffer)
    return false;
  return backend_buffer_wait(capture->buffer, timeout_ms);
}

static void webaudio_capture_stop(void *ctx) {
  webaudio_capture_t *capture = (webaudio_capture_t *)ctx;
  if (!capture || !capture->buffer)
    return;
  backend_buffer_set_state(capture->buffer, BACKEND_STREAM_STOPPED);
  backend_buffer_signal(capture->buffer);
}

static void webaudio_capture_destroy(void *ctx) {
  webaudio_capture_t *capture = (webaudio_capture_t *)ctx;
  if (capture) {
    webaudio_capture_close(capture);
    free(capture);
  }
}

static capture_backend_t *
webaudio_capture_create(const capture_device_config_t *config, int sample_rate,
                        int chunk_size, bool full_duplex,
                        processing_parameters_t *params, backend_error_t *err) {
  (void)full_duplex;
  (void)err;
  webaudio_capture_t *capture =
      (webaudio_capture_t *)calloc(1, sizeof(webaudio_capture_t));
  if (!capture)
    return NULL;

  capture->sample_rate = sample_rate;
  capture->channels = config->cfg.webaudio.channels;
  capture->chunk_size = chunk_size;
  capture->params = params;

  capture_backend_t *backend =
      (capture_backend_t *)calloc(1, sizeof(capture_backend_t));
  if (!backend) {
    free(capture);
    return NULL;
  }
  backend->ctx = capture;
  backend->vtable = &g_webaudio_capture_vtable;
  backend->is_realtime = true;
  return backend;
}

const capture_backend_vtable_t g_webaudio_capture_vtable = {
    .create = webaudio_capture_create,
    .open = webaudio_capture_open,
    .read = webaudio_capture_read,
    .close = webaudio_capture_close,
    .get_pending_rate_change = webaudio_capture_get_pending_rate_change,
    .is_pitch_control_supported = webaudio_capture_pitch_control_supported,
    .set_pitch = webaudio_capture_set_pitch,
    .wait_for_data = webaudio_capture_wait_for_data,
    .stop = webaudio_capture_stop,
    .destroy = webaudio_capture_destroy};

size_t webaudio_capture_push_samples(capture_backend_t *backend,
                                     const float *const *channels,
                                     size_t frames) {
  if (!backend || !backend->ctx || !channels || frames == 0)
    return 0;
  webaudio_capture_t *capture = (webaudio_capture_t *)backend->ctx;
  if (!capture->buffer)
    return 0;
  return backend_buffer_push(capture->buffer, channels, frames);
}

// MARK: - Playback Backend Implementation

static bool webaudio_playback_open(void *ctx, backend_error_t *err) {
  webaudio_playback_t *playback = (webaudio_playback_t *)ctx;
  if (!playback)
    return false;

  size_t pb_min_frames = (size_t)ceil((double)playback->sample_rate * 0.025);
  size_t target_level = playback->target_level > 0
                            ? playback->target_level
                            : (size_t)playback->chunk_size;
  size_t pb_prefill_frames = target_level > (size_t)(3 * playback->chunk_size)
                                 ? target_level
                                 : (size_t)(3 * playback->chunk_size);
  size_t pb_frames_needed =
      pb_prefill_frames + (size_t)(4 * playback->chunk_size);
  if (pb_frames_needed < pb_min_frames)
    pb_frames_needed = pb_min_frames;

  playback->buffer = backend_buffer_create(
      pb_frames_needed, BINARY_SAMPLE_FORMAT_F32_LE, playback->channels,
      playback->sample_rate, true /* is_planar */, playback->params);

  if (!playback->buffer) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to allocate WebAudio playback buffer");
    return false;
  }

  backend_buffer_set_state(playback->buffer, BACKEND_STREAM_RUNNING);
  backend_buffer_set_target_level(playback->buffer, target_level);

  logger_info(&g_logger, "Opened WebAudio playback: rate=%d, channels=%zu",
              playback->sample_rate, playback->channels);
  return true;
}

static bool webaudio_playback_write(void *ctx, const audio_chunk_t *chunk,
                                    backend_error_t *err) {
  webaudio_playback_t *playback = (webaudio_playback_t *)ctx;
  if (!playback || !playback->buffer)
    return false;

  uint32_t sleep_ms = (uint32_t)((double)playback->chunk_size * 1000.0 /
                                 (double)playback->sample_rate / 2.0);
  if (sleep_ms < 1)
    sleep_ms = 1;
  return backend_buffer_write_chunk(playback->buffer, chunk, sleep_ms, 8, err);
}

static void webaudio_playback_close(void *ctx) {
  webaudio_playback_t *playback = (webaudio_playback_t *)ctx;
  if (!playback)
    return;
  if (playback->buffer) {
    backend_buffer_set_state(playback->buffer, BACKEND_STREAM_STOPPED);
    backend_buffer_free(playback->buffer);
    playback->buffer = NULL;
  }
}

static size_t webaudio_playback_get_buffer_level(void *ctx) {
  webaudio_playback_t *playback = (webaudio_playback_t *)ctx;
  if (!playback || !playback->buffer)
    return 0;
  return backend_buffer_get_level(playback->buffer);
}

static bool webaudio_playback_get_pending_rate_change(void *ctx,
                                                      double *out_rate) {
  webaudio_playback_t *playback = (webaudio_playback_t *)ctx;
  if (!playback || !playback->buffer)
    return false;
  if (backend_buffer_has_pending_rate_change(playback->buffer)) {
    backend_buffer_set_pending_rate_change(playback->buffer, false);
    if (out_rate)
      *out_rate = (double)playback->sample_rate;
    return true;
  }
  return false;
}

static bool webaudio_playback_prefill_silence(void *ctx, size_t frames,
                                              backend_error_t *err) {
  webaudio_playback_t *playback = (webaudio_playback_t *)ctx;
  (void)err;
  if (!playback || !playback->buffer)
    return false;
  backend_buffer_prefill_silence(playback->buffer, frames, 0x00);
  return true;
}

static bool webaudio_playback_get_is_paused(void *ctx) {
  webaudio_playback_t *playback = (webaudio_playback_t *)ctx;
  if (!playback || !playback->buffer)
    return false;
  return backend_buffer_get_state(playback->buffer) == BACKEND_STREAM_PAUSED;
}

static void webaudio_playback_set_is_paused(void *ctx, bool paused) {
  webaudio_playback_t *playback = (webaudio_playback_t *)ctx;
  if (!playback || !playback->buffer)
    return;
  backend_buffer_set_state(playback->buffer, paused ? BACKEND_STREAM_PAUSED
                                                    : BACKEND_STREAM_RUNNING);
}

static bool webaudio_playback_pitch_control_supported(void *ctx) {
  (void)ctx;
  return false;
}

static void webaudio_playback_set_pitch(void *ctx, double multiplier) {
  (void)ctx;
  (void)multiplier;
}

static void webaudio_playback_drain(void *ctx) {
  webaudio_playback_t *playback = (webaudio_playback_t *)ctx;
  if (!playback || !playback->buffer)
    return;
  backend_buffer_drain(playback->buffer);
}

static void webaudio_playback_stop(void *ctx) {
  webaudio_playback_t *playback = (webaudio_playback_t *)ctx;
  if (!playback || !playback->buffer)
    return;
  backend_buffer_set_state(playback->buffer, BACKEND_STREAM_STOPPED);
  backend_buffer_signal(playback->buffer);
}

static void webaudio_playback_destroy(void *ctx) {
  webaudio_playback_t *playback = (webaudio_playback_t *)ctx;
  if (playback) {
    webaudio_playback_close(playback);
    free(playback);
  }
}

static playback_backend_t *webaudio_playback_create(
    const playback_device_config_t *config, int sample_rate, int chunk_size,
    bool full_duplex, processing_parameters_t *params, backend_error_t *err) {
  (void)full_duplex;
  (void)err;
  webaudio_playback_t *playback =
      (webaudio_playback_t *)calloc(1, sizeof(webaudio_playback_t));
  if (!playback)
    return NULL;

  playback->sample_rate = sample_rate;
  playback->channels = config->cfg.webaudio.channels;
  playback->chunk_size = chunk_size;
  playback->target_level = (size_t)chunk_size;
  playback->params = params;

  playback_backend_t *backend =
      (playback_backend_t *)calloc(1, sizeof(playback_backend_t));
  if (!backend) {
    free(playback);
    return NULL;
  }
  backend->ctx = playback;
  backend->vtable = &g_webaudio_playback_vtable;
  return backend;
}

const playback_backend_vtable_t g_webaudio_playback_vtable = {
    .create = webaudio_playback_create,
    .open = webaudio_playback_open,
    .write = webaudio_playback_write,
    .close = webaudio_playback_close,
    .get_buffer_level = webaudio_playback_get_buffer_level,
    .get_pending_rate_change = webaudio_playback_get_pending_rate_change,
    .prefill_silence = webaudio_playback_prefill_silence,
    .get_is_paused = webaudio_playback_get_is_paused,
    .set_is_paused = webaudio_playback_set_is_paused,
    .pitch_control_supported = webaudio_playback_pitch_control_supported,
    .set_pitch = webaudio_playback_set_pitch,
    .drain = webaudio_playback_drain,
    .stop = webaudio_playback_stop,
    .destroy = webaudio_playback_destroy};

size_t webaudio_playback_render_samples(playback_backend_t *backend,
                                        float *const *channels,
                                        size_t frames) {
  if (!backend || !backend->ctx || !channels || frames == 0)
    return 0;
  webaudio_playback_t *playback = (webaudio_playback_t *)backend->ctx;
  if (!playback->buffer) {
    for (size_t c = 0; c < playback->channels; c++) {
      if (channels[c])
        memset(channels[c], 0, frames * sizeof(float));
    }
    return frames;
  }
  return backend_buffer_render(playback->buffer, (void *)channels, frames, 0x00);
}

#endif // ENABLE_WEBAUDIO
