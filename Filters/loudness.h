#ifndef CLIB_FILTERS_LOUDNESS_H
#define CLIB_FILTERS_LOUDNESS_H

/**
 * @file loudness.h
 * @brief Equal-loudness contour compensation filter (RME ADI-2 style).
 */

#include "Config/filter_config_types.h"

struct filter_vtable;
extern const struct filter_vtable g_loudness_vtable;

#endif  // CLIB_FILTERS_LOUDNESS_H
