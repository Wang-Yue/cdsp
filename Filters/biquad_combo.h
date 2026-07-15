#ifndef CLIB_FILTERS_BIQUAD_COMBO_H
#define CLIB_FILTERS_BIQUAD_COMBO_H

/**
 * @file biquad_combo.h
 * @brief Combined biquad filters (e.g., Linkwitz-Riley, Butterworth, Tilt EQ,
 * Graphic EQ).
 */

#include <stddef.h>

/**
 * @brief Computes Q values for a Butterworth filter of a given order.
 *
 * @param order The filter order (must be positive).
 * @param out_q Array to store the computed Q values.
 * @param max_q Maximum capacity of the `out_q` array.
 * @return The number of Q values computed (number of biquad stages).
 */
size_t biquad_combo_butterworth_q(int order, double* out_q, size_t max_q);

/**
 * @brief Computes Q values for a Linkwitz-Riley filter of a given order.
 *
 * @param order The filter order (must be positive, typically even).
 * @param out_q Array to store the computed Q values.
 * @param max_q Maximum capacity of the `out_q` array.
 * @return The number of Q values computed (number of biquad stages).
 */
size_t biquad_combo_linkwitz_riley_q(int order, double* out_q, size_t max_q);

struct filter_vtable;
extern const struct filter_vtable g_biquad_combo_vtable;

#endif  // CLIB_FILTERS_BIQUAD_COMBO_H
