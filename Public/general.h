#ifndef CDSP_PUBLIC_GENERAL_H
#define CDSP_PUBLIC_GENERAL_H

#include <stddef.h>

#include "Public/cdsp_pub_types.h"

/**
 * @brief Get the CamillaDSP-C version string.
 * @return Static version string (do not free).
 */
const char* cdsp_get_version(void);

/**
 * @brief Get the supported playback and capture device types.
 *
 * All returned string pointers point to static memory. The arrays themselves
 * are allocated and must be freed by calling cdsp_free_device_types.
 *
 * @param out_playback_types Output array for playback device types.
 * @param out_playback_count Output count for playback device types.
 * @param out_capture_types Output array for capture device types.
 * @param out_capture_count Output count for capture device types.
 */
void cdsp_get_supported_device_types(char*** out_playback_types,
                                     size_t* out_playback_count,
                                     char*** out_capture_types,
                                     size_t* out_capture_count);

/**
 * @brief Free the arrays allocated by cdsp_get_supported_device_types.
 * @param types The array of strings.
 * @param count The number of elements.
 */
void cdsp_free_device_types(char** types, size_t count);

/**
 * @brief Create a new DSP engine instance.
 * @return A pointer to the created engine, or NULL on failure.
 */
dsp_engine_t* cdsp_engine_create(void);

/**
 * @brief Free the DSP engine instance.
 * @param engine Pointer to the engine.
 */
void cdsp_engine_free(dsp_engine_t* engine);

/**
 * @brief Poll the engine for background tasks, state synchronization, and
 * status updates.
 * @param engine Pointer to the engine.
 */
void cdsp_engine_poll(dsp_engine_t* engine);

/**
 * @brief Set the global logging level.
 * @param level_str Name of the log level (e.g. "trace", "debug", "info",
 * "warn", "error").
 */
void cdsp_set_log_level(const char* level_str);

/**
 * @brief Stop processing and put the engine in an inactive state.
 * @param engine Pointer to the engine.
 */
void cdsp_stop(dsp_engine_t* engine);

#endif  // CDSP_PUBLIC_GENERAL_H
