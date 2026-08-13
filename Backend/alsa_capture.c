#include "Backend/alsa_capture.h"

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

#include "Audio/audio_chunk.h"
#include "Audio/processing_parameters.h"
#include "Audio/sample_conversion.h"
#include "Backend/alsa_device.h"
#include "Backend/backend_error.h"
#include "Config/engine_config_types.h"
#include "Logging/app_logger.h"
#include "Utils/cdsp_time.h"

static const logger_t g_logger = {"dsp.backend.alsa"};

struct alsa_capture {
  char device_name[256];
  int sample_rate;          // pipeline rate
  int capture_sample_rate;  // hardware capture rate
  int channels;
  int chunk_size;
  snd_pcm_uframes_t bufsize;
  snd_pcm_uframes_t period;
  size_t last_avail_min;

  bool has_format;
  alsa_sample_format_t requested_format;
  bool stop_on_inactive;
  char link_volume_control[256];
  char link_mute_control[256];

  processing_parameters_t* params;
  snd_ctl_t* ctl;
  snd_hctl_t* hctl;
  snd_hctl_elem_t* hctl_pitch_elem;
  snd_hctl_elem_t* hctl_rate_elem;
  snd_hctl_elem_t* hctl_loopback_active_elem;
  snd_hctl_elem_t* hctl_volume_elem;
  snd_hctl_elem_t* hctl_mute_elem;

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

  snd_pcm_t* pcm;
  snd_pcm_format_t format;

  void* interleaved_buf;
  size_t interleaved_buf_size;
  pthread_mutex_t mixer_mutex;
  _Atomic bool stopped;
};

// Helper to look up an ALSA control element matching find_elem in upstream
// (src/alsa_backend/utils.rs:758-780)
static snd_hctl_elem_t* find_elem(snd_hctl_t* hctl, snd_ctl_elem_iface_t iface,
                                  int device, int subdevice, const char* name,
                                  unsigned int* out_numid) {
  if (!hctl || !name || !name[0]) return NULL;
  snd_ctl_elem_id_t* id;
  snd_ctl_elem_id_alloca(&id);
  snd_ctl_elem_id_set_interface(id, iface);
  if (device >= 0) snd_ctl_elem_id_set_device(id, (unsigned int)device);
  if (subdevice >= 0)
    snd_ctl_elem_id_set_subdevice(id, (unsigned int)subdevice);
  snd_ctl_elem_id_set_name(id, name);

  snd_hctl_elem_t* elem = snd_hctl_find_elem(hctl, id);
  if (elem) {
    snd_ctl_elem_id_t* found_id;
    snd_ctl_elem_id_alloca(&found_id);
    snd_hctl_elem_get_id(elem, found_id);
    if (out_numid) *out_numid = snd_ctl_elem_id_get_numid(found_id);
    logger_debug(&g_logger, "Found element with name %s and numid %u", name,
                 out_numid ? *out_numid : 0);
  }
  return elem;
}

// Read element as integer (utils.rs:473-478)
static bool elem_read_as_int(snd_hctl_elem_t* elem, long* out_val) {
  if (!elem) return false;
  snd_ctl_elem_value_t* val;
  snd_ctl_elem_value_alloca(&val);
  if (snd_hctl_elem_read(elem, val) >= 0) {
    if (out_val) *out_val = snd_ctl_elem_value_get_integer(val, 0);
    return true;
  }
  return false;
}

// Read element as boolean (utils.rs:480-485)
static bool elem_read_as_bool(snd_hctl_elem_t* elem, bool* out_val) {
  if (!elem) return false;
  snd_ctl_elem_value_t* val;
  snd_ctl_elem_value_alloca(&val);
  if (snd_hctl_elem_read(elem, val) >= 0) {
    if (out_val) *out_val = (snd_ctl_elem_value_get_boolean(val, 0) != 0);
    return true;
  }
  return false;
}

// Read volume in dB (utils.rs:487-493)
static bool elem_read_volume_in_db(snd_ctl_t* ctl, snd_hctl_elem_t* elem,
                                   double* out_db) {
  if (!ctl || !elem) return false;
  long intval = 0;
  if (!elem_read_as_int(elem, &intval)) return false;

  snd_ctl_elem_id_t* id;
  snd_ctl_elem_id_alloca(&id);
  snd_hctl_elem_get_id(elem, id);

  long db_gain = 0;
  if (snd_ctl_convert_to_dB(ctl, id, intval, &db_gain) >= 0) {
    if (out_db) *out_db = (double)db_gain / 100.0;
    return true;
  }
  return false;
}

// Write element as integer (utils.rs:506-511)
static void elem_write_as_int(snd_hctl_elem_t* elem, long value) {
  if (!elem) return;
  snd_ctl_elem_value_t* val;
  snd_ctl_elem_value_alloca(&val);
  snd_ctl_elem_value_set_integer(val, 0, value);
  snd_hctl_elem_write(elem, val);
}

// Write element as boolean (utils.rs:513-518)
static void elem_write_as_bool(snd_hctl_elem_t* elem, bool value) {
  if (!elem) return;
  snd_ctl_elem_value_t* val;
  snd_ctl_elem_value_alloca(&val);
  snd_ctl_elem_value_set_boolean(val, 0, value ? 1 : 0);
  snd_hctl_elem_write(elem, val);
}

// Write volume in dB (utils.rs:495-504)
static void elem_write_volume_in_db(snd_ctl_t* ctl, snd_hctl_elem_t* elem,
                                    double db_val) {
  if (!ctl || !elem) return;
  snd_ctl_elem_id_t* id;
  snd_ctl_elem_id_alloca(&id);
  snd_hctl_elem_get_id(elem, id);

  long intval = 0;
  if (snd_ctl_convert_from_dB(ctl, id, (long)(db_val * 100.0), &intval, -1) >=
      0) {
    elem_write_as_int(elem, intval);
  }
}

// Initialize ALSA control elements matching find_elements in upstream
// (src/alsa_backend/utils.rs:723-756 & src/alsa_backend/device.rs:788-846)
static void alsa_capture_init_controls(alsa_capture_t* capture) {
  if (!capture->pcm) return;

  snd_pcm_info_t* info;
  snd_pcm_info_alloca(&info);
  if (snd_pcm_info(capture->pcm, info) < 0) return;

  int card = snd_pcm_info_get_card(info);
  if (card < 0) return;

  char ctl_name[32];
  snprintf(ctl_name, sizeof(ctl_name), "hw:%d", card);

  pthread_mutex_lock(&capture->mixer_mutex);

  // Open Ctl interface (non-blocking) and subscribe to events
  // (device.rs:790-794)
  snd_ctl_t* ctl = NULL;
  if (snd_ctl_open(&ctl, ctl_name, SND_CTL_NONBLOCK) >= 0 && ctl) {
    capture->ctl = ctl;
    snd_ctl_subscribe_events(ctl, 1);
  }

  // Open HCtl interface in non-blocking mode (device.rs:789)
  snd_hctl_t* hctl = NULL;
  if (snd_hctl_open(&hctl, ctl_name, SND_CTL_NONBLOCK) >= 0 && hctl) {
    snd_hctl_nonblock(hctl, 1);
    if (snd_hctl_load(hctl) >= 0) {
      capture->hctl = hctl;
      int dev_idx = snd_pcm_info_get_device(info);
      int subdev_idx = snd_pcm_info_get_subdevice(info);

      // Look up pitch control: PCM Rate Shift 100000 / Capture Pitch 1000000
      capture->hctl_pitch_elem =
          find_elem(hctl, SND_CTL_ELEM_IFACE_PCM, dev_idx, subdev_idx,
                    "PCM Rate Shift 100000", NULL);
      if (capture->hctl_pitch_elem) {
        capture->pitch_is_loopback = true;
        logger_info(&g_logger, "Capture device supports rate adjust");
      } else {
        capture->hctl_pitch_elem =
            find_elem(hctl, SND_CTL_ELEM_IFACE_PCM, dev_idx, subdev_idx,
                      "Capture Pitch 1000000", NULL);
        if (capture->hctl_pitch_elem) {
          capture->pitch_is_loopback = false;
          logger_info(&g_logger, "Capture device supports rate adjust");
        }
      }

      // Look up PCM Slave Active (loopback active)
      capture->hctl_loopback_active_elem =
          find_elem(hctl, SND_CTL_ELEM_IFACE_PCM, dev_idx, subdev_idx,
                    "PCM Slave Active", &capture->loopback_active_numid);
      if (capture->hctl_loopback_active_elem) {
        bool active = false;
        if (elem_read_as_bool(capture->hctl_loopback_active_elem, &active)) {
          if (!active) {
            if (capture->stop_on_inactive) {
              atomic_store_explicit(&capture->is_inactive, true,
                                    memory_order_release);
            }
          } else {
            atomic_store_explicit(&capture->is_inactive, false,
                                  memory_order_release);
          }
        }
      }

      // Look up Capture Rate (gadget rate)
      capture->hctl_rate_elem =
          find_elem(hctl, SND_CTL_ELEM_IFACE_PCM, dev_idx, subdev_idx,
                    "Capture Rate", &capture->gadget_rate_numid);
      if (capture->hctl_rate_elem) {
        long rate = 0;
        if (elem_read_as_int(capture->hctl_rate_elem, &rate)) {
          if (rate == 0) {
            if (capture->stop_on_inactive) {
              atomic_store_explicit(&capture->is_inactive, true,
                                    memory_order_release);
            }
          } else if (rate > 0 && rate != capture->capture_sample_rate) {
            atomic_store_explicit(&capture->pending_rate, (double)rate,
                                  memory_order_release);
            atomic_store_explicit(&capture->has_pending_rate_change, true,
                                  memory_order_release);
            atomic_store_explicit(&capture->is_inactive, false,
                                  memory_order_release);
          }
        }
      }

      // Look up Mixer Volume Control
      if (capture->link_volume_control[0]) {
        capture->hctl_volume_elem =
            find_elem(hctl, SND_CTL_ELEM_IFACE_MIXER, -1, -1,
                      capture->link_volume_control, &capture->volume_numid);
        if (capture->hctl_volume_elem && capture->ctl) {
          double vol_db = 0.0;
          if (elem_read_volume_in_db(capture->ctl, capture->hctl_volume_elem,
                                     &vol_db)) {
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
            find_elem(hctl, SND_CTL_ELEM_IFACE_MIXER, -1, -1,
                      capture->link_mute_control, &capture->mute_numid);
        if (capture->hctl_mute_elem) {
          bool active = false;
          if (elem_read_as_bool(capture->hctl_mute_elem, &active)) {
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
static void alsa_capture_sync_linked_controls(alsa_capture_t* capture) {
  if (!capture->params) return;
  pthread_mutex_lock(&capture->mixer_mutex);

  if (capture->ctl && capture->hctl_volume_elem &&
      capture->has_linked_volume_value) {
    double target_vol =
        processing_parameters_get_target_volume(capture->params);
    if (fabs(capture->linked_volume_value - target_vol) > 0.1) {
      logger_debug(&g_logger, "Updating linked volume control to %.2f dB",
                   target_vol);
    }
    if (capture->linked_volume_value != target_vol) {
      elem_write_volume_in_db(capture->ctl, capture->hctl_volume_elem,
                              target_vol);
      capture->linked_volume_value = target_vol;
    }
  }

  if (capture->hctl_mute_elem && capture->has_linked_mute_value) {
    bool target_mute = processing_parameters_is_muted(capture->params);
    if (capture->linked_mute_value != target_mute) {
      logger_debug(&g_logger, "Updating linked switch control to %d",
                   !target_mute);
      elem_write_as_bool(capture->hctl_mute_elem, !target_mute);
      capture->linked_mute_value = target_mute;
    }
  }

  pthread_mutex_unlock(&capture->mixer_mutex);
}

// Process events from ALSA control interface matching process_events &
// get_event_action in upstream (src/alsa_backend/utils.rs:574-721)
static void alsa_capture_process_events(alsa_capture_t* capture) {
  if (!capture->ctl) return;
  pthread_mutex_lock(&capture->mixer_mutex);

  snd_ctl_event_t* event;
  snd_ctl_event_alloca(&event);

  while (snd_ctl_read(capture->ctl, event) > 0) {
    if (snd_ctl_event_get_type(event) != SND_CTL_EVENT_ELEM) {
      continue;
    }
    unsigned int numid = snd_ctl_event_elem_get_numid(event);
    logger_debug(&g_logger, "Event from numid %u", numid);

    // Loopback active event
    if (capture->hctl_loopback_active_elem &&
        numid == capture->loopback_active_numid) {
      bool active = false;
      if (elem_read_as_bool(capture->hctl_loopback_active_elem, &active)) {
        logger_debug(&g_logger, "Loopback active: %d", active);
        if (!active) {
          if (capture->stop_on_inactive) {
            logger_debug(&g_logger,
                         "Stopping, capture device is inactive and "
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
      if (elem_read_as_int(capture->hctl_rate_elem, &rate)) {
        logger_debug(&g_logger, "Gadget rate: %ld", rate);
        if (rate == 0) {
          if (capture->stop_on_inactive) {
            logger_debug(&g_logger,
                         "Stopping, capture device is inactive and "
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
      if (elem_read_volume_in_db(capture->ctl, capture->hctl_volume_elem,
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
      if (elem_read_as_bool(capture->hctl_mute_elem, &active)) {
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
static bool alsa_capture_open(void* ctx, backend_error_t* err) {
  alsa_capture_t* capture = (alsa_capture_t*)ctx;
  if (!capture) return false;
  pthread_mutex_lock(&g_alsa_mutex);
  if (capture->pcm != NULL) {
    pthread_mutex_unlock(&g_alsa_mutex);
    return true;
  }
  int rc;
  rc = snd_pcm_open(&capture->pcm, capture->device_name, SND_PCM_STREAM_CAPTURE,
                    SND_PCM_NONBLOCK);
  if (rc < 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         snd_strerror(rc));
    pthread_mutex_unlock(&g_alsa_mutex);
    return false;
  }

  snd_pcm_hw_params_t* params;
  snd_pcm_hw_params_alloca(&params);
  rc = snd_pcm_hw_params_any(capture->pcm, params);
  if (rc < 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         snd_strerror(rc));
    goto error_cleanup;
  }

  // Set number of channels
  rc = snd_pcm_hw_params_set_channels(capture->pcm, params, capture->channels);
  if (rc < 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         snd_strerror(rc));
    goto error_cleanup;
  }

  // Set capture samplerate
  unsigned int val = capture->capture_sample_rate;
  int dir = 0;
  rc = snd_pcm_hw_params_set_rate_near(capture->pcm, params, &val, &dir);
  if (rc < 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         snd_strerror(rc));
    goto error_cleanup;
  }

  // Set sample format: if specified, use it; otherwise pick preferred format
  // in descending order: S32_LE -> S24_3_LE -> S24_4_LE -> S16_LE -> F32_LE ->
  // F64_LE (src/alsa_backend/utils.rs:433-456)
  snd_pcm_format_t formats[6];
  size_t num_formats = 0;
  if (capture->has_format) {
    if (capture->requested_format == ALSA_SAMPLE_FORMAT_S16_LE) {
      formats[0] = SND_PCM_FORMAT_S16_LE;
      num_formats = 1;
    } else if (capture->requested_format == ALSA_SAMPLE_FORMAT_S24_3_LE) {
      formats[0] = SND_PCM_FORMAT_S24_3LE;
      num_formats = 1;
    } else if (capture->requested_format == ALSA_SAMPLE_FORMAT_S24_4_LE) {
      formats[0] = SND_PCM_FORMAT_S24_LE;
      num_formats = 1;
    } else if (capture->requested_format == ALSA_SAMPLE_FORMAT_S32_LE) {
      formats[0] = SND_PCM_FORMAT_S32_LE;
      num_formats = 1;
    } else if (capture->requested_format == ALSA_SAMPLE_FORMAT_F32_LE) {
      formats[0] = SND_PCM_FORMAT_FLOAT_LE;
      num_formats = 1;
    } else if (capture->requested_format == ALSA_SAMPLE_FORMAT_F64_LE) {
      formats[0] = SND_PCM_FORMAT_FLOAT64_LE;
      num_formats = 1;
    }
  } else {
    formats[0] = SND_PCM_FORMAT_S32_LE;
    formats[1] = SND_PCM_FORMAT_S24_3LE;
    formats[2] = SND_PCM_FORMAT_S24_LE;
    formats[3] = SND_PCM_FORMAT_S16_LE;
    formats[4] = SND_PCM_FORMAT_FLOAT_LE;
    formats[5] = SND_PCM_FORMAT_FLOAT64_LE;
    num_formats = 6;
  }

  bool format_ok = false;
  for (size_t i = 0; i < num_formats; i++) {
    rc = snd_pcm_hw_params_set_format(capture->pcm, params, formats[i]);
    if (rc >= 0) {
      capture->format = formats[i];
      format_ok = true;
      break;
    }
  }
  if (!format_ok) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Requested or supported ALSA format not available");
    goto error_cleanup;
  }

  // Set access mode, buffersize and periods
  // (src/alsa_backend/device.rs:475-480)
  rc = snd_pcm_hw_params_set_access(capture->pcm, params,
                                    SND_PCM_ACCESS_RW_INTERLEAVED);
  if (rc < 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         snd_strerror(rc));
    goto error_cleanup;
  }

  double resampling_ratio = (capture->capture_sample_rate > 0)
                                ? ((double)capture->sample_rate /
                                   (double)capture->capture_sample_rate)
                                : 1.0;
  if (alsa_apply_buffer_size(capture->pcm, params, capture->chunk_size,
                             resampling_ratio, &capture->bufsize) < 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to set ALSA buffer size");
    goto error_cleanup;
  }

  if (alsa_apply_period_size(capture->pcm, params, capture->bufsize,
                             &capture->period) < 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to set ALSA period size");
    goto error_cleanup;
  }

  rc = snd_pcm_hw_params(capture->pcm, params);
  if (rc < 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         snd_strerror(rc));
    goto error_cleanup;
  }

  // Calculate init_io_size (buffermanager.rs:168: init_io_size = (chunksize as
  // f32 / resampling_ratio) as Frames)
  snd_pcm_uframes_t capture_avail_min =
      (snd_pcm_uframes_t)((double)capture->chunk_size / resampling_ratio);
  if (capture_avail_min > capture->bufsize) {
    char msg[256];
    snprintf(msg, sizeof(msg),
             "Trying to set avail_min to %lu, must be smaller than or equal to "
             "device buffer size of %lu",
             (unsigned long)capture_avail_min, (unsigned long)capture->bufsize);
    logger_error(&g_logger, "%s", msg);
    if (err) backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    goto error_cleanup;
  }

  // Set software parameters (src/alsa_backend/device.rs:483-491)
  snd_pcm_sw_params_t* sw_params;
  snd_pcm_sw_params_alloca(&sw_params);
  rc = snd_pcm_sw_params_current(capture->pcm, sw_params);
  if (rc >= 0) {
    // immediate start after pcmdev.prepare (buffermanager.rs:194)
    snd_pcm_sw_params_set_start_threshold(capture->pcm, sw_params, 0);
    // avail_min = initial io_size (buffermanager.rs:120)
    capture->last_avail_min = (size_t)capture_avail_min;
    snd_pcm_sw_params_set_avail_min(capture->pcm, sw_params, capture_avail_min);
    rc = snd_pcm_sw_params(capture->pcm, sw_params);
    if (rc < 0) {
      logger_warn(&g_logger, "Failed to set ALSA software parameters: %s",
                  snd_strerror(rc));
    }
  }

  size_t sample_size = 4;
  if (capture->format == SND_PCM_FORMAT_S16_LE) {
    sample_size = 2;
  } else if (capture->format == SND_PCM_FORMAT_S24_3LE) {
    sample_size = 3;
  } else if (capture->format == SND_PCM_FORMAT_S24_LE) {
    sample_size = 4;
  } else if (capture->format == SND_PCM_FORMAT_FLOAT64_LE) {
    sample_size = 8;
  }

  // Size buffer generously to accommodate dynamic resampling buffer needs
  // (src/alsa_backend/device.rs:863 & buffermanager.rs:157)
  size_t buffer_frames = (size_t)capture->bufsize;
  if (buffer_frames < (size_t)capture->chunk_size * 2) {
    buffer_frames = (size_t)capture->chunk_size * 2;
  }
  capture->interleaved_buf_size =
      buffer_frames * capture->channels * sample_size;
  capture->interleaved_buf = calloc(capture->interleaved_buf_size, 1);
  if (!capture->interleaved_buf) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to allocate ALSA capture interleaved buffer");
    goto error_cleanup;
  }

  alsa_capture_init_controls(capture);

  rc = snd_pcm_start(capture->pcm);
  if (rc < 0) {
    if (err) {
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         snd_strerror(rc));
    }
    goto error_cleanup;
  }

  pthread_mutex_unlock(&g_alsa_mutex);
  return true;

error_cleanup:
  if (capture->pcm) {
    snd_pcm_close(capture->pcm);
    capture->pcm = NULL;
  }
  if (capture->interleaved_buf) {
    free(capture->interleaved_buf);
    capture->interleaved_buf = NULL;
  }
  pthread_mutex_unlock(&g_alsa_mutex);
  return false;
}

// Capture a buffer matching capture_buffer in upstream
// (src/alsa_backend/device.rs:243-413)
static bool alsa_capture_read(void* ctx, size_t frames, audio_chunk_t* chunk,
                              backend_error_t* err) {
  alsa_capture_t* capture = (alsa_capture_t*)ctx;
  if (!capture || !capture->pcm) return false;

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

  // Sync volume/mute from engine to hardware (src/alsa_backend/device.rs:1090)
  alsa_capture_sync_linked_controls(capture);
  alsa_capture_process_events(capture);

  if (atomic_load_explicit(&capture->is_inactive, memory_order_acquire)) {
    logger_debug(&g_logger,
                 "Capture source inactive and stop_on_inactive is enabled, "
                 "stopping capture");
    if (err) {
      backend_error_init(err, BACKEND_ERROR_NONE, "Capture source inactive");
    }
    return false;
  }

  // Update avail_min and start_threshold if requested input frames changed
  // (src/alsa_backend/device.rs:958 & buffermanager.rs:126-133)
  if (frames != capture->last_avail_min && frames <= capture->bufsize) {
    snd_pcm_sw_params_t* sw_params;
    snd_pcm_sw_params_alloca(&sw_params);
    if (snd_pcm_sw_params_current(capture->pcm, sw_params) >= 0) {
      snd_pcm_sw_params_set_avail_min(capture->pcm, sw_params,
                                      (snd_pcm_uframes_t)frames);
      snd_pcm_sw_params_set_start_threshold(capture->pcm, sw_params, 0);
      if (snd_pcm_sw_params(capture->pcm, sw_params) >= 0) {
        capture->last_avail_min = frames;
      }
    }
  }

  // State checks and recoveries matching device.rs:259-282
  snd_pcm_state_t capture_state = snd_pcm_state(capture->pcm);
  if (capture_state == SND_PCM_STATE_XRUN) {
    logger_warn(&g_logger, "Prepare capture device");
    snd_pcm_prepare(capture->pcm);
  } else if (capture_state == SND_PCM_STATE_SUSPENDED) {
    alsa_recover_suspended_pcm(capture->pcm, "Capture");
    if (snd_pcm_state(capture->pcm) != SND_PCM_STATE_RUNNING) {
      snd_pcm_start(capture->pcm);
    }
  } else if ((int)capture_state < 0) {
    logger_error(&g_logger,
                 "Alsa snd_pcm_state() of capture device returned an "
                 "unexpected error: %s",
                 snd_strerror((int)capture_state));
    if (err)
      backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                         snd_strerror((int)capture_state));
    return false;
  } else if (capture_state != SND_PCM_STATE_RUNNING) {
    logger_debug(&g_logger, "Starting capture from state: %s",
                 alsa_state_desc(capture_state));
    snd_pcm_start(capture->pcm);
  }

  size_t sample_bytes = 4;
  if (capture->format == SND_PCM_FORMAT_S16_LE)
    sample_bytes = 2;
  else if (capture->format == SND_PCM_FORMAT_S24_3LE)
    sample_bytes = 3;
  else if (capture->format == SND_PCM_FORMAT_FLOAT64_LE)
    sample_bytes = 8;
  size_t bytes_per_frame = (size_t)capture->channels * sample_bytes;

  double millis_per_chunk =
      1000.0 * (double)frames / (double)capture->capture_sample_rate;

  char* buffer = (char*)capture->interleaved_buf;
  size_t buffer_len_bytes = frames * bytes_per_frame;

  if (buffer_len_bytes > capture->interleaved_buf_size) {
    if (err) {
      backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                         "Frame count exceeds capture buffer capacity");
    }
    return false;
  }

  // Poll loop matching capture_buffer in upstream
  // (src/alsa_backend/device.rs:287-364)
  for (;;) {
    int pcm_fds_count = snd_pcm_poll_descriptors_count(capture->pcm);
    int ctl_fds_count = 0;
    if (capture->ctl) {
      ctl_fds_count = snd_ctl_poll_descriptors_count(capture->ctl);
    }
    if (pcm_fds_count < 0) pcm_fds_count = 0;
    if (ctl_fds_count < 0) ctl_fds_count = 0;

    int total_fds = pcm_fds_count + ctl_fds_count;
    struct pollfd pfds[total_fds > 0 ? total_fds : 1];
    memset(pfds, 0, sizeof(pfds));

    if (pcm_fds_count > 0) {
      snd_pcm_poll_descriptors(capture->pcm, pfds, (unsigned int)pcm_fds_count);
    }
    if (ctl_fds_count > 0 && capture->ctl) {
      snd_ctl_poll_descriptors(capture->ctl, pfds + pcm_fds_count,
                               (unsigned int)ctl_fds_count);
    }

    uint32_t timeout_millis = (uint32_t)(8.0 * millis_per_chunk);
    if (timeout_millis < 20) timeout_millis = 20;

    logger_trace(&g_logger, "Capture pcmdevice.wait with timeout %u ms",
                 timeout_millis);
    uint32_t remaining_timeout_millis = timeout_millis;

    while (true) {
      if (atomic_load_explicit(&capture->stopped, memory_order_acquire)) {
        if (err) {
          backend_error_init(err, BACKEND_ERROR_NONE, "Capture stopped");
        }
        return false;
      }

      uint32_t poll_slice_millis =
          remaining_timeout_millis < 20 ? remaining_timeout_millis : 20;
      int poll_res = 0;
      if (total_fds > 0) {
        poll_res = poll(pfds, (nfds_t)total_fds, (int)poll_slice_millis);
      } else {
        poll_res = snd_pcm_wait(capture->pcm, (int)poll_slice_millis);
      }

      if (poll_res == 0) {
        if (remaining_timeout_millis <= poll_slice_millis) {
          logger_trace(&g_logger,
                       "Wait timed out, capture device takes too long to "
                       "capture frames");
          snd_pcm_drop(capture->pcm);
          snd_pcm_prepare(capture->pcm);
          if (err) {
            backend_error_init(err, BACKEND_ERROR_NONE,
                               "Capture device wait timeout");
          }
          return false;
        }
        remaining_timeout_millis -= poll_slice_millis;
        continue;
      } else if (poll_res < 0) {
        if (errno == EINTR) {
          if (err) {
            backend_error_init(err, BACKEND_ERROR_NONE,
                               "Capture poll interrupted by signal");
          }
          return false;
        }
        logger_warn(&g_logger,
                    "Capture: poll failed while waiting for available frames, "
                    "error: %s",
                    strerror(errno));
        if (err) {
          backend_error_init(err, BACKEND_ERROR_READ_ERROR, strerror(errno));
        }
        return false;
      }

      // Check control events (device.rs:316-331)
      if (ctl_fds_count > 0) {
        bool ctl_event = false;
        for (int i = pcm_fds_count; i < total_fds; i++) {
          if (pfds[i].revents != 0) {
            ctl_event = true;
            break;
          }
        }
        if (ctl_event) {
          logger_trace(&g_logger, "Got a control event");
          alsa_capture_process_events(capture);
          if (atomic_load_explicit(&capture->is_inactive,
                                   memory_order_acquire)) {
            if (err) {
              backend_error_init(err, BACKEND_ERROR_NONE,
                                 "Capture source inactive");
            }
            return false;
          }
        }
      }

      // Check PCM events (device.rs:332-355)
      if (pcm_fds_count > 0) {
        unsigned short pcm_revents = 0;
        int rev_rc = snd_pcm_poll_descriptors_revents(
            capture->pcm, pfds, (unsigned int)pcm_fds_count, &pcm_revents);
        if (rev_rc < 0) {
          if (rev_rc == -EPIPE) {
            logger_warn(&g_logger,
                        "Capture: wait overrun, trying to recover. Error: %s",
                        snd_strerror(rev_rc));
            snd_pcm_prepare(capture->pcm);
            break;
          } else if (rev_rc == -ESTRPIPE ||
                     snd_pcm_state(capture->pcm) == SND_PCM_STATE_SUSPENDED) {
            logger_warn(&g_logger,
                        "Capture: wait interrupted by suspend, trying to "
                        "recover. Error: %s",
                        snd_strerror(rev_rc));
            alsa_recover_suspended_pcm(capture->pcm, "Capture");
            if (snd_pcm_state(capture->pcm) != SND_PCM_STATE_RUNNING) {
              snd_pcm_start(capture->pcm);
            }
            break;
          } else {
            logger_warn(&g_logger,
                        "Capture: device failed while waiting for available "
                        "frames, error: %s",
                        snd_strerror(rev_rc));
            if (err) {
              backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                                 snd_strerror(rev_rc));
            }
            return false;
          }
        }

        if (pcm_revents & (POLLIN | POLLERR | POLLNVAL)) {
          if (pcm_revents & (POLLERR | POLLNVAL)) {
            snd_pcm_state_t st = snd_pcm_state(capture->pcm);
            if (st == SND_PCM_STATE_XRUN) {
              logger_warn(&g_logger,
                          "Capture: wait overrun, trying to recover.");
              snd_pcm_prepare(capture->pcm);
              break;
            } else if (st == SND_PCM_STATE_SUSPENDED) {
              logger_warn(
                  &g_logger,
                  "Capture: wait interrupted by suspend, trying to recover.");
              alsa_recover_suspended_pcm(capture->pcm, "Capture");
              if (snd_pcm_state(capture->pcm) != SND_PCM_STATE_RUNNING) {
                snd_pcm_start(capture->pcm);
              }
              break;
            }
          }
          break;
        }
      } else {
        break;
      }

      if (remaining_timeout_millis > poll_slice_millis) {
        remaining_timeout_millis -= poll_slice_millis;
      } else {
        remaining_timeout_millis = 0;
      }
    }

    if (atomic_load_explicit(&capture->stopped, memory_order_acquire)) {
      if (err) {
        backend_error_init(err, BACKEND_ERROR_NONE, "Capture stopped");
      }
      return false;
    }

    // Read audio frames matching device.rs:368-411
    size_t frames_req = buffer_len_bytes / bytes_per_frame;
    snd_pcm_sframes_t rc = snd_pcm_readi(capture->pcm, buffer, frames_req);
    if (rc > 0) {
      size_t frames_read = (size_t)rc;
      if (frames_read == frames_req) {
        logger_trace(&g_logger, "Capture read %zu frames as requested",
                     frames_read);
        break;
      } else {
        logger_warn(&g_logger,
                    "Capture read %zu frames instead of the requested %zu",
                    frames_read, frames_req);
        buffer += frames_read * bytes_per_frame;
        buffer_len_bytes -= frames_read * bytes_per_frame;
        continue;
      }
    } else {
      int err_read = (int)rc;
      if (err_read == -EIO) {
        logger_warn(&g_logger, "Capture: read failed with error: %s",
                    snd_strerror(err_read));
        if (err)
          backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                             snd_strerror(err_read));
        return false;
      } else if (err_read == 0 || err_read == -EAGAIN) {
        logger_trace(&g_logger,
                     "Capture: encountered EAGAIN error on read, trying again");
        continue;
      } else if (err_read == -EPIPE) {
        logger_warn(&g_logger,
                    "Capture: read overrun, trying to recover. Error: %s",
                    snd_strerror(err_read));
        snd_pcm_prepare(capture->pcm);
        continue;
      } else if (err_read == -ESTRPIPE ||
                 snd_pcm_state(capture->pcm) == SND_PCM_STATE_SUSPENDED) {
        logger_warn(&g_logger,
                    "Capture: read interrupted by suspend, trying to recover. "
                    "Error: %s",
                    snd_strerror(err_read));
        alsa_recover_suspended_pcm(capture->pcm, "Capture");
        if (snd_pcm_state(capture->pcm) != SND_PCM_STATE_RUNNING) {
          snd_pcm_start(capture->pcm);
        }
        continue;
      } else {
        logger_warn(&g_logger, "Capture failed, error: %s",
                    snd_strerror(err_read));
        if (err)
          backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                             snd_strerror(err_read));
        return false;
      }
    }
  }

  size_t read_frames = frames;
  audio_chunk_set_valid_frames(chunk, read_frames);

  // Decode interleaved samples to planar double audio chunk
  if (capture->format == SND_PCM_FORMAT_FLOAT_LE) {
    float* src = (float*)capture->interleaved_buf;
    for (size_t f = 0; f < read_frames; f++) {
      for (size_t c = 0; c < (size_t)capture->channels; c++) {
        double* dst = audio_chunk_get_channel(chunk, c);
        dst[f] = pcm_sample_decode_f32(src[f * capture->channels + c]);
      }
    }
  } else if (capture->format == SND_PCM_FORMAT_S32_LE) {
    int32_t* src = (int32_t*)capture->interleaved_buf;
    for (size_t f = 0; f < read_frames; f++) {
      for (size_t c = 0; c < (size_t)capture->channels; c++) {
        double* dst = audio_chunk_get_channel(chunk, c);
        dst[f] = pcm_sample_decode_s32(src[f * capture->channels + c]);
      }
    }
  } else if (capture->format == SND_PCM_FORMAT_S24_3LE) {
    uint8_t* src = (uint8_t*)capture->interleaved_buf;
    for (size_t f = 0; f < read_frames; f++) {
      for (size_t c = 0; c < (size_t)capture->channels; c++) {
        size_t offset = (f * capture->channels + c) * 3;
        double* dst = audio_chunk_get_channel(chunk, c);
        dst[f] = pcm_sample_decode_s24_3bytes(&src[offset]);
      }
    }
  } else if (capture->format == SND_PCM_FORMAT_S24_LE) {
    int32_t* src = (int32_t*)capture->interleaved_buf;
    for (size_t f = 0; f < read_frames; f++) {
      for (size_t c = 0; c < (size_t)capture->channels; c++) {
        double* dst = audio_chunk_get_channel(chunk, c);
        dst[f] = pcm_sample_decode_s24(src[f * capture->channels + c]);
      }
    }
  } else if (capture->format == SND_PCM_FORMAT_FLOAT64_LE) {
    double* src = (double*)capture->interleaved_buf;
    for (size_t f = 0; f < read_frames; f++) {
      for (size_t c = 0; c < (size_t)capture->channels; c++) {
        double* dst = audio_chunk_get_channel(chunk, c);
        dst[f] = src[f * capture->channels + c];
      }
    }
  } else if (capture->format == SND_PCM_FORMAT_S16_LE) {
    int16_t* src = (int16_t*)capture->interleaved_buf;
    for (size_t f = 0; f < read_frames; f++) {
      for (size_t c = 0; c < (size_t)capture->channels; c++) {
        double* dst = audio_chunk_get_channel(chunk, c);
        dst[f] = pcm_sample_decode_s16(src[f * capture->channels + c]);
      }
    }
  }

  return true;
}

// Close the ALSA capture device
static void alsa_capture_close(void* ctx) {
  alsa_capture_t* capture = (alsa_capture_t*)ctx;
  if (!capture) return;
  atomic_store_explicit(&capture->stopped, true, memory_order_release);
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
  if (capture->interleaved_buf) {
    free(capture->interleaved_buf);
    capture->interleaved_buf = NULL;
  }
}

// Check for pending rate change matching
// capture_backend_get_pending_rate_change
static bool alsa_capture_get_pending_rate_change(void* ctx, double* out_rate) {
  alsa_capture_t* capture = (alsa_capture_t*)ctx;
  if (!capture) return false;
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

static bool alsa_capture_pitch_control_supported(void* ctx) {
  alsa_capture_t* capture = (alsa_capture_t*)ctx;
  if (!capture) return false;
  pthread_mutex_lock(&capture->mixer_mutex);
  bool res = (capture->hctl_pitch_elem != NULL);
  pthread_mutex_unlock(&capture->mixer_mutex);
  return res;
}

// Set capture pitch matching upstream (src/alsa_backend/device.rs:920-926)
static void alsa_capture_set_pitch(void* ctx, double multiplier) {
  alsa_capture_t* capture = (alsa_capture_t*)ctx;
  if (!capture || multiplier <= 0.0) return;
  pthread_mutex_lock(&capture->mixer_mutex);
  if (capture->hctl_pitch_elem) {
    long value = 0;
    if (capture->pitch_is_loopback) {
      value = (long)round(100000.0 / multiplier);
    } else {
      value = (long)round(multiplier * 1000000.0);
    }
    elem_write_as_int(capture->hctl_pitch_elem, value);
  }
  pthread_mutex_unlock(&capture->mixer_mutex);
}

static bool alsa_capture_wait(void* ctx, uint32_t timeout_ms) {
  alsa_capture_t* capture = (alsa_capture_t*)ctx;
  if (!capture || !capture->pcm) return false;
  if (atomic_load_explicit(&capture->stopped, memory_order_acquire)) {
    return false;
  }
  int err = snd_pcm_wait(capture->pcm, (int)timeout_ms);
  return err > 0;
}

static void alsa_capture_stop(void* ctx) {
  alsa_capture_t* capture = (alsa_capture_t*)ctx;
  if (!capture) return;
  atomic_store_explicit(&capture->stopped, true, memory_order_release);
  pthread_mutex_lock(&g_alsa_mutex);
  if (capture->pcm) {
    snd_pcm_drop(capture->pcm);
  }
  pthread_mutex_unlock(&g_alsa_mutex);
}

static void alsa_capture_destroy(void* ctx) {
  alsa_capture_t* capture = (alsa_capture_t*)ctx;
  if (!capture) return;
  alsa_capture_close(capture);
  pthread_mutex_destroy(&capture->mixer_mutex);
  free(capture);
}

// Create ALSA capture backend matching AlsaCaptureDevice::start in upstream
// (src/alsa_backend/device.rs:1219-1324)
static capture_backend_t* alsa_capture_create(
    const capture_device_config_t* config, int sample_rate, int chunk_size,
    bool full_duplex, processing_parameters_t* params, backend_error_t* err) {
  (void)full_duplex;
  (void)err;
  alsa_capture_t* capture = (alsa_capture_t*)calloc(1, sizeof(alsa_capture_t));
  if (!capture) return NULL;

  snprintf(capture->device_name, sizeof(capture->device_name), "%s",
           config->cfg.alsa.device[0] ? config->cfg.alsa.device : "default");

  capture->sample_rate = sample_rate;
  capture->capture_sample_rate = sample_rate;  // Default to sample_rate
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

  capture_backend_t* backend =
      (capture_backend_t*)calloc(1, sizeof(capture_backend_t));
  if (!backend) {
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

#endif  // defined(ENABLE_ALSA)
