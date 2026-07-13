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
//   state_raw           — state representation. When set to
//   PROCESSING_STATE_INACTIVE,
//                         it serves as the stop signal. Read uses acquire
//                         ordering; write uses release ordering.
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

#include "Logging/app_logger.h"
#include "Pipeline/pipeline.h"

static const logger_t g_logger = {"dsp.engine.state"};

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
   * @brief The reason the engine stopped. See file-level note for publication
   * discipline.
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
   * @brief Raw atomic state representation.
   */
  _Atomic uint8_t state_raw;

  /**
   * @brief Atomic flag to ensure stop logic is executed only once.
   */
  _Atomic bool stop_once;
};

spsc_queue_t* engine_shared_state_get_captured_queue(
    engine_shared_state_t* state) {
  return state ? audio_sync_queue_get_spsc_queue(state->captured_queue) : NULL;
}

spsc_queue_t* engine_shared_state_get_processed_queue(
    engine_shared_state_t* state) {
  return state ? audio_sync_queue_get_spsc_queue(state->processed_queue) : NULL;
}

bool engine_shared_state_should_stop(const engine_shared_state_t* state) {
  return state ? (engine_shared_state_get_state(state) ==
                  PROCESSING_STATE_INACTIVE)
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

const processing_stop_reason_t* engine_shared_state_get_stop_reason(
    const engine_shared_state_t* state) {
  if (!state) return NULL;
  return &state->stop_reason;
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
  atomic_init(&state->resampler_ratio, 1.0);
  atomic_init(&state->state_raw,
              processing_state_to_raw_byte(PROCESSING_STATE_INACTIVE));
  atomic_init(&state->stop_once, false);
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

void engine_shared_state_request_stop(engine_shared_state_t* state,
                                      processing_stop_reason_t reason) {
  if (!state) return;

  bool expected = false;
  // Use compare-exchange (CAS) to ensure that only the first thread to request
  // a stop succeeds. We write the stop reason and transition state to INACTIVE,
  // then release-store stop_requested to true.
  if (atomic_compare_exchange_strong_explicit(&state->stop_once, &expected,
                                              true, memory_order_acq_rel,
                                              memory_order_acquire)) {
    state->stop_reason = reason;
    engine_shared_state_set_state(state, PROCESSING_STATE_INACTIVE);
    audio_sync_queue_shutdown(state->captured_queue);
  }
}

void engine_shared_state_shutdown_captured_queue(engine_shared_state_t* state) {
  if (state && state->captured_queue) {
    audio_sync_queue_shutdown(state->captured_queue);
  }
}

void engine_shared_state_shutdown_processed_queue(
    engine_shared_state_t* state) {
  if (state && state->processed_queue) {
    audio_sync_queue_shutdown(state->processed_queue);
  }
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
      state->captured_queue);
}

audio_chunk_t* engine_shared_state_dequeue_processed_blocking(
    engine_shared_state_t* state) {
  if (!state) return NULL;
  return (audio_chunk_t*)audio_sync_queue_dequeue_blocking(
      state->processed_queue);
}

processing_state_t engine_shared_state_get_state(
    const engine_shared_state_t* state) {
  if (!state) return PROCESSING_STATE_INACTIVE;
  // Use acquire memory order to ensure that any reads of other shared state
  // (like stop_reason) after this call will see the values written before
  // the corresponding release-store in set_state.
  uint8_t raw = atomic_load_explicit(&state->state_raw, memory_order_acquire);
  return processing_state_from_raw_byte(raw);
}

void engine_shared_state_set_state(engine_shared_state_t* state,
                                   processing_state_t new_state) {
  if (!state) return;

  uint8_t expected =
      atomic_load_explicit(&state->state_raw, memory_order_relaxed);
  while (1) {
    if (atomic_load_explicit(&state->stop_once, memory_order_acquire)) {
      if (new_state != PROCESSING_STATE_INACTIVE) {
        return;
      }
    }
    uint8_t desired_raw = processing_state_to_raw_byte(new_state);
    if (atomic_compare_exchange_weak_explicit(&state->state_raw, &expected,
                                              desired_raw, memory_order_release,
                                              memory_order_acquire)) {
      logger_info(&g_logger, "Engine state transitioning to %d", new_state);
      break;
    }
  }
}
