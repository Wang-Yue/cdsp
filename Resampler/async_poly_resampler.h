/**
 * @file async_poly_resampler.h
 * @brief Asynchronous polynomial resampler.
 *
 * Provides polynomial-based resampling (linear, cubic, quintic, septic).
 * This resampler does not perform anti-aliasing, making it suitable when
 * CPU resources are constrained and high-frequency aliasing is acceptable,
 * or for low resampling ratios.
 *
 * Memory is allocated only during initialization.
 */

#ifndef CLIB_RESAMPLER_ASYNC_POLY_RESAMPLER_H
#define CLIB_RESAMPLER_ASYNC_POLY_RESAMPLER_H

#include <stddef.h>

#include "Config/config_error.h"
#include "Config/resampler_config_types.h"
/**
 * @brief Types of polynomial interpolation supported.
 */
typedef enum {
  /** Linear interpolation (requires 2 points). */
  POLY_INTERPOLATION_LINEAR = 0,
  /** Cubic interpolation (requires 4 points). */
  POLY_INTERPOLATION_CUBIC,
  /** Quintic interpolation (requires 6 points). */
  POLY_INTERPOLATION_QUINTIC,
  /** Septic interpolation (requires 8 points). */
  POLY_INTERPOLATION_SEPTIC
} poly_interpolation_t;

#include "audio_resampler.h"

/**
 * @brief Creates an asynchronous polynomial resampler.
 *
 * @param channels Number of audio channels.
 * @param input_rate Input sample rate in Hz.
 * @param output_rate Output sample rate in Hz.
 * @param interpolation The interpolation quality to use.
 * @param chunk_size Fixed number of input frames per process call.
 * @param max_relative_ratio Maximum relative ratio adjustment. Used for buffer
 * pre-allocation.
 * @param err Pointer to a config error struct to populate on failure.
 * @return A new resampler instance, or NULL on failure.
 */
resampler_t* async_poly_resampler_create(
    size_t channels, size_t input_rate, size_t output_rate,
    poly_interpolation_t interpolation, size_t chunk_size,
    double max_relative_ratio, fixed_async_t fixed, config_error_t* err);

/**
 * @brief Creates an asynchronous polynomial resampler from a profile preset.
 */
resampler_t* async_poly_resampler_create_from_profile(
    size_t channels, size_t input_rate, size_t output_rate,
    resampler_profile_t profile, size_t chunk_size, double max_relative_ratio,
    fixed_async_t fixed, config_error_t* err);

#endif  // CLIB_RESAMPLER_ASYNC_POLY_RESAMPLER_H
