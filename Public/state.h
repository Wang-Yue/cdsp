/**
 * @file state.h
 * @brief Public DSP state storage and loading.
 */

#ifndef CDSP_PUBLIC_STATE_H
#define CDSP_PUBLIC_STATE_H

#include <stdbool.h>

#include "cdsp_pub_types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct cdsp_state_s;
/**
 * @brief Opaque structure representing the public DSP state.
 */
typedef struct cdsp_state_s cdsp_state_t;

/**
 * @brief Create a new DSP state instance.
 * @return Pointer to the created cdsp_state_t, or NULL on failure.
 */
CDSP_API cdsp_state_t* cdsp_state_create(void);

/**
 * @brief Free the DSP state instance.
 * @param state The DSP state instance to free.
 */
CDSP_API void cdsp_state_free(cdsp_state_t* state);

/**
 * @brief Load DSP state from a file.
 * @param filename The path to the state file.
 * @param out_state The DSP state instance to load into.
 * @return true on success, false on failure.
 */
CDSP_API bool cdsp_state_load(const char* filename, cdsp_state_t* out_state);

/**
 * @brief Save DSP state to a file.
 * @param filename The path to the state file.
 * @param state The DSP state instance to save.
 * @return true on success, false on failure.
 */
CDSP_API bool cdsp_state_save(const char* filename, const cdsp_state_t* state);

/**
 * @brief Get the configuration path from the DSP state.
 * @param state The DSP state instance.
 * @return The configuration path string, or NULL if not set.
 */
CDSP_API const char* cdsp_state_get_config_path(const cdsp_state_t* state);

/**
 * @brief Set the configuration path in the DSP state.
 * @param state The DSP state instance.
 * @param path The configuration path string to set.
 */
CDSP_API void cdsp_state_set_config_path(cdsp_state_t* state, const char* path);

/**
 * @brief Check if the DSP state has a configuration path.
 * @param state The DSP state instance.
 * @return true if it has a configuration path, false otherwise.
 */
CDSP_API bool cdsp_state_has_config_path(const cdsp_state_t* state);

/**
 * @brief Set whether the DSP state has a configuration path.
 * @param state The DSP state instance.
 * @param has_path True if it has a configuration path, false otherwise.
 */
CDSP_API void cdsp_state_set_has_config_path(cdsp_state_t* state, bool has_path);

/**
 * @brief Get the mute status for a channel index from the DSP state.
 * @param state The DSP state instance.
 * @param index The channel index.
 * @return True if muted, false otherwise.
 */
CDSP_API bool cdsp_state_get_mute(const cdsp_state_t* state, int index);

/**
 * @brief Set the mute status for a channel index in the DSP state.
 * @param state The DSP state instance.
 * @param index The channel index.
 * @param mute True to mute, false to unmute.
 */
CDSP_API void cdsp_state_set_mute(cdsp_state_t* state, int index, bool mute);

/**
 * @brief Get the volume for a channel index from the DSP state.
 * @param state The DSP state instance.
 * @param index The channel index.
 * @return The volume value.
 */
CDSP_API double cdsp_state_get_volume(const cdsp_state_t* state, int index);

/**
 * @brief Set the volume for a channel index in the DSP state.
 * @param state The DSP state instance.
 * @param index The channel index.
 * @param volume The volume value to set.
 */
CDSP_API void cdsp_state_set_volume(cdsp_state_t* state, int index, double volume);

#ifdef __cplusplus
}
#endif

#endif  // CDSP_PUBLIC_STATE_H
