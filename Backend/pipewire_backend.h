/**
 * @file pipewire_backend.h
 * @brief PipeWire capture and playback backends.
 */

#ifndef CLIB_BACKEND_PIPEWIRE_BACKEND_H
#define CLIB_BACKEND_PIPEWIRE_BACKEND_H

#if defined(ENABLE_PIPEWIRE)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
 * @brief Opaque structure representing processing parameters.
 */
typedef struct processing_parameters processing_parameters_t;

// Capture backend factory & methods

/**
 * @brief Create a PipeWire capture backend instance.
 *
 * @param config Pointer to the capture device configuration.
 * @param sample_rate The sample rate in Hz.
 * @param chunk_size The size of each audio chunk in frames.
 * @param params Pointer to processing parameters.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return Pointer to the created capture_backend_t instance, or NULL on
 * failure.
 */
capture_backend_t* pipewire_capture_create(
    const capture_device_config_t* config, int sample_rate, int chunk_size,
    processing_parameters_t* params, backend_error_t* err);

// Playback backend factory & methods

/**
 * @brief Create a PipeWire playback backend instance.
 *
 * @param config Pointer to the playback device configuration.
 * @param sample_rate The sample rate in Hz.
 * @param chunk_size The size of each audio chunk in frames.
 * @param params Pointer to processing parameters.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return Pointer to the created playback_backend_t instance, or NULL on
 * failure.
 */
playback_backend_t* pipewire_playback_create(
    const playback_device_config_t* config, int sample_rate, int chunk_size,
    processing_parameters_t* params, backend_error_t* err);

#endif  // ENABLE_PIPEWIRE

#endif  // CLIB_BACKEND_PIPEWIRE_BACKEND_H
