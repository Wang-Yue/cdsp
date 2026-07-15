/**
 * @file limiter.h
 * @brief Limiter filter implementation.
 *
 * This file defines the functions to manage and apply a limiter filter.
 */

#ifndef CLIB_FILTERS_LIMITER_H
#define CLIB_FILTERS_LIMITER_H

#include <stdbool.h>
#include <stddef.h>

#include "Utils/double_helpers.h"
#include "Config/filter_config_types.h"

/**
 * @brief Opaque structure representing a limiter filter.
 */
typedef struct limiter_filter limiter_filter_t;

/**
 * @brief Create a limiter filter.
 *
 * @param name The name of the filter.
 * @param params Pointer to the limiter parameters.
 * @param err Optional pointer to receive configuration error detail on failure.
 * @return Pointer to the allocated limiter_filter_t, or NULL on failure.
 */
limiter_filter_t* limiter_filter_create(const char* name,
                                        const limiter_config_t* params,
                                        config_error_t* err);

/**
 * @brief Validates limiter filter parameters.
 *
 * @param params Pointer to the limiter parameters to validate.
 * @param err Pointer to a config error struct to populate on failure.
 * @return 0 on success, -1 on failure.
 */
int limiter_config_validate(const limiter_config_t* params,
                             config_error_t* err);

/**
 * @brief Process a waveform buffer in-place by applying limiting.
 *
 * @param filter Pointer to the limiter filter instance.
 * @param waveform The waveform data to process.
 * @param count The number of samples to process.
 */
void limiter_filter_process(limiter_filter_t* filter,
                            mutable_waveform_t waveform, size_t count);

/**
 * @brief Free the limiter filter instance.
 *
 * @param filter Pointer to the limiter filter instance to free.
 */
void limiter_filter_free(limiter_filter_t* filter);

#endif  // CLIB_FILTERS_LIMITER_H
