#ifndef CLIB_ENGINE_ENGINE_PROCESSING_LOOP_H
#define CLIB_ENGINE_ENGINE_PROCESSING_LOOP_H

/**
 * @file engine_processing_loop.h
 * @brief Processing thread loop for the DSP engine.
 *
 * Drains the capture→processing SPSC queue, runs each chunk through the
 * pipeline, then enqueues the result on the processing→playback queue.
 *
 * @section state_ownership State ownership
 * The pre-allocated scratch chunk (`pipelineScratch`) is
 * owned by this loop and only mutated here.
 *
 * @section audio_invariants Audio-thread invariants
 * - No allocations in the steady state. Output chunks are obtained from a
 * pre-allocated `RoundRobinChunkPool`, and the pipeline scratch chunk is
 * pre-allocated at init.
 * - No locks. The shared SPSC queues + semaphores carry chunks and wakeups.
 * - The thread sets a real-time scheduling policy on entry so the OS prefers it
 * over background work.
 */

#include <stdbool.h>
#include <stddef.h>

#include "audio/audio_chunk.h"
#include "audio/processing_parameters.h"
#include "config/configuration.h"
#include "engine/engine_shared_state.h"
#include "pipeline/pipeline.h"

/**
 * @brief Opaque structure representing the processing loop.
 *
 * `@unchecked Sendable` is a *transfer* vouch, not a *share*
 * vouch: the instance is safe to cross the Thread spawn boundary
 * because exactly one thread (the loop thread) ever touches it
 * after `run()` is invoked. The scratch chunks have no internal
 * synchronisation and are *not* safe to use from multiple threads
 * concurrently.
 */
typedef struct engine_processing_loop engine_processing_loop_t;

/**
 * @brief Configuration parameters for creating an engine processing loop
 * instance.
 */
typedef struct {
  engine_shared_state_t *shared;
  processing_parameters_t *processing_params;
  size_t pipeline_rate;
  pipeline_t *pipeline;
  audio_chunk_t *pipeline_scratch;
  round_robin_chunk_pool_t *scratch_pool;
  chunk_callback_t on_chunk_captured;
  void *on_chunk_captured_ctx;
  chunk_callback_t on_chunk_processed;
  void *on_chunk_processed_ctx;
  bool is_realtime;
} engine_processing_loop_config_t;

/**
 * @brief Creates a new engine processing loop instance.
 *
 * @param config Pointer to the processing loop configuration structure.
 * @return Pointer to the created processing loop instance, or NULL on failure.
 */
engine_processing_loop_t *
engine_processing_loop_create(const engine_processing_loop_config_t *config);

/**
 * @brief Frees the engine processing loop instance.
 *
 * @param loop Pointer to the processing loop instance to free.
 */
void engine_processing_loop_free(engine_processing_loop_t *loop);

/**
 * @brief Runs the processing loop.
 *
 * This function blocks and runs the processing loop until it is requested to
 * stop or an error occurs.
 *
 * @param loop Pointer to the processing loop instance.
 */
void engine_processing_loop_run(engine_processing_loop_t *loop);

/**
 * @brief Sets a new pipeline for the processing loop.
 *
 * @param loop Pointer to the processing loop instance.
 * @param new_pipeline Pointer to the new pipeline.
 */
void engine_processing_loop_set_pipeline(engine_processing_loop_t *loop,
                                         pipeline_t *new_pipeline,
                                         bool transfer_filter_state);

#endif // CLIB_ENGINE_ENGINE_PROCESSING_LOOP_H
