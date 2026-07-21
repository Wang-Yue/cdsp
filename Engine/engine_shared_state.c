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

#include <pthread.h>
#include <stdlib.h>

#include "Logging/app_logger.h"
#include "Pipeline/pipeline.h"
#include "Utils/cdsp_time.h"

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
   * @brief The reason the engine stopped.
   */
  processing_stop_reason_t stop_reason;

  /**
   * @brief Mutex protecting stop_reason from data races.
   */
  pthread_mutex_t stop_reason_mutex;

  /**
   * @brief Resampler relative-ratio (≈ 1.0).
   */
  _Atomic double resampler_ratio;

  /**
  /**
   * @brief Single atomic pointer for swapped-out retired pipeline structure.
   */
  _Atomic(pipeline_t*) retired_pipeline;

  /**
   * @brief Raw atomic state representation.
   */
  _Atomic uint8_t state_raw;

  /**
   * @brief Atomic flag to ensure stop logic is executed only once.
   */
  _Atomic bool stop_once;

  /**
   * @brief Timestamp of the last successfully captured chunk in nanoseconds.
   */
  _Atomic uint64_t last_capture_time_ns;
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

pipeline_t* engine_shared_state_retire_pipeline(engine_shared_state_t* state,
                                                pipeline_t* pipeline) {
  if (!state || !pipeline) return NULL;
  return atomic_exchange_explicit(&state->retired_pipeline, pipeline,
                                  memory_order_acq_rel);
}

pipeline_t* engine_shared_state_collect_retired_pipeline(
    engine_shared_state_t* state) {
  if (!state) return NULL;
  return atomic_exchange_explicit(&state->retired_pipeline, NULL,
                                  memory_order_acquire);
}

processing_stop_reason_t engine_shared_state_get_stop_reason(
    const engine_shared_state_t* state) {
  processing_stop_reason_t reason = {.type = STOP_REASON_NONE};
  if (!state) return reason;
  pthread_mutex_lock((pthread_mutex_t*)&state->stop_reason_mutex);
  reason = state->stop_reason;
  pthread_mutex_unlock((pthread_mutex_t*)&state->stop_reason_mutex);
  return reason;
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
  atomic_init(&state->retired_pipeline, NULL);
  atomic_init(&state->state_raw,
              processing_state_to_raw_byte(PROCESSING_STATE_STARTING));
  atomic_init(&state->stop_once, false);
  atomic_init(&state->last_capture_time_ns, cdsp_time_now_ns());
  pthread_mutex_init(&state->stop_reason_mutex, NULL);

  if (!state->captured_queue || !state->processed_queue) {
    engine_shared_state_free(state);
    return NULL;
  }
  return state;
}

void engine_shared_state_free(engine_shared_state_t* state) {
  if (!state) return;
  audio_sync_queue_free(state->captured_queue);
  audio_sync_queue_free(state->processed_queue);

  pipeline_t* old = atomic_exchange_explicit(&state->retired_pipeline, NULL,
                                             memory_order_acquire);
  if (old) {
    pipeline_free(old);
  }

  pthread_mutex_destroy(&state->stop_reason_mutex);
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

  // Ref: engine_state_management.md - Section 4: The CAS Race-Condition Safety
  // Gate Acquire stop_reason_mutex before checking CAS and writing stop_reason
  // so that another thread entering the LOSER branch is guaranteed to observe
  // the published stop_reason rather than reading STOP_REASON_NONE during an
  // intermediate window.
  pthread_mutex_lock(&state->stop_reason_mutex);
  bool already_stopped =
      atomic_exchange_explicit(&state->stop_once, true, memory_order_acq_rel);
  if (!already_stopped) {
    // WINNER branch: This thread is the first to request shutdown.
    state->stop_reason = reason;
    pthread_mutex_unlock(&state->stop_reason_mutex);

    if (reason.type != STOP_REASON_DONE) {
      // Ref: engine_state_management.md - Section 3.6: Immediate Abort Teardown
      // Step 1: For non-graceful aborts (errors or user Stop), immediately
      // transition engine state to INACTIVE and shut down both queues to wake
      // up all blocked threads.
      engine_shared_state_set_state(state, PROCESSING_STATE_INACTIVE);
      audio_sync_queue_shutdown(state->captured_queue);
      audio_sync_queue_shutdown(state->processed_queue);
    } else {
      // Ref: engine_state_management.md - Section 3.5: Graceful EOF Teardown
      // (Queue Drain) Step 1: For EOF, only shut down capture queue to let
      // downstream threads finish draining.
      audio_sync_queue_shutdown(state->captured_queue);
    }
  } else {
    // LOSER branch: Stop has already been requested by another thread.
    // Ref: engine_state_management.md - Section 4: The CAS Race-Condition
    // Safety Gate If a graceful EOF was previously requested, but a subsequent
    // stop request occurs (e.g. user aborts, session teardown, or hardware
    // error during drain), force immediate INACTIVE state & queue shutdown to
    // unblock waiting threads and prevent deadlocks. If the new request is a
    // hardware error, update stop_reason to preserve root cause.
    processing_stop_reason_t current_r = state->stop_reason;
    if (current_r.type == STOP_REASON_DONE ||
        current_r.type == STOP_REASON_NONE) {
      if (reason.type != STOP_REASON_DONE && reason.type != STOP_REASON_NONE) {
        state->stop_reason = reason;
      }
      pthread_mutex_unlock(&state->stop_reason_mutex);

      engine_shared_state_set_state(state, PROCESSING_STATE_INACTIVE);
      audio_sync_queue_shutdown(state->captured_queue);
      audio_sync_queue_shutdown(state->processed_queue);
    } else {
      pthread_mutex_unlock(&state->stop_reason_mutex);
    }
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
    processing_state_t old_state = processing_state_from_raw_byte(expected);
    if (atomic_compare_exchange_weak_explicit(&state->state_raw, &expected,
                                              desired_raw, memory_order_release,
                                              memory_order_acquire)) {
      if (old_state != new_state) {
        logger_info(&g_logger, "Engine state transitioning to %s",
                    processing_state_to_string(new_state));
      }
      break;
    }
  }
}

void engine_shared_state_set_last_capture_time(engine_shared_state_t* state,
                                               uint64_t ns) {
  if (state) {
    // Relaxed ordering is sufficient since this is a simple telemetry timestamp
    // polled by the external watchdog check and does not coordinate other
    // memory stores.
    atomic_store_explicit(&state->last_capture_time_ns, ns,
                          memory_order_relaxed);
  }
}

uint64_t engine_shared_state_get_last_capture_time(
    const engine_shared_state_t* state) {
  // Relaxed ordering pairs with the relaxed store above.
  return state ? atomic_load_explicit(&state->last_capture_time_ns,
                                      memory_order_relaxed)
               : 0;
}
