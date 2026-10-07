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
 * it before freeing the ring. Each direction has its own in-flight counter
 * (callbacks count only the direction they serve). All slot and in-flight
 * counter accesses are sequentially consistent, so a callback that observed a
 * non-NULL slot is always counted before close() samples the counter.
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
  size_t ring_frames; ///< Capacity requested for the current ring.
  backend_buffer_t *buffer;
  processing_parameters_t *params;
} webaudio_playback_t;

// MARK: - Device State

static _Atomic(webaudio_capture_t *) g_active_capture = NULL;
static _Atomic(webaudio_playback_t *) g_active_playback = NULL;
// Device calls (and host format signalling) currently using each direction's
// slot. Per direction, so closing one backend never waits on the other
// direction's AudioWorklet thread.
static atomic_int g_capture_in_flight = 0;
static atomic_int g_playback_in_flight = 0;
// Native format of each direction's device, published by the host (0 = not
// known yet). Capture and playback run on separate AudioContexts with their own
// clocks and rates. Rate and channel count share one atomic word (rate in the
// high 32 bits, channels in the low 32 bits) so a reader never pairs a new rate
// with an old channel count.
static _Atomic uint64_t g_capture_format = 0;
static _Atomic uint64_t g_playback_format = 0;
static _Atomic(webaudio_capture_state_hook_t) g_capture_state_hook = NULL;

static uint64_t webaudio_pack_format(int sample_rate, size_t channels) {
  uint32_t rate = sample_rate > 0 ? (uint32_t)sample_rate : 0;
  uint32_t ch = channels <= UINT32_MAX ? (uint32_t)channels : UINT32_MAX;
  return ((uint64_t)rate << 32) | ch;
}

static int webaudio_format_rate(uint64_t format) {
  return (int)(uint32_t)(format >> 32);
}

static size_t webaudio_format_channels(uint64_t format) {
  return (size_t)(uint32_t)format;
}

// Silence for missing input channels. Never written, so the capture and
// playback render threads can both read it.
static float g_silence[WEBAUDIO_BLOCK_FRAMES];

/** True if a device format can serve a backend at @p rate / @p channels
 * (unknown device values match anything). */
static bool webaudio_format_matches(uint64_t format, int rate,
                                    size_t channels) {
  int device_rate = webaudio_format_rate(format);
  size_t device_channels = webaudio_format_channels(format);
  return (device_rate == 0 || device_rate == rate) &&
         (device_channels == 0 || device_channels == channels);
}

/** Flags a pending format change on the attached backends when @p changed
 * (a known value changed) or when an attached backend no longer matches its
 * direction's device format — e.g. the captured stream's channel count
 * becoming known only after the capture backend opened in another count.
 * Both are flagged even when only one direction changed: the capture loop only
 * stops on a new rate, so a channel-only capture change stops the engine
 * through playback.
 * The engine stops with a FORMAT_CHANGE reason and the host restarts it in the
 * new format, as with the other backends.
 *
 * Runs on the host's (browser main) thread, concurrently with close(): it
 * counts itself in flight like a device callback, so close() waits for it
 * before freeing the backend it may be flagging. */
static void webaudio_signal_format_change(bool changed) {
  atomic_fetch_add(&g_capture_in_flight, 1);
  atomic_fetch_add(&g_playback_in_flight, 1);
  webaudio_capture_t *capture = atomic_load(&g_active_capture);
  webaudio_playback_t *playback = atomic_load(&g_active_playback);
  bool flag = changed;
  if (capture &&
      !webaudio_format_matches(atomic_load(&g_capture_format),
                               capture->sample_rate, capture->channels))
    flag = true;
  if (playback &&
      !webaudio_format_matches(atomic_load(&g_playback_format),
                               playback->sample_rate, playback->channels))
    flag = true;
  if (flag && capture && capture->buffer) {
    backend_buffer_set_pending_rate_change(capture->buffer, true);
    backend_buffer_signal(capture->buffer);
  }
  if (flag && playback && playback->buffer) {
    backend_buffer_set_pending_rate_change(playback->buffer, true);
    backend_buffer_signal(playback->buffer);
  }
  atomic_fetch_sub(&g_playback_in_flight, 1);
  atomic_fetch_sub(&g_capture_in_flight, 1);
}

/** Rate to report with a pending format change: the device's current rate. */
static double webaudio_current_rate(const _Atomic uint64_t *device_format,
                                    int fallback) {
  int rate = webaudio_format_rate(atomic_load(device_format));
  return (double)(rate != 0 ? rate : fallback);
}

/** Stores one direction's format; true if a known value changed. A value
 * becoming known (0 -> value) is not a change. */
static bool webaudio_store_format(_Atomic uint64_t *format_slot,
                                  int sample_rate, size_t channels,
                                  const char *direction) {
  uint64_t new_format = webaudio_pack_format(sample_rate, channels);
  uint64_t old_format = atomic_exchange(format_slot, new_format);
  int new_rate = webaudio_format_rate(new_format);
  int old_rate = webaudio_format_rate(old_format);
  size_t new_channels = webaudio_format_channels(new_format);
  size_t old_channels = webaudio_format_channels(old_format);
  bool changed =
      (old_rate != 0 && new_rate != 0 && old_rate != new_rate) ||
      (old_channels != 0 && new_channels != 0 && old_channels != new_channels);
  // Two messages (the logger takes at most four arguments), each only when
  // its value changed.
  if (changed && old_rate != new_rate)
    logger_warn(&g_logger, "WebAudio %s format changed: %d Hz -> %d Hz",
                direction, old_rate, new_rate);
  if (changed && old_channels != new_channels)
    logger_warn(&g_logger, "WebAudio %s channels changed: %zu -> %zu",
                direction, old_channels, new_channels);
  return changed;
}

void webaudio_device_set_capture_format(int sample_rate, size_t channels) {
  webaudio_signal_format_change(webaudio_store_format(
      &g_capture_format, sample_rate, channels, "capture"));
}

void webaudio_device_set_playback_format(int sample_rate, size_t channels) {
  webaudio_signal_format_change(webaudio_store_format(
      &g_playback_format, sample_rate, channels, "playback"));
}

void webaudio_device_set_capture_state_hook(
    webaudio_capture_state_hook_t hook) {
  atomic_store(&g_capture_state_hook, hook);
}

/** Waits until no device callback can still be using a backend detached from
 * the slot counted by @p in_flight. */
static void webaudio_wait_for_callbacks(atomic_int *in_flight) {
  while (atomic_load(in_flight) != 0) {
    sched_yield();
  }
}

static bool webaudio_format_error(backend_error_t *err, const char *msg) {
  if (err)
    backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
  return false;
}

/**
 * Checks that a backend can open at @p sample_rate with @p channels against its
 * direction's device format. The device runs only in its native format, like
 * the other backends' hardware: a backend in any other format is refused (use
 * the rate and channel count reported by webaudio_describe()). Unknown device
 * values accept any.
 */
static bool webaudio_check_format(bool is_capture, int sample_rate,
                                  size_t channels, backend_error_t *err) {
  char msg[160];
  const char *direction = is_capture ? "capture" : "playback";
  uint64_t format =
      atomic_load(is_capture ? &g_capture_format : &g_playback_format);
  int device_rate = webaudio_format_rate(format);
  size_t device_channels = webaudio_format_channels(format);
  if (device_rate != 0 && device_rate != sample_rate) {
    snprintf(msg, sizeof(msg),
             "WebAudio %s device runs at %d Hz but %d Hz was requested",
             direction, device_rate, sample_rate);
    return webaudio_format_error(err, msg);
  }
  if (device_channels != 0 && device_channels != channels) {
    snprintf(msg, sizeof(msg),
             "WebAudio %s device has %zu channels but %zu were requested",
             direction, device_channels, channels);
    return webaudio_format_error(err, msg);
  }
  return true;
}

void webaudio_device_process(const float *const *inputs, size_t input_channels,
                             float *const *outputs, size_t output_channels,
                             size_t frames) {
  // Capture and playback render on separate contexts: each call serves only
  // the direction it carries buffers for, and is counted in flight for it.
  bool serves_capture = inputs != NULL;
  bool serves_playback = outputs != NULL;
  if (serves_capture)
    atomic_fetch_add(&g_capture_in_flight, 1);
  if (serves_playback)
    atomic_fetch_add(&g_playback_in_flight, 1);
  webaudio_capture_t *capture =
      serves_capture ? atomic_load(&g_active_capture) : NULL;
  webaudio_playback_t *playback =
      serves_playback ? atomic_load(&g_active_playback) : NULL;
  // Backends left in an old format after a device change are not served
  // (they are stopped through the pending format change).
  uint64_t capture_fmt =
      atomic_load_explicit(&g_capture_format, memory_order_relaxed);
  uint64_t playback_fmt =
      atomic_load_explicit(&g_playback_format, memory_order_relaxed);
  if (capture &&
      !webaudio_format_matches(capture_fmt, capture->sample_rate,
                               capture->channels))
    capture = NULL;
  if (playback &&
      !webaudio_format_matches(playback_fmt, playback->sample_rate,
                               playback->channels))
    playback = NULL;
  if (playback && playback->channels != output_channels)
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

  if (serves_playback)
    atomic_fetch_sub(&g_playback_in_flight, 1);
  if (serves_capture)
    atomic_fetch_sub(&g_capture_in_flight, 1);
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

audio_device_descriptor_t *
webaudio_describe(const char *device, bool is_capture, device_error_t *err) {
  // The one device is "default"; no name selects it too, as for ALSA.
  if (device && device[0] != '\0' && strcmp(device, "default") != 0) {
    if (err)
      device_error_init(err, DEVICE_ERROR_NOT_FOUND, "Unknown WebAudio device");
    return NULL;
  }
  // Only the active stream's format: the captured stream's (capture) or the
  // output's (playback) native rate and channel count. Values not known yet
  // (no device, or no captured stream) default to 48 kHz stereo; when the real
  // value differs, the device reports a format change.
  uint64_t format =
      atomic_load(is_capture ? &g_capture_format : &g_playback_format);
  int rate = webaudio_format_rate(format);
  if (rate == 0)
    rate = WEBAUDIO_DEFAULT_SAMPLE_RATE;
  size_t channels = webaudio_format_channels(format);
  if (channels == 0)
    channels = WEBAUDIO_DEFAULT_CHANNELS;
  if (channels > WEBAUDIO_MAX_CHANNELS)
    channels = WEBAUDIO_MAX_CHANNELS;
  const int *rates = &rate;
  size_t rates_count = 1;
  size_t min_channels = channels;
  size_t channel_counts = 1;

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
  if (capture->buffer) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "WebAudio capture is already open");
    return false;
  }
  if (!webaudio_check_format(true, capture->sample_rate, capture->channels,
                             err))
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
  // The format may have changed since webaudio_check_format(): a change that
  // ran before the slot was published did not see this backend. Re-check now
  // (seq_cst: either that change sees the slot or this sees its format).
  webaudio_signal_format_change(false);
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
  webaudio_wait_for_callbacks(&g_capture_in_flight);
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
      *out_rate =
          webaudio_current_rate(&g_capture_format, capture->sample_rate);
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

/** Ring capacity for @p target_level frames of prefill/cushion plus four
 * chunks of headroom (at least 25 ms), as for PipeWire. */
static size_t webaudio_playback_ring_frames(const webaudio_playback_t *playback,
                                            size_t target_level) {
  size_t pb_min_frames = (size_t)ceil((double)playback->sample_rate * 0.025);
  size_t pb_prefill_frames = target_level > (size_t)(3 * playback->chunk_size)
                                 ? target_level
                                 : (size_t)(3 * playback->chunk_size);
  size_t pb_frames_needed =
      pb_prefill_frames + (size_t)(4 * playback->chunk_size);
  if (pb_frames_needed < pb_min_frames)
    pb_frames_needed = pb_min_frames;
  return pb_frames_needed;
}

static backend_buffer_t *
webaudio_playback_create_buffer(const webaudio_playback_t *playback,
                                size_t frames) {
  return backend_buffer_create(frames, BINARY_SAMPLE_FORMAT_F32_LE,
                               playback->channels, playback->sample_rate,
                               true /* is_planar */, playback->params);
}

static bool webaudio_playback_open(void *ctx, backend_error_t *err) {
  webaudio_playback_t *playback = (webaudio_playback_t *)ctx;
  if (!playback)
    return false;
  if (playback->buffer) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "WebAudio playback is already open");
    return false;
  }
  if (!webaudio_check_format(false, playback->sample_rate, playback->channels,
                             err))
    return false;

  size_t target_level = playback->target_level > 0
                            ? playback->target_level
                            : (size_t)playback->chunk_size;
  size_t pb_frames_needed =
      webaudio_playback_ring_frames(playback, target_level);
  playback->buffer =
      webaudio_playback_create_buffer(playback, pb_frames_needed);
  playback->ring_frames = pb_frames_needed;

  if (!playback->buffer) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to allocate WebAudio playback buffer");
    return false;
  }

  backend_buffer_set_state(playback->buffer, BACKEND_STREAM_RUNNING);
  backend_buffer_set_target_level(playback->buffer, target_level);
  atomic_store(&g_active_playback, playback);
  // Re-check the format after publishing, as in webaudio_capture_open().
  webaudio_signal_format_change(false);

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
  webaudio_wait_for_callbacks(&g_playback_in_flight);
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
      *out_rate =
          webaudio_current_rate(&g_playback_format, playback->sample_rate);
    return true;
  }
  return false;
}

/**
 * Grows the playback ring so @p target_level fits with headroom. The engine's
 * prefill carries devices.target_level, which the backend does not know at
 * open() (WebAudio has no per-device target_level); without this the ring,
 * sized from chunk_size, could be too small for the prefill and the rate
 * controller's target. Runs on the playback thread during start-up, before
 * any audio is written (same phase as open()): detaches the backend, waits for
 * the playback worklet, swaps the ring and re-attaches.
 */
static void webaudio_playback_ensure_capacity(webaudio_playback_t *playback,
                                              size_t target_level) {
  size_t needed = webaudio_playback_ring_frames(playback, target_level);
  if (needed <= playback->ring_frames)
    return;
  if (backend_buffer_get_available_read_frames(playback->buffer) > 0) {
    logger_warn(&g_logger,
                "WebAudio playback ring (%zu frames) is smaller than target "
                "level %zu needs, but already holds audio; not resizing",
                playback->ring_frames, target_level);
    return;
  }
  backend_buffer_t *bigger = webaudio_playback_create_buffer(playback, needed);
  if (!bigger) {
    logger_warn(&g_logger,
                "Failed to grow the WebAudio playback ring to %zu frames",
                needed);
    return;
  }
  webaudio_playback_t *expected = playback;
  bool attached =
      atomic_compare_exchange_strong(&g_active_playback, &expected, NULL);
  webaudio_wait_for_callbacks(&g_playback_in_flight);
  backend_buffer_t *old = playback->buffer;
  backend_buffer_set_state(bigger, backend_buffer_get_state(old));
  backend_buffer_set_pending_rate_change(
      bigger, backend_buffer_has_pending_rate_change(old));
  playback->buffer = bigger;
  playback->ring_frames = needed;
  backend_buffer_free(old);
  if (attached) {
    atomic_store(&g_active_playback, playback);
    webaudio_signal_format_change(false);
  }
  logger_info(&g_logger, "Grew WebAudio playback ring to %zu frames", needed);
}

static bool webaudio_playback_prefill_silence(void *ctx, size_t frames,
                                              backend_error_t *err) {
  webaudio_playback_t *playback = (webaudio_playback_t *)ctx;
  (void)err;
  if (!playback || !playback->buffer)
    return false;
  webaudio_playback_ensure_capacity(playback, frames);
  playback->target_level = frames;
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
  if (backend_buffer_get_state(playback->buffer) == BACKEND_STREAM_STOPPED)
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

// No .drain: at end of stream the queued audio must play out. The engine
// waits for the reported level to reach zero (as with CoreAudio/PipeWire);
// backend_buffer_drain() would instead discard it, and would store the ring's
// read index from the engine thread while the AudioWorklet consumer owns it.

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
    .stop = webaudio_playback_stop,
    .destroy = webaudio_playback_destroy};

#endif // ENABLE_WEBAUDIO
