#ifndef CLIB_BACKEND_ALSA_PLAYBACK_H
#define CLIB_BACKEND_ALSA_PLAYBACK_H

#if defined(ENABLE_ALSA)

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

/**
 * @brief Global virtual method table for ALSA playback backend.
 */
extern const playback_backend_vtable_t g_alsa_playback_vtable;

#endif  // ENABLE_ALSA

#endif  // CLIB_BACKEND_ALSA_PLAYBACK_H
