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
 * @brief Provides shared ALSA device resources, mutexes, and buffer/period
 * sizing utilities.
 */

/**
 * @brief Global mutex for protecting ALSA API calls.
 *
 * Matches ALSA_MUTEX in upstream CamillaDSP (src/alsa_backend/device.rs:59).
 */
extern pthread_mutex_t g_alsa_mutex;

/**
 * @brief Calculates and applies a power-of-two buffer size to an ALSA hardware
 * parameters container.
 *
 * Matches calculate_buffer_size and apply_buffer_size in upstream CamillaDSP
 * (src/alsa_backend/buffermanager.rs:34-83).
 *
 * @param pcm Pointer to the ALSA PCM handle.
 * @param hwp Pointer to the ALSA hardware parameters container.
 * @param chunksize The requested audio chunk size in frames.
 * @param resampling_ratio Ratio of samplerate / capture_samplerate (or 1.0 for
 * playback).
 * @param out_bufsize Pointer to receive the applied buffer size in frames.
 * @return 0 on success, or a negative ALSA error code on failure.
 */
int alsa_apply_buffer_size(snd_pcm_t* pcm, snd_pcm_hw_params_t* hwp,
                           size_t chunksize, double resampling_ratio,
                           snd_pcm_uframes_t* out_bufsize);

/**
 * @brief Calculates and applies a period size to an ALSA hardware parameters
 * container.
 *
 * Matches apply_period_size in upstream CamillaDSP
 * (src/alsa_backend/buffermanager.rs:85-106).
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
 * @brief Recovers an ALSA PCM handle from a suspended state
 * (SND_PCM_STATE_SUSPENDED / ESTRPIPE).
 *
 * Matches recover_suspended_pcm in upstream CamillaDSP
 * (src/alsa_backend/utils.rs:279-310).
 *
 * @param pcm Pointer to the ALSA PCM handle.
 * @param direction String label ("PB" for playback, "Capture" for capture) used
 * for logging.
 * @return 0 on success, or a negative ALSA error code on failure.
 */
int alsa_recover_suspended_pcm(snd_pcm_t* pcm, const char* direction);

/**
 * @brief Returns the descriptive string representation of an ALSA PCM state.
 *
 * Matches state_desc in upstream CamillaDSP (src/alsa_backend/utils.rs:255-277).
 *
 * @param state ALSA PCM state code.
 * @return Constant string description.
 */
const char* alsa_state_desc(snd_pcm_state_t state);

#endif  // ENABLE_ALSA

#endif  // CLIB_BACKEND_ALSA_DEVICE_H
