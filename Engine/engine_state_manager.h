#ifndef CLIB_ENGINE_ENGINE_STATE_MANAGER_H
#define CLIB_ENGINE_ENGINE_STATE_MANAGER_H

#include <stdbool.h>
#include <stddef.h>

#include "Audio/processing_parameters.h"

/**
 * @file engine_state_manager.h
 * @brief Manages fader volumes, mute states, config path, and state file
 * serialization.
 */

typedef struct engine_state_manager engine_state_manager_t;

/**
 * @brief Create a new engine state manager.
 */
engine_state_manager_t* engine_state_manager_create(void);

/**
 * @brief Free an engine state manager instance.
 */
void engine_state_manager_free(engine_state_manager_t* mgr);

/**
 * @brief Set volume for a fader.
 */
void engine_state_manager_set_fader_volume(engine_state_manager_t* mgr,
                                           fader_t fader, float db);

/**
 * @brief Get volume for a fader.
 */
float engine_state_manager_get_fader_volume(const engine_state_manager_t* mgr,
                                            fader_t fader);

/**
 * @brief Set mute state for a fader.
 */
void engine_state_manager_set_fader_mute(engine_state_manager_t* mgr,
                                         fader_t fader, bool mute);

/**
 * @brief Check if a fader is muted.
 */
bool engine_state_manager_is_fader_muted(const engine_state_manager_t* mgr,
                                         fader_t fader);

/**
 * @brief Set path to state persistence file.
 */
void engine_state_manager_set_state_file(engine_state_manager_t* mgr,
                                         const char* path);

/**
 * @brief Get state persistence file path.
 */
const char* engine_state_manager_get_state_file(
    const engine_state_manager_t* mgr);

/**
 * @brief Set active config path.
 */
void engine_state_manager_set_config_path(engine_state_manager_t* mgr,
                                          const char* path);

/**
 * @brief Get an allocated copy of the active config path. Caller must free.
 */
char* engine_state_manager_get_config_path(const engine_state_manager_t* mgr);

/**
 * @brief Check if state changes are unsaved (dirty).
 */
bool engine_state_manager_is_dirty(const engine_state_manager_t* mgr);

/**
 * @brief Sync all fader volumes and mute states to processing_parameters.
 */
void engine_state_manager_sync_to_processing_parameters(
    const engine_state_manager_t* mgr, processing_parameters_t* params);

/**
 * @brief Persist state to disk if there are unsaved state changes.
 */
void engine_state_manager_save_if_needed(engine_state_manager_t* mgr);

#endif  // CLIB_ENGINE_ENGINE_STATE_MANAGER_H
