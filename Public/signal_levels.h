#ifndef CDSP_PUBLIC_SIGNAL_LEVELS_H
#define CDSP_PUBLIC_SIGNAL_LEVELS_H

#include <stdbool.h>
#include <stddef.h>
#include "Public/cdsp_pub_types.h"

/**
 * @brief Get the current VU levels for capture and playback sides.
 *
 * The arrays within out_vu are allocated dynamically. The caller must free them
 * by calling cdsp_free_vu_levels.
 *
 * @param engine Pointer to the engine.
 * @param out_vu Pointer to write the VU levels to.
 * @return true on success, false on failure.
 */
bool cdsp_get_vu_levels(const dsp_engine_t* engine, cdsp_vu_levels_t* out_vu);

/**
 * @brief Free the arrays inside the cdsp_vu_levels_t structure.
 * @param vu Pointer to the VU levels structure.
 */
void cdsp_free_vu_levels(cdsp_vu_levels_t* vu);

/**
 * @brief Get the optional channel labels configured for playback and capture.
 *
 * The arrays out_playback_labels and out_capture_labels, along with each string,
 * are allocated dynamically and must be freed by calling cdsp_free_channel_labels.
 * If a channel has no label, its string pointer in the array will be NULL.
 *
 * @param engine Pointer to the engine.
 * @param out_playback_labels Output array of playback channel labels.
 * @param out_playback_count Output count of playback channels.
 * @param out_capture_labels Output array of capture channel labels.
 * @param out_capture_count Output count of capture channels.
 * @return true on success, false on failure (or if no labels are configured).
 */
bool cdsp_get_channel_labels(const dsp_engine_t* engine,
                             char*** out_playback_labels, size_t* out_playback_count,
                             char*** out_capture_labels, size_t* out_capture_count);

/**
 * @brief Free channel labels arrays and strings.
 * @param labels The array of strings.
 * @param count The number of channels/elements.
 */
void cdsp_free_channel_labels(char** labels, size_t count);

#endif // CDSP_PUBLIC_SIGNAL_LEVELS_H
