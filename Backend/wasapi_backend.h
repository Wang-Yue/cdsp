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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
 * @typedef processing_parameters_t
 * @brief Opaque structure representing processing parameters.
 */
typedef struct processing_parameters processing_parameters_t;

// Capture backend factory & methods

/**
 * @brief Creates a WASAPI capture backend.
 *
 * @param config Configuration for the capture device.
 * @param sample_rate The sample rate in Hz.
 * @param chunk_size The chunk size in frames.
 * @param params Processing parameters.
 * @param err Pointer to a backend_error_t to receive error details on failure.
 * @return A pointer to the created capture_backend_t, or NULL on failure.
 */
capture_backend_t* wasapi_capture_create(const capture_device_config_t* config,
                                         int sample_rate, int chunk_size,
                                         processing_parameters_t* params,
                                         backend_error_t* err);

// Playback backend factory & methods

/**
 * @brief Creates a WASAPI playback backend.
 *
 * @param config Configuration for the playback device.
 * @param sample_rate The sample rate in Hz.
 * @param chunk_size The chunk size in frames.
 * @param params Processing parameters.
 * @param err Pointer to a backend_error_t to receive error details on failure.
 * @return A pointer to the created playback_backend_t, or NULL on failure.
 */
playback_backend_t* wasapi_playback_create(
    const playback_device_config_t* config, int sample_rate, int chunk_size,
    processing_parameters_t* params, backend_error_t* err);

#endif  // ENABLE_WASAPI

#endif  // CLIB_BACKEND_WASAPI_BACKEND_H
