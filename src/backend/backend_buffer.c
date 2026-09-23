#include "backend/backend_buffer.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "logging/app_logger.h"
#include "utils/cdsp_time.h"
#include "utils/device_buffer_estimator.h"
#include "utils/lock_free_ring_buffer.h"

static const logger_t g_logger = {"dsp.backend.buffer"};

typedef enum {
  BACKEND_BUFFER_INTERLEAVED = 0,
  BACKEND_BUFFER_PLANAR = 1,
} backend_buffer_type_t;

struct backend_buffer {
  backend_buffer_type_t type;
  binary_sample_format_t format;
  size_t channels;
  size_t bytes_per_sample;
  size_t blockalign;
  size_t capacity_frames;

  spsc_byte_ring_buffer_t *byte_ring;
  spsc_planar_ring_buffer_t *planar_ring;

  // Real-time buffer estimator & underrun state machine (playback)
  device_buffer_estimator_t device;
  _Atomic size_t target_level;
  _Atomic size_t silence_to_insert;
  _Atomic bool is_running;

  // Monitored control flags
  _Atomic bool *thread_running;
  _Atomic bool *stopped;
  _Atomic bool *is_paused;
  _Atomic bool *has_pending_rate_change;
};

/* --- Lifecycle Management --- */

backend_buffer_t *backend_buffer_create(size_t capacity_frames,
                                        binary_sample_format_t format,
                                        size_t channels, double sample_rate,
                                        bool is_planar) {
  if (channels == 0 || capacity_frames == 0)
    return NULL;
  backend_buffer_t *bb =
      (backend_buffer_t *)calloc(1, sizeof(backend_buffer_t));
  if (!bb)
    return NULL;
  bb->type = is_planar ? BACKEND_BUFFER_PLANAR : BACKEND_BUFFER_INTERLEAVED;
  bb->format = format;
  bb->channels = channels;
  bb->bytes_per_sample = sample_format_bytes_per_sample(format);
  if (bb->bytes_per_sample == 0) {
    free(bb);
    return NULL;
  }
  bb->blockalign = bb->bytes_per_sample * channels;
  bb->capacity_frames = capacity_frames;

  device_buffer_estimator_init(&bb->device, sample_rate);
  atomic_init(&bb->target_level, 0);
  atomic_init(&bb->silence_to_insert, 0);
  atomic_init(&bb->is_running, true);

  if (is_planar) {
    bb->planar_ring = spsc_planar_ring_buffer_create(
        channels, bb->bytes_per_sample, capacity_frames);
    if (!bb->planar_ring) {
      free(bb);
      return NULL;
    }
  } else {
    size_t capacity_bytes = capacity_frames * bb->blockalign;
    bb->byte_ring = spsc_byte_ring_buffer_create(capacity_bytes);
    if (!bb->byte_ring) {
      free(bb);
      return NULL;
    }
  }
  return bb;
}

void backend_buffer_free(backend_buffer_t *bb) {
  if (!bb)
    return;
  if (bb->byte_ring) {
    spsc_byte_ring_buffer_free(bb->byte_ring);
    bb->byte_ring = NULL;
  }
  if (bb->planar_ring) {
    spsc_planar_ring_buffer_free(bb->planar_ring);
    bb->planar_ring = NULL;
  }
  free(bb);
}

void backend_buffer_set_control_flags(backend_buffer_t *bb,
                                      _Atomic bool *thread_running,
                                      _Atomic bool *stopped,
                                      _Atomic bool *is_paused,
                                      _Atomic bool *has_pending_rate_change) {
  if (!bb)
    return;
  bb->thread_running = thread_running;
  bb->stopped = stopped;
  bb->is_paused = is_paused;
  bb->has_pending_rate_change = has_pending_rate_change;
}

void backend_buffer_set_rate(backend_buffer_t *bb, double sample_rate) {
  if (!bb)
    return;
  device_buffer_estimator_set_rate(&bb->device, sample_rate);
}

void backend_buffer_reset(backend_buffer_t *bb) {
  if (!bb)
    return;
  device_buffer_estimator_reset(&bb->device);
}

void backend_buffer_publish(backend_buffer_t *bb, size_t device_frames) {
  if (!bb)
    return;
  device_buffer_estimator_add(&bb->device, device_frames);
}

static size_t backend_buffer_level(const backend_buffer_t *bb,
                                   const spsc_byte_ring_buffer_t *ring,
                                   size_t blockalign) {
  size_t ring_frames = 0;
  if (ring && blockalign > 0) {
    ring_frames =
        spsc_byte_ring_buffer_get_available_to_read(ring) / blockalign;
  }
  return ring_frames +
         device_buffer_estimator_estimate(bb ? &bb->device : NULL);
}

static size_t
backend_buffer_planar_level(const backend_buffer_t *bb,
                            const spsc_planar_ring_buffer_t *ring) {
  size_t ring_frames = 0;
  if (ring) {
    ring_frames = spsc_planar_ring_buffer_get_available_to_read(ring);
  }
  return ring_frames +
         device_buffer_estimator_estimate(bb ? &bb->device : NULL);
}

void backend_buffer_set_target_level(backend_buffer_t *bb,
                                     size_t target_level) {
  if (!bb)
    return;
  atomic_store_explicit(&bb->target_level, target_level, memory_order_release);
}

/* --- Silence Prefill & Realtime Device Rendering --- */

static void backend_buffer_prefill_planar(backend_buffer_t *bb,
                                          spsc_planar_ring_buffer_t *ring,
                                          size_t frames) {
  if (!bb)
    return;
  atomic_store_explicit(&bb->target_level, frames, memory_order_release);
  atomic_store_explicit(&bb->silence_to_insert, 0, memory_order_release);
  atomic_store_explicit(&bb->is_running, true, memory_order_release);
  if (ring && frames > 0) {
    spsc_planar_ring_buffer_write_silence(ring, frames);
  }
}

static void backend_buffer_prefill_byte(backend_buffer_t *bb,
                                        spsc_byte_ring_buffer_t *ring,
                                        size_t frames, size_t blockalign,
                                        uint8_t silence_byte) {
  if (!bb)
    return;
  atomic_store_explicit(&bb->target_level, frames, memory_order_release);
  atomic_store_explicit(&bb->silence_to_insert, 0, memory_order_release);
  atomic_store_explicit(&bb->is_running, true, memory_order_release);
  if (ring && frames > 0 && blockalign > 0) {
    spsc_byte_ring_buffer_write_silence(ring, frames * blockalign,
                                        silence_byte);
  }
}

static size_t backend_buffer_render_planar(backend_buffer_t *bb,
                                           spsc_planar_ring_buffer_t *ring,
                                           void *const *dst_channels,
                                           size_t frames,
                                           uint8_t silence_byte) {
  if (!bb || !ring || !dst_channels || frames == 0)
    return 0;

  if (!atomic_load_explicit(&bb->is_running, memory_order_relaxed)) {
    size_t avail = spsc_planar_ring_buffer_get_available_to_read(ring);
    if (avail > 0) {
      atomic_store_explicit(&bb->is_running, true, memory_order_relaxed);
      atomic_store_explicit(
          &bb->silence_to_insert,
          atomic_load_explicit(&bb->target_level, memory_order_relaxed),
          memory_order_relaxed);
    }
  }

  size_t consumed = spsc_planar_ring_buffer_read_with_silence(
      ring, dst_channels, frames, silence_byte, &bb->silence_to_insert,
      &bb->is_running);

  backend_buffer_publish(
      bb, atomic_load_explicit(&bb->silence_to_insert, memory_order_relaxed));
  return consumed;
}

static size_t backend_buffer_render_byte(backend_buffer_t *bb,
                                         spsc_byte_ring_buffer_t *ring,
                                         void *dst, size_t frames,
                                         size_t blockalign,
                                         uint8_t silence_byte) {
  if (!bb || !ring || !dst || frames == 0 || blockalign == 0)
    return 0;

  if (!atomic_load_explicit(&bb->is_running, memory_order_relaxed)) {
    size_t avail_bytes = spsc_byte_ring_buffer_get_available_to_read(ring);
    size_t avail_frames = avail_bytes / blockalign;
    if (avail_frames > 0) {
      atomic_store_explicit(&bb->is_running, true, memory_order_relaxed);
      atomic_store_explicit(
          &bb->silence_to_insert,
          atomic_load_explicit(&bb->target_level, memory_order_relaxed),
          memory_order_relaxed);
    }
  }

  size_t consumed = spsc_byte_ring_buffer_read_with_silence(
      ring, dst, frames, blockalign, silence_byte, &bb->silence_to_insert,
      &bb->is_running);

  backend_buffer_publish(
      bb, atomic_load_explicit(&bb->silence_to_insert, memory_order_relaxed));
  return consumed;
}

/* --- Engine-Side Audio Chunk Ring Buffer IO (Capture & Playback) --- */

static bool backend_buffer_read(spsc_byte_ring_buffer_t *ring_buffer,
                                size_t blockalign, size_t frames_requested,
                                binary_sample_format_t fmt, size_t channels,
                                _Atomic bool *thread_running,
                                _Atomic bool *stopped,
                                _Atomic bool *has_pending_rate_change,
                                audio_chunk_t *chunk, backend_error_t *err) {
  if (!ring_buffer || !chunk || channels == 0 || blockalign == 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_READ_ERROR, "Invalid parameters");
    return false;
  }

  // Hardware sample rate or format changes must be handled immediately without
  // decoding further frames under obsolete parameters to prevent filter
  // corruption.
  if (has_pending_rate_change &&
      atomic_load_explicit(has_pending_rate_change, memory_order_acquire)) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_NONE, "Format change pending");
    return false;
  }

  if (frames_requested == 0) {
    audio_chunk_set_valid_frames(chunk, 0);
    return true;
  }

  if (audio_chunk_get_channels(chunk) < channels) {
    if (err)
      backend_error_init(
          err, BACKEND_ERROR_INVALID_CHANNELS,
          "Chunk channels count does not match capture channels");
    return false;
  }

  if (audio_chunk_get_frames(chunk) < frames_requested) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                         "Chunk frame capacity too small for requested frames");
    return false;
  }

  if (frames_requested > SIZE_MAX / blockalign) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                         "Requested frame count exceeds maximum size");
    return false;
  }

  size_t bytes_requested = frames_requested * blockalign;

  // Draining semantics: Only check `stopped` / `thread_running` flags after
  // confirming that the ring buffer lacks sufficient bytes. If the capture
  // worker thread wrote its final batch of samples and stopped, those remaining
  // frames must still be consumed to avoid dropping audio at EOF / stream
  // shutdown.
  if (spsc_byte_ring_buffer_get_available_to_read(ring_buffer) <
      bytes_requested) {
    if ((stopped && atomic_load_explicit(stopped, memory_order_acquire)) ||
        (thread_running &&
         !atomic_load_explicit(thread_running, memory_order_acquire))) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                           "Capture stream stopped");
      return false;
    }
    if (err)
      backend_error_init(err, BACKEND_ERROR_NONE, "");
    return false;
  }

  const uint8_t *s1 = NULL, *s2 = NULL;
  size_t l1 = 0, l2 = 0;
  size_t read_avail = spsc_byte_ring_buffer_get_read_slices(
      ring_buffer, bytes_requested, &s1, &l1, &s2, &l2);
  if (read_avail < bytes_requested || !s1) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                         "Failed to get read slices from ring buffer");
    return false;
  }

  if (l1 % blockalign == 0) {
    size_t f1 = l1 / blockalign;
    if (f1 > 0) {
      if (!audio_chunk_decode_interleaved_offset(s1, fmt, channels, f1, chunk,
                                                 0)) {
        if (err)
          backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                             "Failed to decode captured audio data");
        return false;
      }
    }
    size_t f2 = l2 / blockalign;
    if (f2 > 0 && s2) {
      if (!audio_chunk_decode_interleaved_offset(s2, fmt, channels, f2, chunk,
                                                 f1)) {
        if (err)
          backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                             "Failed to decode captured audio data");
        return false;
      }
    }
  } else {
    size_t f1 = l1 / blockalign;
    if (f1 > 0) {
      if (!audio_chunk_decode_interleaved_offset(s1, fmt, channels, f1, chunk,
                                                 0)) {
        if (err)
          backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                             "Failed to decode captured audio data");
        return false;
      }
    }
    size_t rem_l1 = l1 % blockalign;
    size_t rem_l2 = blockalign - rem_l1;
    uint8_t split_frame[256];
    if (blockalign <= sizeof(split_frame) && s2 && l2 >= rem_l2) {
      memcpy(split_frame, s1 + f1 * blockalign, rem_l1);
      memcpy(split_frame + rem_l1, s2, rem_l2);
      if (!audio_chunk_decode_interleaved_offset(split_frame, fmt, channels, 1,
                                                 chunk, f1)) {
        if (err)
          backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                             "Failed to decode split audio frame");
        return false;
      }
      size_t f2 = (l2 - rem_l2) / blockalign;
      if (f2 > 0) {
        if (!audio_chunk_decode_interleaved_offset(s2 + rem_l2, fmt, channels,
                                                   f2, chunk, f1 + 1)) {
          if (err)
            backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                               "Failed to decode captured audio data");
          return false;
        }
      }
    }
  }

  spsc_byte_ring_buffer_advance_read(ring_buffer, bytes_requested);
  audio_chunk_set_valid_frames(chunk, frames_requested);
  return true;
}

static bool backend_buffer_write(spsc_byte_ring_buffer_t *ring_buffer,
                                 size_t blockalign, const audio_chunk_t *chunk,
                                 binary_sample_format_t fmt, size_t channels,
                                 uint32_t sleep_ms, uint32_t max_retries,
                                 _Atomic bool *thread_running,
                                 _Atomic bool *stopped, _Atomic bool *is_paused,
                                 _Atomic bool *has_pending_rate_change,
                                 backend_error_t *err) {
  if (!ring_buffer || !chunk || channels == 0 || blockalign == 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_WRITE_ERROR, "Invalid parameters");
    return false;
  }

  if (is_paused && atomic_load_explicit(is_paused, memory_order_acquire)) {
    return true;
  }

  if (has_pending_rate_change &&
      atomic_load_explicit(has_pending_rate_change, memory_order_acquire)) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_NONE, "Format change pending");
    return false;
  }

  if ((stopped && atomic_load_explicit(stopped, memory_order_acquire)) ||
      (thread_running &&
       !atomic_load_explicit(thread_running, memory_order_acquire))) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                         "Playback stream stopped");
    return false;
  }

  if (audio_chunk_get_channels(chunk) < channels) {
    if (err)
      backend_error_init(
          err, BACKEND_ERROR_INVALID_CHANNELS,
          "Chunk channels count does not match playback channels");
    return false;
  }

  size_t frames = audio_chunk_get_valid_frames(chunk);
  if (frames == 0)
    return true;

  if (frames > SIZE_MAX / blockalign) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                         "Frame count exceeds maximum size");
    return false;
  }

  size_t bytes_to_write = frames * blockalign;

  uint32_t retries = (max_retries > 0) ? max_retries : 1;
  for (uint32_t retry = 0; retry < retries; retry++) {
    if (spsc_byte_ring_buffer_get_available_to_write(ring_buffer) >=
        bytes_to_write) {
      break;
    }
    if (has_pending_rate_change &&
        atomic_load_explicit(has_pending_rate_change, memory_order_acquire)) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_NONE, "Format change pending");
      return false;
    }
    if ((stopped && atomic_load_explicit(stopped, memory_order_acquire)) ||
        (thread_running &&
         !atomic_load_explicit(thread_running, memory_order_acquire))) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                           "Playback stream stopped");
      return false;
    }
    if (is_paused && atomic_load_explicit(is_paused, memory_order_acquire)) {
      return true;
    }
    cdsp_sleep_ms(sleep_ms > 0 ? sleep_ms : 1);
  }

  // Audio chunks must be written as atomic units. If the ring buffer cannot fit
  // the complete chunk after backoff, drop the entire chunk rather than pushing
  // a fractured sub-chunk to prevent time-domain waveform discontinuity.
  if (spsc_byte_ring_buffer_get_available_to_write(ring_buffer) <
      bytes_to_write) {
    logger_debug(
        &g_logger,
        "Playback ring buffer is full after %u retries, dropped entire "
        "chunk of %zu bytes to preserve audio framing",
        retries, bytes_to_write);
    if (err)
      backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                         "Playback ring buffer full");
    return false;
  }

  uint8_t *s1 = NULL, *s2 = NULL;
  size_t l1 = 0, l2 = 0;
  size_t write_avail = spsc_byte_ring_buffer_get_write_slices(
      ring_buffer, bytes_to_write, &s1, &l1, &s2, &l2);
  if (write_avail < bytes_to_write || !s1) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                         "Failed to get write slices from ring buffer");
    return false;
  }

  if (l1 % blockalign == 0) {
    size_t f1 = l1 / blockalign;
    if (f1 > 0) {
      if (!audio_chunk_encode_interleaved_offset(chunk, fmt, channels, f1, s1,
                                                 0)) {
        if (err)
          backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                             "Failed to encode audio samples");
        return false;
      }
    }
    size_t f2 = l2 / blockalign;
    if (f2 > 0 && s2) {
      if (!audio_chunk_encode_interleaved_offset(chunk, fmt, channels, f2, s2,
                                                 f1)) {
        if (err)
          backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                             "Failed to encode audio samples");
        return false;
      }
    }
  } else {
    size_t f1 = l1 / blockalign;
    if (f1 > 0) {
      if (!audio_chunk_encode_interleaved_offset(chunk, fmt, channels, f1, s1,
                                                 0)) {
        if (err)
          backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                             "Failed to encode audio samples");
        return false;
      }
    }
    size_t rem_l1 = l1 % blockalign;
    size_t rem_l2 = blockalign - rem_l1;
    uint8_t split_frame[256];
    if (blockalign <= sizeof(split_frame) && s2 && l2 >= rem_l2) {
      if (!audio_chunk_encode_interleaved_offset(chunk, fmt, channels, 1,
                                                 split_frame, f1)) {
        if (err)
          backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                             "Failed to encode split audio frame");
        return false;
      }
      memcpy(s1 + f1 * blockalign, split_frame, rem_l1);
      memcpy(s2, split_frame + rem_l1, rem_l2);
      size_t f2 = (l2 - rem_l2) / blockalign;
      if (f2 > 0) {
        if (!audio_chunk_encode_interleaved_offset(chunk, fmt, channels, f2,
                                                   s2 + rem_l2, f1 + 1)) {
          if (err)
            backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                               "Failed to encode audio samples");
          return false;
        }
      }
    }
  }

  spsc_byte_ring_buffer_advance_write(ring_buffer, bytes_to_write);
  return true;
}

static bool backend_buffer_planar_read(
    spsc_planar_ring_buffer_t *ring_buffer, size_t frames_requested,
    binary_sample_format_t fmt, size_t channels, _Atomic bool *thread_running,
    _Atomic bool *stopped, _Atomic bool *has_pending_rate_change,
    audio_chunk_t *chunk, backend_error_t *err) {
  if (!ring_buffer || !chunk || channels == 0 || frames_requested == 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_READ_ERROR, "Invalid parameters");
    return false;
  }

  if (has_pending_rate_change &&
      atomic_load_explicit(has_pending_rate_change, memory_order_acquire)) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_NONE, "Format change pending");
    return false;
  }

  if ((stopped && atomic_load_explicit(stopped, memory_order_acquire)) ||
      (thread_running &&
       !atomic_load_explicit(thread_running, memory_order_acquire))) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                         "Capture stream stopped");
    return false;
  }

  if (audio_chunk_get_channels(chunk) < channels) {
    if (err)
      backend_error_init(
          err, BACKEND_ERROR_INVALID_CHANNELS,
          "Chunk channels count does not match capture channels");
    return false;
  }

  size_t available_frames =
      spsc_planar_ring_buffer_get_available_to_read(ring_buffer);
  if (available_frames < frames_requested) {
    if (stopped && atomic_load_explicit(stopped, memory_order_acquire)) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                           "Capture stream stopped");
      return false;
    }
    if (err)
      backend_error_init(err, BACKEND_ERROR_NONE, "");
    return false;
  }

  size_t offset = 0, l1 = 0, l2 = 0;
  size_t read_avail = spsc_planar_ring_buffer_get_read_indices(
      ring_buffer, frames_requested, &offset, &l1, &l2);
  if (read_avail < frames_requested) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                         "Failed to get read slices from planar ring buffer");
    return false;
  }

  size_t bps = sample_format_bytes_per_sample(fmt);
  if (bps == 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                         "Invalid sample format");
    return false;
  }

  for (size_t c = 0; c < channels; c++) {
    const uint8_t *chan_storage =
        spsc_planar_ring_buffer_get_channel_ptr(ring_buffer, c);
    if (!chan_storage) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                           "Failed to access planar channel buffer");
      return false;
    }
    if (l1 > 0) {
      if (!audio_chunk_decode_channel(chan_storage + offset * bps, fmt, l1,
                                      chunk, c, 0)) {
        if (err)
          backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                             "Failed to decode captured planar audio data");
        return false;
      }
    }
    if (l2 > 0) {
      if (!audio_chunk_decode_channel(chan_storage, fmt, l2, chunk, c, l1)) {
        if (err)
          backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                             "Failed to decode captured planar audio data");
        return false;
      }
    }
  }

  spsc_planar_ring_buffer_advance_read(ring_buffer, frames_requested);
  audio_chunk_set_valid_frames(chunk, frames_requested);
  return true;
}

static bool backend_buffer_planar_write(
    spsc_planar_ring_buffer_t *ring_buffer, const audio_chunk_t *chunk,
    binary_sample_format_t fmt, size_t channels, uint32_t sleep_ms,
    uint32_t max_retries, _Atomic bool *thread_running, _Atomic bool *stopped,
    _Atomic bool *is_paused, _Atomic bool *has_pending_rate_change,
    backend_error_t *err) {
  if (!ring_buffer || !chunk || channels == 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_WRITE_ERROR, "Invalid parameters");
    return false;
  }

  if (is_paused && atomic_load_explicit(is_paused, memory_order_acquire)) {
    return true;
  }

  if (has_pending_rate_change &&
      atomic_load_explicit(has_pending_rate_change, memory_order_acquire)) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_NONE, "Format change pending");
    return false;
  }

  if ((stopped && atomic_load_explicit(stopped, memory_order_acquire)) ||
      (thread_running &&
       !atomic_load_explicit(thread_running, memory_order_acquire))) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                         "Playback stream stopped");
    return false;
  }

  if (audio_chunk_get_channels(chunk) < channels) {
    if (err)
      backend_error_init(
          err, BACKEND_ERROR_INVALID_CHANNELS,
          "Chunk channels count does not match playback channels");
    return false;
  }

  size_t frames = audio_chunk_get_valid_frames(chunk);
  if (frames == 0)
    return true;

  uint32_t retries = (max_retries > 0) ? max_retries : 1;
  for (uint32_t retry = 0; retry < retries; retry++) {
    if (spsc_planar_ring_buffer_get_available_to_write(ring_buffer) >= frames) {
      break;
    }
    if (has_pending_rate_change &&
        atomic_load_explicit(has_pending_rate_change, memory_order_acquire)) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_NONE, "Format change pending");
      return false;
    }
    if ((stopped && atomic_load_explicit(stopped, memory_order_acquire)) ||
        (thread_running &&
         !atomic_load_explicit(thread_running, memory_order_acquire))) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                           "Playback stream stopped");
      return false;
    }
    if (is_paused && atomic_load_explicit(is_paused, memory_order_acquire)) {
      return true;
    }
    cdsp_sleep_ms(sleep_ms > 0 ? sleep_ms : 1);
  }

  if (spsc_planar_ring_buffer_get_available_to_write(ring_buffer) < frames) {
    logger_debug(
        &g_logger,
        "Playback planar ring buffer is full after %u retries, dropped entire "
        "chunk of %zu frames to preserve audio framing",
        retries, frames);
    if (err)
      backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                         "Playback ring buffer full");
    return false;
  }

  size_t offset = 0, l1 = 0, l2 = 0;
  size_t write_avail = spsc_planar_ring_buffer_get_write_indices(
      ring_buffer, frames, &offset, &l1, &l2);
  if (write_avail < frames) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                         "Failed to get write slices from planar ring buffer");
    return false;
  }

  size_t bps = sample_format_bytes_per_sample(fmt);
  if (bps == 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                         "Invalid sample format");
    return false;
  }

  for (size_t c = 0; c < channels; c++) {
    uint8_t *chan_storage =
        spsc_planar_ring_buffer_get_channel_ptr(ring_buffer, c);
    if (!chan_storage) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                           "Failed to access planar channel buffer");
      return false;
    }
    if (l1 > 0) {
      if (!audio_chunk_encode_channel(chunk, fmt, l1,
                                      chan_storage + offset * bps, c, 0)) {
        if (err)
          backend_error_init(
              err, BACKEND_ERROR_WRITE_ERROR,
              "Failed to encode audio samples into planar buffer");
        return false;
      }
    }
    if (l2 > 0) {
      if (!audio_chunk_encode_channel(chunk, fmt, l2, chan_storage, c, l1)) {
        if (err)
          backend_error_init(
              err, BACKEND_ERROR_WRITE_ERROR,
              "Failed to encode audio samples into planar buffer");
        return false;
      }
    }
  }

  spsc_planar_ring_buffer_advance_write(ring_buffer, frames);
  return true;
}

/* --- Layout-Agnostic Chunk Transfer & Control Operations --- */

bool backend_buffer_write_chunk(backend_buffer_t *bb,
                                const audio_chunk_t *chunk, uint32_t sleep_ms,
                                uint32_t max_retries, backend_error_t *err) {
  if (!bb) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_WRITE_ERROR, "Null backend buffer");
    return false;
  }
  if (bb->type == BACKEND_BUFFER_PLANAR) {
    return backend_buffer_planar_write(
        bb->planar_ring, chunk, bb->format, bb->channels, sleep_ms, max_retries,
        bb->thread_running, bb->stopped, bb->is_paused,
        bb->has_pending_rate_change, err);
  } else {
    return backend_buffer_write(bb->byte_ring, bb->blockalign, chunk,
                                bb->format, bb->channels, sleep_ms, max_retries,
                                bb->thread_running, bb->stopped, bb->is_paused,
                                bb->has_pending_rate_change, err);
  }
}

bool backend_buffer_read_chunk(backend_buffer_t *bb, size_t frames_requested,
                               audio_chunk_t *chunk, backend_error_t *err) {
  if (!bb) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_READ_ERROR, "Null backend buffer");
    return false;
  }
  if (bb->type == BACKEND_BUFFER_PLANAR) {
    return backend_buffer_planar_read(bb->planar_ring, frames_requested,
                                      bb->format, bb->channels,
                                      bb->thread_running, bb->stopped,
                                      bb->has_pending_rate_change, chunk, err);
  } else {
    return backend_buffer_read(bb->byte_ring, bb->blockalign, frames_requested,
                               bb->format, bb->channels, bb->thread_running,
                               bb->stopped, bb->has_pending_rate_change, chunk,
                               err);
  }
}

void backend_buffer_prefill_silence(backend_buffer_t *bb, size_t frames,
                                    uint8_t silence_byte) {
  if (!bb)
    return;
  if (bb->type == BACKEND_BUFFER_PLANAR) {
    backend_buffer_prefill_planar(bb, bb->planar_ring, frames);
  } else {
    backend_buffer_prefill_byte(bb, bb->byte_ring, frames, bb->blockalign,
                                silence_byte);
  }
}

size_t backend_buffer_get_level(const backend_buffer_t *bb) {
  if (!bb)
    return 0;
  if (bb->type == BACKEND_BUFFER_PLANAR) {
    return backend_buffer_planar_level(bb, bb->planar_ring);
  } else {
    return backend_buffer_level(bb, bb->byte_ring, bb->blockalign);
  }
}

size_t backend_buffer_render(backend_buffer_t *bb, void *dst, size_t frames,
                             uint8_t silence_byte) {
  if (!bb || !dst || frames == 0)
    return 0;
  if (bb->type == BACKEND_BUFFER_PLANAR) {
    return backend_buffer_render_planar(bb, bb->planar_ring, (void *const *)dst,
                                        frames, silence_byte);
  } else {
    return backend_buffer_render_byte(bb, bb->byte_ring, dst, frames,
                                      bb->blockalign, silence_byte);
  }
}

size_t backend_buffer_push(backend_buffer_t *bb, const void *src,
                           size_t frames) {
  if (!bb || !src || frames == 0)
    return 0;
  if (bb->type == BACKEND_BUFFER_PLANAR) {
    return spsc_planar_ring_buffer_write_channels(
        bb->planar_ring, (const void *const *)src, frames);
  } else {
    if (bb->blockalign == 0)
      return 0;
    size_t bytes = frames * bb->blockalign;
    size_t written_bytes =
        spsc_byte_ring_buffer_write(bb->byte_ring, (const uint8_t *)src, bytes);
    return written_bytes / bb->blockalign;
  }
}

size_t backend_buffer_consume(backend_buffer_t *bb, void *dst, size_t frames) {
  if (!bb || !dst || frames == 0)
    return 0;
  if (bb->type == BACKEND_BUFFER_PLANAR) {
    return spsc_planar_ring_buffer_read_channels(bb->planar_ring,
                                                 (void *const *)dst, frames);
  } else {
    if (bb->blockalign == 0)
      return 0;
    size_t bytes = frames * bb->blockalign;
    size_t consumed_bytes =
        spsc_byte_ring_buffer_consume(bb->byte_ring, (uint8_t *)dst, bytes);
    return consumed_bytes / bb->blockalign;
  }
}

size_t backend_buffer_get_available_read_frames(const backend_buffer_t *bb) {
  if (!bb)
    return 0;
  if (bb->type == BACKEND_BUFFER_PLANAR) {
    return bb->planar_ring
               ? spsc_planar_ring_buffer_get_available_to_read(bb->planar_ring)
               : 0;
  } else {
    if (!bb->byte_ring || bb->blockalign == 0)
      return 0;
    return spsc_byte_ring_buffer_get_available_to_read(bb->byte_ring) /
           bb->blockalign;
  }
}

size_t backend_buffer_get_available_write_frames(const backend_buffer_t *bb) {
  if (!bb)
    return 0;
  if (bb->type == BACKEND_BUFFER_PLANAR) {
    return bb->planar_ring
               ? spsc_planar_ring_buffer_get_available_to_write(bb->planar_ring)
               : 0;
  } else {
    if (!bb->byte_ring || bb->blockalign == 0)
      return 0;
    return spsc_byte_ring_buffer_get_available_to_write(bb->byte_ring) /
           bb->blockalign;
  }
}

void backend_buffer_drain(backend_buffer_t *bb) {
  if (!bb)
    return;
  if (bb->type == BACKEND_BUFFER_PLANAR) {
    if (bb->planar_ring) {
      spsc_planar_ring_buffer_drain(bb->planar_ring);
    }
  } else {
    if (bb->byte_ring) {
      spsc_byte_ring_buffer_drain(bb->byte_ring);
    }
  }
}
