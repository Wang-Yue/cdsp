#ifndef CDSP_ENGINE_SESSION_BUILDER_H
#define CDSP_ENGINE_SESSION_BUILDER_H

#include "Audio/audio_chunk.h"
#include "Config/configuration.h"
#include "Config/engine_config_types.h"
#include "Engine/dsp_session.h"
#include "Engine/engine_state_manager.h"

/**
 * @brief Constructs, pre-allocates scratch buffers and pipelines, opens
 * backends, and spawns worker threads for a DSP session.
 *
 * @param config Active DSP configuration.
 * @param on_captured Callback for raw captured audio tap.
 * @param captured_ctx Context pointer for on_captured.
 * @param on_processed Callback for post-DSP processed audio tap.
 * @param processed_ctx Context pointer for on_processed.
 * @param state_mgr Optional engine state manager to initialize volume and mute
 * state.
 * @param err Out parameter receiving detailed backend creation/open errors.
 * @return Fully initialized and running dsp_session_t instance, or NULL on
 * error.
 */
dsp_session_t* engine_session_build_and_start(
    dsp_config_t* config, chunk_callback_t on_captured, void* captured_ctx,
    chunk_callback_t on_processed, void* processed_ctx,
    const engine_state_manager_t* state_mgr, audio_backend_error_t* err);

#endif  // CDSP_ENGINE_SESSION_BUILDER_H
