#ifndef CDSP_PUBLIC_GENERAL_H
#define CDSP_PUBLIC_GENERAL_H

#include <stddef.h>

#include "cdsp_export.h"
#include "cdsp_pub_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Get the CDSP version string.
 * @return Static version string (do not free).
 */
CDSP_API const char* cdsp_get_version(void);

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
CDSP_API void cdsp_get_supported_device_types(char*** out_playback_types,
                                              size_t* out_playback_count,
                                              char*** out_capture_types,
                                              size_t* out_capture_count);

/**
 * @brief Free the arrays allocated by cdsp_get_supported_device_types.
 * @param types The array of strings.
 * @param count The number of elements.
 */
CDSP_API void cdsp_free_device_types(char** types, size_t count);

/**
 * @brief Create a new DSP engine instance.
 * @return A pointer to the created engine, or NULL on failure.
 */
CDSP_API dsp_engine_t* cdsp_engine_create(void);

/**
 * @brief Free the DSP engine instance.
 * @param engine Pointer to the engine.
 */
CDSP_API void cdsp_engine_free(dsp_engine_t* engine);

/**
 * @brief Poll the engine for background tasks, state synchronization, and
 * status updates.
 * @param engine Pointer to the engine.
 */
CDSP_API void cdsp_engine_poll(dsp_engine_t* engine);

/**
 * @brief Set the global logging level.
 * @param level_str Name of the log level (e.g. "trace", "debug", "info",
 * "warn", "error").
 */
CDSP_API void cdsp_set_log_level(const char* level_str);

/**
 * @brief Log callback signature for the public API.
 * @param level Log severity level as a string (e.g. "INFO", "WARN", "ERROR").
 * @param label Component label (e.g. "dsp.engine").
 * @param message Formatted log message string.
 * @param user_data User context pointer.
 */
typedef void (*cdsp_log_callback_fn)(const char* level, const char* label,
                                     const char* message, void* user_data);

/**
 * @brief Register a global log callback to receive log messages.
 * @param callback Callback function, or NULL to restore default stdout logging.
 * @param user_data User context pointer passed to callback.
 */
CDSP_API void cdsp_set_log_callback(cdsp_log_callback_fn callback,
                                    void* user_data);

/**
 * @brief Stop processing and put the engine in an inactive state.
 * @param engine Pointer to the engine.
 */
CDSP_API void cdsp_stop(dsp_engine_t* engine);

#ifdef __cplusplus
}
#endif

#endif  // CDSP_PUBLIC_GENERAL_H
