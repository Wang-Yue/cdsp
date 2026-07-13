// Playback thread body. Drains the processing→playback SPSC queue
// and writes each chunk to the playback backend. Also runs the
// rate-adjust control loop: averages the (device-ring + queued-chunks)
// fill level, and once per `adjustPeriod` seconds feeds the average
// to `PIRateController`.
//
// State ownership
// ---------------
// The rate-adjust state — controller, averager, stopwatch, last
// published speed — is local to this loop. The output speed is
// applied either directly to the capture clock (when the capture
// device exposes a tunable clock — BlackHole 0.5.0+) or published
// via `shared.resamplerRatio` so the processing thread picks it up
// on its next chunk.
//
// Audio-thread invariants
// -----------------------
//   * No allocations in the steady state. The controller and
//     averager are constructed once at init; the stopwatch is a
//     plain UInt64 nanosecond timestamp.
//   * No locks. The shared SPSC queue + semaphore carries chunks
//     and wakeups.
//   * The rate-adjust info logger fires at most once per
//     `adjustPeriod` (~10 s default), so its formatting cost is
//     negligible per chunk.

#include "engine_playback_loop.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "Logging/app_logger.h"
#include "thread_priority.h"

struct engine_playback_loop {
  engine_shared_state_t* shared;
  capture_backend_t* capture;
  playback_backend_t* playback;
  processing_parameters_t* processing_params;
  size_t pipeline_rate;
  size_t chunk_size;
  bool pitch_supported;
  bool rate_adjust_enabled;
  double adjust_period;
  int target_level;
  bool has_last_observed_playback_pending_rate;
  double last_observed_playback_pending_rate;
};

/**
 * @brief Applies the calculated speed adjustment to the audio device or
 * resampler.
 *
 * This function compares the new speed with the last applied speed. If the
 * change is greater than 1 ppm (part per million), it updates the speed.
 * Depending on backend capabilities, it sets the pitch on the capture backend,
 * the playback backend, or updates the resampler ratio in the shared state.
 *
 * @param loop Pointer to the playback loop structure.
 * @param speed The new speed ratio to apply.
 * @param last_speed Pointer to the last applied speed ratio (updated if
 * changed).
 * @param average The average buffer level, used for logging.
 */
static void apply_speed(engine_playback_loop_t* loop, double speed,
                        double* last_speed, double average) {
  bool changed = fabs(speed - *last_speed) > 0.000001;
  logger_t logger = logger_create("dsp.playback");
  if (changed) {
    *last_speed = speed;
    if (loop->capture &&
        capture_backend_pitch_control_supported(loop->capture)) {
      capture_backend_set_pitch(loop->capture, speed);
    } else if (loop->playback &&
               playback_backend_pitch_control_supported(loop->playback)) {
      playback_backend_set_pitch(loop->playback, speed);
    } else if (loop->shared) {
      engine_shared_state_set_resampler_ratio(loop->shared, speed);
    }
    const char* method_str = loop->pitch_supported ? "pitch" : "resampler";
    logger_debug(&logger, "Rate adjust: buffer=%f target=%d speed=%f via %s",
                 average, loop->target_level, speed, method_str);
  } else {
    logger_debug(&logger, "Rate adjust: buffer=%f, keeping speed=%f", average,
                 *last_speed);
  }
}

/**
 * @brief Logs the configured rate adjustment mode and parameters.
 *
 * @param loop Pointer to the playback loop structure.
 */
static void log_rate_adjust_mode(engine_playback_loop_t* loop) {
  logger_t logger = logger_create("dsp.playback");
  if (loop->rate_adjust_enabled) {
    const char* method_str =
        loop->pitch_supported ? "capture clock pitch" : "resampler ratio";
    logger_info(
        &logger,
        "Rate adjustment enabled (period=%fs, target_level=%d, method=%s)",
        loop->adjust_period, loop->target_level, method_str);
  } else {
    logger_info(
        &logger,
        "Rate adjustment disabled (enable_rate_adjust not set in config)");
  }
}

engine_playback_loop_t* engine_playback_loop_create(
    engine_shared_state_t* shared, capture_backend_t* capture,
    playback_backend_t* playback, processing_parameters_t* processing_params,
    size_t pipeline_rate, size_t chunk_size, bool rate_adjust_enabled,
    double adjust_period, int target_level) {
  engine_playback_loop_t* loop =
      (engine_playback_loop_t*)calloc(1, sizeof(engine_playback_loop_t));
  if (!loop) return NULL;
  loop->shared = shared;
  loop->capture = capture;
  loop->playback = playback;
  loop->processing_params = processing_params;
  loop->pipeline_rate = pipeline_rate;
  loop->chunk_size = chunk_size;
  loop->pitch_supported =
      (capture && capture_backend_pitch_control_supported(capture)) ||
      (playback && playback_backend_pitch_control_supported(playback));
  loop->rate_adjust_enabled = rate_adjust_enabled;
  loop->adjust_period = adjust_period;
  loop->target_level = target_level;
  return loop;
}

void engine_playback_loop_free(engine_playback_loop_t* loop) {
  if (!loop) return;
  free(loop);
}

void engine_playback_loop_run(engine_playback_loop_t* loop) {
  if (!loop) return;
  logger_t logger = logger_create("dsp.playback");
  logger_info(&logger, "Playback thread started");

  set_realtime_thread_priority("Playback", loop->chunk_size,
                               loop->pipeline_rate);
  log_rate_adjust_mode(loop);

  // Rate-adjust state lives entirely on this thread.
  pi_rate_controller_t* rate_controller = NULL;
  averager_t averager;
  stopwatch_t stopwatch;
  double last_speed = 1.0;

  if (loop->rate_adjust_enabled) {
    rate_controller = pi_rate_controller_create_default(
        (int)loop->pipeline_rate, loop->adjust_period, loop->target_level);
    averager_init(&averager);
    stopwatch_init(&stopwatch);
    stopwatch_restart(&stopwatch);
  }

  bool reached_eos = true;
  audio_chunk_t* chunk = NULL;
  while ((chunk = engine_shared_state_dequeue_processed_blocking(
              loop->shared)) != NULL) {
    double rate = 0.0;
    if (playback_backend_get_pending_rate_change(loop->playback, &rate)) {
      if (!loop->has_last_observed_playback_pending_rate ||
          rate != loop->last_observed_playback_pending_rate) {
        loop->last_observed_playback_pending_rate = rate;
        loop->has_last_observed_playback_pending_rate = true;
        logger_warn(&logger,
                    "Playback device rate changed to %f Hz; stopping engine",
                    rate);
        processing_stop_reason_t reason = {
            .type = STOP_REASON_PLAYBACK_FORMAT_CHANGE,
            .format_change_rate = (int)(rate + 0.5)};
        engine_shared_state_request_stop(loop->shared, reason);
        reached_eos = false;
        break;
      }
    }

    // Calculate total buffer level: frames in the hardware playback buffer
    // plus frames currently queued in the SPSC queue waiting to be written.
    size_t ring_fill = playback_backend_get_buffer_level(loop->playback);
    size_t queued_frames =
        spsc_queue_get_count(
            engine_shared_state_get_processed_queue(loop->shared)) *
        loop->chunk_size;
    processing_parameters_set_buffer_level(loop->processing_params,
                                           (double)(ring_fill + queued_frames));

    if (loop->rate_adjust_enabled && rate_controller) {
      averager_add(&averager, (double)(ring_fill + queued_frames));

      // Only run the PI controller periodically to avoid rapid fluctuations
      // and allow the physical/resampler adjustments to take effect.
      if (stopwatch_elapsed_seconds(&stopwatch) >= loop->adjust_period) {
        double avg = 0.0;
        if (averager_get_average(&averager, &avg)) {
          double speed = pi_rate_controller_next(rate_controller, avg);
          stopwatch_restart(&stopwatch);
          averager_restart(&averager);
          apply_speed(loop, speed, &last_speed, avg);
          processing_parameters_set_rate_adjust(loop->processing_params, speed);
        }
      }
    }

    backend_error_t err;
    backend_error_init(&err, BACKEND_ERROR_NONE, "");
    bool ok = playback_backend_write(loop->playback, chunk, &err);
    if (!ok || err.type != BACKEND_ERROR_NONE) {
      logger_error(&logger, "Playback error: %s", err.message);
      processing_stop_reason_t reason = {.type = STOP_REASON_PLAYBACK_ERROR};
      snprintf(reason.message, sizeof(reason.message), "%s", err.message);
      engine_shared_state_request_stop(loop->shared, reason);
      reached_eos = false;
      break;
    }
  }

  // Drain the hardware playback buffer before exiting if stream ended normally
  // and device is not paused (aligning with CamillaDSP draining behavior).
  bool is_paused = playback_backend_get_is_paused(loop->playback);
  if (reached_eos && !is_paused) {
    logger_info(&logger, "Draining playback hardware buffer...");
    size_t last_level = 0;
    struct timespec last_change_ts;
    clock_gettime(CLOCK_MONOTONIC, &last_change_ts);

    while (1) {
      size_t level = playback_backend_get_buffer_level(loop->playback);
      if (level == 0) {
        break;
      }

      if (level != last_level) {
        last_level = level;
        clock_gettime(CLOCK_MONOTONIC, &last_change_ts);
      } else {
        struct timespec now_ts;
        clock_gettime(CLOCK_MONOTONIC, &now_ts);
        double elapsed =
            (double)(now_ts.tv_sec - last_change_ts.tv_sec) +
            (double)(now_ts.tv_nsec - last_change_ts.tv_nsec) / 1000000000.0;
        if (elapsed > 3.0) {
          logger_warn(&logger, "Playback drain timeout reached; aborting");
          break;
        }
      }

#ifdef _WIN32
      Sleep(10);
#else
      struct timespec req = {.tv_sec = 0, .tv_nsec = 10000000ULL};  // 10ms
      nanosleep(&req, NULL);
#endif
    }
    logger_info(&logger, "Playback hardware buffer drained");
  } else {
    logger_info(&logger,
                "Skipping playback hardware buffer drain (eos=%d, paused=%d)",
                reached_eos, is_paused);
  }

  if (rate_controller) pi_rate_controller_free(rate_controller);
  logger_info(&logger, "Playback thread stopped");
}
