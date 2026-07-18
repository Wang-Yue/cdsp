#ifndef CLIB_ENGINE_ENGINE_CAPTURE_LOOP_H
#define CLIB_ENGINE_ENGINE_CAPTURE_LOOP_H

/**
 * @file engine_capture_loop.h
 * @brief Capture thread loop for the DSP engine.
 *
 * One instance per engine run; the thread closure invokes `run()` exactly once
 * and returns when the shared `shouldStop` flag is set or a stop reason is
 * reported.
 *
 * @section state_ownership State ownership
 * All mutable state — the working chunk, the silence counter, the
 * stall watchdog — lives inside the loop instance and is touched
 * only by the capture thread. Cross-thread communication happens
 * exclusively through the injected `EngineSharedState`.
 *
 * @section audio_invariants Audio-thread invariants
 * - No allocations in the steady-state. Audio chunks are obtained
 *   from a pre-allocated `RoundRobinChunkPool`.
 * - No locks. Coordination uses the shared SPSC queue + semaphore.
 * - No `Date()` / `gettimeofday`. The watchdog uses
 *   `clock_gettime_nsec_np(CLOCK_UPTIME_RAW)` (vDSO read on
 *   Darwin — no syscall).
 */

#include <stdbool.h>
#include <stddef.h>

#include "Audio/audio_chunk.h"
#include "Audio/processing_parameters.h"
#include "Backend/audio_backend.h"
#include "DoP/dop_decoder.h"
#include "engine_shared_state.h"

/**
 * @brief Opaque structure representing the capture loop.
 *
 * `@unchecked Sendable` is a *transfer* vouch, not a *share*
 * vouch: the instance is safe to cross the Thread spawn boundary
 * because exactly one thread (the loop thread) ever touches it
 * after `run()` is invoked. The mutable state — the working
 * `AudioChunk`, the silence counter, the stall watchdog — has no
 * internal synchronisation and is *not* safe to use from multiple
 * threads concurrently.
 */
typedef struct engine_capture_loop engine_capture_loop_t;

/**
 * @brief Configuration parameters for creating an engine capture loop instance.
 */
typedef struct {
  engine_shared_state_t* shared;
  capture_backend_t* capture;
  playback_backend_t* playback;
  processing_parameters_t* processing_params;
  dop_decoder_t* dop_decoder;
  round_robin_chunk_pool_t* chunk_pool;
  size_t chunk_size;
  size_t channels;
  size_t samplerate;
  double silence_threshold_db;
  double silence_timeout_seconds;
  bool stop_on_rate_change;
  double rate_measure_interval_s;
} engine_capture_loop_config_t;

/**
 * @brief Creates a new engine capture loop instance.
 *
 * @param config Pointer to the capture loop configuration structure.
 * @return Pointer to the created capture loop instance, or NULL on failure.
 */
engine_capture_loop_t* engine_capture_loop_create(
    const engine_capture_loop_config_t* config);

/**
 * @brief Frees the engine capture loop instance.
 *
 * @param loop Pointer to the capture loop instance to free.
 */
void engine_capture_loop_free(engine_capture_loop_t* loop);

/**
 * @brief Runs the capture loop.
 *
 * This function blocks and runs the capture loop until it is requested to stop
 * or an error occurs.
 *
 * @param loop Pointer to the capture loop instance.
 */
void engine_capture_loop_run(engine_capture_loop_t* loop);

#ifdef CDSP_TEST
/**
 * @brief Executes a single iteration step of the capture loop.
 *
 * @param loop Pointer to the capture loop instance.
 * @return true if the loop should exit, false otherwise.
 */
bool engine_capture_loop_step(engine_capture_loop_t* loop);
#endif

#endif  // CLIB_ENGINE_ENGINE_CAPTURE_LOOP_H
