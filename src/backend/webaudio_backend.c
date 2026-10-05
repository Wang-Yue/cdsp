/**
 * @file webaudio_backend.c
 * @brief WebAudio backend implementation.
 *
 * Callback-driven backend in the same shape as CoreAudio/ASIO/PipeWire: the
 * engine threads read and write planar F32 backend_buffer_t rings, and the
 * device callback (webaudio_device_process(), run by the AudioWorklet render
 * quantum on the browser's real-time audio thread) pushes captured frames and
 * renders playback frames directly to/from those rings.
 *
 * Attach/detach protocol: open() publishes the backend in an atomic slot;
 * close() clears the slot and waits until no device callback is still using
 * it before freeing the ring. All slot and in-flight counter accesses are
 * sequentially consistent, so a callback that observed a non-NULL slot is
 * always counted before close() samples the counter.
 */

#if defined(ENABLE_WEBAUDIO)

#include "backend/webaudio_backend.h"

#include <math.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/audio_chunk.h"
#include "audio/sample_format.h"
#include "backend/backend_buffer.h"
#include "config/config_gen.h"
#include "logging/app_logger.h"

static const logger_t g_logger = {"dsp.backend.webaudio"};

/** Largest block handled per inner step of webaudio_device_process(). */
#define WEBAUDIO_BLOCK_FRAMES 128

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

// MARK: - Device State

static _Atomic(webaudio_capture_t *) g_active_capture = NULL;
static _Atomic(webaudio_playback_t *) g_active_playback = NULL;
static atomic_int g_callbacks_in_flight = 0;
static atomic_int g_device_sample_rate = 0;
static atomic_size_t g_device_output_channels = 0;
static atomic_size_t g_device_max_output_channels = 0;
static _Atomic(webaudio_format_request_hook_t) g_format_request_hook = NULL;
static _Atomic(webaudio_capture_state_hook_t) g_capture_state_hook = NULL;

// Audio-thread-only scratch: silence for missing input channels. Only the
// single device callback thread touches it.
static float g_silence[WEBAUDIO_BLOCK_FRAMES];

void webaudio_device_set_sample_rate(int sample_rate) {
  atomic_store(&g_device_sample_rate, sample_rate > 0 ? sample_rate : 0);
}

void webaudio_device_set_output_channels(size_t channels, size_t max_channels) {
  atomic_store(&g_device_output_channels, channels);
  atomic_store(&g_device_max_output_channels, max_channels);
}

void webaudio_device_set_format_request_hook(webaudio_format_request_hook_t hook) {
  atomic_store(&g_format_request_hook, hook);
}

void webaudio_device_set_capture_state_hook(webaudio_capture_state_hook_t hook) {
  atomic_store(&g_capture_state_hook, hook);
}

/** Waits until no device callback can still be using a detached backend. */
static void webaudio_wait_for_callbacks(void) {
  while (atomic_load(&g_callbacks_in_flight) != 0) {
    sched_yield();
  }
}

static bool webaudio_format_error(backend_error_t *err, const char *msg) {
  if (err)
    backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
  return false;
}

/**
 * Checks that a backend can open at @p sample_rate with @p input_channels
 * (capture) or @p output_channels (playback; the other is 0), asking the device
 * to switch when it runs in another format. @p other_rate is the rate of the
 * attached backend of the other direction (0 if none): both share the one
 * device.
 */
static bool webaudio_check_format(int sample_rate, int other_rate,
                                  size_t input_channels, size_t output_channels,
                                  backend_error_t *err) {
  char msg[160];
  if (other_rate != 0 && other_rate != sample_rate) {
    snprintf(msg, sizeof(msg),
             "WebAudio capture and playback share one device and must use the "
             "same rate (%d Hz vs %d Hz)",
             sample_rate, other_rate);
    return webaudio_format_error(err, msg);
  }
  size_t max_out = atomic_load(&g_device_max_output_channels);
  if (output_channels != 0 && max_out != 0 && output_channels > max_out) {
    snprintf(msg, sizeof(msg),
             "WebAudio output device supports up to %zu channels but %zu were "
             "requested",
             max_out, output_channels);
    return webaudio_format_error(err, msg);
  }
  int device_rate = atomic_load(&g_device_sample_rate);
  size_t device_out = atomic_load(&g_device_output_channels);
  webaudio_format_request_hook_t hook = atomic_load(&g_format_request_hook);
  if (hook) {
    if (sample_rate < WEBAUDIO_MIN_SAMPLE_RATE ||
        sample_rate > WEBAUDIO_MAX_SAMPLE_RATE) {
      snprintf(msg, sizeof(msg),
               "WebAudio supports %d to %d Hz but %d Hz was requested",
               WEBAUDIO_MIN_SAMPLE_RATE, WEBAUDIO_MAX_SAMPLE_RATE, sample_rate);
      return webaudio_format_error(err, msg);
    }
    // The backend stays silent until the device runs in its format.
    hook(sample_rate, input_channels, output_channels);
    return true;
  }
  if (device_rate != 0 && device_rate != sample_rate) {
    snprintf(msg, sizeof(msg),
             "WebAudio device runs at %d Hz but %d Hz was requested",
             device_rate, sample_rate);
    return webaudio_format_error(err, msg);
  }
  if (output_channels != 0 && device_out != 0 &&
      device_out != output_channels) {
    snprintf(msg, sizeof(msg),
             "WebAudio device renders %zu channels but %zu were requested",
             device_out, output_channels);
    return webaudio_format_error(err, msg);
  }
  return true;
}

void webaudio_device_process(const float *const *inputs, size_t input_channels,
                             float *const *outputs, size_t output_channels,
                             size_t frames) {
  atomic_fetch_add(&g_callbacks_in_flight, 1);
  webaudio_capture_t *capture = atomic_load(&g_active_capture);
  webaudio_playback_t *playback = atomic_load(&g_active_playback);
  // Backends waiting for the device to switch to their format are not served.
  int device_rate = atomic_load_explicit(&g_device_sample_rate,
                                         memory_order_relaxed);
  if (device_rate != 0) {
    if (capture && capture->sample_rate != device_rate)
      capture = NULL;
    if (playback && playback->sample_rate != device_rate)
      playback = NULL;
  }
  if (playback && (!outputs || playback->channels != output_channels))
    playback = NULL;
  for (size_t c = 0; playback && c < output_channels; c++) {
    if (!outputs[c])
      playback = NULL;
  }

  for (size_t offset = 0; offset < frames; offset += WEBAUDIO_BLOCK_FRAMES) {
    size_t n = frames - offset;
    if (n > WEBAUDIO_BLOCK_FRAMES)
      n = WEBAUDIO_BLOCK_FRAMES;

    if (capture) {
      const float *src[WEBAUDIO_MAX_CHANNELS];
      for (size_t c = 0; c < capture->channels; c++) {
        src[c] = (inputs && c < input_channels && inputs[c])
                     ? inputs[c] + offset
                     : g_silence;
      }
      backend_buffer_push(capture->buffer, src, n);
    }

    if (playback) {
      float *dst[WEBAUDIO_MAX_CHANNELS];
      for (size_t c = 0; c < playback->channels; c++)
        dst[c] = outputs[c] + offset;
      backend_buffer_render(playback->buffer, (void *)dst, n, 0x00);
    } else {
      // Without a served playback backend the device output stays silent.
      for (size_t c = 0; outputs && c < output_channels; c++) {
        if (outputs[c])
          memset(outputs[c] + offset, 0, n * sizeof(float));
      }
    }
  }

  atomic_fetch_sub(&g_callbacks_in_flight, 1);
}

// MARK: - Device Enumeration

int webaudio_get_available_devices(bool input, audio_device_t *out_devices,
                                   int max_devices) {
  (void)input;
  if (out_devices && max_devices > 0) {
    snprintf(out_devices[0].name, sizeof(out_devices[0].name), "default");
  }
  return 1;
}

audio_device_descriptor_t *webaudio_describe(const char *device,
                                             bool is_capture,
                                             device_error_t *err) {
  // The one device is "default"; no name selects it too, as for ALSA.
  if (device && device[0] != '\0' && strcmp(device, "default") != 0) {
    if (err)
      device_error_init(err, DEVICE_ERROR_NOT_FOUND, "Unknown WebAudio device");
    return NULL;
  }
  bool switchable = atomic_load(&g_format_request_hook) != NULL;
  // A device that can switch rates (or whose rate is not yet known) supports
  // every standard rate in the browser's range; otherwise only its fixed rate.
  static const int kStandardRates[] = {
      8000,  11025, 16000,  22050,  32000,  44100,  48000,  88200,
      96000, 176400, 192000, 352800, 384000, 705600, 768000};
  int fixed_rate = switchable ? 0 : atomic_load(&g_device_sample_rate);
  const int *rates = fixed_rate ? &fixed_rate : kStandardRates;
  size_t rates_count =
      fixed_rate ? 1 : sizeof(kStandardRates) / sizeof(kStandardRates[0]);

  // Capture takes any node input channel count (the node mixes its sources to
  // it). Playback takes up to the hardware's output channel count; a device
  // that cannot switch only takes the count it renders.
  size_t min_channels = 1;
  size_t max_channels = WEBAUDIO_MAX_CHANNELS;
  if (!is_capture) {
    size_t device_out = atomic_load(&g_device_output_channels);
    size_t max_out = atomic_load(&g_device_max_output_channels);
    max_channels = max_out ? max_out : (device_out ? device_out : 2);
    if (max_channels > WEBAUDIO_MAX_CHANNELS)
      max_channels = WEBAUDIO_MAX_CHANNELS;
    if (!switchable && device_out != 0 && device_out <= max_channels)
      min_channels = max_channels = device_out;
  }
  size_t channel_counts = max_channels - min_channels + 1;

  audio_device_descriptor_t *desc = calloc(1, sizeof(*desc));
  if (!desc)
    goto oom;
  snprintf(desc->name, sizeof(desc->name), "default");
  desc->capability_sets = calloc(1, sizeof(device_capability_set_t));
  if (!desc->capability_sets)
    goto oom;
  desc->capability_sets_count = 1;
  device_capability_set_t *set = &desc->capability_sets[0];
  snprintf(set->mode, sizeof(set->mode), "Shared");
  set->capabilities = calloc(channel_counts, sizeof(channel_capability_t));
  if (!set->capabilities)
    goto oom;
  for (size_t k = 0; k < channel_counts; k++) {
    channel_capability_t *ch = &set->capabilities[k];
    set->capabilities_count = k + 1;
    ch->channels = (int)(min_channels + k);
    ch->samplerates = calloc(rates_count, sizeof(samplerate_capability_t));
    if (!ch->samplerates)
      goto oom;
    for (size_t i = 0; i < rates_count; i++) {
      samplerate_capability_t *sr = &ch->samplerates[i];
      ch->samplerates_count = i + 1;
      sr->samplerate = rates[i];
      sr->formats = calloc(1, sizeof(char *));
      if (!sr->formats)
        goto oom;
      sr->formats[0] = strdup("F32");
      if (!sr->formats[0])
        goto oom;
      sr->formats_count = 1;
    }
  }
  return desc;

oom:
  free_audio_device_descriptor(desc);
  if (err)
    device_error_init(err, DEVICE_ERROR_OTHER, "Out of memory");
  return NULL;
}

// MARK: - Capture Backend Implementation

static bool webaudio_capture_open(void *ctx, backend_error_t *err) {
  webaudio_capture_t *capture = (webaudio_capture_t *)ctx;
  if (!capture)
    return false;
  webaudio_playback_t *playback = atomic_load(&g_active_playback);
  if (!webaudio_check_format(capture->sample_rate,
                             playback ? playback->sample_rate : 0,
                             capture->channels, 0, err))
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
  atomic_store(&g_active_capture, capture);
  webaudio_capture_state_hook_t hook = atomic_load(&g_capture_state_hook);
  if (hook)
    hook(true);
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
  webaudio_capture_t *expected = capture;
  if (atomic_compare_exchange_strong(&g_active_capture, &expected, NULL)) {
    webaudio_capture_state_hook_t hook = atomic_load(&g_capture_state_hook);
    if (hook)
      hook(false);
  }
  webaudio_wait_for_callbacks();
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
  size_t channels = config->cfg.webaudio.channels;
  if (channels == 0 || channels > WEBAUDIO_MAX_CHANNELS) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "WebAudio capture channel count out of range");
    return NULL;
  }
  webaudio_capture_t *capture =
      (webaudio_capture_t *)calloc(1, sizeof(webaudio_capture_t));
  if (!capture)
    return NULL;

  capture->sample_rate = sample_rate;
  capture->channels = channels;
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

// MARK: - Playback Backend Implementation

static bool webaudio_playback_open(void *ctx, backend_error_t *err) {
  webaudio_playback_t *playback = (webaudio_playback_t *)ctx;
  if (!playback)
    return false;
  webaudio_capture_t *capture = atomic_load(&g_active_capture);
  if (!webaudio_check_format(playback->sample_rate,
                             capture ? capture->sample_rate : 0, 0,
                             playback->channels, err))
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
  atomic_store(&g_active_playback, playback);

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
  webaudio_playback_t *expected = playback;
  atomic_compare_exchange_strong(&g_active_playback, &expected, NULL);
  webaudio_wait_for_callbacks();
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
  size_t channels = config->cfg.webaudio.channels;
  if (channels == 0 || channels > WEBAUDIO_MAX_CHANNELS) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "WebAudio playback channel count out of range");
    return NULL;
  }
  webaudio_playback_t *playback =
      (webaudio_playback_t *)calloc(1, sizeof(webaudio_playback_t));
  if (!playback)
    return NULL;

  playback->sample_rate = sample_rate;
  playback->channels = channels;
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

#endif // ENABLE_WEBAUDIO
