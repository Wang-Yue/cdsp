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
  _Atomic bool cushion_active;

  // Stream lifecycle state & events
  _Atomic backend_stream_state_t state;
  _Atomic bool has_pending_rate_change;
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
  atomic_init(&bb->cushion_active, true);

  atomic_init(&bb->state, BACKEND_STREAM_RUNNING);
  atomic_init(&bb->has_pending_rate_change, false);

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

/* --- Stream Lifecycle & State Control --- */

backend_stream_state_t backend_buffer_get_state(const backend_buffer_t *bb) {
  return bb ? atomic_load_explicit(&bb->state, memory_order_acquire)
            : BACKEND_STREAM_STOPPED;
}

void backend_buffer_set_state(backend_buffer_t *bb,
                              backend_stream_state_t state) {
  if (bb) {
    atomic_store_explicit(&bb->state, state, memory_order_release);
  }
}

bool backend_buffer_has_pending_rate_change(const backend_buffer_t *bb) {
  return bb ? atomic_load_explicit(&bb->has_pending_rate_change,
                                   memory_order_acquire)
            : false;
}

void backend_buffer_set_pending_rate_change(backend_buffer_t *bb,
                                            bool pending) {
  if (bb) {
    atomic_store_explicit(&bb->has_pending_rate_change, pending,
                          memory_order_release);
  }
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
  atomic_store_explicit(&bb->cushion_active, true, memory_order_release);
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
  atomic_store_explicit(&bb->cushion_active, true, memory_order_release);
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

  if (!atomic_load_explicit(&bb->cushion_active, memory_order_relaxed)) {
    size_t avail = spsc_planar_ring_buffer_get_available_to_read(ring);
    if (avail > 0) {
      atomic_store_explicit(&bb->cushion_active, true, memory_order_relaxed);
      atomic_store_explicit(
          &bb->silence_to_insert,
          atomic_load_explicit(&bb->target_level, memory_order_relaxed),
          memory_order_relaxed);
    }
  }

  size_t silence_pending =
      atomic_load_explicit(&bb->silence_to_insert, memory_order_relaxed);
  size_t intentional_silence =
      (silence_pending < frames) ? silence_pending : frames;
  size_t audio_needed = frames - intentional_silence;

  size_t consumed = spsc_planar_ring_buffer_read_with_silence(
      ring, dst_channels, frames, silence_byte, &bb->silence_to_insert,
      &bb->cushion_active);

  if (consumed < audio_needed) {
    logger_warn(
        &g_logger,
        "Playback buffer underrun: padded %zu missing frames with silence",
        audio_needed - consumed);
  }

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

  if (!atomic_load_explicit(&bb->cushion_active, memory_order_relaxed)) {
    size_t avail_bytes = spsc_byte_ring_buffer_get_available_to_read(ring);
    size_t avail_frames = avail_bytes / blockalign;
    if (avail_frames > 0) {
      atomic_store_explicit(&bb->cushion_active, true, memory_order_relaxed);
      atomic_store_explicit(
          &bb->silence_to_insert,
          atomic_load_explicit(&bb->target_level, memory_order_relaxed),
          memory_order_relaxed);
    }
  }

  size_t silence_pending =
      atomic_load_explicit(&bb->silence_to_insert, memory_order_relaxed);
  size_t intentional_silence =
      (silence_pending < frames) ? silence_pending : frames;
  size_t audio_needed = frames - intentional_silence;

  size_t consumed = spsc_byte_ring_buffer_read_with_silence(
      ring, dst, frames, blockalign, silence_byte, &bb->silence_to_insert,
      &bb->cushion_active);

  if (consumed < audio_needed) {
    logger_warn(
        &g_logger,
        "Playback buffer underrun: padded %zu missing frames with silence",
        audio_needed - consumed);
  }

  backend_buffer_publish(
      bb, atomic_load_explicit(&bb->silence_to_insert, memory_order_relaxed));
  return consumed;
}

/* --- Engine-Side Audio Chunk Ring Buffer IO (Capture & Playback) --- */

static bool backend_buffer_read(const backend_buffer_t *bb,
                                size_t frames_requested, audio_chunk_t *chunk,
                                backend_error_t *err) {
  if (!bb || !chunk || bb->channels == 0 || bb->blockalign == 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_READ_ERROR, "Invalid parameters");
    return false;
  }

  // Hardware sample rate or format changes must be handled immediately without
  // decoding further frames under obsolete parameters to prevent filter
  // corruption.
  if (atomic_load_explicit(&bb->has_pending_rate_change,
                           memory_order_acquire)) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_NONE, "Format change pending");
    return false;
  }

  if (frames_requested == 0) {
    audio_chunk_set_valid_frames(chunk, 0);
    return true;
  }

  if (audio_chunk_get_channels(chunk) < bb->channels) {
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

  if (frames_requested > SIZE_MAX / bb->blockalign) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                         "Requested frame count exceeds maximum size");
    return false;
  }

  size_t bytes_requested = frames_requested * bb->blockalign;

  // Draining semantics: Only check `is_stopped` / `is_running` flags after
  // confirming that the ring buffer lacks sufficient bytes. If the capture
  // worker thread wrote its final batch of samples and stopped, those remaining
  // frames must still be consumed to avoid dropping audio at EOF / stream
  // shutdown.
  if (spsc_byte_ring_buffer_get_available_to_read(bb->byte_ring) <
      bytes_requested) {
    if (backend_buffer_get_state(bb) == BACKEND_STREAM_STOPPED) {
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
      bb->byte_ring, bytes_requested, &s1, &l1, &s2, &l2);
  if (read_avail < bytes_requested || !s1) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                         "Failed to get read slices from ring buffer");
    return false;
  }

  if (l1 % bb->blockalign == 0) {
    size_t f1 = l1 / bb->blockalign;
    if (f1 > 0) {
      if (!audio_chunk_decode_interleaved_offset(s1, bb->format, bb->channels,
                                                 f1, chunk, 0)) {
        if (err)
          backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                             "Failed to decode captured audio data");
        return false;
      }
    }
    size_t f2 = l2 / bb->blockalign;
    if (f2 > 0 && s2) {
      if (!audio_chunk_decode_interleaved_offset(s2, bb->format, bb->channels,
                                                 f2, chunk, f1)) {
        if (err)
          backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                             "Failed to decode captured audio data");
        return false;
      }
    }
  } else {
    size_t f1 = l1 / bb->blockalign;
    if (f1 > 0) {
      if (!audio_chunk_decode_interleaved_offset(s1, bb->format, bb->channels,
                                                 f1, chunk, 0)) {
        if (err)
          backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                             "Failed to decode captured audio data");
        return false;
      }
    }
    size_t rem_l1 = l1 % bb->blockalign;
    size_t rem_l2 = bb->blockalign - rem_l1;
    uint8_t split_frame[256];
    if (bb->blockalign <= sizeof(split_frame) && s2 && l2 >= rem_l2) {
      memcpy(split_frame, s1 + f1 * bb->blockalign, rem_l1);
      memcpy(split_frame + rem_l1, s2, rem_l2);
      if (!audio_chunk_decode_interleaved_offset(split_frame, bb->format,
                                                 bb->channels, 1, chunk, f1)) {
        if (err)
          backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                             "Failed to decode split audio frame");
        return false;
      }
      size_t f2 = (l2 - rem_l2) / bb->blockalign;
      if (f2 > 0) {
        if (!audio_chunk_decode_interleaved_offset(
                s2 + rem_l2, bb->format, bb->channels, f2, chunk, f1 + 1)) {
          if (err)
            backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                               "Failed to decode captured audio data");
          return false;
        }
      }
    }
  }

  spsc_byte_ring_buffer_advance_read(bb->byte_ring, bytes_requested);
  audio_chunk_set_valid_frames(chunk, frames_requested);
  return true;
}

static bool backend_buffer_write(const backend_buffer_t *bb,
                                 const audio_chunk_t *chunk, uint32_t sleep_ms,
                                 uint32_t max_retries, backend_error_t *err) {
  if (!bb || !chunk || bb->channels == 0 || bb->blockalign == 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_WRITE_ERROR, "Invalid parameters");
    return false;
  }

  backend_stream_state_t state = backend_buffer_get_state(bb);
  if (state == BACKEND_STREAM_PAUSED) {
    return true;
  }

  if (atomic_load_explicit(&bb->has_pending_rate_change,
                           memory_order_acquire)) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_NONE, "Format change pending");
    return false;
  }

  if (state == BACKEND_STREAM_STOPPED) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                         "Playback stream stopped");
    return false;
  }

  if (audio_chunk_get_channels(chunk) < bb->channels) {
    if (err)
      backend_error_init(
          err, BACKEND_ERROR_INVALID_CHANNELS,
          "Chunk channels count does not match playback channels");
    return false;
  }

  size_t frames = audio_chunk_get_valid_frames(chunk);
  if (frames == 0)
    return true;

  if (frames > SIZE_MAX / bb->blockalign) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                         "Frame count exceeds maximum size");
    return false;
  }

  size_t bytes_to_write = frames * bb->blockalign;

  uint32_t retries = (max_retries > 0) ? max_retries : 1;
  for (uint32_t retry = 0; retry < retries; retry++) {
    if (spsc_byte_ring_buffer_get_available_to_write(bb->byte_ring) >=
        bytes_to_write) {
      break;
    }
    if (atomic_load_explicit(&bb->has_pending_rate_change,
                             memory_order_acquire)) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_NONE, "Format change pending");
      return false;
    }
    state = backend_buffer_get_state(bb);
    if (state == BACKEND_STREAM_STOPPED) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                           "Playback stream stopped");
      return false;
    }
    if (state == BACKEND_STREAM_PAUSED) {
      return true;
    }
    cdsp_sleep_ms(sleep_ms > 0 ? sleep_ms : 1);
  }

  // Audio chunks must be written as atomic units. If the ring buffer cannot fit
  // the complete chunk after backoff, drop the entire chunk rather than pushing
  // a fractured sub-chunk to prevent time-domain waveform discontinuity.
  if (spsc_byte_ring_buffer_get_available_to_write(bb->byte_ring) <
      bytes_to_write) {
    logger_warn(
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
      bb->byte_ring, bytes_to_write, &s1, &l1, &s2, &l2);
  if (write_avail < bytes_to_write || !s1) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                         "Failed to get write slices from ring buffer");
    return false;
  }

  if (l1 % bb->blockalign == 0) {
    size_t f1 = l1 / bb->blockalign;
    if (f1 > 0) {
      if (!audio_chunk_encode_interleaved_offset(chunk, bb->format,
                                                 bb->channels, f1, s1, 0)) {
        if (err)
          backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                             "Failed to encode audio samples");
        return false;
      }
    }
    size_t f2 = l2 / bb->blockalign;
    if (f2 > 0 && s2) {
      if (!audio_chunk_encode_interleaved_offset(chunk, bb->format,
                                                 bb->channels, f2, s2, f1)) {
        if (err)
          backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                             "Failed to encode audio samples");
        return false;
      }
    }
  } else {
    size_t f1 = l1 / bb->blockalign;
    if (f1 > 0) {
      if (!audio_chunk_encode_interleaved_offset(chunk, bb->format,
                                                 bb->channels, f1, s1, 0)) {
        if (err)
          backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                             "Failed to encode audio samples");
        return false;
      }
    }
    size_t rem_l1 = l1 % bb->blockalign;
    size_t rem_l2 = bb->blockalign - rem_l1;
    uint8_t split_frame[256];
    if (bb->blockalign <= sizeof(split_frame) && s2 && l2 >= rem_l2) {
      if (!audio_chunk_encode_interleaved_offset(
              chunk, bb->format, bb->channels, 1, split_frame, f1)) {
        if (err)
          backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                             "Failed to encode split audio frame");
        return false;
      }
      memcpy(s1 + f1 * bb->blockalign, split_frame, rem_l1);
      memcpy(s2, split_frame + rem_l1, rem_l2);
      size_t f2 = (l2 - rem_l2) / bb->blockalign;
      if (f2 > 0) {
        if (!audio_chunk_encode_interleaved_offset(
                chunk, bb->format, bb->channels, f2, s2 + rem_l2, f1 + 1)) {
          if (err)
            backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                               "Failed to encode audio samples");
          return false;
        }
      }
    }
  }

  spsc_byte_ring_buffer_advance_write(bb->byte_ring, bytes_to_write);
  return true;
}

static bool backend_buffer_planar_read(const backend_buffer_t *bb,
                                       size_t frames_requested,
                                       audio_chunk_t *chunk,
                                       backend_error_t *err) {
  if (!bb || !chunk || bb->channels == 0 || frames_requested == 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_READ_ERROR, "Invalid parameters");
    return false;
  }

  if (atomic_load_explicit(&bb->has_pending_rate_change,
                           memory_order_acquire)) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_NONE, "Format change pending");
    return false;
  }

  if (backend_buffer_get_state(bb) == BACKEND_STREAM_STOPPED) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                         "Capture stream stopped");
    return false;
  }

  if (audio_chunk_get_channels(chunk) < bb->channels) {
    if (err)
      backend_error_init(
          err, BACKEND_ERROR_INVALID_CHANNELS,
          "Chunk channels count does not match capture channels");
    return false;
  }

  size_t available_frames =
      spsc_planar_ring_buffer_get_available_to_read(bb->planar_ring);
  if (available_frames < frames_requested) {
    if (backend_buffer_get_state(bb) == BACKEND_STREAM_STOPPED) {
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
      bb->planar_ring, frames_requested, &offset, &l1, &l2);
  if (read_avail < frames_requested) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                         "Failed to get read slices from planar ring buffer");
    return false;
  }

  size_t bps = sample_format_bytes_per_sample(bb->format);
  if (bps == 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                         "Invalid sample format");
    return false;
  }

  for (size_t c = 0; c < bb->channels; c++) {
    const uint8_t *chan_storage =
        spsc_planar_ring_buffer_get_channel_ptr(bb->planar_ring, c);
    if (!chan_storage) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                           "Failed to access planar channel buffer");
      return false;
    }
    if (l1 > 0) {
      if (!audio_chunk_decode_channel(chan_storage + offset * bps, bb->format,
                                      l1, chunk, c, 0)) {
        if (err)
          backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                             "Failed to decode captured planar audio data");
        return false;
      }
    }
    if (l2 > 0) {
      if (!audio_chunk_decode_channel(chan_storage, bb->format, l2, chunk, c,
                                      l1)) {
        if (err)
          backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                             "Failed to decode captured planar audio data");
        return false;
      }
    }
  }

  spsc_planar_ring_buffer_advance_read(bb->planar_ring, frames_requested);
  audio_chunk_set_valid_frames(chunk, frames_requested);
  return true;
}

static bool backend_buffer_planar_write(const backend_buffer_t *bb,
                                        const audio_chunk_t *chunk,
                                        uint32_t sleep_ms, uint32_t max_retries,
                                        backend_error_t *err) {
  if (!bb || !chunk || bb->channels == 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_WRITE_ERROR, "Invalid parameters");
    return false;
  }

  backend_stream_state_t state = backend_buffer_get_state(bb);
  if (state == BACKEND_STREAM_PAUSED) {
    return true;
  }

  if (atomic_load_explicit(&bb->has_pending_rate_change,
                           memory_order_acquire)) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_NONE, "Format change pending");
    return false;
  }

  if (state == BACKEND_STREAM_STOPPED) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                         "Playback stream stopped");
    return false;
  }

  if (audio_chunk_get_channels(chunk) < bb->channels) {
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
    if (spsc_planar_ring_buffer_get_available_to_write(bb->planar_ring) >=
        frames) {
      break;
    }
    if (atomic_load_explicit(&bb->has_pending_rate_change,
                             memory_order_acquire)) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_NONE, "Format change pending");
      return false;
    }
    state = backend_buffer_get_state(bb);
    if (state == BACKEND_STREAM_STOPPED) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                           "Playback stream stopped");
      return false;
    }
    if (state == BACKEND_STREAM_PAUSED) {
      return true;
    }
    cdsp_sleep_ms(sleep_ms > 0 ? sleep_ms : 1);
  }

  if (spsc_planar_ring_buffer_get_available_to_write(bb->planar_ring) <
      frames) {
    logger_warn(
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
      bb->planar_ring, frames, &offset, &l1, &l2);
  if (write_avail < frames) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                         "Failed to get write slices from planar ring buffer");
    return false;
  }

  size_t bps = sample_format_bytes_per_sample(bb->format);
  for (size_t c = 0; c < bb->channels; c++) {
    uint8_t *chan_storage =
        spsc_planar_ring_buffer_get_channel_ptr(bb->planar_ring, c);
    if (!chan_storage) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                           "Failed to access planar channel buffer");
      return false;
    }
    if (l1 > 0) {
      if (!audio_chunk_encode_channel(chunk, bb->format, l1,
                                      chan_storage + offset * bps, c, 0)) {
        if (err)
          backend_error_init(
              err, BACKEND_ERROR_WRITE_ERROR,
              "Failed to encode audio samples into planar buffer");
        return false;
      }
    }
    if (l2 > 0) {
      if (!audio_chunk_encode_channel(chunk, bb->format, l2, chan_storage, c,
                                      l1)) {
        if (err)
          backend_error_init(
              err, BACKEND_ERROR_WRITE_ERROR,
              "Failed to encode audio samples into planar buffer");
        return false;
      }
    }
  }

  spsc_planar_ring_buffer_advance_write(bb->planar_ring, frames);
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
    return backend_buffer_planar_write(bb, chunk, sleep_ms, max_retries, err);
  } else {
    return backend_buffer_write(bb, chunk, sleep_ms, max_retries, err);
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
    return backend_buffer_planar_read(bb, frames_requested, chunk, err);
  } else {
    return backend_buffer_read(bb, frames_requested, chunk, err);
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

  if (backend_buffer_get_state(bb) == BACKEND_STREAM_PAUSED) {
    if (bb->type == BACKEND_BUFFER_PLANAR) {
      void *const *dst_channels = (void *const *)dst;
      size_t byte_len = frames * bb->bytes_per_sample;
      for (size_t c = 0; c < bb->channels; c++) {
        if (dst_channels[c]) {
          memset(dst_channels[c], silence_byte, byte_len);
        }
      }
    } else {
      memset(dst, silence_byte, frames * bb->blockalign);
    }
    return 0;
  }

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
  size_t pushed = 0;
  if (bb->type == BACKEND_BUFFER_PLANAR) {
    pushed = spsc_planar_ring_buffer_write_channels(
        bb->planar_ring, (const void *const *)src, frames);
  } else {
    if (bb->blockalign == 0)
      return 0;
    size_t bytes = frames * bb->blockalign;
    size_t written_bytes =
        spsc_byte_ring_buffer_write(bb->byte_ring, (const uint8_t *)src, bytes);
    pushed = written_bytes / bb->blockalign;
  }
  if (pushed < frames) {
    logger_warn(&g_logger,
                "Capture ring buffer is full, dropped %zu out of %zu frames",
                frames - pushed, frames);
  }
  return pushed;
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
