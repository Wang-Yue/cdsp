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

#include <math.h>
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

  silence_counter_t* silence_counter;
  round_robin_chunk_pool_t* chunk_pool;

  uint64_t watchdog_last_success_ns;
  bool watchdog_triggered;
  double watchdog_timeout_seconds;

  sample_rate_watcher_t* rate_watcher;
  uint64_t captured_drop_counter;
  uint64_t last_paused_tick_ns;
};
#include <stdlib.h>

#include "Logging/app_logger.h"
#include "Utils/cdsp_time.h"
#include "thread_priority.h"

static const logger_t g_logger = {"dsp.capture"};

engine_capture_loop_t* engine_capture_loop_create(
    const engine_capture_loop_config_t* config) {
  if (!config) return NULL;

  engine_capture_loop_t* loop =
      (engine_capture_loop_t*)calloc(1, sizeof(engine_capture_loop_t));
  if (!loop) return NULL;

  loop->shared = config->shared;
  loop->capture = config->capture;
  loop->playback = config->playback;
  loop->processing_params = config->processing_params;
  loop->dop_decoder = config->dop_decoder;
  loop->chunk_pool = config->chunk_pool;
  loop->chunk_size = config->chunk_size;
  loop->channels = config->channels;
  loop->samplerate = config->samplerate;
  loop->silence_counter = silence_counter_create(
      config->silence_threshold_db, config->silence_timeout_seconds,
      config->samplerate, config->chunk_size);
  if (!loop->silence_counter) {
    engine_capture_loop_free(loop);
    return NULL;
  }

  loop->rate_watcher = sample_rate_watcher_create(
      (double)config->samplerate, config->rate_measure_interval_s,
      config->stop_on_rate_change);
  if (!loop->rate_watcher) {
    engine_capture_loop_free(loop);
    return NULL;
  }

  loop->watchdog_timeout_seconds = 0.5;
  loop->watchdog_last_success_ns = cdsp_time_now_ns();
  loop->watchdog_triggered = false;
  loop->captured_drop_counter = 0;
  loop->last_paused_tick_ns = 0;

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

/**
 * @brief Checks if the capture hardware backend has reported an unexpected
 * sample rate change.
 *
 * @param loop Pointer to the capture loop context.
 * @return true if a format change occurred and an engine stop was requested,
 * false otherwise.
 */
static bool capture_loop_check_format_change(engine_capture_loop_t* loop) {
  // 1. Hardware Sample-Rate Change Check:
  // Check if the hardware sample rates have drifted or been explicitly
  // modified (e.g. by another application or OS settings). An unexpected
  // hardware rate change invalidates the processing thread pipeline, so we
  // signal a host rebuild stop reason.
  double rate = 0.0;
  if (capture_backend_get_pending_rate_change(loop->capture, &rate)) {
    if (fabs(rate - (double)loop->samplerate) >= 0.5) {
      logger_warn(&g_logger,
                  "Capture device rate changed to %f Hz; stopping engine",
                  rate);
      processing_stop_reason_t reason = {
          .type = STOP_REASON_CAPTURE_FORMAT_CHANGE,
          .format_change_rate = (int)(rate + 0.5)};
      engine_shared_state_request_stop(loop->shared, reason);
      return true;
    }
  }
  return false;
}

/**
 * @brief Handles the condition where reading from the capture backend produced
 * no audio data. Handles EOF, backend errors, paused state, and watchdog stall
 * monitoring.
 *
 * @param loop Pointer to the capture loop context.
 * @param err Error descriptor filled by the capture backend.
 * @return true if the loop must break due to fatal error or EOF, false to
 * continue waiting.
 */
static bool capture_loop_handle_no_data(engine_capture_loop_t* loop,
                                        const backend_error_t* err) {
  if (err->type == BACKEND_ERROR_READ_EOF) {
    // Ref: engine_state_management.md - Section 3.5: Graceful EOF Teardown (Queue Drain)
    // Step 1: Capture loop reaches EOF, requests stop with STOP_REASON_DONE,
    // shuts down the captured queue, and exits without setting state to INACTIVE.
    logger_info(&g_logger,
                "Capture reached End-of-Stream; stopping engine gracefully");
    processing_stop_reason_t reason = {.type = STOP_REASON_DONE};
    snprintf(reason.message, sizeof(reason.message), "EOF");
    engine_shared_state_request_stop(loop->shared, reason);
    return true;
  }
  // If reading fails with an error, trigger an engine stop.
  if (err->type != BACKEND_ERROR_NONE) {
    // Ref: engine_state_management.md - Section 3.6: Immediate Abort Teardown
    // Step 1: Capture thread detects a hardware read error, requests stop with
    // CAPTURE_ERROR, which immediately transitions state to INACTIVE and wakes all loops.
    logger_error(&g_logger, "Capture error: %s", err->message);
    processing_stop_reason_t reason = {.type = STOP_REASON_CAPTURE_ERROR};
    snprintf(reason.message, sizeof(reason.message), "%s", err->message);
    engine_shared_state_request_stop(loop->shared, reason);
    return true;
  }

  // If the engine is in a PAUSED state (no active input signal), reset the
  // watchdog timer to avoid triggering stall warnings while waiting for signal.
  if (engine_shared_state_get_state(loop->shared) == PROCESSING_STATE_PAUSED) {
    loop->watchdog_last_success_ns = cdsp_time_now_ns();
    capture_backend_wait(loop->capture, 20);
    return false;
  }

  // Watchdog / Stall Monitor:
  // If the engine is running but we get no data chunks from the capture device
  // for more than watchdog_timeout_seconds, set state to STALLED and log a
  // warning.
  if (!loop->watchdog_triggered) {
    uint64_t now = cdsp_time_now_ns();
    double elapsed =
        (double)(now - loop->watchdog_last_success_ns) / 1000000000.0;
    if (elapsed > loop->watchdog_timeout_seconds) {
      loop->watchdog_triggered = true;
      engine_shared_state_set_state(loop->shared, PROCESSING_STATE_STALLED);
      logger_warn(&g_logger, "Capture device stalled — no data for %fs",
                  loop->watchdog_timeout_seconds);
    }
  }
  // Block/wait up to 20ms using the backend's synchronization mechanism (e.g.
  // semaphore). This yields CPU time while maintaining real-time scheduling
  // priority.
  capture_backend_wait(loop->capture, 20);
  return false;
}

/**
 * @brief Processes a successfully captured audio chunk and enqueues it to the
 * processing thread. Handles watchdog stall recovery, sample rate measurement,
 * DoP decoding, metering, silence detection auto-pause gate, and lock-free
 * queue push.
 *
 * @param loop Pointer to the capture loop context.
 * @param chunk Pre-allocated audio chunk containing newly captured PCM/DSD
 * samples.
 * @return true if the loop must break (e.g. rate watcher change detected),
 * false otherwise.
 */
static bool capture_loop_process_and_enqueue(engine_capture_loop_t* loop,
                                             audio_chunk_t* chunk) {
  // Ref: engine_state_management.md - Section 3.4: Watchdog Stall & Recovery Flow
  // Step 1: Stall Detection. Update the shared timestamp so the external watchdog is satisfied.
  loop->watchdog_last_success_ns = cdsp_time_now_ns();
  // Update the shared last capture timestamp so the external watchdog check is satisfied.
  engine_shared_state_set_last_capture_time(loop->shared, loop->watchdog_last_success_ns);
  if (loop->watchdog_triggered) {
    loop->watchdog_triggered = false;
    logger_info(&g_logger, "Capture recovered from stall");
  }
  // Ref: engine_state_management.md - Section 3.4: Watchdog Stall & Recovery Flow
  // Step 2: Stall Recovery. If the external watchdog previously marked us STALLED, restore to RUNNING.
  if (engine_shared_state_get_state(loop->shared) == PROCESSING_STATE_STALLED) {
    engine_shared_state_set_state(loop->shared, PROCESSING_STATE_RUNNING);
    logger_info(&g_logger, "Capture recovered from stall (external)");
  }

  // Rate Watcher Measurement:
  double measured_rate = 0.0;
  if (sample_rate_watcher_tick(loop->rate_watcher, loop->chunk_size,
                               &measured_rate)) {
    if (sample_rate_watcher_get_stop_on_rate_change(loop->rate_watcher)) {
      logger_warn(&g_logger,
                  "Sample rate change detected (measured: %f Hz, expected: %zu "
                  "Hz); stopping engine",
                  measured_rate, loop->samplerate);
      processing_stop_reason_t reason = {
          .type = STOP_REASON_CAPTURE_FORMAT_CHANGE,
          .format_change_rate = (int)(measured_rate + 0.5)};
      engine_shared_state_request_stop(loop->shared, reason);
      return true;
    } else {
      logger_info(
          &g_logger,
          "Sample rate drift detected (measured: %f Hz, expected: %zu Hz)",
          measured_rate, loop->samplerate);
    }
  }

  // DoP (DSD over PCM) Decoding:
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

  // Ref: engine_state_management.md - Section 3.3: Silence Auto-Pause & Resume Flow
  // Step 1-2 (Auto-Pause) & Step 3 (Auto-Resume): Set engine state and toggle
  // capture/playback hardware backends is_paused status accordingly.
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

  // Enqueue Captured Chunk:
  // Push the chunk pointer into the bounded lock-free SPSC queue.
  // - Physical/Real-time hardware capture: if queue is full, incoming signal is
  // lost anyway.
  //   Increment drop counter, log warning, and continue without blocking or
  //   spinning.
  // - Non-real-time capture (File/Generator): sleep with nanosleep while
  // waiting for queue space
  //   so no samples are missed and CPU isn't consumed by spin loops.
  if (engine_shared_state_get_state(loop->shared) != PROCESSING_STATE_PAUSED) {
    if (capture_backend_is_realtime(loop->capture)) {
      if (!engine_shared_state_enqueue_captured(loop->shared, chunk)) {
        loop->captured_drop_counter++;
        logger_warn(&g_logger, "Captured chunk dropped (queue full)");
      }
    } else {
      while (!engine_shared_state_enqueue_captured(loop->shared, chunk)) {
        if (engine_shared_state_should_stop(loop->shared)) {
          break;
        }
        cdsp_sleep_ms(1);
      }
    }
  }
  return false;
}

void engine_capture_loop_run(engine_capture_loop_t* loop) {
  if (!loop) return;
  logger_info(&g_logger, "Capture thread started (realtime: %s)",
              capture_backend_is_realtime(loop->capture) ? "yes" : "no");

  backend_error_t berr;
  backend_error_init(&berr, BACKEND_ERROR_NONE, "");
  // Ref: engine_state_management.md - Section 3.1: Startup & Initialization Flow
  // Step 3: Capture Loop opens the capture device backend asynchronously.
  if (!capture_backend_open(loop->capture, &berr)) {
    logger_error(&g_logger, "Capture thread failed to open capture backend: %s",
                 berr.message);
    processing_stop_reason_t reason = {
        .type = STOP_REASON_CAPTURE_ERROR,
        .format_change_rate = 0,
    };
    snprintf(reason.message, sizeof(reason.message), "%s", berr.message);
    engine_shared_state_request_stop(loop->shared, reason);
    if (loop->shared) {
      engine_shared_state_shutdown_captured_queue(loop->shared);
    }
    return;
  }

  // Ref: engine_state_management.md - Section 3.1: Startup & Initialization Flow
  // Step 4: Once capture open succeeds, transition the state_raw state to RUNNING.
  if (engine_shared_state_get_state(loop->shared) ==
      PROCESSING_STATE_STARTING) {
    engine_shared_state_set_state(loop->shared, PROCESSING_STATE_RUNNING);
  }

  set_realtime_thread_priority("Capture", loop->chunk_size, loop->samplerate);
  sample_rate_watcher_reset(loop->rate_watcher);
  loop->watchdog_last_success_ns = cdsp_time_now_ns();

  while (1) {
    if (engine_shared_state_should_stop(loop->shared)) {
      break;
    }

    if (engine_shared_state_get_state(loop->shared) ==
        PROCESSING_STATE_PAUSED) {
      sample_rate_watcher_reset(loop->rate_watcher);

      // Ref: engine_state_management.md - Section 3.3: Silence Auto-Pause & Resume Flow
      // Step 2: Periodic 0-Frame Ticks are enqueued downstream every 200ms during pause
      // to wake up processing loop for pending pipeline swaps.
      // This wakes up the processing loop thread from its blocking dequeue wait,
      // allowing configuration hot-reloads and parameter updates (e.g. volume/mute)
      // to execute and apply immediately instead of being delayed indefinitely until
      // audio signal resumes. Waking up at 5Hz (200ms) consumes negligible CPU.
      uint64_t now = cdsp_time_now_ns();
      if (now - loop->last_paused_tick_ns > 200000000ULL) { // 200ms
        loop->last_paused_tick_ns = now;
        audio_chunk_t* empty_chunk = round_robin_chunk_pool_next(loop->chunk_pool);
        audio_chunk_set_valid_frames(empty_chunk, 0);
        engine_shared_state_enqueue_captured(loop->shared, empty_chunk);
      }
    }

    // 1. Hardware Sample-Rate Change Check
    if (capture_loop_check_format_change(loop)) {
      break;
    }

    // 2. Fetch a chunk buffer from the pre-allocated round-robin pool.
    audio_chunk_t* chunk = round_robin_chunk_pool_next(loop->chunk_pool);
    backend_error_t err;
    backend_error_init(&err, BACKEND_ERROR_NONE, "");

    // 3. Read raw PCM/DSD frame data from the capture backend.
    bool got_data =
        capture_backend_read(loop->capture, loop->chunk_size, chunk, &err);
    if (!got_data) {
      if (capture_loop_handle_no_data(loop, &err)) {
        break;
      }
      continue;
    }

    // 4. Process metering, DoP decode, silence gate, and push to SPSC queue.
    if (capture_loop_process_and_enqueue(loop, chunk)) {
      break;
    }
  }

  if (loop->shared) {
    engine_shared_state_shutdown_captured_queue(loop->shared);
  }
  if (loop->capture) {
    capture_backend_close(loop->capture);
  }
  if (loop->captured_drop_counter > 0) {
    logger_warn(&g_logger,
                "Capture thread stopped. Total dropped captured chunks: %llu",
                (unsigned long long)loop->captured_drop_counter);
  } else {
    logger_info(&g_logger, "Capture thread stopped");
  }
}
