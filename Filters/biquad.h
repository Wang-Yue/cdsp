#ifndef CLIB_FILTERS_BIQUAD_H
#define CLIB_FILTERS_BIQUAD_H

/**
 * @file biquad.h
 * @brief Biquad filter creation, filtering operations, and lifecycle management.
 */

#include <stdbool.h>
#include <stddef.h>

#include "Audio/double_helpers.h"
#include "Config/config_error.h"
#include "Config/filter_config_types.h"

/**
 * @brief Opaque structure representing a biquad filter instance (holds state).
 */
typedef struct biquad_filter biquad_filter_t;

/**
 * @brief Creates a biquad filter instance directly from high-level parameters.
 *
 * Computes coefficients and validates filter stability. If params is NULL, creates a passthrough filter.
 * If the filter is unstable or parameters are invalid, sets err and returns NULL.
 *
 * @param name The name of the filter (for debugging/identification).
 * @param params High-level biquad parameters (NULL for passthrough identity filter).
 * @param sample_rate Sample rate in Hz.
 * @param err Pointer to a config error struct to populate on failure.
 * @return A pointer to the created filter instance, or `NULL` on failure.
 */
biquad_filter_t* biquad_filter_create(const char* name,
                                      const biquad_parameters_t* params,
                                      int sample_rate,
                                      config_error_t* err);

/**
 * @brief Processes an array of samples through the biquad filter.
 *
 * In-place processing.
 *
 * @param filter The filter instance.
 * @param waveform The input/output waveform buffer.
 * @param count The number of samples to process.
 */
void biquad_filter_process(biquad_filter_t* filter, mutable_waveform_t waveform,
                           size_t count);

/**
 * @brief Processes a single sample through the biquad filter.
 *
 * @param filter The filter instance.
 * @param sample The input sample.
 * @return The processed output sample.
 */
double biquad_filter_process_single(biquad_filter_t* filter, double sample);

/**
 * @brief Updates the filter parameters from a new configuration.
 *
 * @param filter The filter instance.
 * @param config The new filter configuration.
 * @param sample_rate The sample rate in Hz.
 */
void biquad_filter_update_parameters(biquad_filter_t* filter,
                                     const filter_config_t* config,
                                     int sample_rate);

/**
 * @brief Transfers internal history state (delay line registers) from src to dest.
 *
 * @param dest The destination biquad filter instance.
 * @param src The source biquad filter instance.
 */
void biquad_filter_transfer_state(biquad_filter_t* dest,
                                  const biquad_filter_t* src);

/**
 * @brief Frees the biquad filter instance.
 *
 * @param filter The filter instance to free.
 */
void biquad_filter_free(biquad_filter_t* filter);

#endif  // CLIB_FILTERS_BIQUAD_H
