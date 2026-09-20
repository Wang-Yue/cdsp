#ifndef CLIB_FILTERS_VOLUME_H
#define CLIB_FILTERS_VOLUME_H

/**
 * @file volume.h
 * @brief Volume filter implementation with ramping/fading support.
 */

#include <stdbool.h>

#include "audio/processing_parameters.h"

typedef struct volume_filter volume_filter_t;

/**
 * @brief Pre-calculates target volume levels and generates ramping array once
 * per chunk.
 */
void volume_filter_prepare_chunk(volume_filter_t *filter);

/**
 * @brief Advances the fader's ramp steps.
 */
void volume_filter_advance_ramp(volume_filter_t *filter);

/**
 * @brief Selects who drives the per-chunk ramp.
 *
 * Pass true for an instance that is shared across all channels of a chunk (the
 * implicit master volume): the owner must then call
 * volume_filter_prepare_chunk() before, and volume_filter_advance_ramp() after,
 * the per-channel process() calls.
 *
 * Pass false (the default) for a per-channel instance, such as a Volume filter
 * built from a pipeline step: process() then drives the ramp itself.
 */
void volume_filter_set_externally_driven(volume_filter_t *filter,
                                         bool externally_driven);

struct filter_vtable;

extern const struct filter_vtable g_volume_vtable;

#endif // CLIB_FILTERS_VOLUME_H
