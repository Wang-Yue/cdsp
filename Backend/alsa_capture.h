/**
 * @file alsa_capture.h
 * @brief ALSA capture backend implementation.
 *
 * This file defines the ALSA capture backend, which implements the
 * `capture_backend_t` interface for capturing audio from an ALSA device.
 * These functions are only available if `ENABLE_ALSA` is defined.
 */

#ifndef CLIB_BACKEND_ALSA_CAPTURE_H
#define CLIB_BACKEND_ALSA_CAPTURE_H

#if defined(ENABLE_ALSA)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_backend.h"

/**
 * @brief Opaque structure representing the ALSA capture backend instance.
 */
typedef struct alsa_capture alsa_capture_t;

typedef struct processing_parameters processing_parameters_t;

/**
 * @brief Creates a new ALSA capture backend instance.
 *
 * This function instantiates the ALSA capture backend, configuring it with the
 * provided device config, sample rate, and chunk size. It returns the generic
 * `capture_backend_t` interface.
 *
 * @param config Pointer to the capture device configuration.
 * @param sample_rate The target sample rate.
 * @param chunk_size The target chunk size (number of frames per read).
 * @param params Pointer to the processing parameters for telemetry updates.
 * @param err Pointer to a backend_error_t to receive error details on failure.
 * @return Pointer to the generic capture_backend_t interface, or NULL on
 * failure.
 */
capture_backend_t* alsa_capture_create(const capture_device_config_t* config,
                                       int sample_rate, int chunk_size,
                                       processing_parameters_t* params,
                                       backend_error_t* err);

#endif  // ENABLE_ALSA

#endif  // CLIB_BACKEND_ALSA_CAPTURE_H
