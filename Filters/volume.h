#ifndef CLIB_FILTERS_VOLUME_H
#define CLIB_FILTERS_VOLUME_H

/**
 * @file volume.h
 * @brief Volume filter implementation with ramping/fading support.
 */

#include "Audio/processing_parameters.h"
#include "Config/filter_config_types.h"

typedef struct volume_filter volume_filter_t;

/**
 * @brief Pre-calculates target volume levels and generates ramping array once
 * per chunk.
 */
void volume_filter_prepare_chunk(volume_filter_t* filter);

/**
 * @brief Advances the fader's ramp steps.
 */
void volume_filter_advance_ramp(volume_filter_t* filter);

struct filter_vtable;
extern const struct filter_vtable g_volume_vtable;

#endif  // CLIB_FILTERS_VOLUME_H
