#ifndef CLIB_FILTERS_LIMITER_H
#define CLIB_FILTERS_LIMITER_H

/**
 * @file limiter.h
 * @brief Simple peak/soft limiter implementation.
 */

#include "Config/filter_config_types.h"

struct filter_vtable;
extern const struct filter_vtable g_limiter_vtable;

#endif  // CLIB_FILTERS_LIMITER_H
