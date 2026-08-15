#include "Backend/alsa_playback.h"

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

#include "Audio/audio_chunk.h"
#include "Audio/sample_conversion.h"
#include "Backend/alsa_device.h"
#include "Backend/backend_error.h"
#include "Config/engine_config_types.h"
#include "Engine/thread_priority.h"
#include "Logging/app_logger.h"
#include "Utils/cdsp_time.h"
#include "Utils/lock_free_ring_buffer.h"

static const logger_t g_logger = {"dsp.backend.alsa"};

struct alsa_playback {
  char device_name[256];
  int sample_rate;
  int channels;
  size_t chunk_size;
  size_t target_level;
  snd_pcm_uframes_t bufsize;
  snd_pcm_uframes_t period;

  bool has_format;
  alsa_sample_format_t requested_format;
  processing_parameters_t* params;

  snd_pcm_t* pcm;
  snd_pcm_format_t format;
  bool can_pause;
  _Atomic bool paused;
  bool currently_paused;
  bool device_stalled;

  void* interleaved_buf;
  size_t interleaved_buf_size;
  size_t bytes_per_sample;
  size_t blockalign;
  void* zero_stall_buf;
  size_t zero_stall_buf_size;

  snd_hctl_t* hctl;
  snd_hctl_elem_t* hctl_pitch_elem;
  snd_mixer_t* mixer;
  snd_mixer_elem_t* pitch_elem;
  pthread_mutex_t mixer_mutex;
  _Atomic bool stopped;

  double pending_rate;
  bool has_pending_rate;

  // Runtime switch for decoupled threaded mode vs direct mode
  // (src/alsa_backend/threaded_device.rs vs src/alsa_backend/device.rs)
  bool threaded;
  spsc_byte_ring_buffer_t* ring_buffer;
  pthread_t inner_thread;
  _Atomic bool inner_running;
};

// Sleep for the target delay matching
// PlaybackBufferManager::sleep_for_target_delay in upstream
// (src/alsa_backend/buffermanager.rs:227-234)
static void sleep_for_target_delay(alsa_playback_t* playback) {
  if (playback->target_level > 0 && playback->sample_rate > 0) {
    double millis_per_frame = 1000.0 / (double)playback->sample_rate;
    uint64_t sleep_millis =
        (uint64_t)((double)playback->target_level * millis_per_frame);
    logger_trace(&g_logger, "Sleeping for %zu frames = %llu ms",
                 playback->target_level, (unsigned long long)sleep_millis);
    cdsp_sleep_ms(sleep_millis);
  }
}

// Dedicated real-time playback inner thread matching AlsaPlaybackInner in
// upstream (src/alsa_backend/threaded_device.rs:1078-1345)
static void* alsa_playback_inner_thread_func(void* arg) {
  alsa_playback_t* playback = (alsa_playback_t*)arg;
  realtime_thread_handle_t* rt_handle = promote_current_thread_to_realtime(
      "AlsaPlaybackInner", playback->chunk_size, (size_t)playback->sample_rate);
  if (rt_handle) {
    logger_debug(&g_logger, "Playback inner thread has real-time priority.");
  }

  size_t bytes_per_frame = playback->blockalign;
  size_t chunk_bytes = playback->chunk_size * bytes_per_frame;
  uint8_t* local_buf = (uint8_t*)malloc(chunk_bytes);
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

    size_t avail_bytes =
        spsc_byte_ring_buffer_get_available_to_read(playback->ring_buffer);
    if (avail_bytes >= bytes_per_frame) {
      size_t to_read = avail_bytes < chunk_bytes ? avail_bytes : chunk_bytes;
      to_read -= (to_read % bytes_per_frame);
      size_t consumed = spsc_byte_ring_buffer_consume(playback->ring_buffer,
                                                      local_buf, to_read);
      if (consumed > 0) {
        size_t frames = consumed / bytes_per_frame;
        snd_pcm_state_t st = snd_pcm_state(playback->pcm);
        if (st == SND_PCM_STATE_XRUN) {
          logger_warn(&g_logger, "PB: Prepare playback after buffer underrun");
          snd_pcm_prepare(playback->pcm);
          alsa_device_prime_delay(
              playback->pcm, playback->target_level, playback->bufsize,
              playback->sample_rate, playback->blockalign, 0,
              playback->zero_stall_buf, playback->zero_stall_buf_size);
        } else if (st == SND_PCM_STATE_SUSPENDED) {
          alsa_recover_suspended_pcm(playback->pcm, "PB");
          alsa_device_prime_delay(
              playback->pcm, playback->target_level, playback->bufsize,
              playback->sample_rate, playback->blockalign, 0,
              playback->zero_stall_buf, playback->zero_stall_buf_size);
        } else if (st == SND_PCM_STATE_PREPARED) {
          alsa_device_prime_delay(
              playback->pcm, playback->target_level, playback->bufsize,
              playback->sample_rate, playback->blockalign, 0,
              playback->zero_stall_buf, playback->zero_stall_buf_size);
        }

        size_t written_frames = 0;
        while (written_frames < frames) {
          if (atomic_load_explicit(&playback->stopped, memory_order_acquire)) {
            break;
          }
          snd_pcm_sframes_t rc = snd_pcm_writei(
              playback->pcm, local_buf + written_frames * bytes_per_frame,
              frames - written_frames);
          if (rc > 0) {
            written_frames += (size_t)rc;
          } else if (rc == -EPIPE) {
            logger_warn(&g_logger, "PB: write underrun, trying to recover");
            snd_pcm_prepare(playback->pcm);
            alsa_device_prime_delay(
                playback->pcm, playback->target_level, playback->bufsize,
                playback->sample_rate, playback->blockalign,
                frames - written_frames, playback->zero_stall_buf,
                playback->zero_stall_buf_size);
          } else if (rc == -ESTRPIPE) {
            alsa_recover_suspended_pcm(playback->pcm, "PB");
            alsa_device_prime_delay(
                playback->pcm, playback->target_level, playback->bufsize,
                playback->sample_rate, playback->blockalign,
                frames - written_frames, playback->zero_stall_buf,
                playback->zero_stall_buf_size);
          } else if (rc == -EAGAIN) {
            snd_pcm_wait(playback->pcm, 10);
          } else {
            break;
          }
        }
        playback_interrupted = false;
      }
    } else {
      // Ring buffer is empty - check if ALSA buffer is running low
      // (src/alsa_backend/threaded_device.rs:512-550 in upstream CamillaDSP)
      snd_pcm_state_t st = snd_pcm_state(playback->pcm);
      bool buffer_low = false;
      if (st == SND_PCM_STATE_RUNNING) {
        snd_pcm_sframes_t avail = snd_pcm_avail(playback->pcm);
        size_t delay = 0;
        if (avail >= 0 && (snd_pcm_uframes_t)avail <= playback->bufsize) {
          delay = playback->bufsize - (snd_pcm_uframes_t)avail;
        }
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
        // Matching play_buffer in threaded_device.rs: wait for PCM readiness before writing
        snd_pcm_wait(playback->pcm, 20);
        snd_pcm_writei(playback->pcm, playback->zero_stall_buf,
                       playback->chunk_size);
      }
      cdsp_sleep_us(500);
    }
  }

  if (local_buf) free(local_buf);
  if (rt_handle) {
    demote_current_thread_from_realtime(rt_handle);
  }
  return NULL;
}

// Open the ALSA playback device matching open_pcm in upstream
// (src/alsa_backend/device.rs:416-493)
static bool alsa_playback_open(void* ctx, backend_error_t* err) {
  alsa_playback_t* playback = (alsa_playback_t*)ctx;
  if (!playback) return false;
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

  // Set software parameters (src/alsa_backend/device.rs:483-491)
  if (playback->chunk_size > playback->bufsize) {
    char msg[256];
    snprintf(msg, sizeof(msg),
             "Trying to set avail_min to %zu, must be smaller than or equal to "
             "device buffer size of %lu",
             playback->chunk_size, (unsigned long)playback->bufsize);
    logger_error(&g_logger, "%s", msg);
    if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    goto error_cleanup;
  }

  // start on first write of any size (buffermanager.rs:248), avail_min =
  // chunksize
  alsa_device_configure_sw(playback->pcm,
                           (snd_pcm_uframes_t)playback->chunk_size, 1);

  size_t sample_size = alsa_format_sample_size(playback->format);

  playback->bytes_per_sample = sample_size;
  playback->blockalign = (size_t)playback->channels * sample_size;
  playback->interleaved_buf_size =
      2 * playback->chunk_size * playback->blockalign;
  playback->interleaved_buf = calloc(playback->interleaved_buf_size, 1);
  if (!playback->interleaved_buf) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to allocate ALSA playback interleaved buffer");
    goto error_cleanup;
  }

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

  // Search for UAC2 gadget pitch control: "Playback Pitch 1000000"
  // (src/alsa_backend/device.rs:530-544)
  char ctl_name[32];
  int dev_idx = 0;
  int subdev_idx = 0;
  if (alsa_device_get_card_ctl_name(playback->pcm, ctl_name, sizeof(ctl_name),
                                    &dev_idx, &subdev_idx)) {
    snd_hctl_t* hctl = NULL;
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

    snd_mixer_t* mixer = NULL;
    if (snd_mixer_open(&mixer, 0) >= 0) {
      if (snd_mixer_attach(mixer, ctl_name) >= 0 &&
          snd_mixer_selem_register(mixer, NULL, NULL) >= 0 &&
          snd_mixer_load(mixer) >= 0) {
        pthread_mutex_lock(&playback->mixer_mutex);
        playback->mixer = mixer;

        snd_mixer_selem_id_t* sid;
        snd_mixer_selem_id_alloca(&sid);
        snd_mixer_selem_id_set_name(sid, "Playback Pitch 1000000");
        playback->pitch_elem = snd_mixer_find_selem(mixer, sid);
        pthread_mutex_unlock(&playback->mixer_mutex);
      } else {
        snd_mixer_close(mixer);
      }
    }
  }

  if (playback->threaded) {
    size_t ring_frames = alsa_playback_ring_capacity_frames(
        playback->target_level, playback->chunk_size);
    playback->ring_buffer =
        spsc_byte_ring_buffer_create(ring_frames * playback->blockalign);
    if (!playback->ring_buffer) {
      if (err) {
        backend_error_init(
            err, BACKEND_ERROR_INITIALIZATION_FAILED,
            "Failed to allocate SPSC ring buffer for threaded ALSA");
      }
      goto error_cleanup;
    }
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
  }

  pthread_mutex_unlock(&g_alsa_mutex);
  return true;

error_cleanup:
  if (playback->pcm) {
    snd_pcm_close(playback->pcm);
    playback->pcm = NULL;
  }
  if (playback->interleaved_buf) {
    free(playback->interleaved_buf);
    playback->interleaved_buf = NULL;
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
static bool alsa_playback_write(void* ctx, const audio_chunk_t* chunk,
                                backend_error_t* err) {
  alsa_playback_t* playback = (alsa_playback_t*)ctx;
  if (!playback || !playback->pcm) return false;

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
  if (total_frames == 0) return true;

  bool paused = atomic_load_explicit(&playback->paused, memory_order_acquire);
  if (paused) {
    if (playback->can_pause && !playback->currently_paused) {
      snd_pcm_pause(playback->pcm, 1);
      playback->currently_paused = true;
    }
    return true;
  } else {
    if (playback->can_pause && playback->currently_paused) {
      snd_pcm_pause(playback->pcm, 0);
      playback->currently_paused = false;
    }
  }

  if (playback->threaded) {
    return audio_backend_ring_buffer_write(
        playback->ring_buffer, playback->interleaved_buf,
        playback->interleaved_buf_size, playback->blockalign, chunk,
        alsa_pcm_format_to_binary_format(playback->format),
        (size_t)playback->channels, 1, 500, &playback->inner_running,
        &playback->stopped, &playback->paused, NULL, err);
  }

  // Device state check and recovery (src/alsa_backend/device.rs:115-147)
  snd_pcm_state_t playback_state = snd_pcm_state(playback->pcm);
  if ((int)playback_state < 0) {
    logger_error(&g_logger,
                 "PB: Alsa snd_pcm_state() of playback device returned an "
                 "unexpected error: %s",
                 snd_strerror((int)playback_state));
    if (err) {
      backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                         snd_strerror((int)playback_state));
    }
    return false;
  } else if (playback_state == SND_PCM_STATE_XRUN) {
    logger_warn(&g_logger, "PB: Prepare playback after buffer underrun");
    snd_pcm_prepare(playback->pcm);
    sleep_for_target_delay(playback);
  } else if (playback_state == SND_PCM_STATE_SUSPENDED) {
    alsa_recover_suspended_pcm(playback->pcm, "PB");
    sleep_for_target_delay(playback);
  } else if (playback_state == SND_PCM_STATE_PREPARED) {
    logger_info(&g_logger, "PB: Starting playback from Prepared state");
    sleep_for_target_delay(playback);
  } else if (playback_state == SND_PCM_STATE_PAUSED) {
    logger_debug(&g_logger, "PB: Device is in paused state, unpausing.");
    if (playback->can_pause) {
      snd_pcm_pause(playback->pcm, 0);
    }
  } else if (playback_state != SND_PCM_STATE_RUNNING) {
    logger_warn(&g_logger, "PB: device is in an unexpected state: %s",
                alsa_state_desc(playback_state));
  }

  size_t bytes_per_frame = playback->blockalign;
  double millis_per_frame = 1000.0 / (double)playback->sample_rate;
  size_t total_bytes = total_frames * bytes_per_frame;

  if (total_bytes > playback->interleaved_buf_size) {
    if (err) {
      backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                         "Frame count exceeds playback buffer capacity");
    }
    return false;
  }

  if (!audio_chunk_encode_interleaved(
          chunk, alsa_pcm_format_to_binary_format(playback->format),
          (size_t)playback->channels, total_frames,
          playback->interleaved_buf)) {
    if (err) {
      backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                         "Failed to encode audio chunk");
    }
    return false;
  }

  char* buf_ptr = (char*)playback->interleaved_buf;
  size_t remaining_frames = total_frames;
  int retry_count = 0;

  // Write loop matching play_buffer in upstream
  // (src/alsa_backend/device.rs:151-238)
  while (remaining_frames > 0) {
    if (atomic_load_explicit(&playback->stopped, memory_order_acquire)) {
      if (err) {
        backend_error_init(err, BACKEND_ERROR_NONE, "Playback stopped");
      }
      return false;
    }

    retry_count++;
    if (retry_count >= 100) {
      logger_warn(&g_logger, "PB: giving up after %d write attempts",
                  retry_count);
      if (err) {
        backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                           "Aborting playback after too many write attempts");
      }
      return false;
    }

    uint32_t timeout_millis =
        (uint32_t)(2.0 * millis_per_frame * (double)remaining_frames);
    if (timeout_millis < 20) timeout_millis = 20;

    int err_wait = snd_pcm_wait(playback->pcm, (int)timeout_millis);
    if (err_wait == 0) {
      // Device stalled: drop, prepare, and prefill stall-check zeros
      // (src/alsa_backend/device.rs:617-640)
      logger_trace(
          &g_logger,
          "PB: Wait timed out, playback device takes too long to drain buffer");
      if (!playback->device_stalled) {
        logger_info(&g_logger, "PB: device stalled");
        snd_pcm_drop(playback->pcm);
        snd_pcm_prepare(playback->pcm);
        snd_pcm_uframes_t frames_to_stall =
            (playback->bufsize >= playback->chunk_size)
                ? (playback->bufsize - playback->chunk_size + 1)
                : 1;
        snd_pcm_sframes_t sw_rc = snd_pcm_writei(
            playback->pcm, playback->zero_stall_buf, frames_to_stall);
        if (sw_rc < 0) {
          logger_warn(&g_logger, "PB: Writing stall-check zeros failed with %s",
                      snd_strerror((int)sw_rc));
        } else {
          logger_trace(&g_logger, "PB: Wrote %ld zero frames", (long)sw_rc);
        }
        playback->device_stalled = true;
      }
      return true;
    } else if (err_wait < 0) {
      if (err_wait == -EPIPE) {
        logger_warn(&g_logger,
                    "PB: wait underrun, trying to recover. Error: %s",
                    snd_strerror(err_wait));
        snd_pcm_prepare(playback->pcm);
      } else if (err_wait == -ESTRPIPE ||
                 snd_pcm_state(playback->pcm) == SND_PCM_STATE_SUSPENDED) {
        logger_warn(
            &g_logger,
            "PB: wait interrupted by suspend, trying to recover. Error: %s",
            snd_strerror(err_wait));
        alsa_recover_suspended_pcm(playback->pcm, "PB");
        sleep_for_target_delay(playback);
      } else {
        logger_warn(&g_logger,
                    "PB: device failed while waiting for available buffer "
                    "space, error: %s",
                    snd_strerror(err_wait));
        if (err) {
          backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                             snd_strerror(err_wait));
        }
        return false;
      }
    }

    snd_pcm_sframes_t rc =
        snd_pcm_writei(playback->pcm, buf_ptr, remaining_frames);
    if (rc > 0) {
      size_t written = (size_t)rc;
      if (playback->device_stalled) {
        logger_info(&g_logger, "PB: device resumed normal operation");
        playback->device_stalled = false;
      }
      if (written == remaining_frames) {
        logger_trace(&g_logger,
                     "PB: wrote %zu frames to playback device as requested",
                     written);
        break;
      } else {
        logger_trace(&g_logger,
                     "PB: wrote %zu instead of requested %zu, trying again to "
                     "write the rest",
                     written, remaining_frames);
        buf_ptr += written * bytes_per_frame;
        remaining_frames -= written;
        continue;
      }
    } else if (rc < 0) {
      int err_write = (int)rc;
      if (err_write == -EAGAIN || err_write == 0) {
        logger_trace(&g_logger,
                     "PB: encountered EAGAIN error on write, trying again");
        continue;
      } else if (err_write == -EPIPE) {
        logger_warn(&g_logger,
                    "PB: write underrun, trying to recover. Error: %s",
                    snd_strerror(err_write));
        snd_pcm_prepare(playback->pcm);
        sleep_for_target_delay(playback);
        snd_pcm_sframes_t retry_rc =
            snd_pcm_writei(playback->pcm, buf_ptr, remaining_frames);
        if (retry_rc < 0) {
          if (err) {
            backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                               snd_strerror((int)retry_rc));
          }
          return false;
        }
        size_t retry_written = (size_t)retry_rc;
        if (retry_written >= remaining_frames) {
          break;
        }
        buf_ptr += retry_written * bytes_per_frame;
        remaining_frames -= retry_written;
        continue;
      } else if (err_write == -ESTRPIPE ||
                 snd_pcm_state(playback->pcm) == SND_PCM_STATE_SUSPENDED) {
        logger_warn(
            &g_logger,
            "PB: write interrupted by suspend, trying to recover. Error: %s",
            snd_strerror(err_write));
        alsa_recover_suspended_pcm(playback->pcm, "PB");
        sleep_for_target_delay(playback);
        continue;
      } else {
        logger_warn(&g_logger, "PB: write failed, error: %s",
                    snd_strerror(err_write));
        if (err) {
          backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                             snd_strerror(err_write));
        }
        return false;
      }
    }
  }

  return true;
}

// Close the ALSA playback device
static void alsa_playback_close(void* ctx) {
  alsa_playback_t* playback = (alsa_playback_t*)ctx;
  if (!playback) return;

  atomic_store_explicit(&playback->stopped, true, memory_order_release);

  if (playback->threaded &&
      atomic_load_explicit(&playback->inner_running, memory_order_acquire)) {
    pthread_join(playback->inner_thread, NULL);
    atomic_store_explicit(&playback->inner_running, false,
                          memory_order_release);
    if (playback->ring_buffer) {
      spsc_byte_ring_buffer_free(playback->ring_buffer);
      playback->ring_buffer = NULL;
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
  if (playback->interleaved_buf) {
    free(playback->interleaved_buf);
    playback->interleaved_buf = NULL;
  }
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
static size_t alsa_playback_get_buffer_level(void* ctx) {
  alsa_playback_t* playback = (alsa_playback_t*)ctx;
  if (!playback) return 0;
  if (playback->threaded) {
    if (!playback->ring_buffer || playback->blockalign == 0) return 0;
    return spsc_byte_ring_buffer_get_available_to_read(playback->ring_buffer) /
           playback->blockalign;
  }
  if (!playback->pcm) return 0;
  snd_pcm_sframes_t avail = snd_pcm_avail(playback->pcm);
  if (avail >= 0 && (snd_pcm_uframes_t)avail <= playback->bufsize) {
    return (size_t)(playback->bufsize - (snd_pcm_uframes_t)avail);
  }
  return 0;
}

static bool alsa_playback_get_pending_rate_change(void* ctx, double* out_rate) {
  alsa_playback_t* playback = (alsa_playback_t*)ctx;
  if (!playback) return false;
  pthread_mutex_lock(&g_alsa_mutex);
  bool pending = playback->has_pending_rate;
  if (pending) {
    if (out_rate) *out_rate = playback->pending_rate;
    playback->has_pending_rate = false;
  }
  pthread_mutex_unlock(&g_alsa_mutex);
  return pending;
}

static bool alsa_playback_prefill_silence(void* ctx, size_t frames,
                                          backend_error_t* err) {
  (void)err;
  alsa_playback_t* playback = (alsa_playback_t*)ctx;
  if (!playback || frames == 0) return true;

  if (playback->threaded) {
    if (!playback->ring_buffer) return true;
    size_t bytes = frames * playback->blockalign;
    uint8_t zero_buf[512] = {0};
    if (alsa_is_dsd_format(playback->format)) {
      memset(zero_buf, 0x69, sizeof(zero_buf));
    }
    while (bytes > 0) {
      size_t chunk_bytes = bytes < sizeof(zero_buf) ? bytes : sizeof(zero_buf);
      size_t written = spsc_byte_ring_buffer_write(playback->ring_buffer,
                                                   zero_buf, chunk_bytes);
      if (written == 0) break;
      bytes -= written;
    }
    return true;
  }

  if (!playback->pcm) return true;
  uint8_t zero_buf[512] = {0};
  if (alsa_is_dsd_format(playback->format)) {
    memset(zero_buf, 0x69, sizeof(zero_buf));
  }
  size_t written_frames = 0;
  while (written_frames < frames) {
    size_t max_chunk_frames = sizeof(zero_buf) / playback->blockalign;
    if (max_chunk_frames == 0) max_chunk_frames = 1;
    size_t chunk = (frames - written_frames) < max_chunk_frames
                       ? (frames - written_frames)
                       : max_chunk_frames;
    snd_pcm_sframes_t rc = snd_pcm_writei(playback->pcm, zero_buf, chunk);
    if (rc > 0) {
      written_frames += (size_t)rc;
    } else {
      break;
    }
  }
  return true;
}

static bool alsa_playback_get_is_paused(void* ctx) {
  alsa_playback_t* playback = (alsa_playback_t*)ctx;
  if (!playback) return false;
  return atomic_load_explicit(&playback->paused, memory_order_acquire);
}

static void alsa_playback_set_is_paused(void* ctx, bool paused) {
  alsa_playback_t* playback = (alsa_playback_t*)ctx;
  if (!playback) return;
  atomic_store_explicit(&playback->paused, paused, memory_order_release);
}

static bool alsa_playback_pitch_control_supported(void* ctx) {
  alsa_playback_t* playback = (alsa_playback_t*)ctx;
  if (!playback) return false;
  pthread_mutex_lock(&playback->mixer_mutex);
  bool res = playback->hctl_pitch_elem != NULL || playback->pitch_elem != NULL;
  pthread_mutex_unlock(&playback->mixer_mutex);
  return res;
}

// Set pitch control matching upstream (src/alsa_backend/device.rs:673-678)
// Note: speed is reciprocal on playback side: (1_000_000.0 / capture_speed) as
// i32
static void alsa_playback_set_pitch(void* ctx, double multiplier) {
  alsa_playback_t* playback = (alsa_playback_t*)ctx;
  if (!playback || multiplier <= 0.0) return;
  pthread_mutex_lock(&playback->mixer_mutex);
  long value = (long)round(1000000.0 / multiplier);
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

static void alsa_playback_stop(void* ctx) {
  alsa_playback_t* playback = (alsa_playback_t*)ctx;
  if (!playback) return;
  atomic_store_explicit(&playback->stopped, true, memory_order_release);
  pthread_mutex_lock(&g_alsa_mutex);
  if (playback->pcm) {
    snd_pcm_drop(playback->pcm);
  }
  pthread_mutex_unlock(&g_alsa_mutex);
}

static void alsa_playback_destroy(void* ctx) {
  alsa_playback_t* playback = (alsa_playback_t*)ctx;
  if (!playback) return;
  alsa_playback_close(playback);
  pthread_mutex_destroy(&playback->mixer_mutex);
  free(playback);
}

// Create ALSA playback backend matching AlsaPlaybackDevice::start in upstream
// (src/alsa_backend/device.rs:1144-1215 and
// src/alsa_backend/threaded_device.rs:1055-1075)
static playback_backend_t* alsa_playback_create(
    const playback_device_config_t* config, int sample_rate, int chunk_size,
    bool full_duplex, processing_parameters_t* params, backend_error_t* err) {
  (void)full_duplex;
  (void)err;
  alsa_playback_t* playback =
      (alsa_playback_t*)calloc(1, sizeof(alsa_playback_t));
  if (!playback) return NULL;

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
  playback->threaded =
      config->cfg.alsa.has_threaded ? config->cfg.alsa.threaded : false;
  pthread_mutex_init(&playback->mixer_mutex, NULL);

  playback_backend_t* backend =
      (playback_backend_t*)calloc(1, sizeof(playback_backend_t));
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
    .stop = alsa_playback_stop,
    .destroy = alsa_playback_destroy};

#endif  // defined(ENABLE_ALSA)
