/**
 * @file async_sinc_resampler.h
 * @brief Asynchronous windowed-sinc resampler.
 *
 * Provides high-quality asynchronous resampling using a windowed-sinc filter.
 * It includes anti-aliasing filtering.
 *
 * Memory is allocated only during initialization.
 */

#ifndef CLIB_RESAMPLER_ASYNC_SINC_RESAMPLER_H
#define CLIB_RESAMPLER_ASYNC_SINC_RESAMPLER_H

#include <stddef.h>

#include "Config/config_error.h"
#include "Config/resampler_config_types.h"
#include "sinc_window_function.h"

/**
 * @brief Sinc table interpolation methods.
 *
 * Determines how values between the discrete points in the oversampled
 * sinc table are calculated.
 */
typedef enum {
  /** No interpolation, use the nearest sample in the table. */
  SINC_INTERPOLATION_NEAREST = 0,
  /** Linear interpolation between the two nearest samples. */
  SINC_INTERPOLATION_LINEAR,
  /** Quadratic interpolation. */
  SINC_INTERPOLATION_QUADRATIC,
  /** Cubic interpolation. */
  SINC_INTERPOLATION_CUBIC
} sinc_interpolation_type_t;

#include "audio_resampler.h"

/**
 * @brief Creates an asynchronous windowed-sinc resampler with explicit
 * parameters.
 *
 * @param channels Number of audio channels.
 * @param input_rate Input sample rate in Hz.
 * @param output_rate Output sample rate in Hz.
 * @param sinc_len Length of the sinc filter (number of taps).
 * @param oversampling_factor Table oversampling factor.
 * @param interpolation Sinc table interpolation type.
 * @param window Window function to apply to the sinc filter.
 * @param f_cutoff Cutoff frequency.
 * @param has_f_cutoff Whether a custom cutoff frequency is used.
 * @param chunk_size Fixed number of input frames per process call.
 * @param max_relative_ratio Maximum relative ratio adjustment. Used for buffer
 * pre-allocation.
 * @param err Pointer to a config error struct to populate on failure.
 * @return A new resampler instance, or NULL on failure.
 */
audio_resampler_t* async_sinc_resampler_create(
    size_t channels, size_t input_rate, size_t output_rate, size_t sinc_len,
    size_t oversampling_factor, sinc_interpolation_type_t interpolation,
    window_function_t window, double f_cutoff, bool has_f_cutoff,
    size_t chunk_size, double max_relative_ratio, fixed_async_t fixed,
    config_error_t* err);

/**
 * @brief Creates an asynchronous windowed-sinc resampler using a quality
 * profile.
 *
 * @param channels Number of audio channels.
 * @param input_rate Input sample rate in Hz.
 * @param output_rate Output sample rate in Hz.
 * @param profile Predefined quality profile (e.g. low, medium, high).
 * @param chunk_size Fixed number of input frames per process call.
 * @param max_relative_ratio Maximum relative ratio adjustment. Used for buffer
 * pre-allocation.
 * @param err Pointer to a config error struct to populate on failure.
 * @return A new resampler instance, or NULL on failure.
 */
audio_resampler_t* async_sinc_resampler_create_from_profile(
    size_t channels, size_t input_rate, size_t output_rate,
    resampler_profile_t profile, size_t chunk_size, double max_relative_ratio,
    config_error_t* err);

#endif  // CLIB_RESAMPLER_ASYNC_SINC_RESAMPLER_H
