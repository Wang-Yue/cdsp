/**
 * @file wasapi_backend.h
 * @brief WASAPI backend for audio capture and playback.
 *
 * This file provides the interface for creating and managing WASAPI
 * capture and playback backends.
 */

#ifndef CLIB_BACKEND_WASAPI_BACKEND_H
#define CLIB_BACKEND_WASAPI_BACKEND_H

#if defined(ENABLE_WASAPI)

#include "audio_backend.h"

/**
 * @typedef wasapi_capture_t
 * @brief Opaque structure representing a WASAPI capture session.
 */
typedef struct wasapi_capture wasapi_capture_t;

/**
 * @typedef wasapi_playback_t
 * @brief Opaque structure representing a WASAPI playback session.
 */
typedef struct wasapi_playback wasapi_playback_t;

/**
 * @brief Global virtual method table for WASAPI capture backend.
 */
extern const capture_backend_vtable_t g_wasapi_capture_vtable;

/**
 * @brief Global virtual method table for WASAPI playback backend.
 */
extern const playback_backend_vtable_t g_wasapi_playback_vtable;

#endif  // ENABLE_WASAPI

#endif  // CLIB_BACKEND_WASAPI_BACKEND_H
