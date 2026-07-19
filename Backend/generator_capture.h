/**
 * @file generator_capture.h
 * @brief Signal generator-based audio capture backend.
 */

#ifndef CLIB_BACKEND_GENERATOR_CAPTURE_H
#define CLIB_BACKEND_GENERATOR_CAPTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_backend.h"

/**
 * @brief Opaque structure representing the generator capture backend.
 */
typedef struct generator_capture generator_capture_t;

/**
 * @brief Opaque structure representing processing parameters.
 */
typedef struct processing_parameters processing_parameters_t;

/**
 * @brief Create a generator capture backend instance.
 *
 * @param config Pointer to the capture device configuration.
 * @param sample_rate The sample rate in Hz.
 * @param chunk_size The size of each audio chunk in frames.
 * @param params Pointer to processing parameters.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return Pointer to the created capture_backend_t instance, or NULL on
 * failure.
 */
capture_backend_t* generator_capture_create(
    const capture_device_config_t* config, int sample_rate, int chunk_size,
    processing_parameters_t* params, backend_error_t* err);

#endif  // CLIB_BACKEND_GENERATOR_CAPTURE_H
