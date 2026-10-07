#ifndef CDSP_ENGINE_SESSION_BUILDER_H
#define CDSP_ENGINE_SESSION_BUILDER_H

#include "audio/audio_chunk.h"
#include "audio/sample_format.h"
#include "config/configuration.h"
#include "engine/dsp_session.h"
#include "engine/engine_state_manager.h"
#include "engine/engine_state_types.h"

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
 * @param seed_telemetry Optional telemetry of the previous session, copied
 * into the new processing parameters before the worker threads start.
 * @param seed_clipped Clipped-sample count carried over from earlier sessions.
 * @param err Out parameter receiving detailed backend creation/open errors.
 * @return Fully initialized and running dsp_session_t instance, or NULL on
 * error.
 */
dsp_session_t *engine_session_build_and_start(
    dsp_config_t *config, chunk_callback_t on_captured, void *captured_ctx,
    chunk_callback_t on_processed, void *processed_ctx,
    const engine_state_manager_t *state_mgr,
    const processing_parameters_t *seed_telemetry, uint64_t seed_clipped,
    audio_backend_error_t *err);

#endif // CDSP_ENGINE_SESSION_BUILDER_H
