#ifndef CLIB_FILTERS_LOUDNESS_H
#define CLIB_FILTERS_LOUDNESS_H

/**
 * @file loudness.h
 * @brief Equal-loudness contour compensation filter (RME ADI-2 style).
 */

#include "filters/volume.h"

typedef struct loudness_filter loudness_filter_t;

/**
 * @brief Makes the filter follow centrally advanced fader levels.
 *
 * Once bound, the filter tracks the end-of-chunk level its fader publishes
 * for the current chunk (upstream FaderLevels::level_db), including the ramp
 * into and out of mute, instead of polling the shared processing parameters.
 * The shelves are recomputed from the bound level immediately, with the
 * construction-time activity threshold (upstream Loudness::from_config).
 *
 * @param levels Must outlive the filter. NULL unbinds.
 */
void loudness_filter_bind_fader_levels(loudness_filter_t *filter,
                                       const fader_levels_t *levels);

struct filter_vtable;

extern const struct filter_vtable g_loudness_vtable;

#endif // CLIB_FILTERS_LOUDNESS_H
