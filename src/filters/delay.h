#ifndef CLIB_FILTERS_DELAY_H
#define CLIB_FILTERS_DELAY_H

/**
 * @file delay.h
 * @brief Delay filter with optional subsample interpolation.
 *
 * Supports integer sample delay and subsample delay using Thiran allpass
 * biquads (1st or 2nd order).
 */

typedef struct delay_filter delay_filter_t;

/**
 * @brief Processes a single sample through the delay filter (used for internal
 * subcomponents).
 */
double delay_filter_process_single(delay_filter_t *filter, double sample);

struct filter_vtable;

extern const struct filter_vtable g_delay_vtable;

#endif // CLIB_FILTERS_DELAY_H
