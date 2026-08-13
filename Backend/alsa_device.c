#include "Backend/alsa_device.h"

#if defined(ENABLE_ALSA)
#include <errno.h>
#include <math.h>
#include <pthread.h>

#include "Logging/app_logger.h"
#include "Utils/cdsp_time.h"

static const logger_t g_alsa_dev_logger = {"dsp.backend.alsa"};

pthread_mutex_t g_alsa_mutex = PTHREAD_MUTEX_INITIALIZER;

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

#endif
