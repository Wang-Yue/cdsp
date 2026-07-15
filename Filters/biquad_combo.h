#ifndef CLIB_FILTERS_BIQUAD_COMBO_H
#define CLIB_FILTERS_BIQUAD_COMBO_H

/**
 * @file biquad_combo.h
 * @brief Combined biquad filters (e.g., Linkwitz-Riley, Butterworth, Tilt EQ,
 * Graphic EQ).
 */

struct filter_vtable;
extern const struct filter_vtable g_biquad_combo_vtable;

#endif  // CLIB_FILTERS_BIQUAD_COMBO_H
