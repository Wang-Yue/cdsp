/**
 * @file wasapi_capture.h
 * @brief WASAPI capture backend interface.
 */

#ifndef CLIB_BACKEND_WASAPI_CAPTURE_H
#define CLIB_BACKEND_WASAPI_CAPTURE_H

#if defined(ENABLE_WASAPI)

#include "audio_backend.h"

typedef struct wasapi_capture wasapi_capture_t;

/**
 * @brief Global virtual method table for WASAPI capture backend.
 */
extern const capture_backend_vtable_t g_wasapi_capture_vtable;

#endif // ENABLE_WASAPI

#endif // CLIB_BACKEND_WASAPI_CAPTURE_H
