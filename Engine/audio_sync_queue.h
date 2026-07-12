#ifndef CLIB_ENGINE_AUDIO_SYNC_QUEUE_H
#define CLIB_ENGINE_AUDIO_SYNC_QUEUE_H

/**
 * @file audio_sync_queue.h
 * @brief Thread-safe single-producer single-consumer queue combining a
 * lock-free SPSC queue with OS kernel semaphore signaling.
 */

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

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
typedef sem_t* engine_semaphore_t;
static inline engine_semaphore_t engine_sem_create(void) {
  sem_t* sem = (sem_t*)calloc(1, sizeof(sem_t));
  if (!sem) return NULL;
  if (sem_init(sem, 0, 0) != 0) {
    free(sem);
    return NULL;
  }
  return sem;
}
static inline void engine_sem_destroy(engine_semaphore_t sem) {
  if (sem) {
    sem_destroy(sem);
    free(sem);
  }
}
static inline void engine_sem_signal(engine_semaphore_t sem) {
  if (sem) sem_post(sem);
}
static inline void engine_sem_wait(engine_semaphore_t sem) {
  if (sem) sem_wait(sem);
}
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef HANDLE engine_semaphore_t;
static inline engine_semaphore_t engine_sem_create(void) {
  return CreateSemaphore(NULL, 0, 2147483647L, NULL);
}
static inline void engine_sem_destroy(engine_semaphore_t sem) {
  if (sem) CloseHandle(sem);
}
static inline void engine_sem_signal(engine_semaphore_t sem) {
  if (sem) ReleaseSemaphore(sem, 1, NULL);
}
static inline void engine_sem_wait(engine_semaphore_t sem) {
  if (sem) WaitForSingleObject(sem, INFINITE);
}
#endif

/**
 * @struct audio_sync_queue
 * @brief Opaque structure for single-producer single-consumer synchronized
 * queue.
 */
typedef struct audio_sync_queue audio_sync_queue_t;

/**
 * @brief Creates a new audio_sync_queue instance.
 *
 * @param depth FIFO queue capacity.
 * @return Pointer to allocated audio_sync_queue_t, or NULL on failure.
 */
audio_sync_queue_t* audio_sync_queue_create(size_t depth);

/**
 * @brief Frees the audio_sync_queue instance and its underlying resources.
 *
 * @param queue Pointer to the audio_sync_queue_t instance to free.
 */
void audio_sync_queue_free(audio_sync_queue_t* queue);

/**
 * @brief Gets the underlying SPSC queue pointer.
 *
 * @param queue Pointer to the audio_sync_queue_t instance.
 * @return Pointer to spsc_queue_t, or NULL if queue is NULL.
 */
spsc_queue_t* audio_sync_queue_get_spsc_queue(const audio_sync_queue_t* queue);

/**
 * @brief Gets the underlying semaphore handle.
 *
 * @param queue Pointer to the audio_sync_queue_t instance.
 * @return The kernel semaphore handle.
 */
engine_semaphore_t audio_sync_queue_get_semaphore(
    const audio_sync_queue_t* queue);

/**
 * @brief Manually signals the queue's semaphore to wake up waiting consumer
 * threads.
 *
 * @param queue Pointer to the audio_sync_queue_t instance.
 */
void audio_sync_queue_signal(audio_sync_queue_t* queue);

/**
 * @brief Enqueues an item into the queue and signals the kernel semaphore.
 *
 * @param queue Pointer to the audio_sync_queue_t instance.
 * @param item Pointer to data item to enqueue.
 * @return true if enqueued successfully, false if the queue was full.
 */
bool audio_sync_queue_enqueue(audio_sync_queue_t* queue, void* item);

/**
 * @brief Dequeues an item from the queue, blocking on the semaphore if empty.
 *
 * Checks non-blocking dequeue first. If empty and stop_requested is true,
 * returns NULL immediately. Otherwise blocks on the kernel semaphore.
 *
 * @param queue Pointer to the audio_sync_queue_t instance.
 * @param stop_requested Optional pointer to atomic stop flag.
 * @return Pointer to dequeued item, or NULL when stopping and queue is empty.
 */
void* audio_sync_queue_dequeue_blocking(audio_sync_queue_t* queue,
                                        const _Atomic bool* stop_requested);

#endif  // CLIB_ENGINE_AUDIO_SYNC_QUEUE_H
