// Inter-thread state for the DSP engine's three audio-priority loops
// (capture / processing / playback). Every field here is either a
// lock-free atomic, a wait-free SPSC queue, or a kernel signaling
// primitive (`DispatchSemaphore`). No mutexes, no `NSLock`, no
// `@unchecked` reads of shared mutable references — so any of the
// three loops can read or write any of these fields without
// coordinating with the others.
//
// Concurrency model
// -----------------
//   shouldStop          — written by `stop()` / read by all three loops
//                         every iteration. Atomic<Bool> w/ release-acquire
//                         so a stop request becomes promptly visible.
//   capturedQueue       — SPSC, single producer = capture, single
//                         consumer = processing.
//   processedQueue      — SPSC, single producer = processing, single
//                         consumer = playback.
//   capturedSemaphore   — capture signals, processing waits.
//   processedSemaphore  — processing signals, playback waits.
//   resamplerRatio      — playback writes (rate-adjust controller),
//                         processing reads (per chunk). 64-bit atomic.
//
// `DispatchSemaphore` is included to be transparent: a semaphore is a
// kernel signaling primitive, not a lock. Producers signal after
// enqueue; consumers wait, then drain. There is never a critical
// section — a single signal can wake the consumer for any number of
// queued items, and the consumer drains until empty before waiting
// again.

#include "engine_shared_state.h"

#include <stdlib.h>

#include "Engine/engine_processing_loop.h"
#include "Pipeline/pipeline.h"

struct engine_shared_state {
  /**
   * @brief Bounded SPSC FIFO from the capture thread to the processing thread.
   * `enqueue` returns `false` when full; the producer drops the chunk rather
   * than allocate.
   */
  spsc_queue_t* captured_queue;

  /**
   * @brief Bounded SPSC FIFO from the processing thread to the playback thread.
   */
  spsc_queue_t* processed_queue;

  /**
   * @brief Wakeup signal for the processing thread.
   * The capture thread signals after every successful `enqueue`.
   */
  engine_semaphore_t captured_semaphore;

  /**
   * @brief Wakeup signal for the playback thread.
   * The processing thread signals after every successful `enqueue`.
   */
  engine_semaphore_t processed_semaphore;

  /**
   * @brief External trigger flag instructing the capture loop to emit an
   * in-band STOP message.
   */
  _Atomic bool stop_requested;

  /**
   * @brief Terminal stop reason explaining why the engine stopped.
   */
  processing_stop_reason_t stop_reason;

  /**
   * @brief Resampler relative-ratio (≈ 1.0).
   * Published by the playback thread (rate-adjust controller); consumed by
   * the processing thread once per chunk via `setRelativeRatio`.
   */
  atomic_double_t* resampler_ratio;

  /**
   * @brief Deferred free queue for old pipeline structures.
   *
   * Holds pipeline instances swapped out by the processing thread. The control
   * thread periodically dequeues and frees them asynchronously to keep the
   * audio thread allocation-free and real-time safe.
   */
  spsc_queue_t* pipeline_garbage_queue;
};

spsc_queue_t* engine_shared_state_get_captured_queue(
    engine_shared_state_t* state) {
  return state ? state->captured_queue : NULL;
}

spsc_queue_t* engine_shared_state_get_processed_queue(
    engine_shared_state_t* state) {
  return state ? state->processed_queue : NULL;
}

engine_semaphore_t* engine_shared_state_get_captured_semaphore(
    engine_shared_state_t* state) {
  return state ? &state->captured_semaphore : NULL;
}

engine_semaphore_t* engine_shared_state_get_processed_semaphore(
    engine_shared_state_t* state) {
  return state ? &state->processed_semaphore : NULL;
}

bool engine_shared_state_get_stop_requested(
    const engine_shared_state_t* state) {
  return state ? atomic_load_explicit(&state->stop_requested,
                                      memory_order_acquire)
               : false;
}

void engine_shared_state_set_stop_requested(engine_shared_state_t* state,
                                            bool requested) {
  if (state) {
    atomic_store_explicit(&state->stop_requested, requested,
                          memory_order_release);
  }
}

double engine_shared_state_get_resampler_ratio(
    const engine_shared_state_t* state) {
  return (state && state->resampler_ratio)
             ? atomic_double_get(state->resampler_ratio)
             : 1.0;
}

void engine_shared_state_set_resampler_ratio(engine_shared_state_t* state,
                                             double ratio) {
  if (state && state->resampler_ratio) {
    atomic_double_set(state->resampler_ratio, ratio);
  }
}

bool engine_shared_state_enqueue_garbage_pipeline(engine_shared_state_t* state,
                                                  pipeline_t* pipeline) {
  if (!state || !state->pipeline_garbage_queue || !pipeline) return false;
  return spsc_queue_enqueue(state->pipeline_garbage_queue, pipeline);
}

pipeline_t* engine_shared_state_dequeue_garbage_pipeline(
    engine_shared_state_t* state) {
  if (!state || !state->pipeline_garbage_queue) return NULL;
  return (pipeline_t*)spsc_queue_dequeue(state->pipeline_garbage_queue);
}

processing_stop_reason_t engine_shared_state_get_stop_reason(
    const engine_shared_state_t* state) {
  processing_stop_reason_t empty = {.type = STOP_REASON_NONE};
  return state ? state->stop_reason : empty;
}

void engine_shared_state_set_stop_reason(engine_shared_state_t* state,
                                         processing_stop_reason_t reason) {
  if (state) state->stop_reason = reason;
}

engine_shared_state_t* engine_shared_state_create(
    size_t captured_queue_depth, size_t processed_queue_depth) {
  engine_shared_state_t* state =
      (engine_shared_state_t*)calloc(1, sizeof(engine_shared_state_t));
  if (!state) return NULL;

  state->captured_queue =
      spsc_queue_create(captured_queue_depth > 0 ? captured_queue_depth : 16);
  state->processed_queue =
      spsc_queue_create(processed_queue_depth > 0 ? processed_queue_depth : 16);
  engine_sem_init(&state->captured_semaphore);
  engine_sem_init(&state->processed_semaphore);
  atomic_init(&state->stop_requested, false);
  state->resampler_ratio = atomic_double_create(1.0);
  state->pipeline_garbage_queue = spsc_queue_create(32);

  if (!state->captured_queue || !state->processed_queue ||
      !state->captured_semaphore || !state->processed_semaphore ||
      !state->resampler_ratio || !state->pipeline_garbage_queue) {
    engine_shared_state_free(state);
    return NULL;
  }
  return state;
}

void engine_shared_state_free(engine_shared_state_t* state) {
  if (!state) return;
  if (state->captured_queue) spsc_queue_free(state->captured_queue);
  if (state->processed_queue) spsc_queue_free(state->processed_queue);
  engine_sem_destroy(&state->captured_semaphore);
  engine_sem_destroy(&state->processed_semaphore);
  if (state->resampler_ratio) atomic_double_free(state->resampler_ratio);

  if (state->pipeline_garbage_queue) {
    void* p;
    while ((p = spsc_queue_dequeue(state->pipeline_garbage_queue)) != NULL) {
      pipeline_free((pipeline_t*)p);
    }
    spsc_queue_free(state->pipeline_garbage_queue);
  }

  free(state);
}

void engine_shared_state_request_stop(engine_shared_state_t* state,
                                      processing_stop_reason_t reason) {
  if (!state) return;
  state->stop_reason = reason;
  // Store stop_requested with release ordering so the write becomes visible
  // to the capture loop immediately on its next check.
  atomic_store_explicit(&state->stop_requested, true, memory_order_release);

  // Wake up capture thread so it can emit the in-band STOP message downstream.
  engine_sem_signal(state->captured_semaphore);
  engine_sem_signal(state->processed_semaphore);
}

audio_chunk_t* engine_shared_state_dequeue_blocking(
    spsc_queue_t* queue, engine_semaphore_t* sem,
    engine_shared_state_t* state) {
  if (!queue || !sem || !state) return NULL;
  while (1) {
    audio_chunk_t* chunk = (audio_chunk_t*)spsc_queue_dequeue(queue);
    if (chunk) {
      return chunk;
    }
    if (engine_shared_state_get_stop_requested(state)) {
      return NULL;
    }
    engine_sem_wait(*sem);
  }
}
