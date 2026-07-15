#ifndef CLIB_FILTERS_GAIN_H
#define CLIB_FILTERS_GAIN_H

/**
 * @file gain.h
 * @brief Gain filter implementation.
 */

#include "Config/filter_config_types.h"

typedef struct gain_filter gain_filter_t;

/**
 * @brief Processes a single sample through the gain filter (used for internal
 * subcomponents).
 */
double gain_filter_process_single(gain_filter_t* filter, double sample);

struct filter_vtable;
extern const struct filter_vtable g_gain_vtable;

#endif  // CLIB_FILTERS_GAIN_H
