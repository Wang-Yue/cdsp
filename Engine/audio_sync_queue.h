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

#include "Utils/lock_free_ring_buffer.h"
#include "cdsp_sem.h"

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
cdsp_sem_t audio_sync_queue_get_semaphore(const audio_sync_queue_t* queue);

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
 * @brief Shuts down the queue, waking up any waiting threads.
 *
 * @param queue Pointer to the audio_sync_queue_t instance.
 */
void audio_sync_queue_shutdown(audio_sync_queue_t* queue);

/**
 * @brief Dequeues an item from the queue, blocking on the semaphore if empty.
 *
 * Checks non-blocking dequeue first. If empty and the queue has been shut down,
 * returns NULL immediately. Otherwise blocks on the kernel semaphore.
 *
 * @param queue Pointer to the audio_sync_queue_t instance.
 * @return Pointer to dequeued item, or NULL when shut down and queue is empty.
 */
void* audio_sync_queue_dequeue_blocking(audio_sync_queue_t* queue);

#endif  // CLIB_ENGINE_AUDIO_SYNC_QUEUE_H
