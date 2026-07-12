/**
 * @file dsp_engine_core.h
 * @brief Top-level engine orchestrator.
 *
 * This class owns the *shape* of an engine run — config, sizing,
 * device handles, the three audio threads — but contains no audio
 * processing logic itself. Each thread body lives in its own file:
 *
 *   - `EngineCaptureLoop`     - capture -> DoP-decode -> level meter -> SPSC
 * queue.
 *   - `EngineProcessingLoop`  - SPSC dequeue -> resample -> pipeline -> SPSC
 * enqueue.
 *   - `EnginePlaybackLoop`    - SPSC dequeue -> rate-adjust controller ->
 * device write.
 *
 * All cross-thread state (the stop flag, the SPSC queues, the
 * resampler-ratio atomic) lives in `EngineSharedState`. State
 * machine + stop-reason publication lives in `EngineStateMachine`.
 *
 * Lock-free / allocation-free guarantees:
 * - The audio threads use lock-free SPSC queues and atomics;
 *   only `DispatchSemaphore` is used for signal/wait, which is
 *   a kernel signaling primitive (not a lock).
 * - Chunks are managed using a pre-allocated `RoundRobinChunkPool`
 *   on each thread to avoid allocations on the hot path.
 * - The resampler and pipeline output scratch buffers are pre-allocated.
 * - The stall watchdog uses `clock_gettime_nsec_np` (vDSO read,
 *   no syscall) — no `Date()` on the hot path.
 */

#ifndef CLIB_ENGINE_DSP_ENGINE_CORE_H
#define CLIB_ENGINE_DSP_ENGINE_CORE_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

#include "Audio/audio_chunk.h"
#include "Audio/processing_parameters.h"
#include "Backend/audio_backend.h"
#include "Config/configuration.h"
#include "engine_capture_loop.h"
#include "engine_playback_loop.h"
#include "engine_processing_loop.h"
#include "engine_shared_state.h"
#include "engine_state_machine.h"

/**
 * @brief Opaque structure representing the core DSP engine.
 *
 * Orchestrates the audio threads, backends, and processing pipeline.
 */
typedef struct dsp_engine_core dsp_engine_core_t;

/**
 * @brief Create a new DSP engine core.
 *
 * @param config Configuration to initialize the core with.
 * @return Pointer to the created core, or NULL on failure.
 */
dsp_engine_core_t* dsp_engine_core_create(dsp_config_t* config);

/**
 * @brief Get the current configuration of the engine core.
 *
 * @param core Pointer to the core instance.
 * @return Pointer to the current configuration, or NULL if core is NULL.
 */
const dsp_config_t* dsp_engine_core_get_config(const dsp_engine_core_t* core);

/**
 * @brief Get the processing parameters instance of the engine core.
 *
 * @param core Pointer to the core instance.
 * @return Pointer to the processing parameters, or NULL if core is NULL.
 */
processing_parameters_t* dsp_engine_core_get_processing_params(
    dsp_engine_core_t* core);

/**
 * @brief Set the chunk visualization callbacks on the engine core.
 *
 * @param core Pointer to the core instance.
 * @param on_captured Callback invoked after a chunk is captured.
 * @param captured_ctx Context pointer passed to on_captured.
 * @param on_processed Callback invoked after a chunk is processed.
 * @param processed_ctx Context pointer passed to on_processed.
 */
void dsp_engine_core_set_chunk_callbacks(dsp_engine_core_t* core,
                                         chunk_callback_t on_captured,
                                         void* captured_ctx,
                                         chunk_callback_t on_processed,
                                         void* processed_ctx);

/**
 * @brief Checks if a stop has been requested by any engine loop thread or
 * caller.
 *
 * @param core Pointer to the core instance.
 * @param out_reason Optional output pointer to store the stop reason if
 * requested.
 * @return true if a stop was requested, false otherwise.
 */
bool dsp_engine_core_is_stop_requested(const dsp_engine_core_t* core,
                                       processing_stop_reason_t* out_reason);

/**
 * @brief Free the DSP engine core.
 * @param core Pointer to the core instance.
 */
void dsp_engine_core_free(dsp_engine_core_t* core);

/**
 * @brief Get the current processing state.
 *
 * @param core Pointer to the core instance.
 * @return Current state.
 */
processing_state_t dsp_engine_core_get_state(const dsp_engine_core_t* core);

/**
 * @brief Get the reason why processing stopped.
 *
 * @param core Pointer to the core instance.
 * @return Pointer to the stop reason, or NULL if not stopped.
 */
const processing_stop_reason_t* dsp_engine_core_get_stop_reason(
    const dsp_engine_core_t* core);

/**
 * @brief Start the DSP engine processing threads.
 *
 * @param core Pointer to the core instance.
 * @param err Pointer to store backend errors if starting fails.
 * @return True on success, false on failure.
 */
bool dsp_engine_core_start(dsp_engine_core_t* core, audio_backend_error_t* err);

/**
 * @brief Stop the DSP engine processing threads.
 *
 * @param core Pointer to the core instance.
 * @param reason Reason for stopping.
 */
void dsp_engine_core_stop(dsp_engine_core_t* core,
                          processing_stop_reason_t reason);

/**
 * @brief Reload the configuration.
 *
 * Rebuilds or updates the processing pipeline against `newConfig` without
 * touching the audio devices. The caller is responsible for verifying that
 * device configurations match.
 *
 * @param core Pointer to the core instance.
 * @param new_config Pointer to the new configuration.
 * @param err Pointer to store backend errors if reload fails.
 * @return True on success, false on failure.
 */
bool dsp_engine_core_reload_config(dsp_engine_core_t* core,
                                   dsp_config_t* new_config,
                                   audio_backend_error_t* err);

/**
 * @brief Dequeues and frees garbage generated by the audio thread.
 *
 * Runs on the control plane.
 *
 * @param core Pointer to the core instance.
 */
void dsp_engine_core_collect_garbage(dsp_engine_core_t* core);

#endif  // CLIB_ENGINE_DSP_ENGINE_CORE_H
