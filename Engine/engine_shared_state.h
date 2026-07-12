#ifndef CLIB_ENGINE_ENGINE_SHARED_STATE_H
#define CLIB_ENGINE_ENGINE_SHARED_STATE_H

/**
 * @file engine_shared_state.h
 * @brief Inter-thread state for the DSP engine's audio-priority loops.
 *
 * Coordinates state between the capture, processing, and playback loops.
 * Every field is either a lock-free atomic, a wait-free SPSC queue, or a kernel
 * signaling primitive (`DispatchSemaphore`/semaphore/Event). No mutexes are
 * used, allowing the loops to read/write fields without coordinating locks.
 *
 * @section concurrency_model Concurrency model
 * - `captured_queue`: SPSC sync queue (`audio_sync_queue_t`), producer =
 * capture, consumer = processing.
 * - `processed_queue`: SPSC sync queue (`audio_sync_queue_t`), producer =
 * processing, consumer = playback.
 * - `stop_requested`: Atomic release-acquire flag set when stopping or
 * encountering fatal errors.
 * - `resampler_ratio`: Playback writes (rate-adjust), processing reads (per
 * chunk). 64-bit atomic.
 *
 * @section semaphores Semaphores
 * Semaphores are encapsulated inside `audio_sync_queue_t` for kernel-level
 * signaling (not locking). Producers signal after enqueueing, and consumers
 * wait then drain.
 */

#include "Audio/audio_chunk.h"
#include "Audio/lock_free_ring_buffer.h"
#include "audio_sync_queue.h"

/**
 * @brief Yields the current thread's CPU execution slice.
 *
 * Yields execution to another thread that is ready to run on the current
 * processor. Maps to sched_yield() on POSIX (Linux/macOS) and SwitchToThread()
 * on Windows. Used to propagate queue backpressure without forcing a minimum
 * sleep duration.
 */
#if defined(__APPLE__) || defined(__linux__)
#include <sched.h>
static inline void engine_yield(void) { sched_yield(); }
#elif defined(_WIN32)
static inline void engine_yield(void) { SwitchToThread(); }
#endif
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "Config/engine_config_types.h"
#include "engine_state_machine.h"

/**
 * @brief Opaque structure representing shared state between the engine threads.
 */
typedef struct engine_shared_state engine_shared_state_t;

/**
 * @brief Gets the captured SPSC queue pointer.
 * @param state Pointer to the shared state instance.
 * @return Pointer to the captured SPSC queue.
 */
spsc_queue_t* engine_shared_state_get_captured_queue(
    engine_shared_state_t* state);

/**
 * @brief Gets the processed SPSC queue pointer.
 * @param state Pointer to the shared state instance.
 * @return Pointer to the processed SPSC queue.
 */
spsc_queue_t* engine_shared_state_get_processed_queue(
    engine_shared_state_t* state);

/**
 * @brief Checks if a stop has been requested on the shared state.
 * @param state Pointer to the shared state instance.
 * @return true if stop was requested, false otherwise.
 */
bool engine_shared_state_get_stop_requested(const engine_shared_state_t* state);

/**
 * @brief Gets the current resampler relative ratio.
 * @param state Pointer to the shared state instance.
 * @return The current relative sample rate ratio (e.g. 1.0).
 */
double engine_shared_state_get_resampler_ratio(
    const engine_shared_state_t* state);

/**
 * @brief Sets the resampler relative ratio.
 * @param state Pointer to the shared state instance.
 * @param ratio The new relative sample rate ratio to apply.
 */
void engine_shared_state_set_resampler_ratio(engine_shared_state_t* state,
                                             double ratio);

typedef struct pipeline_s pipeline_t;

/**
 * @brief Enqueues a swapped-out pipeline instance into the garbage queue for
 * deferred freeing.
 * @param state Pointer to the shared state instance.
 * @param pipeline Pointer to the pipeline object to enqueue.
 * @return true if enqueued successfully, false if the queue was full.
 */
bool engine_shared_state_enqueue_garbage_pipeline(engine_shared_state_t* state,
                                                  pipeline_t* pipeline);

/**
 * @brief Dequeues a pipeline instance from the garbage queue for cleanup.
 * @param state Pointer to the shared state instance.
 * @return Pointer to the dequeued pipeline object, or NULL if empty.
 */
pipeline_t* engine_shared_state_dequeue_garbage_pipeline(
    engine_shared_state_t* state);

/**
 * @brief Gets the current stop reason.
 * @param state Pointer to the shared state instance.
 * @return The processing stop reason structure.
 */
processing_stop_reason_t engine_shared_state_get_stop_reason(
    const engine_shared_state_t* state);

/**
 * @brief Creates a new engine shared state instance.
 *
 * @param captured_queue_depth Depth of the captured SPSC queue.
 * @param processed_queue_depth Depth of the processed SPSC queue.
 * @return Pointer to the created shared state instance, or NULL on failure.
 */
engine_shared_state_t* engine_shared_state_create(size_t captured_queue_depth,
                                                  size_t processed_queue_depth);

/**
 * @brief Frees the engine shared state instance.
 *
 * @param state Pointer to the shared state instance to free.
 */
void engine_shared_state_free(engine_shared_state_t* state);

/**
 * @brief Requests the engine loops to stop.
 *
 * @param state Pointer to the shared state.
 * @param reason The reason for stopping.
 */
void engine_shared_state_request_stop(engine_shared_state_t* state,
                                      processing_stop_reason_t reason);

/**
 * @brief Signals the captured sync queue.
 * @param state Pointer to the shared state.
 */
void engine_shared_state_signal_captured(engine_shared_state_t* state);

/**
 * @brief Signals the processed sync queue and marks processing completion.
 * @param state Pointer to the shared state.
 */
void engine_shared_state_set_processing_done(engine_shared_state_t* state);

/**
 * @brief Enqueues a chunk to the captured queue and signals the captured
 * semaphore.
 * @param state Pointer to the shared state instance.
 * @param chunk Pointer to the audio chunk to enqueue.
 * @return true if enqueued successfully, false if the queue was full.
 */
bool engine_shared_state_enqueue_captured(engine_shared_state_t* state,
                                          audio_chunk_t* chunk);

/**
 * @brief Enqueues a chunk to the processed queue and signals the processed
 * semaphore.
 * @param state Pointer to the shared state instance.
 * @param chunk Pointer to the audio chunk to enqueue.
 * @return true if enqueued successfully, false if the queue was full.
 */
bool engine_shared_state_enqueue_processed(engine_shared_state_t* state,
                                           audio_chunk_t* chunk);

/**
 * @brief Dequeues a chunk from the captured queue, blocking on captured
 * semaphore if empty.
 * @param state Pointer to the shared state instance.
 * @return audio_chunk_t* Dequeued chunk, or NULL if pipeline is stopping and
 * queue is drained.
 */
audio_chunk_t* engine_shared_state_dequeue_captured_blocking(
    engine_shared_state_t* state);

/**
 * @brief Dequeues a chunk from the processed queue, blocking on processed
 * semaphore if empty.
 * @param state Pointer to the shared state instance.
 * @return audio_chunk_t* Dequeued chunk, or NULL if pipeline is stopping and
 * queue is drained.
 */
audio_chunk_t* engine_shared_state_dequeue_processed_blocking(
    engine_shared_state_t* state);

/**
 * @brief Sets the state machine pointer in the shared state.
 * @param state Pointer to the shared state instance.
 * @param sm Pointer to the state machine instance.
 */
void engine_shared_state_set_state_machine(engine_shared_state_t* state,
                                           engine_state_machine_t* sm);

/**
 * @brief Reports that an engine loop thread has exited.
 *
 * Decrements the active thread count. The thread that decrements it to 0
 * transitions the state machine state to PROCESSING_STATE_INACTIVE.
 *
 * @param state Pointer to the shared state instance.
 */
void engine_shared_state_thread_exited(engine_shared_state_t* state);

#endif  // CLIB_ENGINE_ENGINE_SHARED_STATE_H
