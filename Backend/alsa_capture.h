/**
 * @file alsa_capture.h
 * @brief ALSA capture backend implementation.
 *
 * This file defines the ALSA capture backend, which implements the
 * `capture_backend_t` interface for capturing audio from an ALSA device.
 * These functions are only available if `ENABLE_ALSA` is defined.
 */

#ifndef CLIB_BACKEND_ALSA_CAPTURE_H
#define CLIB_BACKEND_ALSA_CAPTURE_H

#if defined(ENABLE_ALSA)

#include "audio_backend.h"

/**
 * @brief Opaque structure representing the ALSA capture backend instance.
 */
typedef struct alsa_capture alsa_capture_t;

/**
 * @brief Global virtual method table for ALSA capture backend.
 */
extern const capture_backend_vtable_t g_alsa_capture_vtable;

#endif  // ENABLE_ALSA

#endif  // CLIB_BACKEND_ALSA_CAPTURE_H
