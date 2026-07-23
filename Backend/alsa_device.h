#ifndef CLIB_BACKEND_ALSA_DEVICE_H
#define CLIB_BACKEND_ALSA_DEVICE_H

#if defined(ENABLE_ALSA)

#include <alsa/asoundlib.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @file alsa_device.h
 * @brief Provides shared ALSA device resources, such as mutexes.
 */

/**
 * @brief Global mutex for protecting ALSA API calls.
 *
 * Since ALSA functions are not all thread-safe or need synchronization
 * when accessed from multiple threads (e.g. playback thread vs control thread),
 * this mutex should be locked before performing ALSA operations.
 */
extern pthread_mutex_t g_alsa_mutex;

/**
 * @brief Calculates and applies a power-of-two buffer size to an ALSA hardware parameters container.
 *
 * Computes a buffer size large enough to accommodate any changes due to resampling,
 * requiring at least 4 times the minimum period size to prevent buffer underruns.
 * If the primary power-of-two buffer size is rejected by the ALSA driver, it falls
 * back to an alternate buffer size calculated as 3 multiplied by a power of two.
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
                           snd_pcm_uframes_t* out_bufsize);

/**
 * @brief Calculates and applies a period size to an ALSA hardware parameters container.
 *
 * Computes a period size derived from the buffer size (bufsize / 8). If the primary
 * period size is rejected by the ALSA driver, it falls back to an alternate period
 * size calculated as 3 multiplied by a power of two.
 *
 * @param pcm Pointer to the ALSA PCM handle.
 * @param hwp Pointer to the ALSA hardware parameters container.
 * @param bufsize The configured buffer size in frames.
 * @param out_period Pointer to receive the applied period size in frames.
 * @return 0 on success, or a negative ALSA error code on failure.
 */
int alsa_apply_period_size(snd_pcm_t* pcm, snd_pcm_hw_params_t* hwp,
                           snd_pcm_uframes_t bufsize,
                           snd_pcm_uframes_t* out_period);

/**
 * @brief Recovers an ALSA PCM handle from a suspended state (SND_PCM_STATE_SUSPENDED / ESTRPIPE).
 *
 * Loops up to 200 times sleeping 10 ms while attempting snd_pcm_resume(). If resume
 * succeeds, returns 0. If resume fails or is pending, falls back to snd_pcm_prepare().
 *
 * @param pcm Pointer to the ALSA PCM handle.
 * @param direction String label ("PB" for playback, "CAP" for capture) used for logging.
 * @return 0 on success, or a negative ALSA error code on failure.
 */
int alsa_recover_suspended_pcm(snd_pcm_t* pcm, const char* direction);

#endif  // ENABLE_ALSA

#endif  // CLIB_BACKEND_ALSA_DEVICE_H
