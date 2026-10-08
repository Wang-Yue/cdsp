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
#include <time.h>

#include "audio/audio_chunk.h"
#include "audio/processing_parameters.h"
#include "backend/alsa_device.h"
#include "backend/audio_backend.h"
#include "backend/backend_buffer.h"
#include "backend/backend_error.h"
#include "config/config_gen.h"
#include "engine/thread_priority.h"
#include "logging/app_logger.h"

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
  _Atomic bool is_inactive;
  // Pitch requested by set_pitch() on the engine thread, written to the
  // control by the inner thread (M6).
  _Atomic double pending_pitch;
  _Atomic bool pitch_pending;

  double linked_volume_value;
  bool has_linked_volume_value;
  bool linked_mute_value;
  bool has_linked_mute_value;

  snd_pcm_t *pcm;
  snd_pcm_format_t format;

  pthread_mutex_t mixer_mutex;

  backend_buffer_t *buffer;
  pthread_t inner_thread;
  bool inner_thread_created;
  bool device_stalled;
  // readi() target, chunk_size frames; allocated by open(), not on the RT
  // thread (L4).
  uint8_t *local_buf;
  size_t local_buf_bytes;

  // Set (release) by the inner thread when it exits because of a device
  // error, after fatal_msg has been written; read() reports it as
  // BACKEND_ERROR_READ_ERROR once the ring has been drained.
  _Atomic bool fatal_error;
  char fatal_msg[256];
};

static void alsa_capture_record_fatal(alsa_capture_t *capture, const char *what,
                                      int alsa_rc) {
  if (alsa_rc != 0) {
    snprintf(capture->fatal_msg, sizeof(capture->fatal_msg), "%s: %s", what,
             snd_strerror(alsa_rc));
  } else {
    snprintf(capture->fatal_msg, sizeof(capture->fatal_msg), "%s", what);
  }
  logger_error(&g_logger, "Capture: %s", capture->fatal_msg);
}

// Defined below; invoked from the capture RT thread whenever poll() reports
// activity on a control descriptor, and once per pass. The inner thread is
// the only user of ctl/hctl while it runs (M6).
static bool alsa_capture_process_events(alsa_capture_t *capture);
static void alsa_capture_sync_linked_controls(alsa_capture_t *capture);
static void alsa_capture_apply_pending_pitch(alsa_capture_t *capture);

static uint64_t alsa_capture_monotonic_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

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

  // Preallocated by open() (chunk_size frames); no heap use on this RT
  // thread (L4). Audio is normally read straight into the ring (see below);
  // local_buf only takes a frame that straddles the ring wrap, or a chunk
  // that must be read and dropped because the ring is full.
  uint8_t *local_buf = capture->local_buf;
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
    alsa_capture_record_fatal(capture,
                              "Failed to get ALSA capture poll descriptors", 0);
    atomic_store_explicit(&capture->fatal_error, true, memory_order_release);
    backend_buffer_set_state(capture->buffer, BACKEND_STREAM_STOPPED);
    if (rt_handle) {
      demote_current_thread_from_realtime(rt_handle);
    }
    return NULL;
  }

  // Set by every exit path caused by a device error, as opposed to a
  // requested stop.
  bool fatal = false;

  while (backend_buffer_get_state(capture->buffer) != BACKEND_STREAM_STOPPED) {
    snd_pcm_state_t capture_state = snd_pcm_state(capture->pcm);
    if ((int)capture_state < 0) {
      alsa_capture_record_fatal(capture, "Capture device error state",
                                (int)capture_state);
      fatal = true;
      break;
    }
    if (capture_state == SND_PCM_STATE_XRUN) {
      logger_warn(&g_logger, "Prepare capture device");
      int prc = snd_pcm_prepare(capture->pcm);
      if (prc < 0) {
        alsa_capture_record_fatal(
            capture, "Failed to restart capture device after XRUN", prc);
        fatal = true;
        break;
      }
      // A PREPARED capture stream never becomes readable, so polling it
      // would burn the whole stall timeout and log a false "stalled". Start
      // it now instead of on the next pass (upstream has the same gap,
      // threaded_device.rs:755-757).
      int strc = snd_pcm_start(capture->pcm);
      if (strc < 0) {
        alsa_capture_record_fatal(
            capture, "Failed to start capture device after XRUN", strc);
        fatal = true;
        break;
      }
    } else if (capture_state == SND_PCM_STATE_SUSPENDED) {
      int src = alsa_recover_suspended_pcm(capture->pcm, "Capture");
      if (src < 0) {
        alsa_capture_record_fatal(
            capture, "Failed to restart capture device after suspension", src);
        fatal = true;
        break;
      }
      // A successful resume leaves the device RUNNING; starting it again
      // would fail with -EBADFD.
      if (snd_pcm_state(capture->pcm) != SND_PCM_STATE_RUNNING) {
        int strc = snd_pcm_start(capture->pcm);
        if (strc < 0) {
          alsa_capture_record_fatal(
              capture, "Failed to restart capture device after suspension",
              strc);
          fatal = true;
          break;
        }
      }
    } else if (capture_state != SND_PCM_STATE_RUNNING) {
      logger_debug(&g_logger, "Starting capture from state: %s",
                   alsa_state_desc((int)capture_state));
      int strc = snd_pcm_start(capture->pcm);
      if (strc < 0) {
        alsa_capture_record_fatal(capture, "Failed to start capture device",
                                  strc);
        fatal = true;
        break;
      }
    }

    // Apply a requested pitch and push the engine volume/mute to the linked
    // controls. Upstream does both on the inner thread once per chunk
    // (threaded_device.rs:1552-1559, 1647-1652); here they run once per
    // pass, which is about once per chunk.
    alsa_capture_apply_pending_pitch(capture);
    alsa_capture_sync_linked_controls(capture);

    // Wait for the device, servicing control events as they arrive.
    // The wait is sliced so a stop request is observed within 20 ms even when
    // the full stall timeout is much longer
    // (src/alsa_backend/threaded_device.rs:778-840).
    bool pcm_ready = false;
    bool wait_timed_out = false;
    bool wait_fatal = false;
    bool ctl_terminal = false;
    // The stall timeout is charged with the time that actually elapsed, not
    // a nominal 20 ms per slice: a burst of control events wakes poll() early
    // and used to make the timeout fire too soon (M6).
    logger_trace(&g_logger, "Capture pcmdevice.wait with timeout %u ms",
                 timeout_millis);
    uint64_t wait_start_ms = alsa_capture_monotonic_ms();
    for (;;) {
      if (backend_buffer_get_state(capture->buffer) == BACKEND_STREAM_STOPPED) {
        break;
      }
      uint64_t elapsed_ms = alsa_capture_monotonic_ms() - wait_start_ms;
      uint32_t remaining_millis =
          elapsed_ms >= timeout_millis
              ? 0
              : (uint32_t)(timeout_millis - (uint32_t)elapsed_ms);
      int poll_slice = remaining_millis < 20 ? (int)remaining_millis : 20;
      for (int i = 0; i < total_fds; i++) {
        pfds[i].revents = 0;
      }
      int nbr_ready = poll(pfds, (nfds_t)total_fds, poll_slice);
      if (nbr_ready < 0) {
        if (errno == EINTR)
          continue;
        int poll_errno = errno;
        snprintf(capture->fatal_msg, sizeof(capture->fatal_msg),
                 "Capture poll fatal error: %s", strerror(poll_errno));
        logger_error(&g_logger, "%s", capture->fatal_msg);
        wait_fatal = true;
        break;
      }
      if (nbr_ready == 0) {
        if (remaining_millis <= (uint32_t)poll_slice) {
          logger_debug(&g_logger,
                       "Capture wait timed out after %u ms, device stalled",
                       timeout_millis);
          wait_timed_out = true;
          break;
        }
        continue;
      }

      logger_trace(&g_logger, "Got %d ready fds", nbr_ready);
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
        logger_trace(&g_logger, "Got a control event");
        if (alsa_capture_process_events(capture)) {
          ctl_terminal = true;
          break;
        }
      }
      if (pcm_ready) {
        logger_trace(
            &g_logger, "Capture waited for %llu ms",
            (unsigned long long)(alsa_capture_monotonic_ms() - wait_start_ms));
        break;
      }
    }

    if (wait_fatal) {
      fatal = true;
      break;
    }
    if (ctl_terminal) {
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

    // Zero-copy (AGENTS.md §3.4, L4): snd_pcm_readi() writes straight into
    // the ring's first free contiguous slice, published with
    // backend_buffer_commit_write(). A frame that would straddle the ring
    // wrap (non-power-of-two blockalign, e.g. S24_3) goes through the
    // one-frame push path, which re-aligns the slices. With a full ring the
    // device is still read so it does not overrun, and the chunk is dropped
    // and reported once per overflow episode (upstream ring_full latch,
    // threaded_device.rs:1589-1603; L5).
    enum { CAPTURE_TO_SLICE, CAPTURE_STRADDLE, CAPTURE_DROP } read_mode;
    void *w1 = NULL, *w2 = NULL;
    size_t n1 = 0, n2 = 0;
    uint8_t *read_dst = local_buf;
    size_t read_frames = (size_t)capture->chunk_size;
    if (backend_buffer_get_write_slices(capture->buffer, read_frames, &w1, &n1,
                                        &w2, &n2) > 0) {
      read_mode = CAPTURE_TO_SLICE;
      read_dst = (uint8_t *)w1;
      read_frames = n1;
    } else if (backend_buffer_get_available_write_frames(capture->buffer) > 0) {
      read_mode = CAPTURE_STRADDLE;
      read_frames = 1;
    } else {
      read_mode = CAPTURE_DROP;
    }
    snd_pcm_sframes_t frames_read =
        snd_pcm_readi(capture->pcm, read_dst, (snd_pcm_uframes_t)read_frames);
    if (frames_read > 0) {
      logger_trace(&g_logger, "Capture read %ld frames (requested %zu)",
                   (long)frames_read, read_frames);
      if (capture->device_stalled) {
        logger_info(&g_logger,
                    "Capture device resumed, processing is running");
        capture->device_stalled = false;
      }
      switch (read_mode) {
      case CAPTURE_TO_SLICE:
        backend_buffer_commit_write(capture->buffer, (size_t)frames_read, 0);
        break;
      case CAPTURE_STRADDLE:
        backend_buffer_push(capture->buffer, local_buf, (size_t)frames_read);
        break;
      case CAPTURE_DROP:
        backend_buffer_commit_write(capture->buffer, 0, (size_t)frames_read);
        break;
      }
    } else if (frames_read == -EPIPE) {
      logger_warn(&g_logger, "Capture: read overrun, trying to recover");
      logger_trace(&g_logger, "snd_pcm_prepare");
      int prc = snd_pcm_prepare(capture->pcm);
      if (prc < 0) {
        alsa_capture_record_fatal(capture, "prepare after overrun", prc);
        fatal = true;
        break;
      }
    } else if (frames_read == -ESTRPIPE) {
      logger_warn(&g_logger,
                  "Capture: read interrupted by suspend, trying to recover");
      int src = alsa_recover_suspended_pcm(capture->pcm, "Capture");
      if (src < 0) {
        alsa_capture_record_fatal(capture, "suspend recovery failed", src);
        fatal = true;
        break;
      }
      if (snd_pcm_state(capture->pcm) != SND_PCM_STATE_RUNNING) {
        int strc = snd_pcm_start(capture->pcm);
        if (strc < 0) {
          alsa_capture_record_fatal(capture, "restart after suspend", strc);
          fatal = true;
          break;
        }
      }
    } else if (frames_read == 0) {
      logger_debug(&g_logger, "Capture read returned 0 frames, device stalled");
      if (!capture->device_stalled) {
        logger_info(&g_logger,
                    "Capture device is stalled, processing is stalled");
        capture->device_stalled = true;
      }
    } else if (frames_read == -EAGAIN || frames_read == -EINTR) {
      // Upstream's capture_buffer reports -EAGAIN/-EINTR as
      // ordinary transient conditions. Keep polling.
      logger_debug(&g_logger,
                   "Capture: encountered EAGAIN/EINTR on read, trying later");
    } else {
      alsa_capture_record_fatal(capture, "Capture read fatal error",
                                (int)frames_read);
      fatal = true;
      break;
    }
  }

  // Errors caused by an engine-requested stop are not reported.
  if (fatal &&
      backend_buffer_get_state(capture->buffer) != BACKEND_STREAM_STOPPED) {
    atomic_store_explicit(&capture->fatal_error, true, memory_order_release);
  }
  backend_buffer_set_state(capture->buffer, BACKEND_STREAM_STOPPED);
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
// upstream (src/alsa_backend/utils.rs:782-808). Runs on the capture inner
// thread only, which exclusively owns ctl/hctl while it runs, so no lock is
// taken (M6).
static void alsa_capture_sync_linked_controls(alsa_capture_t *capture) {
  if (!capture->params)
    return;

  if (capture->ctl && capture->hctl_volume_elem &&
      capture->has_linked_volume_value) {
    double target_vol =
        processing_parameters_get_target_volume(capture->params);
    if (fabs(capture->linked_volume_value - target_vol) > 0.1) {
      logger_debug(&g_logger, "Updating linked volume control to %.2f dB",
                   target_vol);
      alsa_elem_write_volume_in_db(capture->ctl, capture->hctl_volume_elem,
                                   target_vol);
      capture->linked_volume_value = target_vol;
    }
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
}

// Writes a pitch requested by set_pitch() (M6). Inner thread only.
static void alsa_capture_apply_pending_pitch(alsa_capture_t *capture) {
  if (!atomic_exchange_explicit(&capture->pitch_pending, false,
                                memory_order_acq_rel)) {
    return;
  }
  double multiplier =
      atomic_load_explicit(&capture->pending_pitch, memory_order_relaxed);
  if (!capture->hctl_pitch_elem)
    return;
  long value = 0;
  if (capture->pitch_is_loopback) {
    value = (long)trunc(100000.0 / multiplier);
  } else {
    value = (long)trunc(multiplier * 1000000.0);
  }
  alsa_elem_write_as_int(capture->hctl_pitch_elem, value);
}

// Process events from ALSA control interface matching process_events &
// get_event_action in upstream (src/alsa_backend/utils.rs:574-721).
// Inner thread only, without a lock (M6). Like upstream (utils.rs:590-607),
// stops at the first terminal event (source inactive or rate change) (L9).
static bool alsa_capture_process_events(alsa_capture_t *capture) {
  if (!capture->ctl && !capture->hctl)
    return false;

  snd_ctl_event_t *event;
  snd_ctl_event_alloca(&event);

  bool terminal = false;
  while (capture->ctl && snd_ctl_read(capture->ctl, event) > 0) {
    // After a terminal event the rest of the queue is read and discarded:
    // left queued, it would keep the ctl descriptor readable and make the
    // poll() loop spin.
    if (terminal || snd_ctl_event_get_type(event) != SND_CTL_EVENT_ELEM) {
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
        // is_inactive is latched: a later "active" event must not undo a
        // stop that read() has not observed yet (L9). close() clears it.
        if (!active && capture->stop_on_inactive) {
          logger_debug(&g_logger, "Stopping, capture device is inactive and "
                                  "stop_on_inactive is set to true");
          atomic_store_explicit(&capture->is_inactive, true,
                                memory_order_release);
          terminal = true;
        }
      }
    }

    // Gadget rate event
    if (!terminal && capture->hctl_rate_elem &&
        numid == capture->gadget_rate_numid) {
      long rate = 0;
      if (alsa_elem_read_as_int(capture->hctl_rate_elem, &rate)) {
        logger_debug(&g_logger, "Gadget rate: %ld", rate);
        if (rate == 0) {
          if (capture->stop_on_inactive) {
            logger_debug(&g_logger, "Stopping, capture device is inactive and "
                                    "stop_on_inactive is set to true");
            atomic_store_explicit(&capture->is_inactive, true,
                                  memory_order_release);
            terminal = true;
          }
        } else if (rate > 0 && rate != capture->capture_sample_rate) {
          logger_debug(&g_logger,
                       "Stopping, capture device sample format changed");
          atomic_store_explicit(&capture->pending_rate, (double)rate,
                                memory_order_release);
          backend_buffer_set_pending_rate_change(capture->buffer, true);
          terminal = true;
        } else {
          logger_debug(&g_logger,
                       "Capture device resumed with unchanged sample rate");
        }
      }
    }

    // Volume control event
    if (!terminal && capture->hctl_volume_elem &&
        numid == capture->volume_numid) {
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
    if (!terminal && capture->hctl_mute_elem && numid == capture->mute_numid) {
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

  // Always drain the hctl queue so its descriptor stops polling readable.
  if (capture->hctl) {
    snd_hctl_handle_events(capture->hctl);
  }
  if (terminal) {
    backend_buffer_signal(capture->buffer);
  }
  return terminal;
}

// Closes the control handles opened by alsa_capture_init_controls() and
// forgets the element pointers and cached linked values. Caller holds
// mixer_mutex.
static void alsa_capture_close_controls_locked(alsa_capture_t *capture) {
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
  capture->has_linked_volume_value = false;
  capture->has_linked_mute_value = false;
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

  alsa_capture_init_controls(capture);

  size_t ring_frames = alsa_capture_ring_capacity_frames(
      (size_t)capture->chunk_size, capture->period);
  capture->buffer = backend_buffer_create(
      ring_frames, alsa_pcm_format_to_binary_format(capture->format),
      capture->channels, capture->capture_sample_rate, false, capture->params);
  if (!capture->buffer) {
    if (err) {
      backend_error_init(
          err, BACKEND_ERROR_INITIALIZATION_FAILED,
          "Failed to allocate backend buffer for threaded ALSA capture");
    }
    goto error_cleanup;
  }
  // Allocated here rather than on the real-time inner thread (L4).
  capture->local_buf_bytes = (size_t)capture->chunk_size * capture->channels *
                             alsa_format_sample_size(capture->format);
  capture->local_buf = (uint8_t *)malloc(capture->local_buf_bytes);
  if (!capture->local_buf) {
    if (err) {
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to allocate ALSA capture read buffer");
    }
    goto error_cleanup;
  }
  atomic_store_explicit(&capture->fatal_error, false, memory_order_relaxed);
  atomic_store_explicit(&capture->pitch_pending, false, memory_order_relaxed);
  capture->fatal_msg[0] = '\0';
  backend_buffer_set_state(capture->buffer, BACKEND_STREAM_RUNNING);
  if (pthread_create(&capture->inner_thread, NULL,
                     alsa_capture_inner_thread_func, capture) != 0) {
    backend_buffer_set_state(capture->buffer, BACKEND_STREAM_STOPPED);
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
  backend_buffer_free(capture->buffer);
  capture->buffer = NULL;
  free(capture->local_buf);
  capture->local_buf = NULL;
  capture->local_buf_bytes = 0;
  if (capture->pcm) {
    snd_pcm_close(capture->pcm);
    capture->pcm = NULL;
  }
  // ctl/hctl from alsa_capture_init_controls() would otherwise leak, and be
  // overwritten without being closed by the next open() (L3).
  pthread_mutex_lock(&capture->mixer_mutex);
  alsa_capture_close_controls_locked(capture);
  pthread_mutex_unlock(&capture->mixer_mutex);
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

  // No early return on STOPPED: backend_buffer_read_chunk() first drains the
  // frames still queued in the ring (L7) and only then reports
  // BACKEND_ERROR_READ_ERROR "Capture stream stopped". Returning
  // BACKEND_ERROR_NONE here made the engine capture loop busy-spin, because
  // backend_buffer_wait() returns at once for a stopped stream (H3).

  // Control events and linked-control sync are handled on the inner thread
  // only, as upstream does (threaded_device.rs:808-823, 1647-1652). The
  // results arrive through is_inactive, pending_rate and the atomic
  // processing parameters, so this engine thread takes no lock and makes no
  // control ioctls (M6).

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

  bool ok = backend_buffer_read_chunk(capture->buffer, frames, chunk, err);
  if (!ok && err && err->type == BACKEND_ERROR_READ_ERROR &&
      atomic_load_explicit(&capture->fatal_error, memory_order_acquire)) {
    // Report the device error that stopped the inner thread (upstream sends
    // StatusMessage::CaptureError, threaded_device.rs:1634-1643).
    backend_error_init(err, BACKEND_ERROR_READ_ERROR, capture->fatal_msg);
  }
  return ok;
}

// Close the ALSA capture device
static void alsa_capture_close(void *ctx) {
  alsa_capture_t *capture = (alsa_capture_t *)ctx;
  if (!capture)
    return;

  backend_buffer_set_state(capture->buffer, BACKEND_STREAM_STOPPED);

  if (capture->inner_thread_created) {
    pthread_join(capture->inner_thread, NULL);
    capture->inner_thread_created = false;
  }
  backend_buffer_free(capture->buffer);
  capture->buffer = NULL;
  free(capture->local_buf);
  capture->local_buf = NULL;
  capture->local_buf_bytes = 0;

  pthread_mutex_lock(&g_alsa_mutex);
  if (capture->pcm) {
    snd_pcm_drop(capture->pcm);
    snd_pcm_close(capture->pcm);
    capture->pcm = NULL;
  }
  pthread_mutex_unlock(&g_alsa_mutex);
  pthread_mutex_lock(&capture->mixer_mutex);
  alsa_capture_close_controls_locked(capture);
  atomic_store_explicit(&capture->is_inactive, false, memory_order_release);
  pthread_mutex_unlock(&capture->mixer_mutex);
}

// Check for pending rate change matching
// capture_backend_get_pending_rate_change. Control events are handled only on
// the inner thread (M6), which publishes the result through these atomics.
static bool alsa_capture_get_pending_rate_change(void *ctx, double *out_rate) {
  alsa_capture_t *capture = (alsa_capture_t *)ctx;
  if (!capture || !capture->buffer)
    return false;
  if (backend_buffer_has_pending_rate_change(capture->buffer)) {
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
  // hctl_pitch_elem is only written by open()/close(), never while the inner
  // thread runs.
  pthread_mutex_lock(&capture->mixer_mutex);
  bool res = (capture->hctl_pitch_elem != NULL);
  pthread_mutex_unlock(&capture->mixer_mutex);
  return res;
}

// Set capture pitch matching upstream (src/alsa_backend/device.rs:920-926).
// Upstream sends SetSpeed to the inner thread, which writes the element
// (threaded_device.rs:1552-1559). Likewise, only the request is stored here
// and the inner thread writes it, so the control handles are used from one
// thread only and no lock is needed (M6).
static void alsa_capture_set_pitch(void *ctx, double multiplier) {
  alsa_capture_t *capture = (alsa_capture_t *)ctx;
  if (!capture || !(multiplier > 0.0) || !isfinite(multiplier))
    return;
  atomic_store_explicit(&capture->pending_pitch, multiplier,
                        memory_order_relaxed);
  atomic_store_explicit(&capture->pitch_pending, true, memory_order_release);
}

static bool alsa_capture_wait(void *ctx, uint32_t timeout_ms) {
  alsa_capture_t *capture = (alsa_capture_t *)ctx;
  if (!capture)
    return false;
  return backend_buffer_wait(capture->buffer, timeout_ms);
}

// Only signals the inner thread (it polls in <=20 ms slices and exits on
// STOPPED). close() drops the PCM after the join (M4). Dropping here, while the
// inner thread may be in poll()/snd_pcm_readi(), made readi fail with -EBADFD
// and logged a spurious "Capture read fatal error" at every shutdown.
static void alsa_capture_stop(void *ctx) {
  alsa_capture_t *capture = (alsa_capture_t *)ctx;
  if (!capture)
    return;
  backend_buffer_set_state(capture->buffer, BACKEND_STREAM_STOPPED);
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
  atomic_init(&capture->is_inactive, false);
  atomic_init(&capture->pending_pitch, 1.0);
  atomic_init(&capture->pitch_pending, false);
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
    .stop = alsa_capture_stop,
    .destroy = alsa_capture_destroy};

#endif // defined(ENABLE_ALSA)
