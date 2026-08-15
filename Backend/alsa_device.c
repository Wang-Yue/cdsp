#include "Backend/alsa_device.h"

#if defined(ENABLE_ALSA)
#include <errno.h>
#include <math.h>
#include <pthread.h>

#include "Logging/app_logger.h"
#include "Utils/cdsp_time.h"

static const logger_t g_alsa_dev_logger = {"dsp.backend.alsa"};

pthread_mutex_t g_alsa_mutex = PTHREAD_MUTEX_INITIALIZER;

bool alsa_is_dsd_format(snd_pcm_format_t format) {
  return sample_format_is_dsd(alsa_pcm_format_to_binary_format(format));
}

size_t alsa_format_sample_size(snd_pcm_format_t format) {
  return sample_format_bytes_per_sample(
      alsa_pcm_format_to_binary_format(format));
}

snd_pcm_format_t alsa_sample_format_to_pcm_format(alsa_sample_format_t fmt) {
  switch (fmt) {
    case ALSA_SAMPLE_FORMAT_S16_LE:
      return SND_PCM_FORMAT_S16_LE;
    case ALSA_SAMPLE_FORMAT_S24_3_LE:
      return SND_PCM_FORMAT_S24_3LE;
    case ALSA_SAMPLE_FORMAT_S24_4_LE:
      return SND_PCM_FORMAT_S24_LE;
    case ALSA_SAMPLE_FORMAT_S32_LE:
      return SND_PCM_FORMAT_S32_LE;
    case ALSA_SAMPLE_FORMAT_F32_LE:
      return SND_PCM_FORMAT_FLOAT_LE;
    case ALSA_SAMPLE_FORMAT_F64_LE:
      return SND_PCM_FORMAT_FLOAT64_LE;
    case ALSA_SAMPLE_FORMAT_DSD_U8:
      return SND_PCM_FORMAT_DSD_U8;
    case ALSA_SAMPLE_FORMAT_DSD_U16_LE:
      return SND_PCM_FORMAT_DSD_U16_LE;
    case ALSA_SAMPLE_FORMAT_DSD_U16_BE:
      return SND_PCM_FORMAT_DSD_U16_BE;
    case ALSA_SAMPLE_FORMAT_DSD_U32_LE:
      return SND_PCM_FORMAT_DSD_U32_LE;
    case ALSA_SAMPLE_FORMAT_DSD_U32_BE:
      return SND_PCM_FORMAT_DSD_U32_BE;
    default:
      return SND_PCM_FORMAT_UNKNOWN;
  }
}

binary_sample_format_t alsa_pcm_format_to_binary_format(snd_pcm_format_t fmt) {
  switch (fmt) {
    case SND_PCM_FORMAT_S16_LE:
      return BINARY_SAMPLE_FORMAT_S16_LE;
    case SND_PCM_FORMAT_S16_BE:
      return BINARY_SAMPLE_FORMAT_S16_BE;
    case SND_PCM_FORMAT_S24_3LE:
      return BINARY_SAMPLE_FORMAT_S24_3_LE;
    case SND_PCM_FORMAT_S24_3BE:
      return BINARY_SAMPLE_FORMAT_S24_3_BE;
    case SND_PCM_FORMAT_S24_LE:
      return BINARY_SAMPLE_FORMAT_S24_4_LJ_LE;
    case SND_PCM_FORMAT_S24_BE:
      return BINARY_SAMPLE_FORMAT_S24_4_LJ_BE;
    case SND_PCM_FORMAT_S32_LE:
      return BINARY_SAMPLE_FORMAT_S32_LE;
    case SND_PCM_FORMAT_S32_BE:
      return BINARY_SAMPLE_FORMAT_S32_BE;
    case SND_PCM_FORMAT_FLOAT_LE:
      return BINARY_SAMPLE_FORMAT_F32_LE;
    case SND_PCM_FORMAT_FLOAT_BE:
      return BINARY_SAMPLE_FORMAT_F32_BE;
    case SND_PCM_FORMAT_FLOAT64_LE:
      return BINARY_SAMPLE_FORMAT_F64_LE;
    case SND_PCM_FORMAT_FLOAT64_BE:
      return BINARY_SAMPLE_FORMAT_F64_BE;
    case SND_PCM_FORMAT_DSD_U8:
      return BINARY_SAMPLE_FORMAT_DSD_U8;
    case SND_PCM_FORMAT_DSD_U16_LE:
      return BINARY_SAMPLE_FORMAT_DSD_U16_LE;
    case SND_PCM_FORMAT_DSD_U16_BE:
      return BINARY_SAMPLE_FORMAT_DSD_U16_BE;
    case SND_PCM_FORMAT_DSD_U32_LE:
      return BINARY_SAMPLE_FORMAT_DSD_U32_LE;
    case SND_PCM_FORMAT_DSD_U32_BE:
      return BINARY_SAMPLE_FORMAT_DSD_U32_BE;
    default:
      return BINARY_SAMPLE_FORMAT_INVALID;
  }
}

int alsa_apply_format(snd_pcm_t* pcm, snd_pcm_hw_params_t* hwp, bool has_format,
                      alsa_sample_format_t requested_format,
                      snd_pcm_format_t* out_format) {
  snd_pcm_format_t formats[6];
  size_t num_formats = 0;
  if (has_format) {
    snd_pcm_format_t pcm_fmt =
        alsa_sample_format_to_pcm_format(requested_format);
    if (pcm_fmt != SND_PCM_FORMAT_UNKNOWN) {
      formats[0] = pcm_fmt;
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

  for (size_t i = 0; i < num_formats; i++) {
    int rc = snd_pcm_hw_params_set_format(pcm, hwp, formats[i]);
    if (rc >= 0) {
      if (out_format) *out_format = formats[i];
      return 0;
    }
  }
  return -EINVAL;
}

snd_hctl_elem_t* alsa_find_elem(snd_hctl_t* hctl, snd_ctl_elem_iface_t iface,
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
    logger_debug(&g_alsa_dev_logger, "Found element with name %s and numid %u",
                 name, out_numid ? *out_numid : 0);
  }
  return elem;
}

bool alsa_elem_read_as_int(snd_hctl_elem_t* elem, long* out_val) {
  if (!elem) return false;
  snd_ctl_elem_value_t* val;
  snd_ctl_elem_value_alloca(&val);
  if (snd_hctl_elem_read(elem, val) >= 0) {
    if (out_val) *out_val = snd_ctl_elem_value_get_integer(val, 0);
    return true;
  }
  return false;
}

bool alsa_elem_read_as_bool(snd_hctl_elem_t* elem, bool* out_val) {
  if (!elem) return false;
  snd_ctl_elem_value_t* val;
  snd_ctl_elem_value_alloca(&val);
  if (snd_hctl_elem_read(elem, val) >= 0) {
    if (out_val) *out_val = (snd_ctl_elem_value_get_boolean(val, 0) != 0);
    return true;
  }
  return false;
}

bool alsa_elem_read_volume_in_db(snd_ctl_t* ctl, snd_hctl_elem_t* elem,
                                 double* out_db) {
  if (!ctl || !elem) return false;
  long intval = 0;
  if (!alsa_elem_read_as_int(elem, &intval)) return false;

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

void alsa_elem_write_as_int(snd_hctl_elem_t* elem, long value) {
  if (!elem) return;
  snd_ctl_elem_value_t* val;
  snd_ctl_elem_value_alloca(&val);
  snd_ctl_elem_value_set_integer(val, 0, value);
  snd_hctl_elem_write(elem, val);
}

void alsa_elem_write_as_bool(snd_hctl_elem_t* elem, bool value) {
  if (!elem) return;
  snd_ctl_elem_value_t* val;
  snd_ctl_elem_value_alloca(&val);
  snd_ctl_elem_value_set_boolean(val, 0, value ? 1 : 0);
  snd_hctl_elem_write(elem, val);
}

void alsa_elem_write_volume_in_db(snd_ctl_t* ctl, snd_hctl_elem_t* elem,
                                  double db_val) {
  if (!ctl || !elem) return;
  snd_ctl_elem_id_t* id;
  snd_ctl_elem_id_alloca(&id);
  snd_hctl_elem_get_id(elem, id);

  long intval = 0;
  if (snd_ctl_convert_from_dB(ctl, id, (long)(db_val * 100.0), &intval, -1) >=
      0) {
    alsa_elem_write_as_int(elem, intval);
  }
}

// Calculate a power-of-two buffer size that is large enough to accommodate any
// changes due to resampling, and at least 4 times the minimum period size to
// avoid random broken pipes. (buffermanager.rs:34-44)
static snd_pcm_uframes_t calculate_buffer_size(size_t chunksize,
                                               double resampling_ratio,
                                               snd_pcm_uframes_t min_period) {
  if (resampling_ratio <= 0.0) resampling_ratio = 1.0;
  double frames_needed = 3.0 * (double)chunksize / resampling_ratio;
  if (frames_needed < 4.0 * (double)min_period) {
    frames_needed = 4.0 * (double)min_period;
    logger_debug(
        &g_alsa_dev_logger,
        "Minimum period is %lu frames, buffer size is minimum %.0f frames",
        (unsigned long)min_period, frames_needed);
  }
  return (snd_pcm_uframes_t)pow(2.0, ceil(log2(frames_needed)));
}

// Calculate an alternative buffer size that is 3 multiplied by a power-of-two,
// and at least 4 times the minimum period size to avoid random broken pipes.
// (buffermanager.rs:51-61)
static snd_pcm_uframes_t calculate_buffer_size_alt(
    size_t chunksize, double resampling_ratio, snd_pcm_uframes_t min_period) {
  if (resampling_ratio <= 0.0) resampling_ratio = 1.0;
  double frames_needed = 3.0 * (double)chunksize / resampling_ratio;
  if (frames_needed < 4.0 * (double)min_period) {
    frames_needed = 4.0 * (double)min_period;
    logger_debug(&g_alsa_dev_logger,
                 "Minimum period is %lu frames, alternate buffer size is "
                 "minimum %.0f frames",
                 (unsigned long)min_period, frames_needed);
  }
  return (snd_pcm_uframes_t)(3.0 * pow(2.0, ceil(log2(frames_needed / 3.0))));
}

// Calculate a buffer size and apply it to a hwp container. Only for use when
// opening a device. (buffermanager.rs:64-83)
int alsa_apply_buffer_size(snd_pcm_t* pcm, snd_pcm_hw_params_t* hwp,
                           size_t chunksize, double resampling_ratio,
                           snd_pcm_uframes_t* out_bufsize) {
  snd_pcm_uframes_t min_period = 0;
  snd_pcm_hw_params_get_period_size_min(hwp, &min_period, NULL);
  snd_pcm_uframes_t buffer_frames =
      calculate_buffer_size(chunksize, resampling_ratio, min_period);
  snd_pcm_uframes_t alt_buffer_frames =
      calculate_buffer_size_alt(chunksize, resampling_ratio, min_period);

  logger_debug(&g_alsa_dev_logger, "Setting buffer size to %lu frames",
               (unsigned long)buffer_frames);
  snd_pcm_uframes_t val = buffer_frames;
  int rc = snd_pcm_hw_params_set_buffer_size_near(pcm, hwp, &val);
  if (rc >= 0) {
    *out_bufsize = val;
  } else {
    logger_debug(&g_alsa_dev_logger,
                 "Device did not accept a buffer size of %lu frames, trying "
                 "again with %lu",
                 (unsigned long)buffer_frames,
                 (unsigned long)alt_buffer_frames);
    val = alt_buffer_frames;
    rc = snd_pcm_hw_params_set_buffer_size_near(pcm, hwp, &val);
    if (rc >= 0) {
      *out_bufsize = val;
    } else {
      return rc;
    }
  }
  logger_debug(&g_alsa_dev_logger,
               "Device is using a buffer size of %lu frames",
               (unsigned long)*out_bufsize);
  return 0;
}

// Calculate a period size and apply it to a hwp container. Only for use when
// opening a device, after setting buffer size. (buffermanager.rs:86-106)
int alsa_apply_period_size(snd_pcm_t* pcm, snd_pcm_hw_params_t* hwp,
                           snd_pcm_uframes_t bufsize,
                           snd_pcm_uframes_t* out_period) {
  snd_pcm_uframes_t period_frames = bufsize / 8;
  logger_debug(&g_alsa_dev_logger, "Setting period size to %lu frames",
               (unsigned long)period_frames);
  int dir = 0;
  snd_pcm_uframes_t val = period_frames;
  int rc = snd_pcm_hw_params_set_period_size_near(pcm, hwp, &val, &dir);
  if (rc >= 0) {
    *out_period = val;
  } else {
    snd_pcm_uframes_t alt_period_frames =
        (snd_pcm_uframes_t)(3.0 *
                            pow(2.0, ceil(log2((double)period_frames / 2.0))));
    logger_debug(&g_alsa_dev_logger,
                 "Device did not accept a period size of %lu frames, trying "
                 "again with %lu",
                 (unsigned long)period_frames,
                 (unsigned long)alt_period_frames);
    val = alt_period_frames;
    rc = snd_pcm_hw_params_set_period_size_near(pcm, hwp, &val, &dir);
    if (rc >= 0) {
      *out_period = val;
    } else {
      return rc;
    }
  }
  logger_debug(&g_alsa_dev_logger,
               "Device is using a period size of %lu frames",
               (unsigned long)*out_period);
  return 0;
}

// Recovers an ALSA PCM handle from a suspended state. (utils.rs:279-310)
int alsa_recover_suspended_pcm(snd_pcm_t* pcm, const char* direction) {
  logger_warn(&g_alsa_dev_logger, "%s: device is suspended, trying to resume",
              direction);
  for (int attempt = 0; attempt < 200; attempt++) {
    int res = snd_pcm_resume(pcm);
    if (res == 0) {
      logger_info(&g_alsa_dev_logger, "%s: resumed suspended ALSA device",
                  direction);
      return 0;
    }
    if (res == -EAGAIN) {
      if (attempt >= 199) {
        logger_warn(&g_alsa_dev_logger,
                    "%s: resume is still pending after %d attempts, falling "
                    "back to prepare",
                    direction, attempt + 1);
        break;
      }
      cdsp_sleep_ms(10);
      continue;
    }
    logger_debug(&g_alsa_dev_logger,
                 "%s: resume failed with %s, falling back to prepare",
                 direction, snd_strerror(res));
    break;
  }
  return snd_pcm_prepare(pcm);
}

// Returns the descriptive string representation of an ALSA PCM state.
// (utils.rs:255-277)
const char* alsa_state_desc(snd_pcm_state_t state) {
  switch (state) {
    case SND_PCM_STATE_OPEN:
      return "SND_PCM_STATE_OPEN, Open";
    case SND_PCM_STATE_SETUP:
      return "SND_PCM_STATE_SETUP, Setup installed";
    case SND_PCM_STATE_PREPARED:
      return "SND_PCM_STATE_PREPARED, Ready to start";
    case SND_PCM_STATE_RUNNING:
      return "SND_PCM_STATE_RUNNING, Running";
    case SND_PCM_STATE_XRUN:
      return "SND_PCM_STATE_XRUN, Stopped: underrun (playback) or overrun "
             "(capture) detected";
    case SND_PCM_STATE_DRAINING:
      return "SND_PCM_STATE_DRAINING, Draining: running (playback) or stopped "
             "(capture)";
    case SND_PCM_STATE_PAUSED:
      return "SND_PCM_STATE_PAUSED, Paused";
    case SND_PCM_STATE_SUSPENDED:
      return "SND_PCM_STATE_SUSPENDED, Hardware is suspended";
    case SND_PCM_STATE_DISCONNECTED:
      return "SND_PCM_STATE_DISCONNECTED, Hardware is disconnected";
    default:
      return "Unknown ALSA PCM state";
  }
}

int alsa_device_open_and_configure_hw(
    snd_pcm_t** pcm, const char* device_name, snd_pcm_stream_t stream,
    int channels, unsigned int sample_rate, bool has_format,
    alsa_sample_format_t requested_format, size_t chunk_size,
    double resampling_ratio, snd_pcm_format_t* out_format,
    snd_pcm_uframes_t* out_bufsize, snd_pcm_uframes_t* out_period,
    bool* out_can_pause, char* out_error_msg, size_t error_msg_len) {
  if (!pcm || !device_name) return -EINVAL;

  int rc = snd_pcm_open(pcm, device_name, stream, SND_PCM_NONBLOCK);
  if (rc < 0) {
    if (out_error_msg && error_msg_len > 0) {
      snprintf(out_error_msg, error_msg_len, "%s", snd_strerror(rc));
    }
    return rc;
  }

  snd_pcm_hw_params_t* params;
  snd_pcm_hw_params_alloca(&params);
  rc = snd_pcm_hw_params_any(*pcm, params);
  if (rc < 0) {
    if (out_error_msg && error_msg_len > 0) {
      snprintf(out_error_msg, error_msg_len, "%s", snd_strerror(rc));
    }
    snd_pcm_close(*pcm);
    *pcm = NULL;
    return rc;
  }

  // Set channels
  rc = snd_pcm_hw_params_set_channels(*pcm, params, channels);
  if (rc < 0) {
    if (out_error_msg && error_msg_len > 0) {
      snprintf(out_error_msg, error_msg_len, "%s", snd_strerror(rc));
    }
    snd_pcm_close(*pcm);
    *pcm = NULL;
    return rc;
  }

  // Set sample rate
  unsigned int val = sample_rate;
  int dir = 0;
  rc = snd_pcm_hw_params_set_rate_near(*pcm, params, &val, &dir);
  if (rc < 0) {
    if (out_error_msg && error_msg_len > 0) {
      snprintf(out_error_msg, error_msg_len, "%s", snd_strerror(rc));
    }
    snd_pcm_close(*pcm);
    *pcm = NULL;
    return rc;
  }

  // Set sample format
  rc =
      alsa_apply_format(*pcm, params, has_format, requested_format, out_format);
  if (rc < 0) {
    if (out_error_msg && error_msg_len > 0) {
      snprintf(out_error_msg, error_msg_len,
               "Requested or supported ALSA format not available");
    }
    snd_pcm_close(*pcm);
    *pcm = NULL;
    return rc;
  }

  // Set access mode
  rc =
      snd_pcm_hw_params_set_access(*pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED);
  if (rc < 0) {
    if (out_error_msg && error_msg_len > 0) {
      snprintf(out_error_msg, error_msg_len, "%s", snd_strerror(rc));
    }
    snd_pcm_close(*pcm);
    *pcm = NULL;
    return rc;
  }

  // Set buffer size
  if (alsa_apply_buffer_size(*pcm, params, chunk_size, resampling_ratio,
                             out_bufsize) < 0) {
    if (out_error_msg && error_msg_len > 0) {
      snprintf(out_error_msg, error_msg_len, "Failed to set ALSA buffer size");
    }
    snd_pcm_close(*pcm);
    *pcm = NULL;
    return -EINVAL;
  }

  // Set period size
  if (alsa_apply_period_size(*pcm, params, *out_bufsize, out_period) < 0) {
    if (out_error_msg && error_msg_len > 0) {
      snprintf(out_error_msg, error_msg_len, "Failed to set ALSA period size");
    }
    snd_pcm_close(*pcm);
    *pcm = NULL;
    return -EINVAL;
  }

  rc = snd_pcm_hw_params(*pcm, params);
  if (rc < 0) {
    if (out_error_msg && error_msg_len > 0) {
      snprintf(out_error_msg, error_msg_len, "%s", snd_strerror(rc));
    }
    snd_pcm_close(*pcm);
    *pcm = NULL;
    return rc;
  }

  if (out_can_pause) {
    *out_can_pause = (snd_pcm_hw_params_can_pause(params) != 0);
  }

  return 0;
}

int alsa_device_configure_sw(snd_pcm_t* pcm, snd_pcm_uframes_t avail_min,
                             snd_pcm_uframes_t start_threshold) {
  if (!pcm) return -EINVAL;

  snd_pcm_sw_params_t* sw_params;
  snd_pcm_sw_params_alloca(&sw_params);
  int rc = snd_pcm_sw_params_current(pcm, sw_params);
  if (rc < 0) return rc;

  snd_pcm_sw_params_set_start_threshold(pcm, sw_params, start_threshold);
  snd_pcm_sw_params_set_avail_min(pcm, sw_params, avail_min);
  return snd_pcm_sw_params(pcm, sw_params);
}

bool alsa_device_get_card_ctl_name(snd_pcm_t* pcm, char* out_ctl_name,
                                   size_t max_len, int* out_dev_idx,
                                   int* out_subdev_idx) {
  if (!pcm || !out_ctl_name || max_len == 0) return false;

  snd_pcm_info_t* pcm_info;
  snd_pcm_info_alloca(&pcm_info);
  if (snd_pcm_info(pcm, pcm_info) < 0) return false;

  int card = snd_pcm_info_get_card(pcm_info);
  if (card < 0) return false;

  snprintf(out_ctl_name, max_len, "hw:%d", card);
  if (out_dev_idx) {
    *out_dev_idx = snd_pcm_info_get_device(pcm_info);
  }
  if (out_subdev_idx) {
    *out_subdev_idx = snd_pcm_info_get_subdevice(pcm_info);
  }
  return true;
}

bool alsa_device_prime_delay(snd_pcm_t* pcm, size_t target_level,
                             snd_pcm_uframes_t bufsize, int sample_rate,
                             size_t blockalign, size_t queued_frames,
                             const void* silence_buf, size_t silence_buf_size) {
  size_t target_frames = target_level < bufsize ? target_level : bufsize;
  if (target_frames == 0) return true;

  snd_pcm_sframes_t avail = snd_pcm_avail(pcm);
  size_t current_delay = 0;
  if (avail >= 0 && (snd_pcm_uframes_t)avail <= bufsize) {
    current_delay = bufsize - (snd_pcm_uframes_t)avail;
  }
  size_t queued_total = current_delay + queued_frames;
  if (queued_total >= target_frames) return true;
  size_t missing_frames = target_frames - queued_total;
  size_t silence_bytes = missing_frames * blockalign;

  if (silence_bytes > silence_buf_size) {
    logger_warn(&g_alsa_dev_logger, "Playback silence buffer is too small");
    return false;
  }

  size_t bytes_written = 0;
  int retries = 0;
  double millis_per_frame = 1000.0 / (double)sample_rate;
  uint32_t timeout_millis =
      (uint32_t)(2.0 * millis_per_frame * (double)bufsize);
  if (timeout_millis < 20) timeout_millis = 20;

  while (bytes_written < silence_bytes) {
    retries++;
    if (retries >= 100) {
      logger_warn(&g_alsa_dev_logger,
                  "Aborting playback silence priming after too many retries");
      return false;
    }
    const uint8_t* slice = (const uint8_t*)silence_buf + bytes_written;
    size_t frames_to_write = (silence_bytes - bytes_written) / blockalign;
    snd_pcm_sframes_t rc = snd_pcm_writei(pcm, slice, frames_to_write);
    if (rc == 0) {
      if (snd_pcm_wait(pcm, (int)timeout_millis) <= 0) {
        logger_warn(&g_alsa_dev_logger,
                    "Timed out while priming playback delay");
        return false;
      }
    } else if (rc > 0) {
      bytes_written += (size_t)rc * blockalign;
    } else {
      int err = (int)rc;
      if (err == -EAGAIN) {
        if (snd_pcm_wait(pcm, (int)timeout_millis) <= 0) {
          logger_warn(&g_alsa_dev_logger,
                      "Timed out while waiting to prime playback delay");
          return false;
        }
      } else if (err == -EPIPE) {
        logger_warn(&g_alsa_dev_logger,
                    "PB: silence priming underrun, trying to recover");
        snd_pcm_prepare(pcm);
        bytes_written = 0;
      } else if (err == -ESTRPIPE ||
                 snd_pcm_state(pcm) == SND_PCM_STATE_SUSPENDED) {
        logger_warn(
            &g_alsa_dev_logger,
            "PB: silence priming interrupted by suspend, trying to recover");
        alsa_recover_suspended_pcm(pcm, "PB");
        bytes_written = 0;
      } else {
        logger_warn(&g_alsa_dev_logger,
                    "PB: failed to prime playback delay: %s",
                    snd_strerror(err));
        return false;
      }
    }
  }
  logger_trace(&g_alsa_dev_logger,
               "PB: primed playback delay with %zu silent frames",
               missing_frames);
  return true;
}

#endif
