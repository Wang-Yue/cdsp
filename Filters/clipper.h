#ifndef CLIB_FILTERS_CLIPPER_H
#define CLIB_FILTERS_CLIPPER_H

/**
 * @file clipper.h
 * @brief Simple peak/soft clipper implementation.
 */

#include "Config/filter_config_types.h"

struct filter_vtable;
extern const struct filter_vtable g_clipper_vtable;

#endif  // CLIB_FILTERS_CLIPPER_H
