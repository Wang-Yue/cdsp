/**
 * @file pipewire_backend.h
 * @brief PipeWire capture and playback backends.
 */

#ifndef CLIB_BACKEND_PIPEWIRE_BACKEND_H
#define CLIB_BACKEND_PIPEWIRE_BACKEND_H

#if defined(ENABLE_PIPEWIRE)

#include "audio_backend.h"

/**
 * @brief Opaque structure representing the PipeWire capture backend.
 */
typedef struct pipewire_capture pipewire_capture_t;

/**
 * @brief Opaque structure representing the PipeWire playback backend.
 */
typedef struct pipewire_playback pipewire_playback_t;

/**
 * @brief Global virtual method table for PipeWire capture backend.
 */
extern const capture_backend_vtable_t g_pipewire_capture_vtable;

/**
 * @brief Global virtual method table for PipeWire playback backend.
 */
extern const playback_backend_vtable_t g_pipewire_playback_vtable;

#endif  // ENABLE_PIPEWIRE

#endif  // CLIB_BACKEND_PIPEWIRE_BACKEND_H
