/**
 * @file file_backend.h
 * @brief File-based audio capture and playback backends.
 */

#ifndef CLIB_BACKEND_FILE_BACKEND_H
#define CLIB_BACKEND_FILE_BACKEND_H

#include "audio_backend.h"

/**
 * @brief Opaque structure representing the file capture backend.
 */
typedef struct file_capture file_capture_t;

/**
 * @brief Opaque structure representing the file playback backend.
 */
typedef struct file_playback file_playback_t;

/**
 * @brief Global virtual method table for File capture backend.
 */
extern const capture_backend_vtable_t g_file_capture_vtable;

/**
 * @brief Global virtual method table for File playback backend.
 */
extern const playback_backend_vtable_t g_file_playback_vtable;

#endif  // CLIB_BACKEND_FILE_BACKEND_H
