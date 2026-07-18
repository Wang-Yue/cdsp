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

#include "DoP/dsd_encoder.h"
#include "Logging/app_logger.h"
#include "Utils/cdsp_time.h"
#include "thread_priority.h"

static const logger_t g_logger = {"dsp.playback"};

struct engine_playback_loop {
  engine_shared_state_t* shared;
  capture_backend_t* capture;
  playback_backend_t* playback;
  processing_parameters_t* processing_params;
  dsd_encoder_t* dsd_encoder;
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
    logger_debug(&g_logger, "Rate adjust: buffer=%f target=%d speed=%f via %s",
                 average, loop->target_level, speed, method_str);
  } else {
    logger_debug(&g_logger, "Rate adjust: buffer=%f, keeping speed=%f", average,
                 *last_speed);
  }
}

/**
 * @brief Logs the configured rate adjustment mode and parameters.
 *
 * @param loop Pointer to the playback loop structure.
 */
static void log_rate_adjust_mode(engine_playback_loop_t* loop) {
  if (loop->rate_adjust_enabled) {
    const char* method_str =
        loop->pitch_supported ? "capture clock pitch" : "resampler ratio";
    logger_info(
        &g_logger,
        "Rate adjustment enabled (period=%fs, target_level=%d, method=%s)",
        loop->adjust_period, loop->target_level, method_str);
  } else {
    logger_info(
        &g_logger,
        "Rate adjustment disabled (enable_rate_adjust not set in config)");
  }
}

engine_playback_loop_t* engine_playback_loop_create(
    const engine_playback_loop_config_t* config) {
  if (!config) return NULL;

  engine_playback_loop_t* loop =
      (engine_playback_loop_t*)calloc(1, sizeof(engine_playback_loop_t));
  if (!loop) return NULL;
  loop->shared = config->shared;
  loop->capture = config->capture;
  loop->playback = config->playback;
  loop->processing_params = config->processing_params;
  loop->dsd_encoder = config->dsd_encoder;
  loop->pipeline_rate = config->pipeline_rate;
  loop->chunk_size = config->chunk_size;
  loop->pitch_supported =
      (config->capture &&
       capture_backend_pitch_control_supported(config->capture)) ||
      (config->playback &&
       playback_backend_pitch_control_supported(config->playback));
  loop->rate_adjust_enabled = config->rate_adjust_enabled;
  loop->adjust_period = config->adjust_period;
  loop->target_level = config->target_level;
  return loop;
}

void engine_playback_loop_free(engine_playback_loop_t* loop) {
  if (!loop) return;
  free(loop);
}

/**
 * @brief Checks if the playback backend has reported a pending hardware sample
 * rate change.
 *
 * @param loop Pointer to the playback loop context.
 * @return true if a format change occurred and an engine stop was requested,
 * false otherwise.
 */
static bool playback_loop_check_format_change(engine_playback_loop_t* loop) {
  double rate = 0.0;
  if (playback_backend_get_pending_rate_change(loop->playback, &rate)) {
    if (!loop->has_last_observed_playback_pending_rate ||
        rate != loop->last_observed_playback_pending_rate) {
      loop->last_observed_playback_pending_rate = rate;
      loop->has_last_observed_playback_pending_rate = true;
      logger_warn(&g_logger,
                  "Playback device rate changed to %f Hz; stopping engine",
                  rate);
      processing_stop_reason_t reason = {
          .type = STOP_REASON_PLAYBACK_FORMAT_CHANGE,
          .format_change_rate = (int)(rate + 0.5)};
      engine_shared_state_request_stop(loop->shared, reason);
      return true;
    }
  }
  return false;
}

/**
 * @brief Calculates the total buffer level and updates the PI rate controller.
 *
 * Combines the hardware playback ring buffer level with the SPSC processed
 * queue frames to compute total buffer fill, then triggers periodic rate
 * adjustment.
 *
 * @param loop Pointer to the playback loop context.
 * @param rate_controller PI rate controller instance.
 * @param averager Averager buffer level accumulator.
 * @param stopwatch Period timer for controller execution.
 * @param last_speed Pointer to last applied speed ratio.
 */
static void playback_loop_update_rate_adjust(
    engine_playback_loop_t* loop, pi_rate_controller_t* rate_controller,
    averager_t* averager, stopwatch_t* stopwatch, double* last_speed) {
  // Calculate total buffer level: frames in hardware playback buffer plus
  // processed queue frames (matching upstream CamillaDSP).
  size_t ring_fill = playback_backend_get_buffer_level(loop->playback);
  size_t processed_queued =
      spsc_queue_get_count(
          engine_shared_state_get_processed_queue(loop->shared)) *
      loop->chunk_size;
  double total_buffer_fill = (double)(ring_fill + processed_queued);
  processing_parameters_set_buffer_level(loop->processing_params,
                                         total_buffer_fill);

  if (loop->rate_adjust_enabled && rate_controller) {
    averager_add(averager, total_buffer_fill);

    // Only run the PI controller periodically to avoid rapid fluctuations
    // and allow the physical/resampler adjustments to take effect.
    if (stopwatch_elapsed_seconds(stopwatch) >= loop->adjust_period) {
      double avg = 0.0;
      if (averager_get_average(averager, &avg)) {
        double speed = pi_rate_controller_next(rate_controller, avg);
        stopwatch_restart(stopwatch);
        averager_restart(averager);
        apply_speed(loop, speed, last_speed, avg);
        processing_parameters_set_rate_adjust(loop->processing_params, speed);
      }
    }
  }
}

/**
 * @brief Drains remaining audio frames from the hardware playback buffer prior
 * to exit.
 *
 * Polling level until buffer is 0 or a 3-second timeout occurs.
 *
 * @param loop Pointer to the playback loop context.
 */
static void playback_loop_drain_hardware_buffer(engine_playback_loop_t* loop) {
  logger_info(&g_logger, "Draining playback hardware buffer...");
  size_t last_level = 0;
  uint64_t last_change_ns = cdsp_time_now_ns();

  while (1) {
    if (engine_shared_state_should_stop(loop->shared)) {
      logger_info(&g_logger,
                  "Playback hardware buffer drain aborted due to stop request");
      break;
    }

    size_t level = playback_backend_get_buffer_level(loop->playback);
    if (level == 0) {
      break;
    }

    if (level != last_level) {
      last_level = level;
      last_change_ns = cdsp_time_now_ns();
    } else {
      uint64_t now_ns = cdsp_time_now_ns();
      double elapsed = (double)(now_ns - last_change_ns) / 1000000000.0;
      if (elapsed > 3.0) {
        logger_warn(&g_logger, "Playback drain timeout reached; aborting");
        break;
      }
    }

    cdsp_sleep_ms(10);
  }
  logger_info(&g_logger, "Playback hardware buffer drained");
}

void engine_playback_loop_run(engine_playback_loop_t* loop) {
  if (!loop) return;
  logger_info(&g_logger, "Playback thread started");

  backend_error_t berr;
  backend_error_init(&berr, BACKEND_ERROR_NONE, "");
  // Ref: engine_state_management.md - Section 3.1: Startup & Initialization Flow
  // Step 9: Playback Loop opens the playback device backend asynchronously.
  if (!playback_backend_open(loop->playback, &berr)) {
    logger_error(&g_logger,
                 "Playback thread failed to open playback backend: %s",
                 berr.message);
    processing_stop_reason_t reason = {
        .type = STOP_REASON_PLAYBACK_ERROR,
        .format_change_rate = 0,
    };
    snprintf(reason.message, sizeof(reason.message), "%s", berr.message);
    engine_shared_state_request_stop(loop->shared, reason);
    return;
  }

  // Ref: engine_state_management.md - Section 3.1: Startup & Initialization Flow
  // Step 9: Pre-fill hardware DAC buffer with silence frames asynchronously.
  // Prefill playback silence to feed the DAC immediately on start,
  // preventing immediate buffer underrun errors. If rate adjust is enabled,
  // we match its target level; otherwise, we pre-fill chunk_size.
  size_t prefill_frames =
      loop->target_level > 0 ? (size_t)loop->target_level : loop->chunk_size;
  if (loop->dsd_encoder && dsd_encoder_is_enabled(loop->dsd_encoder)) {
    size_t channels =
        processing_parameters_get_playback_channels(loop->processing_params);
    audio_chunk_t* prefill_chunk = audio_chunk_create(prefill_frames, channels);
    if (prefill_chunk) {
      dsd_encoder_fill_silence(loop->dsd_encoder, prefill_chunk);
      playback_backend_write(loop->playback, prefill_chunk, &berr);
      audio_chunk_free(prefill_chunk);
    }
  } else {
    playback_backend_prefill_silence(loop->playback, prefill_frames, &berr);
  }

  if (berr.type != BACKEND_ERROR_NONE) {
    logger_error(&g_logger,
                 "Playback thread failed to prefill hardware silence: %s",
                 berr.message);
    processing_stop_reason_t reason = {
        .type = STOP_REASON_PLAYBACK_ERROR,
        .format_change_rate = 0,
    };
    snprintf(reason.message, sizeof(reason.message), "%s", berr.message);
    engine_shared_state_request_stop(loop->shared, reason);
    return;
  }

  set_realtime_thread_priority("Playback", loop->chunk_size,
                               loop->pipeline_rate);
  log_rate_adjust_mode(loop);

  double last_speed = 1.0;
  pi_rate_controller_t* rate_controller = NULL;
  averager_t averager = {0};
  stopwatch_t stopwatch = {0};

  if (loop->rate_adjust_enabled) {
    rate_controller = pi_rate_controller_create_default(
        (int)loop->pipeline_rate, loop->adjust_period, loop->target_level);
    averager_init(&averager);
    stopwatch_init(&stopwatch);
    stopwatch_restart(&stopwatch);
  }

  bool reached_eos = true;
  audio_chunk_t* chunk = NULL;
  bool was_paused = false;
  // Ref: engine_state_management.md - Section 3.2: Steady-State Audio Loops & Section 3.6: Immediate Abort Teardown
  // Dequeue chunks from processed_queue. Blocks on processed semaphore if queue is empty.
  while ((chunk = engine_shared_state_dequeue_processed_blocking(
              loop->shared)) != NULL) {
    if (engine_shared_state_should_stop(loop->shared)) {
      reached_eos = false;
      break;
    }

    bool is_paused = playback_backend_get_is_paused(loop->playback);
    if (was_paused && !is_paused) {
      // Ref: engine_state_management.md - Section 3.3: Silence Auto-Pause & Resume Flow
      // Step 3: Reset PI rate controller stopwatch and averager when auto-resuming from pause
      // to prevent resampler ratio and pitch speed glitches caused by wall-clock time accumulation.
      if (loop->rate_adjust_enabled) {
        averager_restart(&averager);
        stopwatch_restart(&stopwatch);
      }
      logger_info(
          &g_logger,
          "Playback auto-resumed from pause; reset rate adjust timer and averager");
    }
    was_paused = is_paused;

    size_t frames = audio_chunk_get_valid_frames(chunk);
    // Ref: engine_state_management.md - Section 3.3: Silence Auto-Pause & Resume Flow
    // Step 2: Ignore 0-frame control/tick chunks. Drop them immediately to bypass writing to hardware.
    if (frames == 0) {
      continue;
    }

    // 1. Hardware Sample-Rate Change Check
    if (playback_loop_check_format_change(loop)) {
      reached_eos = false;
      break;
    }

    // 2. Buffer fill level calculation and PI rate adjustment
    playback_loop_update_rate_adjust(loop, rate_controller, &averager,
                                     &stopwatch, &last_speed);

    // 3. Write PCM chunk to physical audio output backend
    backend_error_t err;
    backend_error_init(&err, BACKEND_ERROR_NONE, "");
    bool ok = playback_backend_write(loop->playback, chunk, &err);
    if (!ok || err.type != BACKEND_ERROR_NONE) {
      // Ref: engine_state_management.md - Section 3.6: Immediate Abort Teardown
      // Step 1: Playback thread detects a hardware write error. Requests stop with PLAYBACK_ERROR,
      // which immediately transitions state to INACTIVE and shuts down both queues.
      logger_error(&g_logger, "Playback error: %s", err.message);
      processing_stop_reason_t reason = {.type = STOP_REASON_PLAYBACK_ERROR};
      snprintf(reason.message, sizeof(reason.message), "%s", err.message);
      engine_shared_state_request_stop(loop->shared, reason);
      reached_eos = false;
      break;
    }
  }

  // 4. Drain the hardware playback buffer before exiting if stream ended
  // normally and device is not paused (aligning with CamillaDSP draining
  // behavior).
  bool is_paused = playback_backend_get_is_paused(loop->playback);
  if (reached_eos && !is_paused &&
      !engine_shared_state_should_stop(loop->shared)) {
    // Ref: engine_state_management.md - Section 3.5: Graceful EOF Teardown (Queue Drain)
    // Step 3: Playback loop runs playback_loop_drain_hardware_buffer to wait for the DAC buffer to hit 0.
    playback_loop_drain_hardware_buffer(loop);
  } else {
    logger_info(&g_logger,
                "Skipping playback hardware buffer drain (eos=%d, paused=%d)",
                reached_eos, is_paused);
  }

  if (rate_controller) pi_rate_controller_free(rate_controller);
  if (loop->shared) {
    // Ref: engine_state_management.md - Section 3.5: Graceful EOF Teardown (Queue Drain)
    // Step 3: Sets state to INACTIVE via engine_shared_state_set_state(state, INACTIVE).
    engine_shared_state_set_state(loop->shared, PROCESSING_STATE_INACTIVE);
  }
  if (loop->playback) {
    playback_backend_close(loop->playback);
  }
  logger_info(&g_logger, "Playback thread stopped");
}
