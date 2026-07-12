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
 * - In-Band AudioMessage Tagging (`AUDIO_MSG_DATA`, `AUDIO_MSG_EOF`,
 * `AUDIO_MSG_STOP`): Passed sequentially through SPSC queues for zero-flag
 * atomic coordination.
 * - `captured_queue`: SPSC, producer = capture, consumer = processing.
 * - `processed_queue`: SPSC, producer = processing, consumer = playback.
 * - `captured_semaphore`: Capture signals, processing waits.
 * - `processed_semaphore`: Processing signals, playback waits.
 * - `resampler_ratio`: Playback writes (rate-adjust), processing reads (per
 * chunk). 64-bit atomic.
 *
 * @section semaphores Semaphores
 * Semaphores are used for kernel-level signaling (not locking). Producers
 * signal after enqueueing, and consumers wait then drain.
 */

#include "Audio/audio_chunk.h"
#include "Audio/lock_free_ring_buffer.h"
#ifdef __APPLE__
#include <dispatch/dispatch.h>
/**
 * @brief Platform-specific semaphore wrapper handle.
 */
typedef dispatch_semaphore_t engine_semaphore_t;
/**
 * @brief Creates a platform-specific semaphore wrapper.
 * @return The initialized semaphore handle, or NULL on failure.
 */
static inline engine_semaphore_t engine_sem_create(void) {
  return dispatch_semaphore_create(0);
}
/**
 * @brief Destroys the semaphore.
 * @param sem The semaphore handle to destroy.
 */
static inline void engine_sem_destroy(engine_semaphore_t sem) {
  if (sem) dispatch_release(sem);
}
/**
 * @brief Signals the semaphore.
 * @param sem The semaphore handle.
 */
static inline void engine_sem_signal(engine_semaphore_t sem) {
  if (sem) dispatch_semaphore_signal(sem);
}
/**
 * @brief Waits on the semaphore.
 * @param sem The semaphore handle.
 */
static inline void engine_sem_wait(engine_semaphore_t sem) {
  if (sem) dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
}
#elif defined(__linux__)
#include <semaphore.h>
#include <stdlib.h>
/**
 * @brief Platform-specific semaphore wrapper handle.
 */
typedef sem_t* engine_semaphore_t;
/**
 * @brief Creates a platform-specific semaphore wrapper.
 * @return The initialized semaphore handle, or NULL on failure.
 */
static inline engine_semaphore_t engine_sem_create(void) {
  sem_t* sem = (sem_t*)calloc(1, sizeof(sem_t));
  if (!sem) return NULL;
  if (sem_init(sem, 0, 0) != 0) {
    free(sem);
    return NULL;
  }
  return sem;
}
/**
 * @brief Destroys the semaphore.
 * @param sem The semaphore handle to destroy.
 */
static inline void engine_sem_destroy(engine_semaphore_t sem) {
  if (sem) {
    sem_destroy(sem);
    free(sem);
  }
}
/**
 * @brief Signals the semaphore.
 * @param sem The semaphore handle.
 */
static inline void engine_sem_signal(engine_semaphore_t sem) {
  if (sem) sem_post(sem);
}
/**
 * @brief Waits on the semaphore.
 * @param sem The semaphore handle.
 */
static inline void engine_sem_wait(engine_semaphore_t sem) {
  if (sem) sem_wait(sem);
}
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
/**
 * @brief Platform-specific semaphore wrapper handle.
 */
typedef HANDLE engine_semaphore_t;
/**
 * @brief Creates a platform-specific semaphore wrapper.
 * @return The initialized semaphore handle, or NULL on failure.
 */
static inline engine_semaphore_t engine_sem_create(void) {
  return CreateSemaphore(NULL, 0, 32767, NULL);
}
/**
 * @brief Destroys the semaphore.
 * @param sem The semaphore handle to destroy.
 */
static inline void engine_sem_destroy(engine_semaphore_t sem) {
  if (sem) CloseHandle(sem);
}
/**
 * @brief Signals the semaphore.
 * @param sem The semaphore handle.
 */
static inline void engine_sem_signal(engine_semaphore_t sem) {
  if (sem) ReleaseSemaphore(sem, 1, NULL);
}
/**
 * @brief Waits on the semaphore.
 * @param sem The semaphore handle.
 */
static inline void engine_sem_wait(engine_semaphore_t sem) {
  if (sem) WaitForSingleObject(sem, INFINITE);
}
#endif

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
 * @brief Gets the captured semaphore handle.
 * @param state Pointer to the shared state instance.
 * @return The captured semaphore handle.
 */
engine_semaphore_t engine_shared_state_get_captured_semaphore(
    const engine_shared_state_t* state);

/**
 * @brief Gets the processed semaphore handle.
 * @param state Pointer to the shared state instance.
 * @return The processed semaphore handle.
 */
engine_semaphore_t engine_shared_state_get_processed_semaphore(
    const engine_shared_state_t* state);

/**
 * @brief Checks if a stop has been requested on the shared state.
 * @param state Pointer to the shared state instance.
 * @return true if stop was requested, false otherwise.
 */
bool engine_shared_state_get_stop_requested(const engine_shared_state_t* state);

/**
 * @brief Sets the stop_requested flag on the shared state.
 * @param state Pointer to the shared state instance.
 * @param requested Value to set the stop_requested flag to.
 */
void engine_shared_state_set_stop_requested(engine_shared_state_t* state,
                                            bool requested);

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
 * @brief Sets the current stop reason.
 * @param state Pointer to the shared state instance.
 * @param reason The processing stop reason to record.
 */
void engine_shared_state_set_stop_reason(engine_shared_state_t* state,
                                         processing_stop_reason_t reason);

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
 * @brief Requests the engine loops to stop via in-band AudioMessage.
 *
 * @param state Pointer to the shared state.
 * @param reason The reason for stopping.
 */
void engine_shared_state_request_stop(engine_shared_state_t* state,
                                      processing_stop_reason_t reason);

/**
 * @brief Signals the captured semaphore on the shared state.
 * @param state Pointer to the shared state instance.
 */
void engine_shared_state_signal_captured(engine_shared_state_t* state);

/**
 * @brief Signals the processed semaphore on the shared state.
 * @param state Pointer to the shared state instance.
 */
void engine_shared_state_signal_processed(engine_shared_state_t* state);

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
 * @brief Dequeues an audio chunk from an SPSC queue, blocking on the semaphore
 * if empty.
 *
 * @param queue Pointer to the SPSC queue.
 * @param sem The semaphore handle to wait on when empty.
 * @param state Pointer to the shared state instance.
 * @return audio_chunk_t* Dequeued chunk, or NULL if the pipeline is stopping
 * and queue is drained.
 */
audio_chunk_t* engine_shared_state_dequeue_blocking(
    spsc_queue_t* queue, engine_semaphore_t sem, engine_shared_state_t* state);

#endif  // CLIB_ENGINE_ENGINE_SHARED_STATE_H
