#if defined(ENABLE_ALSA)
#include "alsa_device.h"

#include <pthread.h>

#include <errno.h>
#include <math.h>
#include <pthread.h>

#include "Logging/app_logger.h"
#include "Utils/cdsp_time.h"

static const logger_t g_alsa_dev_logger = {"dsp.backend.alsa"};

pthread_mutex_t g_alsa_mutex = PTHREAD_MUTEX_INITIALIZER;

static snd_pcm_uframes_t calculate_buffer_size(size_t chunksize, double ratio,
                                                snd_pcm_uframes_t min_period) {
  double frames_needed =
      3.0 * (double)chunksize / (ratio > 0.0 ? ratio : 1.0);
  if (frames_needed < 4.0 * (double)min_period) {
    frames_needed = 4.0 * (double)min_period;
  }
  return (snd_pcm_uframes_t)pow(2.0, ceil(log2(frames_needed)));
}

static snd_pcm_uframes_t calculate_buffer_size_alt(
    size_t chunksize, double ratio, snd_pcm_uframes_t min_period) {
  double frames_needed =
      3.0 * (double)chunksize / (ratio > 0.0 ? ratio : 1.0);
  if (frames_needed < 4.0 * (double)min_period) {
    frames_needed = 4.0 * (double)min_period;
  }
  return (snd_pcm_uframes_t)(3.0 * pow(2.0, ceil(log2(frames_needed / 3.0))));
}

/**
 * @brief Calculates and applies a power-of-two buffer size to an ALSA hardware parameters container.
 *
 * @param pcm Pointer to the ALSA PCM handle.
 * @param hwp Pointer to the ALSA hardware parameters container.
 * @param chunksize The requested audio chunk size in frames.
 * @param resampling_ratio The resampling ratio (input rate / output rate).
 * @param out_bufsize Pointer to receive the applied buffer size in frames.
 * @return 0 on success, or a negative ALSA error code on failure.
 */
int alsa_apply_buffer_size(snd_pcm_t* pcm, snd_pcm_hw_params_t* hwp,
                           size_t chunksize, double resampling_ratio,
                           snd_pcm_uframes_t* out_bufsize) {
  snd_pcm_uframes_t min_period = 0;
  snd_pcm_hw_params_get_period_size_min(hwp, &min_period, NULL);
  snd_pcm_uframes_t buffer_frames =
      calculate_buffer_size(chunksize, resampling_ratio, min_period);
  snd_pcm_uframes_t alt_buffer_frames =
      calculate_buffer_size_alt(chunksize, resampling_ratio, min_period);

  snd_pcm_uframes_t val = buffer_frames;
  int rc = snd_pcm_hw_params_set_buffer_size_near(pcm, hwp, &val);
  if (rc >= 0) {
    *out_bufsize = val;
  } else {
    val = alt_buffer_frames;
    rc = snd_pcm_hw_params_set_buffer_size_near(pcm, hwp, &val);
    if (rc >= 0) {
      *out_bufsize = val;
    } else {
      return rc;
    }
  }
  return 0;
}

/**
 * @brief Calculates and applies a period size to an ALSA hardware parameters container.
 *
 * @param pcm Pointer to the ALSA PCM handle.
 * @param hwp Pointer to the ALSA hardware parameters container.
 * @param bufsize The configured buffer size in frames.
 * @param out_period Pointer to receive the applied period size in frames.
 * @return 0 on success, or a negative ALSA error code on failure.
 */
int alsa_apply_period_size(snd_pcm_t* pcm, snd_pcm_hw_params_t* hwp,
                           snd_pcm_uframes_t bufsize,
                           snd_pcm_uframes_t* out_period) {
  snd_pcm_uframes_t period_frames = bufsize / 8;
  int dir = 0;
  snd_pcm_uframes_t val = period_frames;
  int rc = snd_pcm_hw_params_set_period_size_near(pcm, hwp, &val, &dir);
  if (rc >= 0) {
    *out_period = val;
  } else {
    snd_pcm_uframes_t alt_period_frames =
        (snd_pcm_uframes_t)(3.0 *
                            pow(2.0, ceil(log2((double)period_frames / 2.0))));
    val = alt_period_frames;
    rc = snd_pcm_hw_params_set_period_size_near(pcm, hwp, &val, &dir);
    if (rc >= 0) {
      *out_period = val;
    } else {
      return rc;
    }
  }
  return 0;
}

/**
 * @brief Recovers an ALSA PCM handle from a suspended state (SND_PCM_STATE_SUSPENDED / ESTRPIPE).
 *
 * @param pcm Pointer to the ALSA PCM handle.
 * @param direction String label ("PB" for playback, "CAP" for capture) used for logging.
 * @return 0 on success, or a negative ALSA error code on failure.
 */
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

#endif
