#ifndef CDSP_ENGINE_SESSION_BUILDER_H
#define CDSP_ENGINE_SESSION_BUILDER_H

#include "Engine/dsp_engine_core.h"

/**
 * @brief Constructs, pre-allocates scratch buffers and pipelines, opens
 * backends, and spawns worker threads for a DSP engine core session.
 *
 * @param config Active DSP configuration.
 * @param on_captured Callback for raw captured audio tap.
 * @param captured_ctx Context pointer for on_captured.
 * @param on_processed Callback for post-DSP processed audio tap.
 * @param processed_ctx Context pointer for on_processed.
 * @param err Out parameter receiving detailed backend creation/open errors.
 * @return Fully initialized and running dsp_engine_core_t instance, or NULL on
 * error.
 */
dsp_engine_core_t* engine_session_build_and_start(dsp_config_t* config,
                                                  chunk_callback_t on_captured,
                                                  void* captured_ctx,
                                                  chunk_callback_t on_processed,
                                                  void* processed_ctx,
                                                  audio_backend_error_t* err);

#endif  // CDSP_ENGINE_SESSION_BUILDER_H
