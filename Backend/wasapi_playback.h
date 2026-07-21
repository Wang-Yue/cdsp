/**
 * @file wasapi_playback.h
 * @brief WASAPI playback backend interface.
 */

#ifndef CLIB_BACKEND_WASAPI_PLAYBACK_H
#define CLIB_BACKEND_WASAPI_PLAYBACK_H

#if defined(ENABLE_WASAPI)

#include "audio_backend.h"

typedef struct wasapi_playback wasapi_playback_t;

/**
 * @brief Global virtual method table for WASAPI playback backend.
 */
extern const playback_backend_vtable_t g_wasapi_playback_vtable;

#endif // ENABLE_WASAPI

#endif // CLIB_BACKEND_WASAPI_PLAYBACK_H
