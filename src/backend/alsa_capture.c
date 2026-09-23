#include "backend/alsa_capture.h"

#if defined(ENABLE_ALSA)
#include <alsa/asoundlib.h>
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/audio_chunk.h"
#include "audio/processing_parameters.h"
#include "backend/alsa_device.h"
#include "backend/audio_backend.h"
#include "backend/backend_error.h"
#include "config/config_gen.h"
#include "engine/cdsp_sem.h"
#include "engine/thread_priority.h"
#include "logging/app_logger.h"
#include "utils/lock_free_ring_buffer.h"

static const logger_t g_logger = {"dsp.backend.alsa"};

struct alsa_capture {
  char device_name[256];
  int sample_rate;         // pipeline rate
  int capture_sample_rate; // hardware capture rate
  size_t channels;
  int chunk_size;
  snd_pcm_uframes_t bufsize;
  snd_pcm_uframes_t period;

  bool has_format;
  alsa_sample_format_t requested_format;
  bool stop_on_inactive;
  char link_volume_control[256];
  char link_mute_control[256];

  processing_parameters_t *params;
  snd_ctl_t *ctl;
  snd_hctl_t *hctl;
  snd_hctl_elem_t *hctl_pitch_elem;
  snd_hctl_elem_t *hctl_rate_elem;
  snd_hctl_elem_t *hctl_loopback_active_elem;
  snd_hctl_elem_t *hctl_volume_elem;
  snd_hctl_elem_t *hctl_mute_elem;

  unsigned int loopback_active_numid;
  unsigned int gadget_rate_numid;
  unsigned int volume_numid;
  unsigned int mute_numid;

  bool pitch_is_loopback;
  _Atomic double pending_rate;
  _Atomic bool has_pending_rate_change;
  _Atomic bool is_inactive;

  double linked_volume_value;
  bool has_linked_volume_value;
  bool linked_mute_value;
  bool has_linked_mute_value;

  snd_pcm_t *pcm;
  snd_pcm_format_t format;

  pthread_mutex_t mixer_mutex;
  _Atomic bool stopped;

  spsc_byte_ring_buffer_t *ring_buffer;
  cdsp_sem_t semaphore;
  pthread_t inner_thread;
  bool inner_thread_created;
  _Atomic bool inner_running;
  bool device_stalled;
};

// Defined below; invoked from the capture RT thread whenever poll() reports
// activity on a control descriptor.
static void alsa_capture_process_events(alsa_capture_t *capture);

// Maximum number of poll descriptors collected from the PCM and control
// handles. ALSA devices realistically expose one or two each.
#define ALSA_CAPTURE_MAX_POLL_FDS 32

// Collects the poll descriptors of the PCM handle followed by those of the
// control handles, matching upstream's FileDescriptors { fds, nbr_pcm_fds }
// (src/alsa_backend/utils.rs:532-535 &
// src/alsa_backend/threaded_device.rs:1442-1482).
static int alsa_capture_collect_poll_fds(alsa_capture_t *capture,
                                         struct pollfd *pfds, int max_fds,
                                         int *out_nbr_pcm_fds) {
  int total = 0;
  *out_nbr_pcm_fds = 0;

  int pcm_count = snd_pcm_poll_descriptors_count(capture->pcm);
  if (pcm_count <= 0 || pcm_count > max_fds) {
    return 0;
  }
  int got =
      snd_pcm_poll_descriptors(capture->pcm, pfds, (unsigned int)pcm_count);
  if (got <= 0) {
    return 0;
  }
  total = got;
  *out_nbr_pcm_fds = got;

  // Control descriptors are appended after the PCM descriptors so the
  // PCM/control split can be recovered from the index alone after poll().
  if (capture->ctl) {
    int ctl_count = snd_ctl_poll_descriptors_count(capture->ctl);
    if (ctl_count > 0 && total + ctl_count <= max_fds) {
      int n = snd_ctl_poll_descriptors(capture->ctl, pfds + total,
                                       (unsigned int)ctl_count);
      if (n > 0)
        total += n;
    }
  }
  if (capture->hctl) {
    int hctl_count = snd_hctl_poll_descriptors_count(capture->hctl);
    if (hctl_count > 0 && total + hctl_count <= max_fds) {
      int n = snd_hctl_poll_descriptors(capture->hctl, pfds + total,
                                        (unsigned int)hctl_count);
      if (n > 0)
        total += n;
    }
  }
  return total;
}

// Dedicated real-time capture inner thread matching AlsaCaptureInner in
// upstream (src/alsa_backend/threaded_device.rs:1368-1540)
static void *alsa_capture_inner_thread_func(void *arg) {
  alsa_capture_t *capture = (alsa_capture_t *)arg;
  realtime_thread_handle_t *rt_handle = promote_current_thread_to_realtime(
      "AlsaCaptureInner", (size_t)capture->chunk_size,
      (size_t)capture->capture_sample_rate);
  if (rt_handle) {
    logger_debug(&g_logger, "Capture inner thread has real-time priority.");
  }

  size_t sample_bytes = alsa_format_sample_size(capture->format);
  size_t bytes_per_frame = (size_t)capture->channels * sample_bytes;
  size_t chunk_bytes = (size_t)capture->chunk_size * bytes_per_frame;
  uint8_t *local_buf = (uint8_t *)malloc(chunk_bytes);
  if (!local_buf) {
    logger_error(&g_logger, "Failed to allocate ALSA capture read buffer");
    atomic_store_explicit(&capture->inner_running, false, memory_order_release);
    if (capture->semaphore) {
      cdsp_sem_signal(capture->semaphore);
    }
    if (rt_handle) {
      demote_current_thread_from_realtime(rt_handle);
    }
    return NULL;
  }
  double millis_per_chunk = 1000.0 * (double)capture->chunk_size /
                            (double)capture->capture_sample_rate;
  uint32_t timeout_millis = (uint32_t)(8.0 * millis_per_chunk);
  if (timeout_millis < 20)
    timeout_millis = 20;

  // Collect the descriptor set once; it stays valid for the lifetime of the
  // PCM and control handles.
  struct pollfd pfds[ALSA_CAPTURE_MAX_POLL_FDS];
  int nbr_pcm_fds = 0;
  int total_fds = alsa_capture_collect_poll_fds(
      capture, pfds, ALSA_CAPTURE_MAX_POLL_FDS, &nbr_pcm_fds);
  if (total_fds <= 0) {
    // Upstream builds FileDescriptors unconditionally and propagates the
    // error if the descriptors cannot be obtained
    // (src/alsa_backend/threaded_device.rs:1442-1482).
    logger_error(&g_logger, "Failed to get ALSA capture poll descriptors");
    free(local_buf);
    atomic_store_explicit(&capture->inner_running, false, memory_order_release);
    if (capture->semaphore) {
      cdsp_sem_signal(capture->semaphore);
    }
    if (rt_handle) {
      demote_current_thread_from_realtime(rt_handle);
    }
    return NULL;
  }

  while (!atomic_load_explicit(&capture->stopped, memory_order_acquire)) {
    snd_pcm_state_t capture_state = snd_pcm_state(capture->pcm);
    if ((int)capture_state < 0) {
      logger_error(&g_logger, "Capture device error state: %s",
                   snd_strerror((int)capture_state));
      break;
    }
    if (capture_state == SND_PCM_STATE_XRUN) {
      logger_warn(&g_logger, "Prepare capture device");
      if (snd_pcm_prepare(capture->pcm) < 0) {
        logger_error(&g_logger, "Failed to restart capture device after XRUN");
        break;
      }
    } else if (capture_state == SND_PCM_STATE_SUSPENDED) {
      if (alsa_recover_suspended_pcm(capture->pcm, "Capture") < 0) {
        logger_error(&g_logger,
                     "Failed to restart capture device after suspension");
        break;
      }
      // A successful resume leaves the device RUNNING; starting it again
      // would fail with -EBADFD.
      if (snd_pcm_state(capture->pcm) != SND_PCM_STATE_RUNNING &&
          snd_pcm_start(capture->pcm) < 0) {
        logger_error(&g_logger,
                     "Failed to restart capture device after suspension");
        break;
      }
    } else if (capture_state != SND_PCM_STATE_RUNNING) {
      logger_debug(&g_logger, "Starting capture from state: %s",
                   alsa_state_desc((int)capture_state));
      if (snd_pcm_start(capture->pcm) < 0) {
        logger_error(&g_logger, "Failed to start capture device");
        break;
      }
    }

    // Wait for the device, servicing control events as they arrive.
    // The wait is sliced so a stop request is observed within 20 ms even when
    // the full stall timeout is much longer
    // (src/alsa_backend/threaded_device.rs:778-840).
    bool pcm_ready = false;
    bool wait_timed_out = false;
    bool wait_fatal = false;
    uint32_t remaining_millis = timeout_millis;
    for (;;) {
      if (atomic_load_explicit(&capture->stopped, memory_order_acquire)) {
        break;
      }
      int poll_slice = remaining_millis < 20 ? (int)remaining_millis : 20;
      for (int i = 0; i < total_fds; i++) {
        pfds[i].revents = 0;
      }
      int nbr_ready = poll(pfds, (nfds_t)total_fds, poll_slice);
      if (nbr_ready < 0) {
        if (errno == EINTR)
          continue;
        logger_error(&g_logger, "Capture poll fatal error: %s",
                     strerror(errno));
        wait_fatal = true;
        break;
      }
      if (nbr_ready == 0) {
        if (remaining_millis <= (uint32_t)poll_slice) {
          wait_timed_out = true;
          break;
        }
        remaining_millis -= (uint32_t)poll_slice;
        continue;
      }

      int nbr_found = 0;
      for (int i = 0; i < nbr_pcm_fds; i++) {
        if (pfds[i].revents != 0) {
          pcm_ready = true;
          nbr_found++;
        }
      }
      // There were other ready file descriptors than PCM, must be controls
      // (src/alsa_backend/utils.rs:565-570).
      if (nbr_found < nbr_ready) {
        alsa_capture_process_events(capture);
      }
      if (pcm_ready) {
        break;
      }
      remaining_millis = remaining_millis > (uint32_t)poll_slice
                             ? remaining_millis - (uint32_t)poll_slice
                             : 0;
    }

    if (wait_fatal) {
      break;
    }
    if (wait_timed_out) {
      if (!capture->device_stalled) {
        logger_info(&g_logger,
                    "Capture device is stalled, processing is stalled");
        capture->device_stalled = true;
      }
      continue;
    }
    if (!pcm_ready) {
      // Interrupted by a stop request.
      continue;
    }

    snd_pcm_sframes_t frames_read = snd_pcm_readi(
        capture->pcm, local_buf, (snd_pcm_uframes_t)capture->chunk_size);
    if (frames_read > 0) {
      if (capture->device_stalled) {
        capture->device_stalled = false;
      }
      size_t bytes_read = (size_t)frames_read * bytes_per_frame;
      size_t pushed = spsc_byte_ring_buffer_write(capture->ring_buffer,
                                                  local_buf, bytes_read);
      if (pushed < bytes_read) {
        logger_warn(&g_logger,
                    "Capture ring buffer is full, dropped %zu out of %zu bytes",
                    bytes_read - pushed, bytes_read);
      }
      if (capture->semaphore) {
        cdsp_sem_signal(capture->semaphore);
      }
    } else if (frames_read == -EPIPE) {
      logger_warn(&g_logger, "Capture: read overrun, trying to recover");
      if (snd_pcm_prepare(capture->pcm) < 0) {
        break;
      }
    } else if (frames_read == -ESTRPIPE) {
      logger_warn(&g_logger,
                  "Capture: read interrupted by suspend, trying to recover");
      if (alsa_recover_suspended_pcm(capture->pcm, "Capture") < 0) {
        break;
      }
      if (snd_pcm_state(capture->pcm) != SND_PCM_STATE_RUNNING &&
          snd_pcm_start(capture->pcm) < 0) {
        break;
      }
    } else if (frames_read == 0) {
      if (!capture->device_stalled) {
        logger_info(&g_logger,
                    "Capture device is stalled, processing is stalled");
        capture->device_stalled = true;
      }
    } else if (frames_read == -EAGAIN || frames_read == -EINTR) {
      // Upstream's capture_buffer reports -EAGAIN/-EINTR as
      // ordinary transient conditions. Keep polling.
    } else {
      logger_error(&g_logger, "Capture read fatal error: %s",
                   snd_strerror((int)frames_read));
      break;
    }
  }

  atomic_store_explicit(&capture->inner_running, false, memory_order_release);
  if (capture->semaphore) {
    cdsp_sem_signal(capture->semaphore);
  }
  if (local_buf)
    free(local_buf);
  if (rt_handle) {
    demote_current_thread_from_realtime(rt_handle);
  }
  return NULL;
}

// Initialize ALSA control elements matching find_elements in upstream
// (src/alsa_backend/utils.rs:723-756 & src/alsa_backend/device.rs:788-846)
static void alsa_capture_init_controls(alsa_capture_t *capture) {
  if (!capture->pcm)
    return;

  char ctl_name[32];
  int dev_idx = 0;
  int subdev_idx = 0;
  if (!alsa_device_get_card_ctl_name(capture->pcm, ctl_name, sizeof(ctl_name),
                                     &dev_idx, &subdev_idx)) {
    return;
  }

  pthread_mutex_lock(&capture->mixer_mutex);

  // Open Ctl interface (non-blocking) and subscribe to events
  // (device.rs:790-794)
  snd_ctl_t *ctl = NULL;
  if (snd_ctl_open(&ctl, ctl_name, SND_CTL_NONBLOCK) >= 0 && ctl) {
    capture->ctl = ctl;
    snd_ctl_subscribe_events(ctl, 1);
  }

  // Open HCtl interface in non-blocking mode (device.rs:789)
  snd_hctl_t *hctl = NULL;
  if (snd_hctl_open(&hctl, ctl_name, SND_CTL_NONBLOCK) >= 0 && hctl) {
    snd_hctl_nonblock(hctl, 1);
    if (snd_hctl_load(hctl) >= 0) {
      capture->hctl = hctl;

      // Look up pitch control: PCM Rate Shift 100000 / Capture Pitch 1000000
      capture->hctl_pitch_elem =
          alsa_find_elem(hctl, SND_CTL_ELEM_IFACE_PCM, dev_idx, subdev_idx,
                         "PCM Rate Shift 100000", NULL);
      if (capture->hctl_pitch_elem) {
        capture->pitch_is_loopback = true;
        logger_info(&g_logger, "Capture device supports rate adjust");
      } else {
        capture->hctl_pitch_elem =
            alsa_find_elem(hctl, SND_CTL_ELEM_IFACE_PCM, dev_idx, subdev_idx,
                           "Capture Pitch 1000000", NULL);
        if (capture->hctl_pitch_elem) {
          capture->pitch_is_loopback = false;
          logger_info(&g_logger, "Capture device supports rate adjust");
        }
      }

      // Look up PCM Slave Active (loopback active)
      capture->hctl_loopback_active_elem =
          alsa_find_elem(hctl, SND_CTL_ELEM_IFACE_PCM, dev_idx, subdev_idx,
                         "PCM Slave Active", &capture->loopback_active_numid);

      // Look up Capture Rate (gadget rate)
      capture->hctl_rate_elem =
          alsa_find_elem(hctl, SND_CTL_ELEM_IFACE_PCM, dev_idx, subdev_idx,
                         "Capture Rate", &capture->gadget_rate_numid);

      // Look up Mixer Volume Control
      if (capture->link_volume_control[0]) {
        capture->hctl_volume_elem = alsa_find_elem(
            hctl, SND_CTL_ELEM_IFACE_MIXER, -1, -1,
            capture->link_volume_control, &capture->volume_numid);
        if (capture->hctl_volume_elem && capture->ctl) {
          double vol_db = 0.0;
          if (alsa_elem_read_volume_in_db(capture->ctl,
                                          capture->hctl_volume_elem, &vol_db)) {
            logger_info(&g_logger, "Using initial volume from Alsa: %.2f dB",
                        vol_db);
            capture->linked_volume_value = vol_db;
            capture->has_linked_volume_value = true;
            if (capture->params) {
              processing_parameters_set_target_volume(capture->params, vol_db);
            }
          }
        }
      }

      // Look up Mixer Mute Control
      if (capture->link_mute_control[0]) {
        capture->hctl_mute_elem =
            alsa_find_elem(hctl, SND_CTL_ELEM_IFACE_MIXER, -1, -1,
                           capture->link_mute_control, &capture->mute_numid);
        if (capture->hctl_mute_elem) {
          bool active = false;
          if (alsa_elem_read_as_bool(capture->hctl_mute_elem, &active)) {
            logger_info(&g_logger, "Using initial active switch from Alsa: %d",
                        active);
            capture->linked_mute_value = !active;
            capture->has_linked_mute_value = true;
            if (capture->params) {
              processing_parameters_set_muted(capture->params, !active);
            }
          }
        }
      }
    } else {
      snd_hctl_close(hctl);
    }
  }

  pthread_mutex_unlock(&capture->mixer_mutex);
}

// Sync linked controls to ALSA hardware matching sync_linked_controls in
// upstream (src/alsa_backend/utils.rs:782-808)
static void alsa_capture_sync_linked_controls(alsa_capture_t *capture) {
  if (!capture->params)
    return;
  pthread_mutex_lock(&capture->mixer_mutex);

  if (capture->ctl && capture->hctl_volume_elem &&
      capture->has_linked_volume_value) {
    double target_vol =
        processing_parameters_get_target_volume(capture->params);
    if (fabs(capture->linked_volume_value - target_vol) > 0.1) {
      logger_debug(&g_logger, "Updating linked volume control to %.2f dB",
                   target_vol);
    }
    alsa_elem_write_volume_in_db(capture->ctl, capture->hctl_volume_elem,
                                 target_vol);
  }

  if (capture->hctl_mute_elem && capture->has_linked_mute_value) {
    bool target_mute = processing_parameters_is_muted(capture->params);
    if (capture->linked_mute_value != target_mute) {
      logger_debug(&g_logger, "Updating linked switch control to %d",
                   !target_mute);
      alsa_elem_write_as_bool(capture->hctl_mute_elem, !target_mute);
      capture->linked_mute_value = target_mute;
    }
  }

  pthread_mutex_unlock(&capture->mixer_mutex);
}

// Process events from ALSA control interface matching process_events &
// get_event_action in upstream (src/alsa_backend/utils.rs:574-721)
static void alsa_capture_process_events(alsa_capture_t *capture) {
  if (!capture->ctl && !capture->hctl)
    return;
  pthread_mutex_lock(&capture->mixer_mutex);

  snd_ctl_event_t *event;
  snd_ctl_event_alloca(&event);

  while (capture->ctl && snd_ctl_read(capture->ctl, event) > 0) {
    if (snd_ctl_event_get_type(event) != SND_CTL_EVENT_ELEM) {
      continue;
    }
    unsigned int numid = snd_ctl_event_elem_get_numid(event);
    logger_debug(&g_logger, "Event from numid %u", numid);

    // Loopback active event
    if (capture->hctl_loopback_active_elem &&
        numid == capture->loopback_active_numid) {
      bool active = false;
      if (alsa_elem_read_as_bool(capture->hctl_loopback_active_elem, &active)) {
        logger_debug(&g_logger, "Loopback active: %d", active);
        if (!active) {
          if (capture->stop_on_inactive) {
            logger_debug(&g_logger, "Stopping, capture device is inactive and "
                                    "stop_on_inactive is set to true");
            atomic_store_explicit(&capture->is_inactive, true,
                                  memory_order_release);
          }
        } else {
          atomic_store_explicit(&capture->is_inactive, false,
                                memory_order_release);
        }
      }
    }

    // Gadget rate event
    if (capture->hctl_rate_elem && numid == capture->gadget_rate_numid) {
      long rate = 0;
      if (alsa_elem_read_as_int(capture->hctl_rate_elem, &rate)) {
        logger_debug(&g_logger, "Gadget rate: %ld", rate);
        if (rate == 0) {
          if (capture->stop_on_inactive) {
            logger_debug(&g_logger, "Stopping, capture device is inactive and "
                                    "stop_on_inactive is set to true");
            atomic_store_explicit(&capture->is_inactive, true,
                                  memory_order_release);
          }
        } else if (rate > 0 && rate != capture->capture_sample_rate) {
          logger_debug(&g_logger,
                       "Stopping, capture device sample format changed");
          atomic_store_explicit(&capture->pending_rate, (double)rate,
                                memory_order_release);
          atomic_store_explicit(&capture->has_pending_rate_change, true,
                                memory_order_release);
          atomic_store_explicit(&capture->is_inactive, false,
                                memory_order_release);
        } else {
          logger_debug(&g_logger,
                       "Capture device resumed with unchanged sample rate");
        }
      }
    }

    // Volume control event
    if (capture->hctl_volume_elem && numid == capture->volume_numid) {
      double vol_db = 0.0;
      if (alsa_elem_read_volume_in_db(capture->ctl, capture->hctl_volume_elem,
                                      &vol_db)) {
        logger_debug(&g_logger,
                     "Alsa volume change event, set main fader to %.2f dB",
                     vol_db);
        capture->linked_volume_value = vol_db;
        capture->has_linked_volume_value = true;
        if (capture->params) {
          processing_parameters_set_target_volume(capture->params, vol_db);
        }
      }
    }

    // Mute control event
    if (capture->hctl_mute_elem && numid == capture->mute_numid) {
      bool active = false;
      if (alsa_elem_read_as_bool(capture->hctl_mute_elem, &active)) {
        logger_debug(&g_logger, "Alsa mute change event, set mute state to %d",
                     !active);
        capture->linked_mute_value = !active;
        capture->has_linked_mute_value = true;
        if (capture->params) {
          processing_parameters_set_muted(capture->params, !active);
        }
      }
    }
  }

  if (capture->hctl) {
    snd_hctl_handle_events(capture->hctl);
  }

  pthread_mutex_unlock(&capture->mixer_mutex);
}

// Open the ALSA capture device matching open_pcm in upstream
// (src/alsa_backend/device.rs:416-493)
static bool alsa_capture_open(void *ctx, backend_error_t *err) {
  alsa_capture_t *capture = (alsa_capture_t *)ctx;
  if (!capture)
    return false;
  pthread_mutex_lock(&g_alsa_mutex);
  if (capture->pcm != NULL) {
    pthread_mutex_unlock(&g_alsa_mutex);
    return true;
  }

  double resampling_ratio = (capture->capture_sample_rate > 0)
                                ? ((double)capture->sample_rate /
                                   (double)capture->capture_sample_rate)
                                : 1.0;

  char error_msg[256] = {0};
  int rc = alsa_device_open_and_configure_hw(
      &capture->pcm, capture->device_name, SND_PCM_STREAM_CAPTURE,
      capture->channels, (unsigned int)capture->capture_sample_rate,
      capture->has_format, capture->requested_format,
      (size_t)capture->chunk_size, resampling_ratio, &capture->format,
      &capture->bufsize, &capture->period, NULL, error_msg, sizeof(error_msg));
  if (rc < 0) {
    if (err) {
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         error_msg[0] ? error_msg : snd_strerror(rc));
    }
    pthread_mutex_unlock(&g_alsa_mutex);
    return false;
  }

  // Set software parameters (threaded_buffermanager.rs:106-122)
  snd_pcm_uframes_t capture_avail_min =
      capture->period > 0 ? (snd_pcm_uframes_t)capture->period : 1;
  if (capture_avail_min > (snd_pcm_uframes_t)capture->bufsize) {
    char msg[256];
    snprintf(msg, sizeof(msg),
             "Trying to set avail_min to %lu, must be smaller than or equal to "
             "device buffer size of %lu",
             (unsigned long)capture_avail_min, (unsigned long)capture->bufsize);
    logger_error(&g_logger, "%s", msg);
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    goto error_cleanup;
  }
  int sw_rc = alsa_device_configure_sw(capture->pcm, capture_avail_min, 0);
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

  size_t sample_size = alsa_format_sample_size(capture->format);

  alsa_capture_init_controls(capture);

  size_t ring_frames = alsa_capture_ring_capacity_frames(
      (size_t)capture->chunk_size, capture->period);
  capture->ring_buffer = spsc_byte_ring_buffer_create(
      ring_frames * (size_t)capture->channels * sample_size);
  if (!capture->ring_buffer) {
    if (err) {
      backend_error_init(
          err, BACKEND_ERROR_INITIALIZATION_FAILED,
          "Failed to allocate SPSC ring buffer for threaded ALSA capture");
    }
    goto error_cleanup;
  }
  capture->semaphore = cdsp_sem_create();
  if (!capture->semaphore) {
    if (err) {
      backend_error_init(
          err, BACKEND_ERROR_INITIALIZATION_FAILED,
          "Failed to allocate semaphore for threaded ALSA capture");
    }
    goto error_cleanup;
  }
  atomic_store_explicit(&capture->inner_running, true, memory_order_release);
  if (pthread_create(&capture->inner_thread, NULL,
                     alsa_capture_inner_thread_func, capture) != 0) {
    atomic_store_explicit(&capture->inner_running, false, memory_order_release);
    if (err) {
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to spawn ALSA capture inner thread");
    }
    goto error_cleanup;
  }
  capture->inner_thread_created = true;

  pthread_mutex_unlock(&g_alsa_mutex);
  return true;

error_cleanup:
  if (capture->ring_buffer) {
    spsc_byte_ring_buffer_free(capture->ring_buffer);
    capture->ring_buffer = NULL;
  }
  if (capture->semaphore) {
    cdsp_sem_destroy(capture->semaphore);
    capture->semaphore = NULL;
  }
  if (capture->pcm) {
    snd_pcm_close(capture->pcm);
    capture->pcm = NULL;
  }
  pthread_mutex_unlock(&g_alsa_mutex);
  return false;
}

// Capture a buffer matching capture_buffer in upstream
// (src/alsa_backend/device.rs:243-413 and
// src/alsa_backend/threaded_device.rs:1350-1650)
static bool alsa_capture_read(void *ctx, size_t frames, audio_chunk_t *chunk,
                              backend_error_t *err) {
  alsa_capture_t *capture = (alsa_capture_t *)ctx;
  if (!capture || !capture->pcm)
    return false;

  if (atomic_load_explicit(&capture->stopped, memory_order_acquire)) {
    if (err) {
      backend_error_init(err, BACKEND_ERROR_NONE, "Capture stopped");
    }
    return false;
  }

  if (audio_chunk_get_channels(chunk) < (size_t)capture->channels) {
    if (err) {
      backend_error_init(
          err, BACKEND_ERROR_READ_ERROR,
          "Chunk channels count is smaller than capture device channels");
    }
    return false;
  }

  // Process events from ALSA control interface first, then sync linked controls
  // (matches CamillaDSP device.rs:1090)
  alsa_capture_process_events(capture);
  alsa_capture_sync_linked_controls(capture);

  if (atomic_load_explicit(&capture->is_inactive, memory_order_acquire)) {
    logger_info(&g_logger,
                "Capture source inactive and stop_on_inactive is enabled, "
                "stopping capture");
    if (err) {
      backend_error_init(err, BACKEND_ERROR_READ_EOF,
                         "Capture source inactive");
    }
    return false;
  }

  size_t sample_bytes = alsa_format_sample_size(capture->format);
  size_t blockalign = (size_t)capture->channels * sample_bytes;
  return audio_backend_ring_buffer_read(
      capture->ring_buffer, blockalign, frames,
      alsa_pcm_format_to_binary_format(capture->format),
      (size_t)capture->channels, &capture->inner_running, &capture->stopped,
      &capture->has_pending_rate_change, chunk, err);
}

// Close the ALSA capture device
static void alsa_capture_close(void *ctx) {
  alsa_capture_t *capture = (alsa_capture_t *)ctx;
  if (!capture)
    return;
  atomic_store_explicit(&capture->stopped, true, memory_order_release);

  if (capture->semaphore) {
    cdsp_sem_signal(capture->semaphore);
  }
  if (capture->inner_thread_created) {
    pthread_join(capture->inner_thread, NULL);
    capture->inner_thread_created = false;
    atomic_store_explicit(&capture->inner_running, false, memory_order_release);
  }
  if (capture->ring_buffer) {
    spsc_byte_ring_buffer_free(capture->ring_buffer);
    capture->ring_buffer = NULL;
  }
  if (capture->semaphore) {
    cdsp_sem_destroy(capture->semaphore);
    capture->semaphore = NULL;
  }

  pthread_mutex_lock(&g_alsa_mutex);
  if (capture->pcm) {
    snd_pcm_drop(capture->pcm);
    snd_pcm_close(capture->pcm);
    capture->pcm = NULL;
  }
  pthread_mutex_unlock(&g_alsa_mutex);
  pthread_mutex_lock(&capture->mixer_mutex);
  if (capture->ctl) {
    snd_ctl_close(capture->ctl);
    capture->ctl = NULL;
  }
  if (capture->hctl) {
    snd_hctl_close(capture->hctl);
    capture->hctl = NULL;
  }
  capture->hctl_pitch_elem = NULL;
  capture->hctl_rate_elem = NULL;
  capture->hctl_loopback_active_elem = NULL;
  capture->hctl_volume_elem = NULL;
  capture->hctl_mute_elem = NULL;
  atomic_store_explicit(&capture->has_pending_rate_change, false,
                        memory_order_release);
  atomic_store_explicit(&capture->is_inactive, false, memory_order_release);
  pthread_mutex_unlock(&capture->mixer_mutex);
}

// Check for pending rate change matching
// capture_backend_get_pending_rate_change
static bool alsa_capture_get_pending_rate_change(void *ctx, double *out_rate) {
  alsa_capture_t *capture = (alsa_capture_t *)ctx;
  if (!capture)
    return false;
  alsa_capture_process_events(capture);
  if (atomic_load_explicit(&capture->has_pending_rate_change,
                           memory_order_acquire)) {
    if (out_rate) {
      *out_rate =
          atomic_load_explicit(&capture->pending_rate, memory_order_acquire);
    }
    return true;
  }
  return false;
}

static bool alsa_capture_pitch_control_supported(void *ctx) {
  alsa_capture_t *capture = (alsa_capture_t *)ctx;
  if (!capture)
    return false;
  pthread_mutex_lock(&capture->mixer_mutex);
  bool res = (capture->hctl_pitch_elem != NULL);
  pthread_mutex_unlock(&capture->mixer_mutex);
  return res;
}

// Set capture pitch matching upstream (src/alsa_backend/device.rs:920-926)
static void alsa_capture_set_pitch(void *ctx, double multiplier) {
  alsa_capture_t *capture = (alsa_capture_t *)ctx;
  if (!capture || multiplier <= 0.0)
    return;
  pthread_mutex_lock(&capture->mixer_mutex);
  if (capture->hctl_pitch_elem) {
    long value = 0;
    if (capture->pitch_is_loopback) {
      value = (long)trunc(100000.0 / multiplier);
    } else {
      value = (long)trunc(multiplier * 1000000.0);
    }
    alsa_elem_write_as_int(capture->hctl_pitch_elem, value);
  }
  pthread_mutex_unlock(&capture->mixer_mutex);
}

static bool alsa_capture_wait(void *ctx, uint32_t timeout_ms) {
  alsa_capture_t *capture = (alsa_capture_t *)ctx;
  if (!capture)
    return false;
  if (atomic_load_explicit(&capture->stopped, memory_order_acquire)) {
    return false;
  }
  if (!capture->semaphore)
    return false;
  return cdsp_sem_timedwait(capture->semaphore, timeout_ms);
}

static void alsa_capture_stop(void *ctx) {
  alsa_capture_t *capture = (alsa_capture_t *)ctx;
  if (!capture)
    return;
  atomic_store_explicit(&capture->stopped, true, memory_order_release);
  if (capture->semaphore) {
    cdsp_sem_signal(capture->semaphore);
  }
  pthread_mutex_lock(&g_alsa_mutex);
  if (capture->pcm) {
    snd_pcm_drop(capture->pcm);
  }
  pthread_mutex_unlock(&g_alsa_mutex);
}

static void alsa_capture_destroy(void *ctx) {
  alsa_capture_t *capture = (alsa_capture_t *)ctx;
  if (!capture)
    return;
  alsa_capture_close(capture);
  pthread_mutex_destroy(&capture->mixer_mutex);
  free(capture);
}

// Create ALSA capture backend matching AlsaCaptureDevice::start in upstream
// (src/alsa_backend/device.rs:1219-1324 and
// src/alsa_backend/threaded_device.rs:1350-1368)
static capture_backend_t *
alsa_capture_create(const capture_device_config_t *config, int sample_rate,
                    int chunk_size, bool full_duplex,
                    processing_parameters_t *params, backend_error_t *err) {
  (void)full_duplex;
  (void)err;
  alsa_capture_t *capture = (alsa_capture_t *)calloc(1, sizeof(alsa_capture_t));
  if (!capture)
    return NULL;

  snprintf(capture->device_name, sizeof(capture->device_name), "%s",
           config->cfg.alsa.device[0] ? config->cfg.alsa.device : "default");

  capture->sample_rate = sample_rate;
  capture->capture_sample_rate = sample_rate; // Default to sample_rate
  capture->channels = config->cfg.alsa.channels;
  capture->chunk_size = chunk_size;

  capture->has_format = config->cfg.alsa.has_format;
  capture->requested_format = config->cfg.alsa.format;
  capture->params = params;
  capture->stop_on_inactive = config->cfg.alsa.stop_on_inactive;
  snprintf(capture->link_volume_control, sizeof(capture->link_volume_control),
           "%s", config->cfg.alsa.link_volume_control);
  snprintf(capture->link_mute_control, sizeof(capture->link_mute_control), "%s",
           config->cfg.alsa.link_mute_control);
  atomic_init(&capture->pending_rate, 0.0);
  atomic_init(&capture->has_pending_rate_change, false);
  atomic_init(&capture->is_inactive, false);
  pthread_mutex_init(&capture->mixer_mutex, NULL);

  capture_backend_t *backend =
      (capture_backend_t *)calloc(1, sizeof(capture_backend_t));
  if (!backend) {
    pthread_mutex_destroy(&capture->mixer_mutex);
    free(capture);
    return NULL;
  }
  backend->ctx = capture;
  backend->vtable = &g_alsa_capture_vtable;
  backend->is_realtime = true;
  return backend;
}

const capture_backend_vtable_t g_alsa_capture_vtable = {
    .create = alsa_capture_create,
    .open = alsa_capture_open,
    .read = alsa_capture_read,
    .close = alsa_capture_close,
    .get_pending_rate_change = alsa_capture_get_pending_rate_change,
    .is_pitch_control_supported = alsa_capture_pitch_control_supported,
    .set_pitch = alsa_capture_set_pitch,
    .wait_for_data = alsa_capture_wait,
    .set_is_paused = NULL,
    .stop = alsa_capture_stop,
    .destroy = alsa_capture_destroy};

#endif // defined(ENABLE_ALSA)
