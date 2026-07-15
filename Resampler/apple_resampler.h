/**
 * @file apple_resampler.h
 * @brief Resampler implementation using Apple's AudioToolbox AudioConverter.
 *
 * This module provides a wrapper around Apple's AudioConverter for performing
 * sample rate conversion on macOS/iOS.
 */

#ifndef CLIB_RESAMPLER_APPLE_RESAMPLER_H
#define CLIB_RESAMPLER_APPLE_RESAMPLER_H

#if defined(ENABLE_COREAUDIO)

#include <stddef.h>

#include "Config/config_error.h"
#include "Config/resampler_config_types.h"

#include "audio_resampler.h"

/**
 * @brief Creates a new Apple AudioConverter resampler.
 *
 * @param channels Number of audio channels.
 * @param input_rate Input sample rate in Hz.
 * @param output_rate Output sample rate in Hz.
 * @param quality Resampling quality.
 * @param complexity Resampling complexity.
 * @param chunk_size Maximum number of frames per processing chunk.
 * @param err Pointer to a config error struct to populate on failure.
 * @return Pointer to newly allocated audio_resampler_t, or NULL on failure.
 */
resampler_t* apple_resampler_create(
    size_t channels, size_t input_rate, size_t output_rate,
    apple_resampler_quality_t quality, apple_resampler_complexity_t complexity,
    size_t chunk_size, config_error_t* err);

#endif  // ENABLE_COREAUDIO

#endif  // CLIB_RESAMPLER_APPLE_RESAMPLER_H
