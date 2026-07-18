#ifndef CLIB_FILTERS_BIQUAD_H
#define CLIB_FILTERS_BIQUAD_H

/**
 * @file biquad.h
 * @brief Biquad filter creation, filtering operations, and lifecycle
 * management.
 */

#include "Config/filter_config_types.h"

typedef struct biquad_filter biquad_filter_t;

/**
 * @brief Processes a single sample through the biquad filter.
 */
double biquad_filter_process_single(biquad_filter_t* filter, double sample);

/**
 * @brief Updates the filter parameters from a new configuration.
 */
void biquad_filter_update_parameters(biquad_filter_t* filter,
                                     const filter_config_t* config,
                                     int sample_rate);

/**
 * @brief Gets the name of the biquad filter section.
 */
const char* biquad_filter_get_name(const biquad_filter_t* filter);

struct filter_vtable;
extern const struct filter_vtable g_biquad_vtable;

#endif  // CLIB_FILTERS_BIQUAD_H
