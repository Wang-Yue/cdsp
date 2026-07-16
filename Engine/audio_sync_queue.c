#include "audio_sync_queue.h"

#include <stdlib.h>

struct audio_sync_queue {
  spsc_queue_t* queue;
  cdsp_sem_t semaphore;
  _Atomic bool is_shutdown;
};

audio_sync_queue_t* audio_sync_queue_create(size_t depth) {
  audio_sync_queue_t* queue =
      (audio_sync_queue_t*)calloc(1, sizeof(audio_sync_queue_t));
  if (!queue) return NULL;

  queue->queue = spsc_queue_create(depth > 0 ? depth : 16);
  queue->semaphore = cdsp_sem_create();
  atomic_init(&queue->is_shutdown, false);

  if (!queue->queue || !queue->semaphore) {
    audio_sync_queue_free(queue);
    return NULL;
  }

  return queue;
}

void audio_sync_queue_free(audio_sync_queue_t* queue) {
  if (!queue) return;
  if (queue->queue) {
    spsc_queue_free(queue->queue);
  }
  cdsp_sem_destroy(queue->semaphore);
  free(queue);
}

spsc_queue_t* audio_sync_queue_get_spsc_queue(const audio_sync_queue_t* queue) {
  return queue ? queue->queue : NULL;
}

cdsp_sem_t audio_sync_queue_get_semaphore(const audio_sync_queue_t* queue) {
  return queue ? queue->semaphore : NULL;
}

void audio_sync_queue_signal(audio_sync_queue_t* queue) {
  if (queue) {
    cdsp_sem_signal(queue->semaphore);
  }
}

bool audio_sync_queue_enqueue(audio_sync_queue_t* queue, void* item) {
  if (!queue || !queue->queue) return false;
  bool ok = spsc_queue_enqueue(queue->queue, item);
  if (ok) {
    cdsp_sem_signal(queue->semaphore);
  }
  return ok;
}

void audio_sync_queue_shutdown(audio_sync_queue_t* queue) {
  if (queue) {
    atomic_store_explicit(&queue->is_shutdown, true, memory_order_release);
    cdsp_sem_signal(queue->semaphore);
  }
}

void* audio_sync_queue_dequeue_blocking(audio_sync_queue_t* queue) {
  if (!queue || !queue->queue || !queue->semaphore) return NULL;

  while (1) {
    // 1. Attempt 1: Fast-path non-blocking dequeue.
    // Try to retrieve an audio chunk immediately from the SPSC ring buffer.
    void* item = spsc_queue_dequeue(queue->queue);
    if (item) {
      // Consume the pending semaphore token posted by the producer during
      // enqueue. Doing a non-blocking timedwait (timeout=0) keeps the OS
      // semaphore count in exact 1:1 synchronization with actual items in the
      // ring buffer, preventing accumulated "ghost" signal counts when the
      // queue later becomes empty.
      cdsp_sem_timedwait(queue->semaphore, 0);
      return item;
    }

    // 2. Shutdown & Drain check.
    // If Attempt 1 returned NULL, check if the producer signaled shutdown.
    if (atomic_load_explicit(&queue->is_shutdown, memory_order_acquire)) {
      // Attempt 2: Post-shutdown drain check.
      // Re-try dequeue to handle a timing race condition: if the producer
      // enqueued a final audio chunk right before setting is_shutdown, Attempt
      // 1 above might have run a nanosecond before the item landed in the ring
      // buffer. Attempt 2 guarantees all remaining audio chunks are completely
      // drained/flushed before returning NULL.
      item = spsc_queue_dequeue(queue->queue);
      if (item) {
        cdsp_sem_timedwait(queue->semaphore, 0);
        return item;
      }
      return NULL;
    }

    // 3. Kernel Wait / CPU Yield.
    // When the queue is empty and not shut down, block indefinitely on the OS
    // kernel semaphore (cdsp_sem_wait). This puts the calling consumer thread
    // to sleep at the OS scheduler level without consuming CPU cycles in a
    // spin-wait loop, and automatically wakes the thread when the producer
    // enqueues new audio chunks or signals shutdown.
    cdsp_sem_wait(queue->semaphore);
  }
}
