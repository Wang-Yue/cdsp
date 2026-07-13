// Capture thread body. One instance per engine run; the thread
// closure invokes `run()` exactly once and returns when the shared
// `shouldStop` flag is set or a stop reason is reported.
//
// State ownership
// ---------------
// All mutable state — the working chunk, the silence counter, the
// stall watchdog — lives inside the loop instance and is touched
// only by the capture thread. Cross-thread communication happens
// exclusively through the injected `EngineSharedState`.
//
// Audio-thread invariants
// -----------------------
//   * No allocations in the steady-state. Audio chunks are obtained
//     from a pre-allocated `RoundRobinChunkPool`.
//   * No locks. Coordination uses the shared SPSC queue + semaphore.
//   * No `Date()` / `gettimeofday`. The watchdog uses
//     `clock_gettime_nsec_np(CLOCK_UPTIME_RAW)` (vDSO read on
//     Darwin — no syscall).
#include "engine_capture_loop.h"

#include <stdio.h>

#include "Audio/silence_counter.h"
#include "sample_rate_watcher.h"

struct engine_capture_loop {
  engine_shared_state_t* shared;
  capture_backend_t* capture;
  playback_backend_t* playback;
  processing_parameters_t* processing_params;
  dop_decoder_t* dop_decoder;

  size_t chunk_size;
  size_t channels;
  size_t samplerate;
  double last_observed_pending_rate;
  bool has_last_observed_pending_rate;

  silence_counter_t* silence_counter;
  round_robin_chunk_pool_t* chunk_pool;

  uint64_t watchdog_last_success_ns;
  bool watchdog_triggered;
  double watchdog_timeout_seconds;

  sample_rate_watcher_t* rate_watcher;
};
#include <stdlib.h>
#include <time.h>

#include "Logging/app_logger.h"
#include "thread_priority.h"

#ifndef __APPLE__
#define CLOCK_UPTIME_RAW CLOCK_MONOTONIC
/**
 * @brief Helper function to retrieve the raw system uptime in nanoseconds.
 * Used for the watchdog stall detector. On non-Apple platforms, this wraps
 * clock_gettime(CLOCK_MONOTONIC).
 *
 * @param clock_id The system clock identifier.
 * @return The current time value in nanoseconds.
 */
static inline uint64_t clock_gettime_nsec_np(int clock_id) {
  struct timespec ts;
  clock_gettime(clock_id, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

#endif

engine_capture_loop_t* engine_capture_loop_create(
    engine_shared_state_t* shared, capture_backend_t* capture,
    playback_backend_t* playback, processing_parameters_t* processing_params,
    dop_decoder_t* dop_decoder, round_robin_chunk_pool_t* chunk_pool,
    size_t chunk_size, size_t channels, size_t samplerate,
    double silence_threshold_db, double silence_timeout_seconds,
    bool stop_on_rate_change, double rate_measure_interval) {
  engine_capture_loop_t* loop =
      (engine_capture_loop_t*)calloc(1, sizeof(engine_capture_loop_t));
  if (!loop) return NULL;

  loop->shared = shared;
  loop->capture = capture;
  loop->playback = playback;
  loop->processing_params = processing_params;
  loop->dop_decoder = dop_decoder;
  loop->chunk_pool = chunk_pool;
  loop->chunk_size = chunk_size;
  loop->channels = channels;
  loop->samplerate = samplerate;
  loop->silence_counter = silence_counter_create(
      silence_threshold_db, silence_timeout_seconds, samplerate, chunk_size);
  if (!loop->silence_counter) {
    engine_capture_loop_free(loop);
    return NULL;
  }

  loop->rate_watcher = sample_rate_watcher_create(
      (double)samplerate, rate_measure_interval, stop_on_rate_change);
  if (!loop->rate_watcher) {
    engine_capture_loop_free(loop);
    return NULL;
  }

  loop->watchdog_timeout_seconds = 0.5;
  loop->watchdog_last_success_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  loop->watchdog_triggered = false;

  return loop;
}

void engine_capture_loop_free(engine_capture_loop_t* loop) {
  if (!loop) return;
  if (loop->silence_counter) {
    silence_counter_free(loop->silence_counter);
  }
  if (loop->rate_watcher) {
    sample_rate_watcher_free(loop->rate_watcher);
  }
  free(loop);
}

void engine_capture_loop_run(engine_capture_loop_t* loop) {
  if (!loop) return;
  logger_t logger = logger_create("dsp.capture");
  logger_info(&logger, "Capture thread started");

  set_realtime_thread_priority("Capture", loop->chunk_size, loop->samplerate);
  sample_rate_watcher_reset(loop->rate_watcher);

  while (1) {
    if (engine_shared_state_should_stop(loop->shared)) {
      break;
    }

    if (engine_shared_state_get_state(loop->shared) ==
        PROCESSING_STATE_PAUSED) {
      sample_rate_watcher_reset(loop->rate_watcher);
    }

    // 1. Hardware Sample-Rate Change Check:
    // Check if the hardware sample rates have drifted or been explicitly
    // modified (e.g. by another application or OS settings). An unexpected
    // hardware rate change invalidates the processing thread pipeline, so we
    // signal a host rebuild stop reason.
    double rate = 0.0;
    if (capture_backend_get_pending_rate_change(loop->capture, &rate)) {
      if (!loop->has_last_observed_pending_rate ||
          rate != loop->last_observed_pending_rate) {
        loop->last_observed_pending_rate = rate;
        loop->has_last_observed_pending_rate = true;
        logger_warn(&logger,
                    "Capture device rate changed to %f Hz; stopping engine",
                    rate);
        processing_stop_reason_t reason = {
            .type = STOP_REASON_CAPTURE_FORMAT_CHANGE,
            .format_change_rate = (int)(rate + 0.5)};
        engine_shared_state_request_stop(loop->shared, reason);
        break;
      }
    }

    // 2. Fetch a chunk buffer from the pre-allocated round-robin pool.
    audio_chunk_t* chunk = round_robin_chunk_pool_next(loop->chunk_pool);
    backend_error_t err;
    backend_error_init(&err, BACKEND_ERROR_NONE, "");

    // 3. Read raw PCM/DSD frame data from the capture backend.
    bool got_data =
        capture_backend_read(loop->capture, loop->chunk_size, chunk, &err);
    if (!got_data) {
      if (err.type == BACKEND_ERROR_READ_EOF) {
        logger_info(
            &logger,
            "Capture reached End-of-Stream; stopping engine gracefully");
        processing_stop_reason_t reason = {.type = STOP_REASON_DONE};
        snprintf(reason.message, sizeof(reason.message), "EOF");
        engine_shared_state_request_stop(loop->shared, reason);
        break;
      }
      // If reading fails with an error, trigger an engine stop.
      if (err.type != BACKEND_ERROR_NONE) {
        logger_error(&logger, "Capture error: %s", err.message);
        processing_stop_reason_t reason = {.type = STOP_REASON_CAPTURE_ERROR};
        snprintf(reason.message, sizeof(reason.message), "%s", err.message);
        engine_shared_state_request_stop(loop->shared, reason);
        break;
      }
      // If the engine is in a PAUSED state (no active input signal), reset the
      // watchdog timer to avoid triggering stall warnings while waiting for
      // signal.
      if (engine_shared_state_get_state(loop->shared) ==
          PROCESSING_STATE_PAUSED) {
        loop->watchdog_last_success_ns =
            clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        capture_backend_wait(loop->capture, 20);
        continue;
      }
      // 4. Watchdog / Stall Monitor:
      // If the engine is running but we get no data chunks from the capture
      // device for more than watchdog_timeout_seconds, set state to STALLED and
      // log a warning.
      if (!loop->watchdog_triggered) {
        uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        double elapsed =
            (double)(now - loop->watchdog_last_success_ns) / 1000000000.0;
        if (elapsed > loop->watchdog_timeout_seconds) {
          loop->watchdog_triggered = true;
          engine_shared_state_set_state(loop->shared, PROCESSING_STATE_STALLED);
          logger_warn(&logger, "Capture device stalled — no data for %fs",
                      loop->watchdog_timeout_seconds);
        }
      }
      // Block/wait up to 20ms using the backend's synchronization mechanism
      // (e.g. semaphore). This yields CPU time while maintaining real-time
      // scheduling priority.
      capture_backend_wait(loop->capture, 20);
      continue;
    }

    // 5. Watchdog Stall Recovery:
    // Reset the watchdog status if we successfully read data after a stall.
    loop->watchdog_last_success_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (loop->watchdog_triggered) {
      loop->watchdog_triggered = false;
      logger_info(&logger, "Capture recovered from stall");
    }

    // 5.5. Rate Watcher Measurement:
    double measured_rate = 0.0;
    if (sample_rate_watcher_tick(loop->rate_watcher, loop->chunk_size,
                                 &measured_rate)) {
      if (sample_rate_watcher_get_stop_on_rate_change(loop->rate_watcher)) {
        logger_warn(
            &logger,
            "Sample rate change detected (measured: %f Hz, expected: %zu "
            "Hz); stopping engine",
            measured_rate, loop->samplerate);
        processing_stop_reason_t reason = {
            .type = STOP_REASON_CAPTURE_FORMAT_CHANGE,
            .format_change_rate = (int)(measured_rate + 0.5)};
        engine_shared_state_request_stop(loop->shared, reason);
        break;
      } else {
        logger_info(
            &logger,
            "Sample rate drift detected (measured: %f Hz, expected: %zu "
            "Hz)",
            measured_rate, loop->samplerate);
      }
    }

    // 6. DoP (DSD over PCM) Decoding:
    // If DoP decoding is active, process the PCM chunk to detect DSD marker
    // flags and decode them back to raw DSD samples in-place. Decoding is done
    // before metering so RMS/Peak values reflect the actual signal instead of
    // the carrier noise.
    if (loop->dop_decoder) {
      dop_decoder_detect_and_process(loop->dop_decoder, chunk);
    }

    // Update level meters with the peak/rms of this chunk.
    double loudest_peak = processing_parameters_update_capture_levels(
        loop->processing_params, chunk);

    // 7. Silence/Auto-pause Gate:
    // Update the silence counter. If the signal level is below the threshold
    // for longer than the timeout duration, desired is set to
    // PROCESSING_STATE_PAUSED. We toggle the backends' state to paused to stop
    // downstream devices.
    processing_state_t desired =
        silence_counter_update(loop->silence_counter, loudest_peak);
    processing_state_t current = engine_shared_state_get_state(loop->shared);
    if (desired != current) {
      engine_shared_state_set_state(loop->shared, desired);
      playback_backend_set_is_paused(loop->playback,
                                     (desired == PROCESSING_STATE_PAUSED));
      capture_backend_set_is_paused(loop->capture,
                                    (desired == PROCESSING_STATE_PAUSED));
    }

    // 8. Enqueue Captured Chunk:
    // Push the chunk pointer into the bounded lock-free SPSC queue.
    if (engine_shared_state_get_state(loop->shared) !=
        PROCESSING_STATE_PAUSED) {
      while (!engine_shared_state_enqueue_captured(loop->shared, chunk)) {
        if (engine_shared_state_should_stop(loop->shared)) {
          break;
        }
        engine_yield();
      }
    }
  }

  if (loop->shared) {
    engine_shared_state_shutdown_captured_queue(loop->shared);
  }
  logger_info(&logger, "Capture thread stopped");
  engine_shared_state_thread_exited(loop->shared);
}
