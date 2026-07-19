/**
 * @file dsp_session.h
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

#ifndef CLIB_ENGINE_DSP_SESSION_H
#define CLIB_ENGINE_DSP_SESSION_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

#include "Audio/audio_chunk.h"
#include "Audio/processing_parameters.h"
#include "Backend/audio_backend.h"
#include "Config/configuration.h"
#include "engine_shared_state.h"

/**
 * @brief Opaque structure representing the active DSP session.
 *
 * Orchestrates the audio threads, backends, and processing pipeline.
 */
typedef struct dsp_session dsp_session_t;

/**
 * @brief Creates and starts a new DSP session.
 *
 * Allocates all shared state, pre-allocates chunk pools, opens audio backends,
 * builds the processing pipeline, and spawns the 3 audio worker threads.
 *
 * @param config Configuration to initialize the session with.
 * @param on_captured Callback invoked after a chunk is captured.
 * @param captured_ctx Context pointer passed to on_captured.
 * @param on_processed Callback invoked after a chunk is processed.
 * @param processed_ctx Context pointer passed to on_processed.
 * @param err Optional pointer to receive error details on failure.
 * @return Pointer to the running session, or NULL on failure.
 */
dsp_session_t* dsp_session_create_and_start(dsp_config_t* config,
                                            chunk_callback_t on_captured,
                                            void* captured_ctx,
                                            chunk_callback_t on_processed,
                                            void* processed_ctx,
                                            audio_backend_error_t* err);

/**
 * @brief Get the current configuration of the DSP session.
 *
 * @param session Pointer to the session instance.
 * @return Pointer to the current configuration, or NULL if session is NULL.
 */
const dsp_config_t* dsp_session_get_config(const dsp_session_t* session);

/**
 * @brief Get the processing parameters instance of the DSP session.
 *
 * @param session Pointer to the session instance.
 * @return Pointer to the processing parameters, or NULL if session is NULL.
 */
processing_parameters_t* dsp_session_get_processing_params(
    dsp_session_t* session);

/**
 * @brief Checks if a stop has been requested by any engine loop thread or
 * caller.
 *
 * @param session Pointer to the session instance.
 * @param out_reason Optional output pointer to store the stop reason if
 * requested.
 * @return true if a stop was requested, false otherwise.
 */
bool dsp_session_is_stop_requested(const dsp_session_t* session,
                                   processing_stop_reason_t* out_reason);

/**
 * @brief Get the current processing state.
 *
 * @param session Pointer to the session instance.
 * @return Current state.
 */
processing_state_t dsp_session_get_state(const dsp_session_t* session);

/**
 * @brief Get the reason why processing stopped.
 *
 * @param session Pointer to the session instance.
 * @return Pointer to the stop reason, or NULL if not stopped.
 */
processing_stop_reason_t dsp_session_get_stop_reason(
    const dsp_session_t* session);

/**
 * @brief Stops audio threads, closes devices, and frees the DSP session.
 *
 * @param session Pointer to the session to stop and free.
 * @param reason Reason for stopping.
 * @return Final processing stop reason captured during teardown.
 */
processing_stop_reason_t dsp_session_stop_and_free(
    dsp_session_t* session, processing_stop_reason_t reason);

/**
 * @brief Reload the configuration.
 *
 * Rebuilds or updates the processing pipeline against `newConfig` without
 * touching the audio devices. The caller is responsible for verifying that
 * device configurations match.
 *
 * @param session Pointer to the session instance.
 * @param new_config Pointer to the new configuration.
 * @param err Pointer to store backend errors if reload fails.
 * @return True on success, false on failure.
 */
bool dsp_session_reload_config(dsp_session_t* session, dsp_config_t* new_config,
                               audio_backend_error_t* err);

/**
 * @brief Dequeues and frees garbage generated by the audio thread.
 *
 * Runs on the control plane.
 *
 * @param session Pointer to the session instance.
 */
void dsp_session_collect_garbage(dsp_session_t* session);

#endif  // CLIB_ENGINE_DSP_SESSION_H
