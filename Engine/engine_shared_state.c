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
//   stop_requested      — written by `request_stop()` / read by all loops.
//                         Atomic<Bool> w/ release-acquire so a stop request
//                         becomes promptly visible.
//   captured_queue      — audio_sync_queue_t (SPSC queue + kernel semaphore),
//                         single producer = capture, single consumer =
//                         processing.
//   processed_queue     — audio_sync_queue_t (SPSC queue + kernel semaphore),
//                         single producer = processing, single consumer =
//                         playback.
//   resampler_ratio     — playback writes (rate-adjust controller),
//                         processing reads (per chunk). 64-bit atomic.
//
// `engine_semaphore_t` is included to be transparent: a semaphore is a
// kernel signaling primitive, not a lock. Producers signal after
// enqueue; consumers wait, then drain. There is never a critical
// section — a single signal can wake the consumer for any number of
// queued items, and the consumer drains until empty before waiting
// again.

#include "engine_shared_state.h"

#include <stdlib.h>

#include "Pipeline/pipeline.h"

struct engine_shared_state {
  /**
   * @brief Bounded sync queue from the capture thread to the processing thread.
   */
  audio_sync_queue_t* captured_queue;

  /**
   * @brief Bounded sync queue from the processing thread to the playback
   * thread.
   */
  audio_sync_queue_t* processed_queue;

  /**
   * @brief External trigger flag instructing loops to stop.
   */
  _Atomic bool stop_requested;

  /**
   * @brief Atomic flag indicating that the processing loop has finished.
   */
  _Atomic bool processing_done;

  /**
   * @brief Terminal stop reason explaining why the engine stopped.
   */
  processing_stop_reason_t stop_reason;

  /**
   * @brief Resampler relative-ratio (≈ 1.0).
   */
  _Atomic double resampler_ratio;

  /**
   * @brief Deferred free queue for old pipeline structures.
   */
  spsc_queue_t* pipeline_garbage_queue;

  /**
   * @brief Reference to the engine state machine.
   */
  engine_state_machine_t* state_machine;

  /**
   * @brief Count of active audio-priority loop threads.
   */
  _Atomic int active_threads;
};

spsc_queue_t* engine_shared_state_get_captured_queue(
    engine_shared_state_t* state) {
  return state ? audio_sync_queue_get_spsc_queue(state->captured_queue) : NULL;
}

spsc_queue_t* engine_shared_state_get_processed_queue(
    engine_shared_state_t* state) {
  return state ? audio_sync_queue_get_spsc_queue(state->processed_queue) : NULL;
}

bool engine_shared_state_get_stop_requested(
    const engine_shared_state_t* state) {
  return state ? atomic_load_explicit(&state->stop_requested,
                                      memory_order_acquire)
               : false;
}

double engine_shared_state_get_resampler_ratio(
    const engine_shared_state_t* state) {
  return state ? atomic_load_explicit(&state->resampler_ratio,
                                      memory_order_relaxed)
               : 1.0;
}

void engine_shared_state_set_resampler_ratio(engine_shared_state_t* state,
                                             double ratio) {
  if (state) {
    atomic_store_explicit(&state->resampler_ratio, ratio, memory_order_relaxed);
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

engine_shared_state_t* engine_shared_state_create(
    size_t captured_queue_depth, size_t processed_queue_depth) {
  engine_shared_state_t* state =
      (engine_shared_state_t*)calloc(1, sizeof(engine_shared_state_t));
  if (!state) return NULL;

  state->captured_queue = audio_sync_queue_create(
      captured_queue_depth > 0 ? captured_queue_depth : 16);
  state->processed_queue = audio_sync_queue_create(
      processed_queue_depth > 0 ? processed_queue_depth : 16);
  atomic_init(&state->stop_requested, false);
  atomic_init(&state->resampler_ratio, 1.0);
  atomic_init(&state->active_threads, 3);
  state->pipeline_garbage_queue = spsc_queue_create(32);

  if (!state->captured_queue || !state->processed_queue ||
      !state->pipeline_garbage_queue) {
    engine_shared_state_free(state);
    return NULL;
  }
  return state;
}

void engine_shared_state_free(engine_shared_state_t* state) {
  if (!state) return;
  audio_sync_queue_free(state->captured_queue);
  audio_sync_queue_free(state->processed_queue);

  if (state->pipeline_garbage_queue) {
    void* p;
    while ((p = spsc_queue_dequeue(state->pipeline_garbage_queue)) != NULL) {
      pipeline_free((pipeline_t*)p);
    }
    spsc_queue_free(state->pipeline_garbage_queue);
  }

  free(state);
}

void engine_shared_state_signal_captured(engine_shared_state_t* state) {
  if (!state) return;
  if (state->captured_queue) {
    audio_sync_queue_signal(state->captured_queue);
  }
}

void engine_shared_state_set_processing_done(engine_shared_state_t* state) {
  if (!state) return;
  atomic_store_explicit(&state->processing_done, true, memory_order_release);
  if (state->processed_queue) {
    audio_sync_queue_signal(state->processed_queue);
  }
}

void engine_shared_state_request_stop(engine_shared_state_t* state,
                                      processing_stop_reason_t reason) {
  if (!state) return;
  state->stop_reason = reason;
  atomic_store_explicit(&state->stop_requested, true, memory_order_release);

  // Signal captured queue to wake processing loop if waiting on sem
  engine_shared_state_signal_captured(state);
}

bool engine_shared_state_enqueue_captured(engine_shared_state_t* state,
                                          audio_chunk_t* chunk) {
  if (!state) return false;
  return audio_sync_queue_enqueue(state->captured_queue, chunk);
}

bool engine_shared_state_enqueue_processed(engine_shared_state_t* state,
                                           audio_chunk_t* chunk) {
  if (!state) return false;
  return audio_sync_queue_enqueue(state->processed_queue, chunk);
}

audio_chunk_t* engine_shared_state_dequeue_captured_blocking(
    engine_shared_state_t* state) {
  if (!state) return NULL;
  return (audio_chunk_t*)audio_sync_queue_dequeue_blocking(
      state->captured_queue, &state->stop_requested);
}

audio_chunk_t* engine_shared_state_dequeue_processed_blocking(
    engine_shared_state_t* state) {
  if (!state) return NULL;
  return (audio_chunk_t*)audio_sync_queue_dequeue_blocking(
      state->processed_queue, &state->processing_done);
}

void engine_shared_state_set_state_machine(engine_shared_state_t* state,
                                           engine_state_machine_t* sm) {
  if (state) {
    state->state_machine = sm;
  }
}

void engine_shared_state_thread_exited(engine_shared_state_t* state) {
  if (!state) return;
  int remaining = atomic_fetch_sub_explicit(&state->active_threads, 1,
                                            memory_order_acq_rel) -
                  1;
  if (remaining == 0) {
    if (state->state_machine) {
      engine_state_machine_set_state(state->state_machine,
                                     PROCESSING_STATE_INACTIVE);
    }
  }
}
