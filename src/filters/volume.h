#ifndef CLIB_FILTERS_VOLUME_H
#define CLIB_FILTERS_VOLUME_H

/**
 * @file volume.h
 * @brief Volume filter implementation with ramping/fading support.
 */

#include <stdbool.h>
#include <stddef.h>

#include "audio/processing_parameters.h"

typedef struct volume_filter volume_filter_t;

/// Level of a fader while muted, in dB (upstream MUTE_LEVEL_DB).
#define FADER_MUTE_LEVEL_DB (-100.0)

/**
 * @brief The gain one fader applies during the current chunk.
 *
 * Written once per chunk by the pipeline's fader bank (see
 * pipeline/pipeline_faders.h) before any pipeline step runs, and only read by
 * Volume and Loudness filters while the steps run.
 */
typedef struct {
  double start_db; ///< Level at the start of the chunk.
  double end_db;   ///< Level at the end of the chunk (upstream level_db()).
  double gain;     ///< Linear gain while not ramping.
  bool ramping;    ///< True: ramp start_db -> end_db over the chunk.
} fader_level_t;

/// Per-chunk levels of all faders (upstream FaderLevels).
typedef struct fader_levels {
  fader_level_t faders[FADER_COUNT];
} fader_levels_t;

/**
 * @brief Pre-calculates target volume levels and generates ramping array once
 * per chunk.
 */
void volume_filter_prepare_chunk(volume_filter_t *filter);

/**
 * @brief Like volume_filter_prepare_chunk(), but lays the ramp out over
 * @p frames samples (the length the following process() calls will use)
 * instead of the configured chunk size.
 */
void volume_filter_prepare_frames(volume_filter_t *filter, size_t frames);

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

/**
 * @brief Makes the filter a pure reader of centrally advanced fader levels.
 *
 * Once bound, the filter keeps no ramp state of its own and never writes the
 * shared processing parameters: every chunk it applies the gain published in
 * @p levels for its fader, like upstream's Volume reading FaderLevels. This
 * is how the pipeline uses every Volume filter, including the implicit master
 * volume. Unbound filters keep the self-driven behaviour described above.
 *
 * @param levels Must outlive the filter. NULL unbinds.
 */
void volume_filter_bind_fader_levels(volume_filter_t *filter,
                                     const fader_levels_t *levels);

struct filter_vtable;

extern const struct filter_vtable g_volume_vtable;

#endif // CLIB_FILTERS_VOLUME_H
