/**
 * @file generator_capture.h
 * @brief Signal generator-based audio capture backend.
 */

#ifndef CLIB_BACKEND_GENERATOR_CAPTURE_H
#define CLIB_BACKEND_GENERATOR_CAPTURE_H

#include "audio_backend.h"

/**
 * @brief Opaque structure representing the generator capture backend.
 */
typedef struct generator_capture generator_capture_t;

/**
 * @brief Global virtual method table for Generator capture backend.
 */
extern const capture_backend_vtable_t g_generator_capture_vtable;

#endif  // CLIB_BACKEND_GENERATOR_CAPTURE_H
