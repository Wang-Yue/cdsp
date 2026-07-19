#ifndef CLIB_BACKEND_ASIO_BACKEND_H
#define CLIB_BACKEND_ASIO_BACKEND_H

#if defined(ENABLE_ASIO)

#include "audio_backend.h"

/**
 * @file asio_backend.h
 * @brief ASIO capture and playback backend factory.
 *
 * Provides factory functions to create capture and playback backend instances
 * using the ASIO (Audio Stream Input/Output) API.
 */

/**
 * @struct asio_capture
 * @brief Opaque structure representing the ASIO capture backend.
 */
typedef struct asio_capture asio_capture_t;

/**
 * @struct asio_playback
 * @brief Opaque structure representing the ASIO playback backend.
 */
typedef struct asio_playback asio_playback_t;

/**
 * @brief Global virtual method table for ASIO capture backend.
 */
extern const capture_backend_vtable_t g_asio_capture_vtable;

/**
 * @brief Global virtual method table for ASIO playback backend.
 */
extern const playback_backend_vtable_t g_asio_playback_vtable;

#endif  // ENABLE_ASIO

#endif  // CLIB_BACKEND_ASIO_BACKEND_H
