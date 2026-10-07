#include "backend/backend_buffer.h"
#include "backend/backend_clip.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "audio/processing_parameters.h"
#include "engine/cdsp_sem.h"
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

  // Underrun/overflow log episode state. Touched only by the device callback
  // thread (render for playback, push for capture), so plain bools suffice.
  // Like upstream, each episode is warned once and per-callback detail goes
  // to trace, so a starved stream cannot flood the log from the RT thread.
  bool render_playing;     ///< Audio has been rendered since the last underrun.
  bool render_interrupted; ///< An underrun was reported and not yet recovered.
  bool capture_ring_full;  ///< Capture overflow episode in progress.
  bool write_ring_full;    ///< Playback drop-on-full episode (writer thread).

  // Stream lifecycle state & events
  _Atomic backend_stream_state_t state;
  _Atomic bool has_pending_rate_change;

  // Synchronization semaphore for data readiness and lifecycle wakeups
  cdsp_sem_t semaphore;

  // Pre-allocated staging buffer for straddling split frames at ring wrap
  uint8_t *split_frame_buf;

  // Processing parameters telemetry sink (optional)
  processing_parameters_t *processing_params;
  uint64_t last_clip_warn_ns; ///< Rate limit for the clipping warning.
};

/* --- Lifecycle Management --- */

backend_buffer_t *backend_buffer_create(size_t capacity_frames,
                                        binary_sample_format_t format,
                                        size_t channels, double sample_rate,
                                        bool is_planar,
                                        processing_parameters_t *params) {
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
  bb->processing_params = params;

  device_buffer_estimator_init(&bb->device, sample_rate);
  atomic_init(&bb->target_level, 0);
  atomic_init(&bb->silence_to_insert, 0);
  atomic_init(&bb->cushion_active, true);

  atomic_init(&bb->state, BACKEND_STREAM_RUNNING);
  atomic_init(&bb->has_pending_rate_change, false);

  bb->semaphore = cdsp_sem_create();
  if (!bb->semaphore) {
    free(bb);
    return NULL;
  }

  if (is_planar) {
    bb->planar_ring = spsc_planar_ring_buffer_create(
        channels, bb->bytes_per_sample, capacity_frames);
    if (!bb->planar_ring) {
      cdsp_sem_destroy(bb->semaphore);
      free(bb);
      return NULL;
    }
  } else {
    if (bb->blockalign != 0 && capacity_frames > SIZE_MAX / bb->blockalign) {
      cdsp_sem_destroy(bb->semaphore); // capacity overflow (F15)
      free(bb);
      return NULL;
    }
    size_t capacity_bytes = capacity_frames * bb->blockalign;
    bb->byte_ring = spsc_byte_ring_buffer_create(capacity_bytes);
    if (!bb->byte_ring) {
      cdsp_sem_destroy(bb->semaphore);
      free(bb);
      return NULL;
    }
    bb->split_frame_buf = (uint8_t *)calloc(1, bb->blockalign);
    if (!bb->split_frame_buf) {
      spsc_byte_ring_buffer_free(bb->byte_ring);
      cdsp_sem_destroy(bb->semaphore);
      free(bb);
      return NULL;
    }
  }
  return bb;
}

void backend_buffer_free(backend_buffer_t *bb) {
  if (!bb)
    return;
  if (bb->split_frame_buf) {
    free(bb->split_frame_buf);
    bb->split_frame_buf = NULL;
  }
  if (bb->semaphore) {
    cdsp_sem_destroy(bb->semaphore);
    bb->semaphore = NULL;
  }
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
    if (state == BACKEND_STREAM_STOPPED && bb->semaphore) {
      cdsp_sem_signal(bb->semaphore);
    }
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
                                          size_t frames, uint8_t silence_byte) {
  if (!bb)
    return;
  // Ring silence first, callback-shared state second (F9): the device may
  // already be running. If the old order (flags first) let the callback
  // underrun in between, it re-armed the cushion after cushion_active was set
  // and the cushion was inserted on top of the prefill (twice). With the
  // ring filled first an underrun in the window requires the callback to
  // have consumed the whole prefill, and the stores below then simply
  // declare the single cushion that is in the ring.
  if (ring && frames > 0) {
    // Native DSD needs its idle pattern (0x69); all-zero DSD is not silent.
    spsc_planar_ring_buffer_write_silence_byte(ring, frames, silence_byte);
  }
  atomic_store_explicit(&bb->target_level, frames, memory_order_release);
  atomic_store_explicit(&bb->silence_to_insert, 0, memory_order_release);
  atomic_store_explicit(&bb->cushion_active, true, memory_order_release);
}

static void backend_buffer_prefill_byte(backend_buffer_t *bb,
                                        spsc_byte_ring_buffer_t *ring,
                                        size_t frames, size_t blockalign,
                                        uint8_t silence_byte) {
  if (!bb)
    return;
  // Same ordering rationale as backend_buffer_prefill_planar (F9).
  if (ring && frames > 0 && blockalign > 0) {
    spsc_byte_ring_buffer_write_silence(ring, frames * blockalign,
                                        silence_byte);
  }
  atomic_store_explicit(&bb->target_level, frames, memory_order_release);
  atomic_store_explicit(&bb->silence_to_insert, 0, memory_order_release);
  atomic_store_explicit(&bb->cushion_active, true, memory_order_release);
}

/**
 * Log playback underrun episodes like upstream (coreaudio device.rs:600-622):
 * one warning when a playing stream runs dry, one info when it restarts, and
 * per-callback detail at trace level only. Runs on the device callback thread;
 * the logger is lock-free.
 */
static void backend_buffer_report_render(backend_buffer_t *bb, bool rearmed,
                                         bool was_active, size_t consumed,
                                         size_t audio_needed) {
  if (rearmed && bb->render_interrupted) {
    logger_info(&g_logger, "Restarting playback after buffer underrun.");
    bb->render_interrupted = false;
  }
  if (consumed > 0)
    bb->render_playing = true;
  if (consumed < audio_needed) {
    if (was_active && bb->render_playing) {
      logger_warn(&g_logger, "Playback interrupted, no data available.");
      bb->render_interrupted = true;
      bb->render_playing = false;
    }
    logger_trace(&g_logger,
                 "Playback callback underrun, padded %zu frames with silence",
                 audio_needed - consumed);
  }
}

static size_t backend_buffer_render_planar(backend_buffer_t *bb,
                                           spsc_planar_ring_buffer_t *ring,
                                           void *const *dst_channels,
                                           size_t frames,
                                           uint8_t silence_byte) {
  if (!bb || !ring || !dst_channels || frames == 0)
    return 0;

  bool rearmed = false;
  if (!atomic_load_explicit(&bb->cushion_active, memory_order_relaxed)) {
    size_t avail = spsc_planar_ring_buffer_get_available_to_read(ring);
    if (avail > 0) {
      atomic_store_explicit(&bb->cushion_active, true, memory_order_relaxed);
      atomic_store_explicit(
          &bb->silence_to_insert,
          atomic_load_explicit(&bb->target_level, memory_order_relaxed),
          memory_order_relaxed);
      rearmed = true;
    }
  }

  bool was_active =
      atomic_load_explicit(&bb->cushion_active, memory_order_relaxed);
  size_t silence_pending =
      atomic_load_explicit(&bb->silence_to_insert, memory_order_relaxed);
  size_t intentional_silence =
      (silence_pending < frames) ? silence_pending : frames;
  size_t audio_needed = frames - intentional_silence;

  size_t consumed = spsc_planar_ring_buffer_read_with_silence(
      ring, dst_channels, frames, silence_byte, &bb->silence_to_insert,
      &bb->cushion_active);

  backend_buffer_report_render(bb, rearmed, was_active, consumed, audio_needed);

  backend_buffer_publish(
      bb, atomic_load_explicit(&bb->silence_to_insert, memory_order_relaxed));
  if (consumed > 0 && bb->semaphore) {
    cdsp_sem_signal(bb->semaphore);
  }
  return consumed;
}

static size_t backend_buffer_render_byte(backend_buffer_t *bb,
                                         spsc_byte_ring_buffer_t *ring,
                                         void *dst, size_t frames,
                                         size_t blockalign,
                                         uint8_t silence_byte) {
  if (!bb || !ring || !dst || frames == 0 || blockalign == 0)
    return 0;

  bool rearmed = false;
  if (!atomic_load_explicit(&bb->cushion_active, memory_order_relaxed)) {
    size_t avail_bytes = spsc_byte_ring_buffer_get_available_to_read(ring);
    size_t avail_frames = avail_bytes / blockalign;
    if (avail_frames > 0) {
      atomic_store_explicit(&bb->cushion_active, true, memory_order_relaxed);
      atomic_store_explicit(
          &bb->silence_to_insert,
          atomic_load_explicit(&bb->target_level, memory_order_relaxed),
          memory_order_relaxed);
      rearmed = true;
    }
  }

  bool was_active =
      atomic_load_explicit(&bb->cushion_active, memory_order_relaxed);
  size_t silence_pending =
      atomic_load_explicit(&bb->silence_to_insert, memory_order_relaxed);
  size_t intentional_silence =
      (silence_pending < frames) ? silence_pending : frames;
  size_t audio_needed = frames - intentional_silence;

  size_t consumed = spsc_byte_ring_buffer_read_with_silence(
      ring, dst, frames, blockalign, silence_byte, &bb->silence_to_insert,
      &bb->cushion_active);

  backend_buffer_report_render(bb, rearmed, was_active, consumed, audio_needed);

  backend_buffer_publish(
      bb, atomic_load_explicit(&bb->silence_to_insert, memory_order_relaxed));
  if (consumed > 0 && bb->semaphore) {
    cdsp_sem_signal(bb->semaphore);
  }
  return consumed;
}

/* --- Writer Backoff (engine thread only) --- */

typedef enum {
  BACKEND_BUFFER_SPACE_OK = 0, ///< Enough room for the whole chunk.
  BACKEND_BUFFER_SPACE_FULL,   ///< Still full when the backoff budget ran out.
  BACKEND_BUFFER_SPACE_PAUSED, ///< Stream paused; caller drops silently.
  BACKEND_BUFFER_SPACE_ERROR,  ///< Stopped or format change; err is set.
} backend_buffer_space_t;

/**
 * Wait until @p frames fit in the ring, for at most max_retries * sleep_ms.
 *
 * The budget is a time deadline, not a count of waits. The semaphore is a
 * counting one that every render posts, also while the writer is not waiting,
 * so a count of waits would be used up instantly by stale posts and the chunk
 * dropped long before the device had a chance to drain (upstream
 * RingBufferFeeder sleeps a fixed interval per retry, ringbuffer.rs:47-52).
 * Stale posts now only cost a re-check of the free space.
 */
static backend_buffer_space_t
backend_buffer_wait_for_space(backend_buffer_t *bb, size_t frames,
                              uint32_t sleep_ms, uint32_t max_retries,
                              backend_error_t *err) {
  if (backend_buffer_get_available_write_frames(bb) >= frames)
    return BACKEND_BUFFER_SPACE_OK;

  uint32_t retries = (max_retries > 0) ? max_retries : 1;
  uint32_t wait_timeout = (sleep_ms > 0) ? sleep_ms : 1;
  uint64_t budget_ms = (uint64_t)retries * (uint64_t)wait_timeout;
  uint64_t deadline_ns = cdsp_time_now_ns() + budget_ms * 1000000ULL;

  for (;;) {
    if (atomic_load_explicit(&bb->has_pending_rate_change,
                             memory_order_acquire)) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_NONE, "Format change pending");
      return BACKEND_BUFFER_SPACE_ERROR;
    }
    backend_stream_state_t state = backend_buffer_get_state(bb);
    if (state == BACKEND_STREAM_STOPPED) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                           "Playback stream stopped");
      return BACKEND_BUFFER_SPACE_ERROR;
    }
    if (state == BACKEND_STREAM_PAUSED)
      return BACKEND_BUFFER_SPACE_PAUSED;

    uint64_t now_ns = cdsp_time_now_ns();
    if (now_ns >= deadline_ns)
      break;
    uint64_t remaining_ms = (deadline_ns - now_ns + 999999ULL) / 1000000ULL;
    uint32_t wait_ms =
        remaining_ms < wait_timeout ? (uint32_t)remaining_ms : wait_timeout;
    if (bb->semaphore) {
      cdsp_sem_timedwait(bb->semaphore, wait_ms);
    } else {
      cdsp_sleep_ms(wait_ms);
    }
    if (backend_buffer_get_available_write_frames(bb) >= frames)
      return BACKEND_BUFFER_SPACE_OK;
  }
  return backend_buffer_get_available_write_frames(bb) >= frames
             ? BACKEND_BUFFER_SPACE_OK
             : BACKEND_BUFFER_SPACE_FULL;
}

/// Drop-on-full logging: warn once per episode, trace per chunk (upstream
/// ringbuffer.rs:53-66).
static void backend_buffer_report_drop(backend_buffer_t *bb, size_t amount,
                                       const char *unit) {
  if (!bb->write_ring_full) {
    logger_warn(&g_logger, "Playback ring buffer is full, dropping chunks");
    bb->write_ring_full = true;
  }
  logger_trace(&g_logger,
               "Playback ring buffer is full, dropped chunk of %zu %s", amount,
               unit);
}

/**
 * Scan an output chunk for clipped samples (outside [-1.0, 1.0)), which cannot
 * be represented in fixed-point integers. Matching upstream CamillaDSP,
 * clipping is only counted for integer playback formats (float formats keep
 * headroom), only for the channels actually encoded, and only for chunks that
 * are really written: upstream counts inside the conversion, so paused,
 * stopped or dropped chunks do not contribute.
 */
static void backend_buffer_count_clipped(backend_buffer_t *bb,
                                         const audio_chunk_t *chunk) {
  if (bb->processing_params && !sample_format_is_float(bb->format) &&
      !sample_format_is_dsd(bb->format)) {
    backend_count_clipped_channels(bb->processing_params, chunk, bb->channels,
                                   &bb->last_clip_warn_ns, &g_logger);
  }
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

  if (audio_chunk_get_channels(chunk) < bb->channels) {
    if (err)
      backend_error_init(
          err, BACKEND_ERROR_INVALID_CHANNELS,
          "Chunk channels count does not match capture channels");
    return false;
  }

  size_t chunk_capacity = audio_chunk_get_frames(chunk);
  if (frames_requested > chunk_capacity) {
    frames_requested = chunk_capacity;
  }

  if (frames_requested == 0) {
    audio_chunk_set_valid_frames(chunk, 0);
    return true;
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
    // Unreachable while the request is a whole number of frames and the
    // slices cover it, but fail loudly instead of reporting frames that were
    // never decoded (F15).
    if (!s2 || l2 < rem_l2 || !bb->split_frame_buf) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                           "Ring buffer slices do not cover a split frame");
      return false;
    }
    if (s2 && l2 >= rem_l2) {
      if (bb->split_frame_buf) {
        memcpy(bb->split_frame_buf, s1 + f1 * bb->blockalign, rem_l1);
        memcpy(bb->split_frame_buf + rem_l1, s2, rem_l2);
        if (!audio_chunk_decode_interleaved_offset(
                bb->split_frame_buf, bb->format, bb->channels, 1, chunk, f1)) {
          if (err)
            backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                               "Failed to decode split audio frame");
          return false;
        }
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

static bool backend_buffer_write(backend_buffer_t *bb,
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

  switch (
      backend_buffer_wait_for_space(bb, frames, sleep_ms, max_retries, err)) {
  case BACKEND_BUFFER_SPACE_OK:
    backend_buffer_count_clipped(bb, chunk);
    break;
  case BACKEND_BUFFER_SPACE_PAUSED:
    return true;
  case BACKEND_BUFFER_SPACE_ERROR:
    return false;
  case BACKEND_BUFFER_SPACE_FULL:
    // Audio chunks must be written as atomic units. If the ring buffer cannot
    // fit the complete chunk after backoff, drop the entire chunk rather than
    // pushing a fractured sub-chunk (waveform discontinuity).
    backend_buffer_report_drop(bb, bytes_to_write, "bytes");
    if (err)
      backend_error_init(err, BACKEND_ERROR_NONE, "");
    return true;
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
    // Unreachable while the request is a whole number of frames and the
    // slices cover it, but fail loudly instead of reporting frames that were
    // never encoded (F15).
    if (!s2 || l2 < rem_l2 || !bb->split_frame_buf) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                           "Ring buffer slices do not cover a split frame");
      return false;
    }
    if (s2 && l2 >= rem_l2) {
      if (bb->split_frame_buf) {
        if (!audio_chunk_encode_interleaved_offset(
                chunk, bb->format, bb->channels, 1, bb->split_frame_buf, f1)) {
          if (err)
            backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                               "Failed to encode split audio frame");
          return false;
        }
        memcpy(s1 + f1 * bb->blockalign, bb->split_frame_buf, rem_l1);
        memcpy(s2, bb->split_frame_buf + rem_l1, rem_l2);
      }
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
  bb->write_ring_full = false;
  return true;
}

static bool backend_buffer_planar_read(const backend_buffer_t *bb,
                                       size_t frames_requested,
                                       audio_chunk_t *chunk,
                                       backend_error_t *err) {
  if (!bb || !chunk || bb->channels == 0) {
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

  if (audio_chunk_get_channels(chunk) < bb->channels) {
    if (err)
      backend_error_init(
          err, BACKEND_ERROR_INVALID_CHANNELS,
          "Chunk channels count does not match capture channels");
    return false;
  }

  size_t chunk_capacity = audio_chunk_get_frames(chunk);
  if (frames_requested > chunk_capacity) {
    frames_requested = chunk_capacity;
  }

  if (frames_requested == 0) {
    audio_chunk_set_valid_frames(chunk, 0);
    return true;
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

static bool backend_buffer_planar_write(backend_buffer_t *bb,
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

  switch (
      backend_buffer_wait_for_space(bb, frames, sleep_ms, max_retries, err)) {
  case BACKEND_BUFFER_SPACE_OK:
    backend_buffer_count_clipped(bb, chunk);
    break;
  case BACKEND_BUFFER_SPACE_PAUSED:
    return true;
  case BACKEND_BUFFER_SPACE_ERROR:
    return false;
  case BACKEND_BUFFER_SPACE_FULL:
    // Audio chunks must be written as atomic units. If the ring buffer cannot
    // fit the complete chunk after backoff, drop the entire chunk rather than
    // pushing a fractured sub-chunk (waveform discontinuity).
    backend_buffer_report_drop(bb, frames, "frames");
    if (err)
      backend_error_init(err, BACKEND_ERROR_NONE, "");
    return true;
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
  bb->write_ring_full = false;
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
    backend_buffer_prefill_planar(bb, bb->planar_ring, frames, silence_byte);
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

/**
 * Capture overflow logging, shared by push and commit_write: one warning per
 * episode, trace per callback (upstream coreaudio device.rs:898-911, asio
 * device.rs:449-460, alsa threaded_device.rs:1589-1603). Producer-only state.
 */
static void backend_buffer_report_capture_overflow(backend_buffer_t *bb,
                                                   size_t dropped,
                                                   size_t total) {
  if (dropped > 0) {
    if (!bb->capture_ring_full) {
      logger_warn(&g_logger, "Capture ring buffer is full, dropping samples");
      bb->capture_ring_full = true;
    }
    logger_trace(&g_logger,
                 "Capture ring buffer is full, dropped %zu out of %zu frames",
                 dropped, total);
  } else {
    bb->capture_ring_full = false;
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
    // Push whole frames only. With formats where sample size is not a power of
    // 2 (e.g. S24_3), a partial byte push could end inside a frame and shift
    // every later sample.
    size_t avail_bytes =
        spsc_byte_ring_buffer_get_available_to_write(bb->byte_ring);
    size_t whole_frames = avail_bytes / bb->blockalign;
    if (whole_frames > frames) {
      whole_frames = frames;
    }
    size_t bytes_to_write = whole_frames * bb->blockalign;
    size_t written_bytes = 0;
    if (bytes_to_write > 0) {
      written_bytes = spsc_byte_ring_buffer_write(
          bb->byte_ring, (const uint8_t *)src, bytes_to_write);
    }
    pushed = written_bytes / bb->blockalign;
  }
  backend_buffer_report_capture_overflow(bb, frames - pushed, frames);
  if (pushed > 0 && bb->semaphore) {
    cdsp_sem_signal(bb->semaphore);
  }
  return pushed;
}

size_t backend_buffer_consume(backend_buffer_t *bb, void *dst, size_t frames) {
  if (!bb || !dst || frames == 0)
    return 0;
  size_t consumed = 0;
  if (bb->type == BACKEND_BUFFER_PLANAR) {
    consumed = spsc_planar_ring_buffer_read_channels(
        bb->planar_ring, (void *const *)dst, frames);
  } else {
    if (bb->blockalign == 0)
      return 0;
    size_t bytes = frames * bb->blockalign;
    size_t consumed_bytes =
        spsc_byte_ring_buffer_consume(bb->byte_ring, (uint8_t *)dst, bytes);
    consumed = consumed_bytes / bb->blockalign;
  }
  if (consumed > 0 && bb->semaphore) {
    cdsp_sem_signal(bb->semaphore);
  }
  return consumed;
}

/* Whole-frame view of two byte slices (see backend_buffer_get_read_slices). */
static size_t backend_buffer_frame_slices(const backend_buffer_t *bb, size_t l1,
                                          size_t l2, size_t *n1, size_t *n2) {
  *n1 = l1 / bb->blockalign;
  // The second slice starts at storage[0]; it continues the first one only if
  // the first ends exactly on a frame boundary.
  *n2 = (l1 % bb->blockalign == 0) ? l2 / bb->blockalign : 0;
  return *n1 + *n2;
}

size_t backend_buffer_get_read_slices(backend_buffer_t *bb, size_t max_frames,
                                      const void **p1, size_t *n1,
                                      const void **p2, size_t *n2) {
  if (p1)
    *p1 = NULL;
  if (p2)
    *p2 = NULL;
  if (n1)
    *n1 = 0;
  if (n2)
    *n2 = 0;
  if (!bb || !p1 || !p2 || !n1 || !n2 || bb->type == BACKEND_BUFFER_PLANAR ||
      !bb->byte_ring || bb->blockalign == 0 || max_frames == 0 ||
      max_frames > SIZE_MAX / bb->blockalign)
    return 0;
  const uint8_t *s1 = NULL, *s2 = NULL;
  size_t l1 = 0, l2 = 0;
  spsc_byte_ring_buffer_get_read_slices(
      bb->byte_ring, max_frames * bb->blockalign, &s1, &l1, &s2, &l2);
  size_t total = backend_buffer_frame_slices(bb, l1, l2, n1, n2);
  *p1 = *n1 ? s1 : NULL;
  *p2 = *n2 ? s2 : NULL;
  return total;
}

void backend_buffer_commit_read(backend_buffer_t *bb, size_t frames) {
  if (!bb || frames == 0 || bb->type == BACKEND_BUFFER_PLANAR ||
      !bb->byte_ring || bb->blockalign == 0)
    return;
  spsc_byte_ring_buffer_advance_read(bb->byte_ring, frames * bb->blockalign);
  if (bb->semaphore)
    cdsp_sem_signal(bb->semaphore);
}

size_t backend_buffer_get_write_slices(backend_buffer_t *bb, size_t max_frames,
                                       void **p1, size_t *n1, void **p2,
                                       size_t *n2) {
  if (p1)
    *p1 = NULL;
  if (p2)
    *p2 = NULL;
  if (n1)
    *n1 = 0;
  if (n2)
    *n2 = 0;
  if (!bb || !p1 || !p2 || !n1 || !n2 || bb->type == BACKEND_BUFFER_PLANAR ||
      !bb->byte_ring || bb->blockalign == 0 || max_frames == 0 ||
      max_frames > SIZE_MAX / bb->blockalign)
    return 0;
  uint8_t *s1 = NULL, *s2 = NULL;
  size_t l1 = 0, l2 = 0;
  spsc_byte_ring_buffer_get_write_slices(
      bb->byte_ring, max_frames * bb->blockalign, &s1, &l1, &s2, &l2);
  size_t total = backend_buffer_frame_slices(bb, l1, l2, n1, n2);
  *p1 = *n1 ? s1 : NULL;
  *p2 = *n2 ? s2 : NULL;
  return total;
}

void backend_buffer_commit_write(backend_buffer_t *bb, size_t frames,
                                 size_t frames_dropped) {
  if (!bb || bb->type == BACKEND_BUFFER_PLANAR || !bb->byte_ring ||
      bb->blockalign == 0)
    return;
  if (frames > 0)
    spsc_byte_ring_buffer_advance_write(bb->byte_ring, frames * bb->blockalign);
  backend_buffer_report_capture_overflow(bb, frames_dropped,
                                         frames + frames_dropped);
  if (frames > 0 && bb->semaphore)
    cdsp_sem_signal(bb->semaphore);
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

/* --- Thread Synchronization --- */

void backend_buffer_signal(backend_buffer_t *bb) {
  if (bb && bb->semaphore) {
    cdsp_sem_signal(bb->semaphore);
  }
}

bool backend_buffer_wait(backend_buffer_t *bb, uint32_t timeout_ms) {
  if (!bb || !bb->semaphore) {
    return false;
  }
  if (backend_buffer_get_state(bb) == BACKEND_STREAM_STOPPED) {
    return false;
  }
  return cdsp_sem_timedwait(bb->semaphore, timeout_ms);
}
