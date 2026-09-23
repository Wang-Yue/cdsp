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
  _Atomic bool paused;
  bool currently_paused;
  bool device_stalled;

  size_t blockalign;
  void *zero_stall_buf;
  size_t zero_stall_buf_size;

  snd_hctl_t *hctl;
  snd_hctl_elem_t *hctl_pitch_elem;
  snd_mixer_t *mixer;
  snd_mixer_elem_t *pitch_elem;
  pthread_mutex_t mixer_mutex;
  _Atomic bool stopped;

  double pending_rate;
  bool has_pending_rate;

  backend_buffer_t *buffer;
  pthread_t inner_thread;
  bool inner_thread_created;
  _Atomic bool inner_running;
  _Atomic bool draining;
};

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
  size_t chunk_bytes = playback->chunk_size * bytes_per_frame;
  // Sized generously, like upstream: the ring holds several chunks, so the
  // bytes available in one pass can exceed a single chunksize
  // (src/alsa_backend/threaded_device.rs:320-323).
  size_t local_buf_bytes = 4 * chunk_bytes;
  uint8_t *local_buf = (uint8_t *)malloc(local_buf_bytes);
  if (!local_buf) {
    logger_error(&g_logger, "Failed to allocate ALSA playback write buffer");
    atomic_store_explicit(&playback->inner_running, false,
                          memory_order_release);
    if (rt_handle) {
      demote_current_thread_from_realtime(rt_handle);
    }
    return NULL;
  }
  bool playback_interrupted = false;

  while (!atomic_load_explicit(&playback->stopped, memory_order_acquire)) {
    if (atomic_load_explicit(&playback->paused, memory_order_acquire)) {
      if (playback->can_pause && !playback->currently_paused) {
        snd_pcm_pause(playback->pcm, 1);
        playback->currently_paused = true;
      }
      cdsp_sleep_ms(5);
      continue;
    } else {
      if (playback->can_pause && playback->currently_paused) {
        snd_pcm_pause(playback->pcm, 0);
        playback->currently_paused = false;
      }
    }

    size_t remainder_frames = 0;
    size_t remainder_offset = 0;
    bool write_fatal = false;

    size_t avail_frames =
        backend_buffer_get_available_read_frames(playback->buffer);
    if (avail_frames > 0) {
      size_t max_frames = local_buf_bytes / bytes_per_frame;
      size_t to_read = avail_frames < max_frames ? avail_frames : max_frames;
      size_t consumed =
          backend_buffer_consume(playback->buffer, local_buf, to_read);
      if (consumed > 0) {
        remainder_frames = consumed;
        remainder_offset = 0;
      }
    }

    if (remainder_frames > 0) {
      snd_pcm_state_t st = snd_pcm_state(playback->pcm);
      if ((int)st < 0) {
        logger_error(&g_logger, "PB: Device disconnected or in error state: %s",
                     snd_strerror((int)st));
        break;
      }
      size_t ring_queued =
          backend_buffer_get_available_read_frames(playback->buffer);
      size_t total_queued = remainder_frames + ring_queued;

      if (st == SND_PCM_STATE_XRUN) {
        logger_warn(&g_logger, "PB: Prepare playback after buffer underrun");
        snd_pcm_prepare(playback->pcm);
        if (!alsa_device_prime_delay(
                playback->pcm, playback->target_level, playback->bufsize,
                playback->sample_rate, playback->blockalign, total_queued,
                playback->zero_stall_buf, playback->zero_stall_buf_size)) {
          logger_error(&g_logger, "PB: Failed to prime delay after XRUN");
          write_fatal = true;
          break;
        }
      } else if (st == SND_PCM_STATE_SUSPENDED) {
        alsa_recover_suspended_pcm(playback->pcm, "PB");
        if (!alsa_device_prime_delay(
                playback->pcm, playback->target_level, playback->bufsize,
                playback->sample_rate, playback->blockalign, total_queued,
                playback->zero_stall_buf, playback->zero_stall_buf_size)) {
          logger_error(&g_logger, "PB: Failed to prime delay after suspend");
          write_fatal = true;
          break;
        }
      } else if (st == SND_PCM_STATE_PREPARED) {
        logger_info(&g_logger, "PB: Starting playback from Prepared state");
        if (!alsa_device_prime_delay(
                playback->pcm, playback->target_level, playback->bufsize,
                playback->sample_rate, playback->blockalign, total_queued,
                playback->zero_stall_buf, playback->zero_stall_buf_size)) {
          logger_error(&g_logger, "PB: Failed to prime delay from prepared");
          write_fatal = true;
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
        if (atomic_load_explicit(&playback->stopped, memory_order_acquire)) {
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
          if (++no_progress > max_no_progress) {
            logger_error(&g_logger,
                         "PB: no progress after %d stall attempts, aborting",
                         max_no_progress);
            write_fatal = true;
            break;
          }
          continue;
        } else if (wait_rc < 0 && wait_rc != -EPIPE && wait_rc != -ESTRPIPE &&
                   wait_rc != -EINTR) {
          logger_error(&g_logger, "PB: wait error: %s", snd_strerror(wait_rc));
          write_fatal = true;
          break;
        }

        snd_pcm_sframes_t rc = snd_pcm_writei(
            playback->pcm, local_buf + remainder_offset * bytes_per_frame,
            remainder_frames);
        if (rc > 0) {
          playback->device_stalled = false;
          remainder_offset += (size_t)rc;
          remainder_frames -= (size_t)rc;
          no_progress = 0;
        } else if (rc == 0 || rc == -EAGAIN || rc == -EINTR) {
          if (++no_progress > max_no_progress) {
            logger_error(&g_logger,
                         "PB: no write progress after %d attempts, "
                         "treating device as stalled",
                         max_no_progress);
            write_fatal = true;
            break;
          }
          int wr = snd_pcm_wait(playback->pcm, 10);
          if (wr < 0 && wr != -EPIPE && wr != -ESTRPIPE && wr != -EINTR) {
            logger_error(&g_logger, "PB: wait error: %s", snd_strerror(wr));
            write_fatal = true;
            break;
          }
        } else if (rc == -EPIPE) {
          logger_warn(&g_logger, "PB: write underrun, trying to recover");
          no_progress = 0;
          if (snd_pcm_prepare(playback->pcm) < 0) {
            write_fatal = true;
            break;
          }
          size_t ring_rem =
              backend_buffer_get_available_read_frames(playback->buffer);
          if (!alsa_device_prime_delay(
                  playback->pcm, playback->target_level, playback->bufsize,
                  playback->sample_rate, playback->blockalign,
                  remainder_frames + ring_rem, playback->zero_stall_buf,
                  playback->zero_stall_buf_size)) {
            logger_error(&g_logger, "PB: Failed to prime delay after EPIPE");
            write_fatal = true;
            break;
          }
        } else if (rc == -ESTRPIPE) {
          no_progress = 0;
          if (alsa_recover_suspended_pcm(playback->pcm, "PB") < 0) {
            write_fatal = true;
            break;
          }
          size_t ring_rem =
              backend_buffer_get_available_read_frames(playback->buffer);
          if (!alsa_device_prime_delay(
                  playback->pcm, playback->target_level, playback->bufsize,
                  playback->sample_rate, playback->blockalign,
                  remainder_frames + ring_rem, playback->zero_stall_buf,
                  playback->zero_stall_buf_size)) {
            logger_error(&g_logger,
                         "PB: Failed to prime delay after suspend recovery");
            write_fatal = true;
            break;
          }
        } else {
          logger_error(&g_logger, "PB: unrecoverable write error: %s",
                       snd_strerror((int)rc));
          write_fatal = true;
          break;
        }
      }
      if (write_fatal) {
        break;
      }
      playback_interrupted = false;

      // Publish the buffer level for the engine thread. Done here, on the
      // thread that owns the PCM handle (threaded_device.rs:449-460).
      if (!playback->device_stalled &&
          snd_pcm_state(playback->pcm) == SND_PCM_STATE_RUNNING) {
        snd_pcm_sframes_t avail = snd_pcm_avail(playback->pcm);
        if (avail >= 0 && (snd_pcm_uframes_t)avail <= playback->bufsize) {
          backend_buffer_publish(
              playback->buffer,
              (size_t)(playback->bufsize - (snd_pcm_uframes_t)avail));
        }
      }
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
          if (snd_pcm_prepare(playback->pcm) < 0) {
            break;
          }
        } else if (st == SND_PCM_STATE_SUSPENDED) {
          if (alsa_recover_suspended_pcm(playback->pcm, "PB") < 0) {
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
        if (wait_rc < 0 && wait_rc != -EPIPE && wait_rc != -ESTRPIPE &&
            wait_rc != -EINTR) {
          logger_error(&g_logger,
                       "PB: wait error while recovering low buffer: %s",
                       snd_strerror(wait_rc));
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
          snd_pcm_prepare(playback->pcm);
        } else if (sw_rc == -ESTRPIPE) {
          alsa_recover_suspended_pcm(playback->pcm, "PB");
        }
      } else {
        // The device still holds plenty of audio; yield rather than inject
        // silence into a healthy stream (threaded_device.rs:549).
        cdsp_sleep_us(500);
      }
    }
  }

  atomic_store_explicit(&playback->inner_running, false, memory_order_release);
  if (local_buf)
    free(local_buf);
  if (rt_handle) {
    demote_current_thread_from_realtime(rt_handle);
  }
  return NULL;
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

  playback->paused = false;
  playback->currently_paused = false;
  playback->device_stalled = false;
  atomic_store_explicit(&playback->draining, false, memory_order_relaxed);

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
        pthread_mutex_lock(&playback->mixer_mutex);
        playback->hctl = hctl;
        playback->hctl_pitch_elem =
            alsa_find_elem(hctl, SND_CTL_ELEM_IFACE_PCM, dev_idx, subdev_idx,
                           "Playback Pitch 1000000", NULL);
        if (playback->hctl_pitch_elem) {
          logger_info(&g_logger, "Playback device supports rate adjust");
        }
        pthread_mutex_unlock(&playback->mixer_mutex);
      } else {
        snd_hctl_close(hctl);
      }
    }

    snd_mixer_t *mixer = NULL;
    if (snd_mixer_open(&mixer, 0) >= 0) {
      if (snd_mixer_attach(mixer, ctl_name) >= 0 &&
          snd_mixer_selem_register(mixer, NULL, NULL) >= 0 &&
          snd_mixer_load(mixer) >= 0) {
        pthread_mutex_lock(&playback->mixer_mutex);
        playback->mixer = mixer;

        snd_mixer_selem_id_t *sid;
        snd_mixer_selem_id_alloca(&sid);
        snd_mixer_selem_id_set_name(sid, "Playback Pitch 1000000");
        playback->pitch_elem = snd_mixer_find_selem(mixer, sid);
        pthread_mutex_unlock(&playback->mixer_mutex);
      } else {
        snd_mixer_close(mixer);
      }
    }
  }

  size_t ring_frames = alsa_playback_ring_capacity_frames(
      playback->target_level, playback->chunk_size);
  playback->buffer = backend_buffer_create(
      ring_frames, alsa_pcm_format_to_binary_format(playback->format),
      playback->channels, playback->sample_rate, false);
  if (!playback->buffer) {
    if (err) {
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to allocate backend buffer for threaded ALSA");
    }
    goto error_cleanup;
  }
  backend_buffer_set_control_flags(playback->buffer, &playback->inner_running,
                                   &playback->stopped, &playback->paused, NULL);
  backend_buffer_set_target_level(playback->buffer, playback->target_level);
  atomic_store_explicit(&playback->inner_running, true, memory_order_release);
  if (pthread_create(&playback->inner_thread, NULL,
                     alsa_playback_inner_thread_func, playback) != 0) {
    atomic_store_explicit(&playback->inner_running, false,
                          memory_order_release);
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
  if (playback->buffer) {
    backend_buffer_free(playback->buffer);
    playback->buffer = NULL;
  }
  if (playback->pcm) {
    snd_pcm_close(playback->pcm);
    playback->pcm = NULL;
  }
  if (playback->zero_stall_buf) {
    free(playback->zero_stall_buf);
    playback->zero_stall_buf = NULL;
  }
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

  if (atomic_load_explicit(&playback->stopped, memory_order_acquire)) {
    if (err) {
      backend_error_init(err, BACKEND_ERROR_NONE, "Playback stopped");
    }
    return false;
  }

  if (audio_chunk_get_channels(chunk) < (size_t)playback->channels) {
    if (err) {
      backend_error_init(
          err, BACKEND_ERROR_WRITE_ERROR,
          "Chunk channels count is smaller than playback device channels");
    }
    return false;
  }

  size_t total_frames = audio_chunk_get_valid_frames(chunk);
  if (total_frames == 0)
    return true;

  return backend_buffer_write_chunk(playback->buffer, chunk, 1, 500, err);
}

// Close the ALSA playback device
static void alsa_playback_close(void *ctx) {
  alsa_playback_t *playback = (alsa_playback_t *)ctx;
  if (!playback)
    return;

  atomic_store_explicit(&playback->stopped, true, memory_order_release);

  if (playback->inner_thread_created) {
    pthread_join(playback->inner_thread, NULL);
    playback->inner_thread_created = false;
    atomic_store_explicit(&playback->inner_running, false,
                          memory_order_release);
    if (playback->buffer) {
      backend_buffer_free(playback->buffer);
      playback->buffer = NULL;
    }
  }

  if (playback->pcm) {
    if (!atomic_load_explicit(&playback->paused, memory_order_acquire)) {
      snd_pcm_drain(playback->pcm);
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
  pthread_mutex_lock(&playback->mixer_mutex);
  if (playback->hctl) {
    snd_hctl_close(playback->hctl);
    playback->hctl = NULL;
  }
  playback->hctl_pitch_elem = NULL;
  if (playback->mixer) {
    snd_mixer_close(playback->mixer);
    playback->mixer = NULL;
    playback->pitch_elem = NULL;
  }
  pthread_mutex_unlock(&playback->mixer_mutex);
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
  if (!playback || !playback->buffer)
    return 0;
  return backend_buffer_get_level(playback->buffer);
}

static bool alsa_playback_get_pending_rate_change(void *ctx, double *out_rate) {
  alsa_playback_t *playback = (alsa_playback_t *)ctx;
  if (!playback)
    return false;
  pthread_mutex_lock(&g_alsa_mutex);
  bool pending = playback->has_pending_rate;
  if (pending) {
    if (out_rate)
      *out_rate = playback->pending_rate;
    playback->has_pending_rate = false;
  }
  pthread_mutex_unlock(&g_alsa_mutex);
  return pending;
}

static bool alsa_playback_prefill_silence(void *ctx, size_t frames,
                                          backend_error_t *err) {
  (void)err;
  alsa_playback_t *playback = (alsa_playback_t *)ctx;
  if (!playback || frames == 0 || !playback->buffer)
    return true;

  uint8_t silence_byte = alsa_is_dsd_format(playback->format) ? 0x69 : 0x00;
  backend_buffer_prefill_silence(playback->buffer, frames, silence_byte);
  return true;
}

static bool alsa_playback_get_is_paused(void *ctx) {
  alsa_playback_t *playback = (alsa_playback_t *)ctx;
  if (!playback)
    return false;
  return atomic_load_explicit(&playback->paused, memory_order_acquire);
}

static void alsa_playback_set_is_paused(void *ctx, bool paused) {
  alsa_playback_t *playback = (alsa_playback_t *)ctx;
  if (!playback)
    return;
  atomic_store_explicit(&playback->paused, paused, memory_order_release);
}

static bool alsa_playback_pitch_control_supported(void *ctx) {
  alsa_playback_t *playback = (alsa_playback_t *)ctx;
  if (!playback)
    return false;
  pthread_mutex_lock(&playback->mixer_mutex);
  bool res = playback->hctl_pitch_elem != NULL || playback->pitch_elem != NULL;
  pthread_mutex_unlock(&playback->mixer_mutex);
  return res;
}

// Set pitch control matching upstream (src/alsa_backend/device.rs:673-678)
// Note: speed is reciprocal on playback side: (1_000_000.0 / capture_speed) as
// i32
static void alsa_playback_set_pitch(void *ctx, double multiplier) {
  alsa_playback_t *playback = (alsa_playback_t *)ctx;
  if (!playback || multiplier <= 0.0)
    return;
  pthread_mutex_lock(&playback->mixer_mutex);
  long value = (long)trunc(1000000.0 / multiplier);
  if (playback->hctl_pitch_elem) {
    alsa_elem_write_as_int(playback->hctl_pitch_elem, value);
  } else if (playback->pitch_elem) {
    if (snd_mixer_selem_has_playback_volume(playback->pitch_elem)) {
      snd_mixer_selem_set_playback_volume_all(playback->pitch_elem, value);
    } else if (snd_mixer_selem_has_capture_volume(playback->pitch_elem)) {
      snd_mixer_selem_set_capture_volume_all(playback->pitch_elem, value);
    }
  }
  pthread_mutex_unlock(&playback->mixer_mutex);
}

static void alsa_playback_drain(void *ctx) {
  alsa_playback_t *playback = (alsa_playback_t *)ctx;
  if (!playback)
    return;
  atomic_store_explicit(&playback->draining, true, memory_order_release);
}

static void alsa_playback_stop(void *ctx) {
  alsa_playback_t *playback = (alsa_playback_t *)ctx;
  if (!playback)
    return;
  atomic_store_explicit(&playback->stopped, true, memory_order_release);
  pthread_mutex_lock(&g_alsa_mutex);
  if (playback->pcm) {
    snd_pcm_drop(playback->pcm);
  }
  pthread_mutex_unlock(&g_alsa_mutex);
}

static void alsa_playback_destroy(void *ctx) {
  alsa_playback_t *playback = (alsa_playback_t *)ctx;
  if (!playback)
    return;
  alsa_playback_close(playback);
  pthread_mutex_destroy(&playback->mixer_mutex);
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
  atomic_init(&playback->paused, false);
  playback->currently_paused = false;
  pthread_mutex_init(&playback->mixer_mutex, NULL);

  playback_backend_t *backend =
      (playback_backend_t *)calloc(1, sizeof(playback_backend_t));
  if (!backend) {
    pthread_mutex_destroy(&playback->mixer_mutex);
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
