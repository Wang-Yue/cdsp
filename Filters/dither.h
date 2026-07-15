#ifndef CLIB_FILTERS_DITHER_H
#define CLIB_FILTERS_DITHER_H

#include <stdbool.h>
#include <stddef.h>

#include "Utils/double_helpers.h"
#include "Config/filter_config_types.h"

/**
 * @file dither.h
 * @brief Dithering and noise shaping filters.
 *
 * Provides structures and functions for applying dither and noise shaping
 * to audio signals.
 */

// MARK: - DitherFilter

/**
 * @brief Opaque handle to a dither filter instance.
 */
typedef struct dither_filter dither_filter_t;

/**
 * @brief Create a new dither filter.
 *
 * @param name The name of the filter.
 * @param params The dither parameters (type, bits, etc.).
 * @param err Optional pointer to receive configuration error detail on failure.
 * @return A pointer to the created dither filter, or NULL on failure.
 */
dither_filter_t* dither_filter_create(const char* name,
                                      const dither_config_t* params,
                                      config_error_t* err);

/**
 * @brief Validates dither filter parameters.
 *
 * @param params Pointer to the dither parameters to validate.
 * @param err Pointer to a config error struct to populate on failure.
 * @return 0 on success, -1 on failure.
 */
int dither_config_validate(const dither_config_t* params,
                            config_error_t* err);

/**
 * @brief Process a block of samples in-place, applying dither.
 *
 * @param filter The dither filter instance.
 * @param waveform The input/output waveform buffer.
 * @param count The number of samples to process.
 */
void dither_filter_process(dither_filter_t* filter, mutable_waveform_t waveform,
                           size_t count);

/**
 * @brief Free the dither filter instance and its associated resources.
 *
 * @param filter The dither filter instance to free.
 */
void dither_filter_free(dither_filter_t* filter);

#endif  // CLIB_FILTERS_DITHER_H
