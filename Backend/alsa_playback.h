#ifndef CLIB_BACKEND_ALSA_PLAYBACK_H
#define CLIB_BACKEND_ALSA_PLAYBACK_H

#if defined(ENABLE_ALSA)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_backend.h"

/**
 * @file alsa_playback.h
 * @brief ALSA playback backend implementation.
 *
 * Implements the playback backend interface for ALSA (Advanced Linux Sound
 * Architecture) devices.
 */

/**
 * @struct alsa_playback
 * @brief Opaque structure representing the ALSA playback backend.
 */
typedef struct alsa_playback alsa_playback_t;

typedef struct processing_parameters processing_parameters_t;

/**
 * @brief Create an ALSA playback backend instance.
 *
 * @param config Configuration for the playback device.
 * @param sample_rate The nominal sample rate in Hz.
 * @param chunk_size The size of each audio chunk in frames.
 * @param params Opaque processing parameters pointer.
 * @param[out] err Pointer to store error details if creation fails.
 * @return A pointer to the created playback_backend_t interface wrapper, or
 * NULL on error.
 */
playback_backend_t* alsa_playback_create(const playback_device_config_t* config,
                                         int sample_rate, int chunk_size,
                                         processing_parameters_t* params,
                                         backend_error_t* err);

#endif  // ENABLE_ALSA

#endif  // CLIB_BACKEND_ALSA_PLAYBACK_H

#endif  // ENABLE_ALSA

#endif  // CLIB_BACKEND_ALSA_PLAYBACK_H
