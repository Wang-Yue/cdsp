#include "backend/alsa_playback.h"
#include "backend/backend_buffer.h"

#if defined(ENABLE_ALSA)
#include <alsa/asoundlib.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/audio_chunk.h"
#include "audio/sample_conversion.h"
#include "backend/alsa_device.h"
#include "backend/audio_backend.h"
#include "backend/backend_error.h"
#include "config/config_gen.h"
#include "engine/thread_priority.h"
#include "logging/app_logger.h"
#include "utils/cdsp_time.h"

static const logger_t g_logger = {"dsp.backend.alsa"};

struct alsa_playback {
  char device_name[256];
  int sample_rate;
  size_t channels;
  size_t chunk_size;
  size_t target_level;
  snd_pcm_uframes_t bufsize;
  snd_pcm_uframes_t period;

  bool has_format;
  alsa_sample_format_t requested_format;
  processing_parameters_t *params;

  snd_pcm_t *pcm;
  snd_pcm_format_t format;
  bool can_pause;
  bool currently_paused;
  bool device_stalled;

  size_t blockalign;
  void *zero_stall_buf;
  size_t zero_stall_buf_size;
  uint8_t *local_buf;
  size_t local_buf_bytes;

  // Inner thread only: set once the first audio has been taken from the
  // ring. Before that a PREPARED device with an empty ring just waits for the
  // engine's prefill/first chunk instead of being fed keep-alive silence (L1).
  bool has_started;

  snd_hctl_t *hctl;
  snd_hctl_elem_t *hctl_pitch_elem;
  _Atomic double pending_pitch;
  _Atomic bool has_pending_pitch;

  backend_buffer_t *buffer;
  pthread_t inner_thread;
  bool inner_thread_created;
  _Atomic bool draining;

  // Set (release) by the inner thread when it exits because of a device
  // error, after fatal_msg has been written. write() reports it as
  // BACKEND_ERROR_WRITE_ERROR so the engine raises PLAYBACK_ERROR instead of
  // treating the stop as a clean end of stream (upstream sends
  // StatusMessage::PlaybackError, threaded_device.rs:288-293, 478-483).
  _Atomic bool fatal_error;
  char fatal_msg[256];

  // Set by the inner thread just before it publishes STOPPED on exit. Lets
  // set_is_paused() undo a RUNNING/PAUSED store that raced with the exit, so
  // a dead stream is never revived (M5).
  _Atomic bool inner_exited;
};

static void alsa_playback_mark_inner_exited(alsa_playback_t *playback) {
  atomic_store_explicit(&playback->inner_exited, true, memory_order_seq_cst);
  // Pairs with the fence in alsa_playback_set_is_paused(): either that call
  // observes inner_exited, or this thread's following STOPPED store is later
  // in the state's modification order than the RUNNING/PAUSED store.
  atomic_thread_fence(memory_order_seq_cst);
}

static void alsa_playback_record_fatal(alsa_playback_t *playback,
                                       const char *what, int alsa_rc) {
  if (alsa_rc != 0) {
    snprintf(playback->fatal_msg, sizeof(playback->fatal_msg), "%s: %s", what,
             snd_strerror(alsa_rc));
  } else {
    snprintf(playback->fatal_msg, sizeof(playback->fatal_msg), "%s", what);
  }
  logger_error(&g_logger, "PB: %s", playback->fatal_msg);
}

// Writes a pitch requested by set_pitch() on the inner thread, matching
// upstream'sPlaybackDeviceMessage::SetAdjustSpeed handling
// (src/alsa_backend/threaded_device.rs:308-329, 397-402) and alsa_capture.c.
static void alsa_playback_apply_pending_pitch(alsa_playback_t *playback) {
  if (!atomic_exchange_explicit(&playback->has_pending_pitch, false,
                                memory_order_acq_rel)) {
    return;
  }
  double multiplier =
      atomic_load_explicit(&playback->pending_pitch, memory_order_relaxed);
  if (!playback->hctl_pitch_elem)
    return;
  long value = (long)trunc(multiplier * 1000000.0);
  alsa_elem_write_as_int(playback->hctl_pitch_elem, value);
}

// Publishes the frames that live outside the ring: the device delay
// (bufsize - avail) plus `staged` frames moved into the one-frame wrap
// buffer local_buf but not yet written (AGENTS.md §3.1, M2). Upstream publishes
// delay + ring fill as one snapshot after each write
// (threaded_device.rs:449-460). Skipped while stalled or when the device
// cannot report avail (XRUN, SUSPENDED); the estimator then keeps decaying
// the previous value.
static void alsa_playback_publish_level(alsa_playback_t *playback,
                                        size_t staged) {
  if (playback->device_stalled)
    return;
  snd_pcm_state_t st = snd_pcm_state(playback->pcm);
  if (st != SND_PCM_STATE_RUNNING && st != SND_PCM_STATE_PREPARED)
    return;
  snd_pcm_sframes_t avail = snd_pcm_avail(playback->pcm);
  if (avail < 0 || (snd_pcm_uframes_t)avail > playback->bufsize)
    return;
  backend_buffer_publish(
      playback->buffer,
      (size_t)(playback->bufsize - (snd_pcm_uframes_t)avail) + staged);
}

// Dedicated real-time playback inner thread matching AlsaPlaybackInner in
// upstream (src/alsa_backend/threaded_device.rs:1078-1345)
static void *alsa_playback_inner_thread_func(void *arg) {
  alsa_playback_t *playback = (alsa_playback_t *)arg;
  realtime_thread_handle_t *rt_handle = promote_current_thread_to_realtime(
      "AlsaPlaybackInner", playback->chunk_size, (size_t)playback->sample_rate);
  if (rt_handle) {
    logger_debug(&g_logger, "Playback inner thread has real-time priority.");
  }

  size_t bytes_per_frame = playback->blockalign;
  // One-frame wrap-straddle buffer, pre-allocated in open() (AGENTS.md §1.1:
  // no allocation on the RT thread).
  uint8_t *local_buf = playback->local_buf;
  bool playback_interrupted = false;
  // Set by every exit path that is caused by a device error, as opposed to a
  // requested stop (state STOPPED) or the end-of-stream drain.
  bool fatal = false;
  bool pause_failed = false;

  while (backend_buffer_get_state(playback->buffer) != BACKEND_STREAM_STOPPED) {
    alsa_playback_apply_pending_pitch(playback);
    if (backend_buffer_get_state(playback->buffer) == BACKEND_STREAM_PAUSED) {
      if (playback->can_pause && !playback->currently_paused && !pause_failed) {
        // Record the pause only if it took effect (upstream:
        // threaded_device.rs:393-395). snd_pcm_pause() fails in PREPARED,
        // XRUN and similar states; then keep the device fed instead, as for
        // hardware without pause support, and do not retry every pass.
        int prc = snd_pcm_pause(playback->pcm, 1);
        if (prc >= 0) {
          playback->currently_paused = true;
        } else {
          logger_debug(&g_logger, "PB: pause failed (%s), keeping device fed",
                       snd_strerror(prc));
          pause_failed = true;
        }
      }
      if (playback->currently_paused) {
        cdsp_sleep_ms(5);
        continue;
      }
      // Hardware does not support pause (or pausing failed): fall through so
      // the keep-alive silence path keeps the device fed and prevents XRUN.
    } else {
      pause_failed = false;
      if (playback->can_pause && playback->currently_paused) {
        snd_pcm_pause(playback->pcm, 0);
        playback->currently_paused = false;
      }
    }

    size_t remainder_frames = 0;
    // Zero-copy (AGENTS.md §3.4, L4): snd_pcm_writei() reads straight from
    // the ring's first contiguous slice, and frames are released with
    // backend_buffer_commit_read() only after the device accepted them. They
    // stay in the ring until then, so the live ring term of the buffer level
    // already counts them and nothing extra is published (M2). The second
    // slice is picked up on the next pass. A frame that straddles the ring
    // wrap (non-power-of-two blockalign, e.g. S24_3) is moved into the
    // one-frame local_buf with backend_buffer_consume(), which re-aligns the
    // slices; only those frames live outside the ring.
    const uint8_t *write_ptr = NULL;
    bool from_ring = false;
    bool write_fatal = false;

    size_t avail_frames =
        backend_buffer_get_available_read_frames(playback->buffer);
    if (avail_frames > 0) {
      const void *p1 = NULL, *p2 = NULL;
      size_t n1 = 0, n2 = 0;
      size_t max_frames = 4 * (size_t)playback->chunk_size;
      if (backend_buffer_get_read_slices(playback->buffer, max_frames, &p1, &n1,
                                         &p2, &n2) > 0) {
        write_ptr = (const uint8_t *)p1;
        remainder_frames = n1;
        from_ring = true;
      } else if (backend_buffer_consume(playback->buffer, local_buf, 1) == 1) {
        write_ptr = local_buf;
        remainder_frames = 1;
      }
      if (remainder_frames > 0) {
        playback->has_started = true;
        alsa_playback_publish_level(playback, from_ring ? 0 : remainder_frames);
      }
    }

    if (remainder_frames > 0) {
      if (playback_interrupted) {
        logger_info(&g_logger, "PB: Normal playback resumes");
        playback_interrupted = false;
      }
      snd_pcm_state_t st = snd_pcm_state(playback->pcm);
      if ((int)st < 0) {
        alsa_playback_record_fatal(
            playback, "Device disconnected or in error state", (int)st);
        fatal = true;
        break;
      }
      size_t ring_queued =
          backend_buffer_get_available_read_frames(playback->buffer);
      // Frames written from a ring slice are still in the ring (ring_queued).
      size_t staged = from_ring ? 0 : remainder_frames;
      size_t total_queued = staged + ring_queued;

      if (st == SND_PCM_STATE_XRUN) {
        logger_warn(&g_logger, "PB: Prepare playback after buffer underrun");
        int prc = snd_pcm_prepare(playback->pcm);
        if (prc < 0) {
          alsa_playback_record_fatal(playback, "prepare after underrun", prc);
          fatal = true;
          break;
        }
        if (!alsa_device_prime_delay(
                playback->pcm, playback->target_level, playback->bufsize,
                playback->sample_rate, playback->blockalign, total_queued,
                playback->zero_stall_buf, playback->zero_stall_buf_size)) {
          alsa_playback_record_fatal(playback,
                                     "Failed to prime delay after XRUN", 0);
          fatal = true;
          break;
        }
      } else if (st == SND_PCM_STATE_SUSPENDED) {
        int src = alsa_recover_suspended_pcm(playback->pcm, "PB");
        if (src < 0) {
          alsa_playback_record_fatal(playback, "suspend recovery failed", src);
          fatal = true;
          break;
        }
        if (!alsa_device_prime_delay(
                playback->pcm, playback->target_level, playback->bufsize,
                playback->sample_rate, playback->blockalign, total_queued,
                playback->zero_stall_buf, playback->zero_stall_buf_size)) {
          alsa_playback_record_fatal(playback,
                                     "Failed to prime delay after suspend", 0);
          fatal = true;
          break;
        }
      } else if (st == SND_PCM_STATE_PREPARED) {
        logger_info(&g_logger, "PB: Starting playback from Prepared state");
        if (!alsa_device_prime_delay(
                playback->pcm, playback->target_level, playback->bufsize,
                playback->sample_rate, playback->blockalign, total_queued,
                playback->zero_stall_buf, playback->zero_stall_buf_size)) {
          alsa_playback_record_fatal(playback,
                                     "Failed to prime delay from prepared", 0);
          fatal = true;
          break;
        }
      } else if (st == SND_PCM_STATE_PAUSED) {
        // The device can also be paused from outside this backend.
        logger_debug(&g_logger, "PB: Device is in paused state, unpausing.");
        int unpause_rc = snd_pcm_pause(playback->pcm, 0);
        if (unpause_rc < 0) {
          logger_warn(&g_logger, "Error unpausing playback device: %s",
                      snd_strerror(unpause_rc));
        } else {
          playback->currently_paused = false;
        }
      } else if (st != SND_PCM_STATE_RUNNING) {
        logger_warn(&g_logger, "PB: device is in an unexpected state: %s",
                    alsa_state_desc((int)st));
      }

      // Write loop matching play_buffer and apply_playback_write_result in
      // upstream (src/alsa_backend/threaded_device.rs:636-720, 250-294)
      const int max_no_progress = 100;
      int no_progress = 0;
      while (remainder_frames > 0) {
        if (backend_buffer_get_state(playback->buffer) ==
            BACKEND_STREAM_STOPPED) {
          break;
        }

        // Pre-write wait with buffer-derived timeout
        // (threaded_device.rs:642-663)
        double millis_per_frame = 1000.0 / (double)playback->sample_rate;
        uint32_t timeout_millis =
            (uint32_t)(2.0 * millis_per_frame * (double)playback->bufsize);
        if (timeout_millis < 20)
          timeout_millis = 20;

        int wait_rc = snd_pcm_wait(playback->pcm, (int)timeout_millis);
        if (wait_rc == 0) {
          logger_trace(&g_logger,
                       "PB: Wait timed out, playback device takes too long to "
                       "drain buffer");
          if (!playback->device_stalled) {
            logger_warn(&g_logger, "PB: device stalled");
            snd_pcm_drop(playback->pcm);
            snd_pcm_prepare(playback->pcm);
            snd_pcm_uframes_t avail_min =
                playback->period > 0 ? (snd_pcm_uframes_t)playback->period : 1;
            snd_pcm_uframes_t frames_to_stall =
                (playback->bufsize >= avail_min)
                    ? (playback->bufsize - avail_min + 1)
                    : 1;
            if (frames_to_stall > 0 && playback->zero_stall_buf) {
              snd_pcm_sframes_t sw_rc = snd_pcm_writei(
                  playback->pcm, playback->zero_stall_buf, frames_to_stall);
              if (sw_rc < 0) {
                logger_warn(&g_logger,
                            "PB: Writing stall-check zeros failed with %s",
                            snd_strerror((int)sw_rc));
              } else {
                logger_trace(&g_logger, "PB: Wrote %ld zero frames",
                             (long)sw_rc);
              }
            }
            playback->device_stalled = true;
          }
          continue;
        } else if (wait_rc == -EPIPE) {
          logger_debug(&g_logger, "PB: wait underrun, trying to recover");
          no_progress = 0;
          int prc = snd_pcm_prepare(playback->pcm);
          if (prc < 0) {
            alsa_playback_record_fatal(playback, "prepare after underrun", prc);
            write_fatal = true;
            break;
          }
          continue;
        } else if (wait_rc == -ESTRPIPE) {
          no_progress = 0;
          int src = alsa_recover_suspended_pcm(playback->pcm, "PB");
          if (src < 0) {
            alsa_playback_record_fatal(playback, "suspend recovery failed",
                                       src);
            write_fatal = true;
            break;
          }
          continue;
        } else if (wait_rc < 0 && wait_rc != -EINTR) {
          alsa_playback_record_fatal(playback, "wait error", wait_rc);
          write_fatal = true;
          break;
        }

        snd_pcm_sframes_t rc =
            snd_pcm_writei(playback->pcm, write_ptr, remainder_frames);
        if (rc > 0) {
          playback->device_stalled = false;
          if (from_ring)
            backend_buffer_commit_read(playback->buffer, (size_t)rc);
          write_ptr += (size_t)rc * bytes_per_frame;
          remainder_frames -= (size_t)rc;
          staged = from_ring ? 0 : remainder_frames;
          no_progress = 0;
          // Only frames moved into local_buf are outside the ring, so only
          // they belong in the published term (AGENTS.md §3.1, M2).
          alsa_playback_publish_level(playback, staged);
        } else if (rc == 0 || rc == -EAGAIN || rc == -EINTR) {
          if (!playback->device_stalled && ++no_progress > max_no_progress) {
            logger_warn(&g_logger,
                        "PB: no write progress after %d attempts, "
                        "treating device as stalled",
                        max_no_progress);
            playback->device_stalled = true;
          }
          int wr = snd_pcm_wait(playback->pcm, 10);
          if (wr == -EPIPE) {
            logger_debug(&g_logger, "PB: wait underrun, trying to recover");
            no_progress = 0;
            int prc = snd_pcm_prepare(playback->pcm);
            if (prc < 0) {
              alsa_playback_record_fatal(playback, "prepare after underrun",
                                         prc);
              write_fatal = true;
              break;
            }
            continue;
          } else if (wr == -ESTRPIPE) {
            no_progress = 0;
            int src = alsa_recover_suspended_pcm(playback->pcm, "PB");
            if (src < 0) {
              alsa_playback_record_fatal(playback, "suspend recovery failed",
                                         src);
              write_fatal = true;
              break;
            }
            continue;
          } else if (wr < 0 && wr != -EINTR) {
            alsa_playback_record_fatal(playback, "wait error", wr);
            write_fatal = true;
            break;
          }
        } else if (rc == -EPIPE) {
          logger_warn(&g_logger, "PB: write underrun, trying to recover");
          no_progress = 0;
          int prc = snd_pcm_prepare(playback->pcm);
          if (prc < 0) {
            alsa_playback_record_fatal(playback, "prepare after underrun", prc);
            write_fatal = true;
            break;
          }
          size_t ring_rem =
              backend_buffer_get_available_read_frames(playback->buffer);
          if (!alsa_device_prime_delay(playback->pcm, playback->target_level,
                                       playback->bufsize, playback->sample_rate,
                                       playback->blockalign, staged + ring_rem,
                                       playback->zero_stall_buf,
                                       playback->zero_stall_buf_size)) {
            alsa_playback_record_fatal(playback,
                                       "Failed to prime delay after EPIPE", 0);
            write_fatal = true;
            break;
          }
        } else if (rc == -ESTRPIPE) {
          no_progress = 0;
          int src = alsa_recover_suspended_pcm(playback->pcm, "PB");
          if (src < 0) {
            alsa_playback_record_fatal(playback, "suspend recovery failed",
                                       src);
            write_fatal = true;
            break;
          }
          size_t ring_rem =
              backend_buffer_get_available_read_frames(playback->buffer);
          if (!alsa_device_prime_delay(playback->pcm, playback->target_level,
                                       playback->bufsize, playback->sample_rate,
                                       playback->blockalign, staged + ring_rem,
                                       playback->zero_stall_buf,
                                       playback->zero_stall_buf_size)) {
            alsa_playback_record_fatal(
                playback, "Failed to prime delay after suspend recovery", 0);
            write_fatal = true;
            break;
          }
        } else {
          alsa_playback_record_fatal(playback, "unrecoverable write error",
                                     (int)rc);
          write_fatal = true;
          break;
        }
      }
      if (write_fatal) {
        fatal = true;
        break;
      }

      // Publish the buffer level for the engine thread. Done here, on the
      // thread that owns the PCM handle (threaded_device.rs:449-460).
      alsa_playback_publish_level(playback, staged);
    } else {
      // If draining (EOF), all queued data has been written to the device.
      // Drain PCM device and terminate inner thread matching upstream
      // (src/alsa_backend/threaded_device.rs:461-477).
      if (atomic_load_explicit(&playback->draining, memory_order_acquire)) {
        if (playback->pcm && !playback->currently_paused) {
          snd_pcm_drain(playback->pcm);
        }
        break;
      }

      // Ring buffer is empty during active playback - check if ALSA buffer is
      // running low (src/alsa_backend/threaded_device.rs:512-550 in upstream
      // CamillaDSP)
      snd_pcm_state_t st = snd_pcm_state(playback->pcm);
      if (!playback->has_started && st == SND_PCM_STATE_PREPARED) {
        // Not started yet and nothing queued: the device is not running, so
        // it cannot underrun. Wait for the engine's prefill or first chunk;
        // the data path then primes to target_level with the queued frames
        // counted. Feeding silence here started the device with one chunk of
        // headroom and logged "Playback interrupted" at every start (L1).
        cdsp_sleep_us(500);
        continue;
      }
      bool buffer_low = false;
      if (st == SND_PCM_STATE_RUNNING) {
        snd_pcm_sframes_t avail = snd_pcm_avail(playback->pcm);
        size_t delay = 0;
        if (avail >= 0 && (snd_pcm_uframes_t)avail <= playback->bufsize) {
          delay = playback->bufsize - (snd_pcm_uframes_t)avail;
        }
        backend_buffer_publish(playback->buffer, delay);
        size_t low_threshold = playback->period > 0 ? playback->period : 1;
        buffer_low = (delay < low_threshold);
      } else {
        buffer_low = true;
      }

      if (buffer_low) {
        if (!playback_interrupted) {
          logger_warn(&g_logger, "PB: Playback interrupted, no data available");
          playback_interrupted = true;
        }
        // Upstream routes this keep-alive silence through play_buffer, so it
        // goes through the same state recovery as real audio. Writing into an
        // XRUN'd or suspended device would otherwise silently do nothing.
        if (st == SND_PCM_STATE_XRUN) {
          logger_warn(&g_logger, "PB: Prepare playback after buffer underrun");
          int prc = snd_pcm_prepare(playback->pcm);
          if (prc < 0) {
            alsa_playback_record_fatal(playback, "prepare after underrun", prc);
            fatal = true;
            break;
          }
          if (!alsa_device_prime_delay(
                  playback->pcm, playback->target_level, playback->bufsize,
                  playback->sample_rate, playback->blockalign, 0,
                  playback->zero_stall_buf, playback->zero_stall_buf_size)) {
            alsa_playback_record_fatal(
                playback, "Failed to prime delay after underrun in keep-alive",
                0);
            fatal = true;
            break;
          }
        } else if (st == SND_PCM_STATE_SUSPENDED) {
          int src = alsa_recover_suspended_pcm(playback->pcm, "PB");
          if (src < 0) {
            alsa_playback_record_fatal(playback, "suspend recovery failed",
                                       src);
            fatal = true;
            break;
          }
          if (!alsa_device_prime_delay(
                  playback->pcm, playback->target_level, playback->bufsize,
                  playback->sample_rate, playback->blockalign, 0,
                  playback->zero_stall_buf, playback->zero_stall_buf_size)) {
            alsa_playback_record_fatal(
                playback, "Failed to prime delay after suspend in keep-alive",
                0);
            fatal = true;
            break;
          }
        } else if (st == SND_PCM_STATE_PREPARED) {
          // E.g. after an external prepare or a stall recovery: prime to
          // target_level like the data path and upstream's play_buffer
          // (threaded_device.rs:627-637), not just one chunk (L1).
          if (!alsa_device_prime_delay(
                  playback->pcm, playback->target_level, playback->bufsize,
                  playback->sample_rate, playback->blockalign, 0,
                  playback->zero_stall_buf, playback->zero_stall_buf_size)) {
            alsa_playback_record_fatal(
                playback, "Failed to prime delay from prepared in keep-alive",
                0);
            fatal = true;
            break;
          }
        } else if (st == SND_PCM_STATE_PAUSED) {
          if (snd_pcm_pause(playback->pcm, 0) >= 0) {
            playback->currently_paused = false;
          }
        }

        // Matching play_buffer in threaded_device.rs:531-548: wait for PCM
        // readiness before writing silence buffer to maintain stream during
        // underrun
        int wait_rc = snd_pcm_wait(playback->pcm, 20);
        if (wait_rc == -EPIPE) {
          int prc = snd_pcm_prepare(playback->pcm);
          if (prc < 0) {
            alsa_playback_record_fatal(playback, "prepare after underrun", prc);
            fatal = true;
            break;
          }
        } else if (wait_rc == -ESTRPIPE) {
          int src = alsa_recover_suspended_pcm(playback->pcm, "PB");
          if (src < 0) {
            alsa_playback_record_fatal(playback, "suspend recovery failed",
                                       src);
            fatal = true;
            break;
          }
        } else if (wait_rc < 0 && wait_rc != -EINTR) {
          alsa_playback_record_fatal(
              playback, "wait error while recovering low buffer", wait_rc);
          fatal = true;
          break;
        }
        // zero_stall_buf holds bufsize frames; never write past it.
        snd_pcm_uframes_t silence_frames =
            playback->chunk_size < playback->bufsize
                ? (snd_pcm_uframes_t)playback->chunk_size
                : playback->bufsize;
        snd_pcm_sframes_t sw_rc = snd_pcm_writei(
            playback->pcm, playback->zero_stall_buf, silence_frames);
        if (sw_rc == -EPIPE) {
          int prc = snd_pcm_prepare(playback->pcm);
          if (prc < 0) {
            alsa_playback_record_fatal(playback, "prepare after underrun", prc);
            fatal = true;
            break;
          }
        } else if (sw_rc == -ESTRPIPE) {
          int src = alsa_recover_suspended_pcm(playback->pcm, "PB");
          if (src < 0) {
            alsa_playback_record_fatal(playback, "suspend recovery failed",
                                       src);
            fatal = true;
            break;
          }
        }
      } else {
        // The device still holds plenty of audio; yield rather than inject
        // silence into a healthy stream (threaded_device.rs:549).
        cdsp_sleep_us(500);
      }
    }
  }

  // A device error while the engine still wants audio is reported through
  // write() as a playback error. If the engine already requested the stop,
  // the error is a side effect of shutting down and is not reported.
  if (fatal &&
      backend_buffer_get_state(playback->buffer) != BACKEND_STREAM_STOPPED) {
    atomic_store_explicit(&playback->fatal_error, true, memory_order_release);
  }
  alsa_playback_mark_inner_exited(playback);
  backend_buffer_set_state(playback->buffer, BACKEND_STREAM_STOPPED);
  if (rt_handle) {
    demote_current_thread_from_realtime(rt_handle);
  }
  return NULL;
}

// Closes the pitch-control handles. Called only when the inner thread is not
// running (open() before thread creation / cleanup, or close() after join).
static void alsa_playback_close_controls(alsa_playback_t *playback) {
  if (playback->hctl) {
    snd_hctl_close(playback->hctl);
    playback->hctl = NULL;
  }
  playback->hctl_pitch_elem = NULL;
}

// Open the ALSA playback device matching open_pcm in upstream
// (src/alsa_backend/device.rs:416-493)
static bool alsa_playback_open(void *ctx, backend_error_t *err) {
  alsa_playback_t *playback = (alsa_playback_t *)ctx;
  if (!playback)
    return false;
  pthread_mutex_lock(&g_alsa_mutex);
  if (playback->pcm != NULL) {
    pthread_mutex_unlock(&g_alsa_mutex);
    return true;
  }

  char error_msg[256] = {0};
  int rc = alsa_device_open_and_configure_hw(
      &playback->pcm, playback->device_name, SND_PCM_STREAM_PLAYBACK,
      playback->channels, (unsigned int)playback->sample_rate,
      playback->has_format, playback->requested_format, playback->chunk_size,
      1.0, &playback->format, &playback->bufsize, &playback->period,
      &playback->can_pause, error_msg, sizeof(error_msg));
  if (rc < 0) {
    if (err) {
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         error_msg[0] ? error_msg : snd_strerror(rc));
    }
    pthread_mutex_unlock(&g_alsa_mutex);
    return false;
  }

  // Set software parameters (threaded_buffermanager.rs:106-122)
  snd_pcm_uframes_t avail_min =
      playback->period > 0 ? (snd_pcm_uframes_t)playback->period : 1;
  if (avail_min > (snd_pcm_uframes_t)playback->bufsize) {
    char msg[256];
    snprintf(msg, sizeof(msg),
             "Trying to set avail_min to %lu, must be smaller than or equal to "
             "device buffer size of %lu",
             (unsigned long)avail_min, (unsigned long)playback->bufsize);
    logger_error(&g_logger, "%s", msg);
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    goto error_cleanup;
  }
  int sw_rc = alsa_device_configure_sw(playback->pcm, avail_min, 1);
  if (sw_rc < 0) {
    char msg[256];
    snprintf(msg, sizeof(msg), "Failed to configure ALSA sw params: %s",
             snd_strerror(sw_rc));
    logger_error(&g_logger, "%s", msg);
    if (err) {
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
    goto error_cleanup;
  }

  size_t sample_size = alsa_format_sample_size(playback->format);

  playback->blockalign = (size_t)playback->channels * sample_size;

  // Preallocate zero stall buffer (src/alsa_backend/device.rs:524-525)
  playback->zero_stall_buf_size =
      (size_t)playback->bufsize * playback->channels * sample_size;
  playback->zero_stall_buf = calloc(playback->zero_stall_buf_size, 1);
  if (!playback->zero_stall_buf) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to allocate ALSA playback zero stall buffer");
    goto error_cleanup;
  }
  if (alsa_is_dsd_format(playback->format)) {
    memset(playback->zero_stall_buf, 0x69, playback->zero_stall_buf_size);
  }

  // One-frame wrap-straddle buffer for the inner thread, allocated here rather
  // than on the RT thread (AGENTS.md §1.1). Audio is otherwise written straight
  // from the ring slices, so only a frame that straddles the ring wrap is
  // staged (AGENTS.md §3.4, L4).
  playback->local_buf_bytes = playback->blockalign;
  playback->local_buf = (uint8_t *)malloc(playback->local_buf_bytes);
  if (!playback->local_buf) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to allocate ALSA playback write buffer");
    goto error_cleanup;
  }

  playback->currently_paused = false;
  playback->device_stalled = false;
  playback->has_started = false;
  atomic_store_explicit(&playback->has_pending_pitch, false,
                        memory_order_relaxed);
  atomic_store_explicit(&playback->draining, false, memory_order_relaxed);
  atomic_store_explicit(&playback->fatal_error, false, memory_order_relaxed);
  atomic_store_explicit(&playback->inner_exited, false, memory_order_relaxed);
  playback->fatal_msg[0] = '\0';

  // Search for UAC2 gadget pitch control: "Playback Pitch 1000000"
  // (src/alsa_backend/device.rs:530-544)
  char ctl_name[32];
  int dev_idx = 0;
  int subdev_idx = 0;
  if (alsa_device_get_card_ctl_name(playback->pcm, ctl_name, sizeof(ctl_name),
                                    &dev_idx, &subdev_idx)) {
    snd_hctl_t *hctl = NULL;
    if (snd_hctl_open(&hctl, ctl_name, SND_CTL_NONBLOCK) >= 0 && hctl) {
      snd_hctl_nonblock(hctl, 1);
      if (snd_hctl_load(hctl) >= 0) {
        playback->hctl = hctl;
        playback->hctl_pitch_elem =
            alsa_find_elem(hctl, SND_CTL_ELEM_IFACE_PCM, dev_idx, subdev_idx,
                           "Playback Pitch 1000000", NULL);
        if (playback->hctl_pitch_elem) {
          logger_info(&g_logger, "Playback device supports rate adjust");
        }
      } else {
        snd_hctl_close(hctl);
      }
    }
  }

  size_t ring_frames = alsa_playback_ring_capacity_frames(
      playback->target_level, playback->chunk_size);
  playback->buffer = backend_buffer_create(
      ring_frames, alsa_pcm_format_to_binary_format(playback->format),
      playback->channels, playback->sample_rate, false, playback->params);
  if (!playback->buffer) {
    if (err) {
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to allocate backend buffer for threaded ALSA");
    }
    goto error_cleanup;
  }
  backend_buffer_set_target_level(playback->buffer, playback->target_level);
  backend_buffer_set_state(playback->buffer, BACKEND_STREAM_RUNNING);
  if (pthread_create(&playback->inner_thread, NULL,
                     alsa_playback_inner_thread_func, playback) != 0) {
    backend_buffer_set_state(playback->buffer, BACKEND_STREAM_STOPPED);
    if (err) {
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to spawn ALSA playback inner thread");
    }
    goto error_cleanup;
  }
  playback->inner_thread_created = true;

  pthread_mutex_unlock(&g_alsa_mutex);
  return true;

error_cleanup:
  backend_buffer_free(playback->buffer);
  playback->buffer = NULL;
  if (playback->pcm) {
    snd_pcm_close(playback->pcm);
    playback->pcm = NULL;
  }
  if (playback->zero_stall_buf) {
    free(playback->zero_stall_buf);
    playback->zero_stall_buf = NULL;
  }
  free(playback->local_buf);
  playback->local_buf = NULL;
  playback->local_buf_bytes = 0;
  // The hctl handle opened above would otherwise leak, and be overwritten
  // without being closed by the next open() (L3).
  alsa_playback_close_controls(playback);
  pthread_mutex_unlock(&g_alsa_mutex);
  return false;
}

// Play a buffer matching play_buffer in upstream
// (src/alsa_backend/device.rs:107-239)
static bool alsa_playback_write(void *ctx, const audio_chunk_t *chunk,
                                backend_error_t *err) {
  alsa_playback_t *playback = (alsa_playback_t *)ctx;
  if (!playback || !playback->pcm)
    return false;

  if (backend_buffer_get_state(playback->buffer) == BACKEND_STREAM_STOPPED) {
    if (err) {
      if (atomic_load_explicit(&playback->fatal_error, memory_order_acquire)) {
        backend_error_init(err, BACKEND_ERROR_WRITE_ERROR, playback->fatal_msg);
      } else {
        backend_error_init(err, BACKEND_ERROR_NONE, "Playback stopped");
      }
    }
    return false;
  }

  size_t total_frames = audio_chunk_get_valid_frames(chunk);
  if (total_frames == 0)
    return true;

  bool ok = backend_buffer_write_chunk(playback->buffer, chunk, 1, 500, err);
  if (!ok && err &&
      atomic_load_explicit(&playback->fatal_error, memory_order_acquire)) {
    // The inner thread died while this chunk was waiting for ring space.
    backend_error_init(err, BACKEND_ERROR_WRITE_ERROR, playback->fatal_msg);
  }
  return ok;
}

// Close the ALSA playback device
static void alsa_playback_close(void *ctx) {
  alsa_playback_t *playback = (alsa_playback_t *)ctx;
  if (!playback)
    return;

  bool is_paused =
      backend_buffer_get_state(playback->buffer) == BACKEND_STREAM_PAUSED;
  backend_buffer_set_state(playback->buffer, BACKEND_STREAM_STOPPED);

  if (playback->inner_thread_created) {
    pthread_join(playback->inner_thread, NULL);
    playback->inner_thread_created = false;
  }
  backend_buffer_free(playback->buffer);
  playback->buffer = NULL;

  // The PCM is only touched here, after the inner thread has been joined, so
  // it is never used from two threads at once (M4).
  if (playback->pcm) {
    snd_pcm_state_t st = snd_pcm_state(playback->pcm);
    bool eos = atomic_load_explicit(&playback->draining, memory_order_acquire);
    if (eos && !is_paused && !playback->currently_paused &&
        (st == SND_PCM_STATE_RUNNING || st == SND_PCM_STATE_DRAINING)) {
      // End of stream: let the device play out what it still holds. The
      // handle is non-blocking, where snd_pcm_drain() returns -EAGAIN at
      // once, so switch to blocking mode first (L10). The engine has already
      // waited for the estimated level to reach zero, so this only covers
      // what the estimate under-counts (USB/FIFO latency).
      snd_pcm_nonblock(playback->pcm, 0);
      snd_pcm_drain(playback->pcm);
    } else {
      snd_pcm_drop(playback->pcm);
    }
  }
  pthread_mutex_lock(&g_alsa_mutex);
  if (playback->pcm) {
    snd_pcm_close(playback->pcm);
    playback->pcm = NULL;
  }
  pthread_mutex_unlock(&g_alsa_mutex);
  if (playback->zero_stall_buf) {
    free(playback->zero_stall_buf);
    playback->zero_stall_buf = NULL;
  }
  free(playback->local_buf);
  playback->local_buf = NULL;
  playback->local_buf_bytes = 0;
  alsa_playback_close_controls(playback);
}

// Get the current buffer level matching PlaybackBufferManager::current_delay
// (src/alsa_backend/buffermanager.rs:255: self.data.bufsize - avail)
//
// The device component is read from the estimate published by the inner RT
// thread rather than queried here: snd_pcm_avail() and snd_pcm_writei() must
// not run concurrently on the same handle. Mirrors upstream, where the outer
// thread samples a DeviceBufferEstimator updated by the inner thread
// (src/alsa_backend/threaded_device.rs:1197-1205).
static size_t alsa_playback_get_buffer_level(void *ctx) {
  alsa_playback_t *playback = (alsa_playback_t *)ctx;
  if (!playback)
    return 0;
  return backend_buffer_get_level(playback->buffer);
}

// Called by the engine playback loop for every chunk. ALSA playback never
// detects a rate change by itself (upstream has no such path either), so this
// is a constant. It used to lock g_alsa_mutex to read a flag that was never
// set, putting a global mutex (also held during device enumeration and the
// other direction's open/close) on the audio thread (AGENTS.md §1.2).
static bool alsa_playback_get_pending_rate_change(void *ctx, double *out_rate) {
  (void)ctx;
  (void)out_rate;
  return false;
}

static bool alsa_playback_prefill_silence(void *ctx, size_t frames,
                                          backend_error_t *err) {
  (void)err;
  alsa_playback_t *playback = (alsa_playback_t *)ctx;
  if (!playback || frames == 0)
    return true;

  uint8_t silence_byte = alsa_is_dsd_format(playback->format) ? 0x69 : 0x00;
  backend_buffer_prefill_silence(playback->buffer, frames, silence_byte);
  return true;
}

static bool alsa_playback_get_is_paused(void *ctx) {
  alsa_playback_t *playback = (alsa_playback_t *)ctx;
  if (!playback)
    return false;
  return backend_buffer_get_state(playback->buffer) == BACKEND_STREAM_PAUSED;
}

static void alsa_playback_set_is_paused(void *ctx, bool paused) {
  alsa_playback_t *playback = (alsa_playback_t *)ctx;
  if (!playback)
    return;
  // A STOPPED stream (requested stop, or the inner thread died) must stay
  // STOPPED: reviving it would make write() feed a ring that nobody drains
  // and hide the device error (M5).
  if (backend_buffer_get_state(playback->buffer) == BACKEND_STREAM_STOPPED)
    return;
  backend_buffer_set_state(playback->buffer, paused ? BACKEND_STREAM_PAUSED
                                                    : BACKEND_STREAM_RUNNING);
  // The inner thread may have exited between the check and the store above.
  atomic_thread_fence(memory_order_seq_cst);
  if (atomic_load_explicit(&playback->inner_exited, memory_order_seq_cst)) {
    backend_buffer_set_state(playback->buffer, BACKEND_STREAM_STOPPED);
  }
}

static bool alsa_playback_pitch_control_supported(void *ctx) {
  alsa_playback_t *playback = (alsa_playback_t *)ctx;
  if (!playback)
    return false;
  return playback->hctl_pitch_elem != NULL;
}

// Set the UAC2 gadget playback pitch. Upstream writes
// (1_000_000.0 / speed) as i32 on the inner thread
// (src/alsa_backend/threaded_device.rs:324-325, 397-402, 1218-1222).
// `multiplier` is the playback *clock* multiplier: the engine already passes
// 1.0 / speed (engine_playback_loop.c apply_speed), so it must not be inverted
// a second time here.
static void alsa_playback_set_pitch(void *ctx, double multiplier) {
  alsa_playback_t *playback = (alsa_playback_t *)ctx;
  if (!playback || !(multiplier > 0.0) || !isfinite(multiplier))
    return;
  atomic_store_explicit(&playback->pending_pitch, multiplier,
                        memory_order_relaxed);
  atomic_store_explicit(&playback->has_pending_pitch, true,
                        memory_order_release);
#ifdef CDSP_TEST
  // In unit tests that immediately inspect the ALSA control after set_pitch(),
  // wait briefly for the running inner thread to apply the pending pitch.
  for (int i = 0; i < 200 &&
                  atomic_load_explicit(&playback->has_pending_pitch,
                                       memory_order_acquire) &&
                  playback->inner_thread_created &&
                  backend_buffer_get_state(playback->buffer) ==
                      BACKEND_STREAM_RUNNING;
       i++) {
    cdsp_sleep_us(500);
  }
#endif
}

static void alsa_playback_drain(void *ctx) {
  alsa_playback_t *playback = (alsa_playback_t *)ctx;
  if (!playback)
    return;
  atomic_store_explicit(&playback->draining, true, memory_order_release);
}

// Only signals the inner thread, which observes STOPPED within one wait slice
// and exits. The PCM is dropped or drained in close(), after the join, so the
// engine thread never touches the handle while the inner thread may be inside
// snd_pcm_wait()/snd_pcm_writei()/prime (M4; upstream sends a message and the
// inner thread drops its own handle, threaded_device.rs:408-412, 521-525).
static void alsa_playback_stop(void *ctx) {
  alsa_playback_t *playback = (alsa_playback_t *)ctx;
  if (!playback)
    return;
  backend_buffer_set_state(playback->buffer, BACKEND_STREAM_STOPPED);
}

static void alsa_playback_destroy(void *ctx) {
  alsa_playback_t *playback = (alsa_playback_t *)ctx;
  if (!playback)
    return;
  alsa_playback_close(playback);
  free(playback);
}

// Create ALSA playback backend matching AlsaPlaybackDevice::start in upstream
// (src/alsa_backend/device.rs:1144-1215 and
// src/alsa_backend/threaded_device.rs:1055-1075)
static playback_backend_t *
alsa_playback_create(const playback_device_config_t *config, int sample_rate,
                     int chunk_size, bool full_duplex,
                     processing_parameters_t *params, backend_error_t *err) {
  (void)full_duplex;
  (void)err;
  alsa_playback_t *playback =
      (alsa_playback_t *)calloc(1, sizeof(alsa_playback_t));
  if (!playback)
    return NULL;

  snprintf(playback->device_name, sizeof(playback->device_name), "%s",
           config->cfg.alsa.device[0] ? config->cfg.alsa.device : "default");

  playback->sample_rate = sample_rate;
  playback->channels = config->cfg.alsa.channels;
  playback->chunk_size = (size_t)chunk_size;

  // target_level defaults to chunksize matching upstream device.rs:1153-1157
  playback->target_level =
      (config->cfg.alsa.has_target_level && config->cfg.alsa.target_level > 0)
          ? (size_t)config->cfg.alsa.target_level
          : (size_t)chunk_size;

  playback->has_format = config->cfg.alsa.has_format;
  playback->requested_format = config->cfg.alsa.format;
  playback->params = params;
  playback->currently_paused = false;
  atomic_store_explicit(&playback->pending_pitch, 1.0, memory_order_relaxed);
  atomic_store_explicit(&playback->has_pending_pitch, false,
                        memory_order_relaxed);

  playback_backend_t *backend =
      (playback_backend_t *)calloc(1, sizeof(playback_backend_t));
  if (!backend) {
    free(playback);
    return NULL;
  }
  backend->ctx = playback;
  backend->vtable = &g_alsa_playback_vtable;
  return backend;
}

const playback_backend_vtable_t g_alsa_playback_vtable = {
    .create = alsa_playback_create,
    .open = alsa_playback_open,
    .write = alsa_playback_write,
    .close = alsa_playback_close,
    .get_buffer_level = alsa_playback_get_buffer_level,
    .get_pending_rate_change = alsa_playback_get_pending_rate_change,
    .prefill_silence = alsa_playback_prefill_silence,
    .get_is_paused = alsa_playback_get_is_paused,
    .set_is_paused = alsa_playback_set_is_paused,
    .pitch_control_supported = alsa_playback_pitch_control_supported,
    .set_pitch = alsa_playback_set_pitch,
    .drain = alsa_playback_drain,
    .stop = alsa_playback_stop,
    .destroy = alsa_playback_destroy};

#endif // defined(ENABLE_ALSA)
