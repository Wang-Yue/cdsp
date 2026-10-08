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
#include "engine/engine_capture_loop.h"

#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

#include "audio/audio_chunk.h"
#include "audio/processing_parameters.h"
#include "audio/sample_format.h"
#include "audio/silence_counter.h"
#include "backend/audio_backend.h"
#include "backend/backend_error.h"
#include "dsd/dsd_decoder.h"
#include "engine/engine_state_types.h"
#include "engine/sample_rate_watcher.h"

struct engine_capture_loop {
  engine_shared_state_t *shared;
  capture_backend_t *capture;
  processing_parameters_t *processing_params;
  dsd_decoder_t *dsd_decoder;
  resampler_t *resampler;
  audio_chunk_t *raw_chunk;

  size_t chunk_size;
  size_t pipeline_chunk_size;
  size_t channels;
  size_t samplerate;
  size_t pipeline_rate;
  _Atomic bool *used_channels;
  bool *local_channel_mask;
  // True once a real used-channel mask has been supplied (config or
  // engine_capture_loop_set_used_channels). Until then raw_chunk keeps an
  // all-true mask, equivalent to "all channels active".
  _Atomic bool has_used_channels;

  silence_counter_t *silence_counter;
  round_robin_chunk_pool_t *chunk_pool;

  audio_chunk_t *pending_chunk;

  sample_rate_watcher_t *rate_watcher;
  uint64_t captured_drop_counter;
  uint64_t last_paused_tick_ns;

  // Re-read after capture_backend_open (CoreAudio only knows after open
  // whether the device exposes a clock-source pitch control, 03 CA-01).
  // Capture-thread only; published to the playback loop through
  // engine_shared_state_set_capture_pitch_supported().
  bool pitch_supported;
  double last_applied_pitch;
  size_t resampler_overloaded_chunks;
};
#include <stdlib.h>

#include "engine/thread_priority.h"
#include "logging/app_logger.h"
#include "utils/cdsp_time.h"

static const logger_t g_logger = {"dsp.capture"};

engine_capture_loop_t *
engine_capture_loop_create(const engine_capture_loop_config_t *config) {
  if (!config)
    return NULL;

  engine_capture_loop_t *loop =
      (engine_capture_loop_t *)calloc(1, sizeof(engine_capture_loop_t));
  if (!loop)
    return NULL;

  loop->shared = config->shared;
  loop->capture = config->capture;
  loop->processing_params = config->processing_params;
  loop->dsd_decoder = config->dsd_decoder;
  loop->chunk_pool = config->chunk_pool;
  loop->resampler = config->resampler;
  loop->chunk_size = config->chunk_size;
  loop->pipeline_chunk_size = config->pipeline_chunk_size > 0
                                  ? config->pipeline_chunk_size
                                  : config->chunk_size;
  loop->pipeline_rate =
      config->pipeline_rate > 0 ? config->pipeline_rate : config->samplerate;
  loop->channels = config->channels;
  loop->samplerate = config->samplerate;
  if (config->channels > 0) {
    loop->used_channels =
        (_Atomic bool *)calloc(config->channels, sizeof(_Atomic bool));
    if (loop->used_channels && config->used_channels) {
      for (size_t i = 0; i < config->channels; i++) {
        atomic_init(&loop->used_channels[i], config->used_channels[i]);
      }
    }
    atomic_init(&loop->has_used_channels, config->used_channels != NULL);
    loop->local_channel_mask = (bool *)calloc(config->channels, sizeof(bool));
  }
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

  if (loop->resampler) {
    size_t max_in = resampler_get_max_input_frames(loop->resampler);
    if (max_in < config->chunk_size) {
      max_in = config->chunk_size;
    }
    loop->raw_chunk = audio_chunk_create(max_in, config->channels);
    if (!loop->raw_chunk) {
      engine_capture_loop_free(loop);
      return NULL;
    }
    // Pre-allocate raw_chunk's used-channel mask (all active) so the hot path
    // can refresh it in place (memcpy, no allocation) and the resampler can
    // skip inactive channels (upstream resampling.rs update_channel_mask).
    if (loop->local_channel_mask) {
      for (size_t i = 0; i < config->channels; i++) {
        loop->local_channel_mask[i] = true;
      }
      audio_chunk_set_used_channels(loop->raw_chunk, loop->local_channel_mask);
      if (!audio_chunk_get_used_channels(loop->raw_chunk)) {
        engine_capture_loop_free(loop);
        return NULL;
      }
      for (size_t i = 0; i < config->channels; i++) {
        loop->local_channel_mask[i] = false;
      }
    }
  }

  loop->pending_chunk = NULL;
  loop->captured_drop_counter = 0;
  loop->last_paused_tick_ns = 0;
  loop->pitch_supported =
      config->capture ? capture_backend_pitch_control_supported(config->capture)
                      : false;
  engine_shared_state_set_capture_pitch_supported(loop->shared,
                                                  loop->pitch_supported);
  loop->last_applied_pitch = 1.0;

  return loop;
}

void engine_capture_loop_free(engine_capture_loop_t *loop) {
  if (!loop)
    return;
  if (loop->silence_counter) {
    silence_counter_free(loop->silence_counter);
  }
  if (loop->rate_watcher) {
    sample_rate_watcher_free(loop->rate_watcher);
  }
  if (loop->used_channels) {
    free(loop->used_channels);
  }
  if (loop->local_channel_mask) {
    free(loop->local_channel_mask);
  }
  if (loop->raw_chunk) {
    audio_chunk_free(loop->raw_chunk);
  }
  free(loop);
}

void engine_capture_loop_set_used_channels(engine_capture_loop_t *loop,
                                           const bool *used_channels) {
  if (!loop || !used_channels || loop->channels == 0)
    return;
  if (!loop->used_channels) {
    loop->used_channels =
        (_Atomic bool *)calloc(loop->channels, sizeof(_Atomic bool));
  }
  if (loop->used_channels) {
    for (size_t i = 0; i < loop->channels; i++) {
      atomic_store_explicit(&loop->used_channels[i], used_channels[i],
                            memory_order_relaxed);
    }
    atomic_store_explicit(&loop->has_used_channels, true, memory_order_release);
  }
}

/**
 * @brief Checks if the capture hardware backend has reported an unexpected
 * sample rate change.
 *
 * @param loop Pointer to the capture loop context.
 * @return true if a format change occurred and an engine stop was requested,
 * false otherwise.
 */
static bool capture_loop_check_format_change(engine_capture_loop_t *loop) {
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
 * @brief Checks if a periodic 0-frame control tick is due while in PAUSED state
 * and enqueues it downstream to wake up the processing loop for pending
 * pipeline swaps.
 */
static void capture_loop_send_paused_tick_if_due(engine_capture_loop_t *loop) {
  if (engine_shared_state_get_state(loop->shared) != PROCESSING_STATE_PAUSED) {
    return;
  }

  // Ref: docs/engine_state_management.md - Section 3.3: Silence Auto-Pause &
  // Resume Flow Step 2: Periodic 0-Frame Ticks are enqueued downstream every
  // 200ms during pause to wake up processing loop for pending pipeline swaps.
  // This wakes up the processing loop thread from its blocking dequeue wait,
  // allowing configuration hot-reloads and parameter updates (e.g. volume/mute)
  // to execute and apply immediately instead of being delayed indefinitely
  // until audio signal resumes. Waking up at 5Hz (200ms) consumes negligible
  // CPU.
  //
  // Ref: docs/engine_state_management.md - Section 3.3 (Buffer Retention):
  // While in PAUSED state, read chunks are retained in loop->pending_chunk
  // rather than repeatedly requesting fresh chunks from
  // round_robin_chunk_pool_next(). This guarantees that the pre-allocated
  // round-robin chunk pool does not advance and wrap around, protecting
  // in-flight queued buffers from concurrent data race overwrites.
  uint64_t now = cdsp_time_now_ns();
  if (now - loop->last_paused_tick_ns >= 200000000ULL) { // 200ms
    audio_chunk_t *tick_chunk = loop->pending_chunk;
    if (!tick_chunk) {
      tick_chunk = round_robin_chunk_pool_next(loop->chunk_pool);
    }
    audio_chunk_set_valid_frames(tick_chunk, 0);
    if (engine_shared_state_enqueue_captured(loop->shared, tick_chunk)) {
      loop->last_paused_tick_ns = now;
      loop->pending_chunk = NULL;
    } else {
      loop->pending_chunk = tick_chunk;
    }
  }
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
static bool capture_loop_handle_no_data(engine_capture_loop_t *loop,
                                        const backend_error_t *err) {
  if (err->type == BACKEND_ERROR_READ_EOF) {
    // Ref: docs/engine_state_management.md - Section 3.5: Graceful EOF Teardown
    // (Queue Drain) Step 1: Capture loop reaches EOF, requests stop with
    // STOP_REASON_DONE, shuts down the captured queue, and exits without
    // setting state to INACTIVE.
    logger_info(&g_logger,
                "Capture reached End-of-Stream; stopping engine gracefully");
    processing_stop_reason_t reason = {.type = STOP_REASON_DONE};
    snprintf(reason.message, sizeof(reason.message), "EOF");
    engine_shared_state_request_stop(loop->shared, reason);
    return true;
  }
  // If reading fails with an error, trigger an engine stop.
  if (err->type != BACKEND_ERROR_NONE) {
    // Ref: docs/engine_state_management.md - Section 4.1: Prevention of
    // False-Alarm Shutdown Errors (Loop Guards)
    if (engine_shared_state_should_stop(loop->shared)) {
      return true;
    }
    // Check if there is a pending rate change first (e.g. from service
    // invalidation/format changes)
    double rate = 0.0;
    if (capture_backend_get_pending_rate_change(loop->capture, &rate)) {
      if (fabs(rate - (double)loop->samplerate) >= 0.5) {
        logger_warn(&g_logger,
                    "Capture device rate changed to %f Hz during read error; "
                    "stopping engine",
                    rate);
        processing_stop_reason_t reason = {
            .type = STOP_REASON_CAPTURE_FORMAT_CHANGE,
            .format_change_rate = (int)(rate + 0.5)};
        engine_shared_state_request_stop(loop->shared, reason);
        return true;
      }
    }
    // Ref: docs/engine_state_management.md - Section 3.6: Immediate Abort
    // Teardown Step 1: Capture thread detects a hardware read error, requests
    // stop with CAPTURE_ERROR, which immediately transitions state to INACTIVE
    // and wakes all loops.
    logger_error(&g_logger, "Capture error: %s", err->message);
    processing_stop_reason_t reason = {.type = STOP_REASON_CAPTURE_ERROR};
    snprintf(reason.message, sizeof(reason.message), "%s", err->message);
    engine_shared_state_request_stop(loop->shared, reason);
    return true;
  }

  // If the engine is in a PAUSED state (no active input signal), send a
  // 0-frame tick if the 200ms periodic interval has elapsed. The stall
  // watchdog timestamp is deliberately NOT refreshed here: no data arrived,
  // and (as upstream) a device that stops delivering while paused must be
  // reported as STALLED.
  if (engine_shared_state_get_state(loop->shared) == PROCESSING_STATE_PAUSED) {
    capture_loop_send_paused_tick_if_due(loop);
    capture_backend_wait(loop->capture, 20);
    return false;
  }

  // Block/wait up to 20ms using the backend's synchronization mechanism (e.g.
  // semaphore). This yields CPU time while maintaining real-time scheduling
  // priority.
  capture_backend_wait(loop->capture, 20);
  return false;
}

/**
 * @brief Enqueues an active audio chunk to the captured queue in RUNNING state.
 */
static void capture_loop_enqueue_running_chunk(engine_capture_loop_t *loop,
                                               audio_chunk_t *chunk) {
  // Ref: docs/engine_state_management.md - Section 3.2 (Real-Time Bounded Queue
  // Drops) & Section 1.7.2 (Rule 5) Enqueue Captured Chunk: Push the chunk
  // pointer into the bounded lock-free SPSC queue.
  // - Physical/Real-time hardware capture: if queue is full, incoming signal is
  //   lost anyway. Increment drop counter and retain un-enqueued chunk in
  //   loop->pending_chunk to avoid round-robin pool index wrap-around from
  //   overwriting active in-flight queued buffers.
  // - Non-real-time capture (File/Generator): sleep with nanosleep while
  // waiting
  //   for queue space so no samples are missed and CPU isn't consumed by spin
  //   loops.
  if (capture_backend_is_realtime(loop->capture)) {
    if (!engine_shared_state_enqueue_captured(loop->shared, chunk)) {
      loop->captured_drop_counter++;
      static uint64_t last_drop_log = 0;
      uint64_t now = cdsp_time_now_ns() / 1000000;
      if (now - last_drop_log > 1000) {
        logger_warn(&g_logger, "Captured chunk dropped (queue full)");
        last_drop_log = now;
      }
      loop->pending_chunk = chunk;
    } else {
      loop->pending_chunk = NULL;
    }
  } else {
    while (!engine_shared_state_enqueue_captured(loop->shared, chunk)) {
      if (engine_shared_state_should_stop(loop->shared)) {
        break;
      }
      engine_shared_state_set_last_capture_time(loop->shared,
                                                cdsp_time_now_ns());
      cdsp_sleep_ms(1);
    }
    loop->pending_chunk = NULL;
  }
}

/**
 * @brief Synchronizes hardware clock pitch multiplier if supported.
 */
static void capture_loop_update_pitch(engine_capture_loop_t *loop) {
  // Clock pitch adjustment check:
  // If the capture backend supports hardware clock pitch tuning, sync to
  // the shared speed ratio published by the playback rate controller.
  if (loop->pitch_supported && loop->shared) {
    double desired_pitch = engine_shared_state_get_capture_pitch(loop->shared);
    if (fabs(desired_pitch - loop->last_applied_pitch) > 0.000001) {
      loop->last_applied_pitch = desired_pitch;
      capture_backend_set_pitch(loop->capture, desired_pitch);
    }
  }
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
static bool capture_loop_process_and_enqueue(engine_capture_loop_t *loop,
                                             audio_chunk_t *chunk) {
  // Ref: docs/engine_state_management.md - Section 3.4: Watchdog Stall &
  // Recovery Flow Step 1: Update shared last capture timestamp so the
  // main-thread watchdog check is satisfied.
  engine_shared_state_set_last_capture_time(loop->shared, cdsp_time_now_ns());

  // Step 2: Stall Recovery. If the main-thread watchdog previously marked us
  // STALLED, restore to RUNNING.
  if (engine_shared_state_get_state(loop->shared) == PROCESSING_STATE_STALLED) {
    engine_shared_state_set_state(loop->shared, PROCESSING_STATE_RUNNING);
    // Upstream resets measured_rate to 0 when the capture stalls, so the
    // stale pre-stall rate is not republished after recovery. Restarting the
    // measurement window also keeps the stall gap out of the next
    // measurement (which would otherwise read as a bogus low rate).
    sample_rate_watcher_reset(loop->rate_watcher);
    logger_info(&g_logger, "Capture recovered from stall");
  }

  // Rate Watcher Measurement:
  // Backends whose chunk timing is not a device clock (the generator) set
  // skip_rate_watcher; their "measured rate" would spuriously trip
  // stop_on_rate_change (06 F-08; upstream generator has no watcher).
  double measured_rate = 0.0;
  size_t valid_frames = audio_chunk_get_valid_frames(chunk);
  if (!loop->capture->skip_rate_watcher &&
      sample_rate_watcher_tick(loop->rate_watcher, valid_frames,
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

  if (loop->processing_params && loop->rate_watcher) {
    double current_measured =
        sample_rate_watcher_get_last_measured_rate(loop->rate_watcher);
    processing_parameters_set_measured_capture_rate(loop->processing_params,
                                                    current_measured);
  }

  // DSD (DoP / Native DSD) Decoding:
  // If DSD decoding is active, process the chunk to decode DSD back to
  // high-resolution PCM in-place. Decoding is done before metering so
  // RMS/Peak values reflect the actual signal instead of carrier noise.
  if (loop->dsd_decoder) {
    dsd_decoder_process(loop->dsd_decoder, chunk);
  }

  // Update level meters with the peak/rms of this chunk.
  processing_parameters_update_capture_levels(loop->processing_params, chunk);

  // Ref: docs/engine_state_management.md - Section 3.3: Silence Auto-Pause &
  // Resume Flow Step 1-2 (Auto-Pause) & Step 3 (Auto-Resume): Set engine state
  // and toggle capture hardware backend is_paused status accordingly.
  // Copy atomic used_channels into thread-local mask once per chunk without
  // locking.
  if (loop->used_channels && loop->local_channel_mask) {
    for (size_t i = 0; i < loop->channels; i++) {
      loop->local_channel_mask[i] =
          atomic_load_explicit(&loop->used_channels[i], memory_order_relaxed);
    }
  }
  // Hand the same mask to the resampler through raw_chunk's pre-allocated
  // used_channels (in-place memcpy, no allocation on the hot path).
  if (loop->resampler && chunk == loop->raw_chunk && loop->local_channel_mask &&
      audio_chunk_get_used_channels(loop->raw_chunk) &&
      atomic_load_explicit(&loop->has_used_channels, memory_order_acquire)) {
    audio_chunk_set_used_channels(loop->raw_chunk, loop->local_channel_mask);
  }
  float value_range = (float)audio_chunk_get_value_range_used(
      chunk, loop->local_channel_mask ? loop->local_channel_mask : NULL);
  if (loop->processing_params) {
    processing_parameters_set_signal_range(loop->processing_params,
                                           value_range);
  }
  // Chunks the backend flags as not silence-gated (file EOF extra_samples
  // tail, 06 F-10; the signal generator, 06 F-07) bypass the counter:
  // upstream sends them unconditionally, so a tail also resumes a paused
  // engine rather than being swallowed.
  processing_state_t desired =
      loop->capture->skip_silence_detection
          ? PROCESSING_STATE_RUNNING
          : silence_counter_update(loop->silence_counter, value_range);
  processing_state_t current = engine_shared_state_get_state(loop->shared);
  if (desired != current) {
    logger_debug(&g_logger, "%s processing (state: %d -> %d)",
                 desired == PROCESSING_STATE_PAUSED ? "Pausing" : "Resuming",
                 (int)current, (int)desired);
    engine_shared_state_set_state(loop->shared, desired);
    if (desired == PROCESSING_STATE_PAUSED && loop->processing_params) {
      processing_parameters_bump_pause_count(loop->processing_params);
    }
  }

  // Ref: docs/engine_state_management.md - Section 3.3 (Silence Auto-Pause &
  // Resume Flow) Enqueue chunk based on engine processing state:
  // - While PAUSED, retain the chunk in loop->pending_chunk (so the round-robin
  // chunk
  //   pool does not advance/wrap around) and emit 0-frame control ticks every
  //   200ms so processing_thread can unblock and process pending pipeline
  //   swaps.
  // - While RUNNING, push active audio chunks to captured_queue for downstream
  // processing.
  if (engine_shared_state_get_state(loop->shared) == PROCESSING_STATE_PAUSED) {
    if (!loop->resampler) {
      loop->pending_chunk = chunk;
    }
    capture_loop_send_paused_tick_if_due(loop);
    // A non-realtime source (file / stdin) is not paced by a device, so
    // while paused it would re-read and re-check as fast as the CPU allows.
    // Sleep for about one chunk of capture time, as upstream does
    // (file_backend/device.rs sleep_until_next: io_duration - 2 ms)
    // (06 F-11). Realtime backends are paced by their blocking read.
    if (!capture_backend_is_realtime(loop->capture) && loop->samplerate > 0 &&
        valid_frames > 0) {
      uint64_t io_ms = (uint64_t)valid_frames * 1000ULL / loop->samplerate;
      if (io_ms > 2) {
        cdsp_sleep_ms((uint32_t)(io_ms - 2));
      }
    }
  } else {
    if (loop->resampler) {
      // The relative ratio was already applied in engine_capture_loop_step()
      // before resampler_get_input_frames_next() was queried, so the frame
      // count read from the device matches what resampler_process() expects.
      audio_chunk_t *out_chunk = round_robin_chunk_pool_next(loop->chunk_pool);
      uint64_t res_start = cdsp_time_now_ns();
      resampler_error_t rerr =
          resampler_process(loop->resampler, chunk, out_chunk);
      uint64_t res_end = cdsp_time_now_ns();

      if (rerr != RESAMPLER_OK) {
        logger_error(&g_logger, "Capture resampler error: %s",
                     resampler_error_description(rerr));
        processing_stop_reason_t reason = {.type = STOP_REASON_UNKNOWN_ERROR};
        snprintf(reason.message, sizeof(reason.message), "Resampler error: %s",
                 resampler_error_description(rerr));
        engine_shared_state_request_stop(loop->shared, reason);
        return true;
      }

      if (loop->processing_params && loop->pipeline_rate > 0) {
        size_t nominal_frames = audio_chunk_get_valid_frames(out_chunk);
        if (nominal_frames > 0) {
          uint64_t chunk_dur_ns =
              (uint64_t)nominal_frames * 1000000000ULL / loop->pipeline_rate;
          if (chunk_dur_ns > 0) {
            double r_load =
                ((double)(res_end - res_start) / (double)chunk_dur_ns) * 100.0;
            processing_parameters_set_resampler_load(loop->processing_params,
                                                     r_load);
            logger_trace(&g_logger, "Resampling load: %.2f%%", r_load);
            if (r_load > 100.0) {
              loop->resampler_overloaded_chunks++;
              if (loop->resampler_overloaded_chunks == 10) {
                logger_warn(&g_logger,
                            "Resampler is overloaded (load > 100%% for 10 "
                            "consecutive chunks, current: %.2f%%)",
                            r_load);
              }
            } else {
              loop->resampler_overloaded_chunks = 0;
            }
          }
        }
      }

      capture_loop_enqueue_running_chunk(loop, out_chunk);
    } else {
      capture_loop_enqueue_running_chunk(loop, chunk);
    }
  }
  return false;
}

bool engine_capture_loop_step(engine_capture_loop_t *loop) {
  if (!loop)
    return true;

  // 1. Clock pitch adjustment check:
  // If the capture backend supports hardware clock pitch tuning, sync to
  // the shared speed ratio published by the playback rate controller.
  capture_loop_update_pitch(loop);

  if (loop->resampler) {
    if (loop->pending_chunk) {
      capture_loop_enqueue_running_chunk(loop, loop->pending_chunk);
      if (loop->pending_chunk) {
        capture_backend_wait(loop->capture, 5);
        return false;
      }
    }

    // Apply the latest relative ratio published by the playback rate
    // controller *before* asking the resampler how many input frames it
    // needs (upstream order: SetSpeed -> input_frames_next() -> read ->
    // resample). set_relative_ratio recomputes needed_input_size, so
    // applying it between the read and resampler_process() would make the
    // just-captured chunk look short (zero-padded as a partial chunk) or
    // long (excess frames dropped).
    double ratio = engine_shared_state_get_resampler_ratio(loop->shared);
    resampler_set_relative_ratio(loop->resampler, ratio);

    size_t needed_frames = resampler_get_input_frames_next(loop->resampler);
    backend_error_t err;
    backend_error_init(&err, BACKEND_ERROR_NONE, "");

    bool got_data = capture_backend_read(loop->capture, needed_frames,
                                         loop->raw_chunk, &err);
    if (!got_data) {
      return capture_loop_handle_no_data(loop, &err);
    }

    return capture_loop_process_and_enqueue(loop, loop->raw_chunk);
  }

  // Ref: docs/engine_state_management.md - Section 3.2 & Section 1.7.2 (Rule 5)
  // Fetch a chunk buffer from the pre-allocated round-robin pool,
  // or reuse an un-enqueued chunk if the previous enqueue was dropped due to
  // full queue.
  audio_chunk_t *chunk = loop->pending_chunk;
  if (!chunk) {
    chunk = round_robin_chunk_pool_next(loop->chunk_pool);
  }
  backend_error_t err;
  backend_error_init(&err, BACKEND_ERROR_NONE, "");

  // Read raw PCM/DSD frame data from the capture backend.
  bool got_data =
      capture_backend_read(loop->capture, loop->chunk_size, chunk, &err);
  if (!got_data) {
    loop->pending_chunk = chunk;
    return capture_loop_handle_no_data(loop, &err);
  }

  // Process metering, DoP decode, silence gate, and push to SPSC queue.
  return capture_loop_process_and_enqueue(loop, chunk);
}

void engine_capture_loop_run(engine_capture_loop_t *loop) {
  if (!loop)
    return;
  logger_info(&g_logger, "Capture thread started (realtime: %s)",
              capture_backend_is_realtime(loop->capture) ? "yes" : "no");

  backend_error_t berr;
  backend_error_init(&berr, BACKEND_ERROR_NONE, "");
  // Ref: docs/engine_state_management.md - Section 3.1: Startup &
  // Initialization Flow Step 9: Capture Loop opens the capture device backend
  // asynchronously.
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

  // Pitch-control support is only known once the device is open (CoreAudio
  // probes kAudioDevicePropertyClockSource in open). Publish the refreshed
  // value through the shared state so the playback thread's rate controller
  // picks the right adjustment method (03 CA-01).
  loop->pitch_supported =
      capture_backend_pitch_control_supported(loop->capture);
  engine_shared_state_set_capture_pitch_supported(loop->shared,
                                                  loop->pitch_supported);
  logger_debug(&g_logger, "Capture clock pitch control: %s",
               loop->pitch_supported ? "supported" : "not supported");

  // Ref: docs/engine_state_management.md - Section 3.1: Startup &
  // Initialization Flow Step 10: Once capture open succeeds, transition the
  // state_raw state to RUNNING.
  if (engine_shared_state_get_state(loop->shared) ==
      PROCESSING_STATE_STARTING) {
    engine_shared_state_set_state(loop->shared, PROCESSING_STATE_RUNNING);
  }

  realtime_thread_handle_t *rt_handle = promote_current_thread_to_realtime(
      "Capture", loop->chunk_size, loop->samplerate);
  sample_rate_watcher_reset(loop->rate_watcher);
  engine_shared_state_set_last_capture_time(loop->shared, cdsp_time_now_ns());

  while (1) {
    if (engine_shared_state_should_stop(loop->shared)) {
      break;
    }

    // 1. Hardware Sample-Rate Change Check
    if (capture_loop_check_format_change(loop)) {
      break;
    }

    // Ref: docs/engine_state_management.md - Section 3.2 & Section 1.7.2 (Rule
    // 5)
    // 2. Fetch chunk buffer (or pending_chunk on drop), read backend data, and
    // enqueue to SPSC queue.
    if (engine_capture_loop_step(loop)) {
      break;
    }
  }

  if (loop->shared) {
    engine_shared_state_shutdown_captured_queue(loop->shared);
  }
  if (loop->processing_params) {
    // The resampler runs on this thread; clear its load on exit (upstream
    // resets resampler_load when processing stops).
    processing_parameters_set_resampler_load(loop->processing_params, 0.0);
  }
  if (rt_handle) {
    demote_current_thread_from_realtime(rt_handle);
  }
  if (loop->capture) {
    capture_backend_stop(loop->capture);
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
