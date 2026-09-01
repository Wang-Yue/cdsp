#ifndef CLIB_FILTERS_BIQUAD_CANON_H
#define CLIB_FILTERS_BIQUAD_CANON_H

/**
 * @file biquad_canon.h
 * @brief Internal biquad cascade / canon filter.
 *
 * Implements systolic canon scheduling for a cascade of biquad sections
 * packaged as a generic filter_t without user-facing configuration definitions.
 */

struct filter_vtable;

extern const struct filter_vtable g_biquad_canon_vtable;

#endif  // CLIB_FILTERS_BIQUAD_CANON_H
