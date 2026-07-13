#ifndef CLIB_BACKEND_AUDIO_BACKEND_FACTORY_H
#define CLIB_BACKEND_AUDIO_BACKEND_FACTORY_H

#include <stdbool.h>
#include <stddef.h>

#include "Config/engine_config_types.h"
#include "audio_backend.h"

/**
 * @file audio_backend_factory.h
 * @brief Factory for instantiating and mapping capture and playback backends.
 */

/**
 * @brief Creates a capture backend from configuration and maps error types
 * cleanly.
 *
 * @param config Capture device configuration.
 * @param sample_rate Nominal sample rate in Hz.
 * @param chunk_size Buffer chunk size in frames.
 * @param full_duplex True if running in full duplex mode.
 * @param params Processing parameters.
 * @param[out] out_err High-level audio backend error output.
 * @return Allocated capture_backend_t interface pointer, or NULL on error.
 */
capture_backend_t* audio_backend_factory_create_capture(
    const capture_device_config_t* config, int sample_rate, int chunk_size,
    bool full_duplex, processing_parameters_t* params,
    audio_backend_error_t* out_err);

/**
 * @brief Creates a playback backend from configuration and maps error types
 * cleanly.
 *
 * @param config Playback device configuration.
 * @param sample_rate Nominal sample rate in Hz.
 * @param chunk_size Buffer chunk size in frames.
 * @param full_duplex True if running in full duplex mode.
 * @param params Processing parameters.
 * @param[out] out_err High-level audio backend error output.
 * @return Allocated playback_backend_t interface pointer, or NULL on error.
 */
playback_backend_t* audio_backend_factory_create_playback(
    const playback_device_config_t* config, int sample_rate, int chunk_size,
    bool full_duplex, processing_parameters_t* params,
    audio_backend_error_t* out_err);

#endif  // CLIB_BACKEND_AUDIO_BACKEND_FACTORY_H
