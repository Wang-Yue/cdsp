// top-level engine orchestrator.
//
// This class owns the *shape* of an engine run — config, sizing,
// device handles, the three audio threads — but contains no audio
// processing logic itself. Each thread body lives in its own file:
//
//   * `EngineCaptureLoop`     — capture → DoP-decode → level meter
//                               → SPSC queue.
//   * `EngineProcessingLoop`  — SPSC dequeue → resample → pipeline
//                               → SPSC enqueue.
//   * `EnginePlaybackLoop`    — SPSC dequeue → rate-adjust controller
//                               → device write.
//
// All cross-thread state (the stop flag, the SPSC queues, the
// resampler-ratio atomic) lives in `EngineSharedState`. State
// machine + stop-reason publication lives in `EngineStateMachine`.
//
// Lock-free / allocation-free guarantees
// --------------------------------------
//   * The audio threads use lock-free SPSC queues and atomics;
//     only `DispatchSemaphore` is used for signal/wait, which is
//     a kernel signaling primitive (not a lock).
//   * Chunks are managed using a pre-allocated `RoundRobinChunkPool`
//     on each thread to avoid allocations on the hot path.
//   * The resampler and pipeline output scratch buffers are pre-allocated.
//   * The stall watchdog uses `clock_gettime_nsec_np` (vDSO read,
//     no syscall) — no `Date()` on the hot path.

#include "Engine/dsp_session.h"

#ifdef _WIN32
#include <mmsystem.h>
#include <windows.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Config/config_diff.h"
#include "Engine/dsp_session_internal.h"
#include "Engine/engine_session_builder.h"
#include "Logging/app_logger.h"
#include "Utils/cdsp_time.h"

static const logger_t g_logger = {"dsp.session"};

const dsp_config_t* dsp_session_get_config(const dsp_session_t* core) {
  if (!core) return NULL;
  pthread_mutex_lock((pthread_mutex_t*)&core->config_mutex);
  const dsp_config_t* cfg = core->current_config;
  pthread_mutex_unlock((pthread_mutex_t*)&core->config_mutex);
  return cfg;
}

processing_parameters_t* dsp_session_get_processing_params(
    dsp_session_t* core) {
  return core ? core->processing_params : NULL;
}

void dsp_session_set_chunk_callbacks(dsp_session_t* core,
                                     chunk_callback_t on_captured,
                                     void* captured_ctx,
                                     chunk_callback_t on_processed,
                                     void* processed_ctx) {
  if (!core) return;
  core->on_chunk_captured = on_captured;
  core->on_chunk_captured_ctx = captured_ctx;
  core->on_chunk_processed = on_processed;
  core->on_chunk_processed_ctx = processed_ctx;
}

bool dsp_session_is_stop_requested(const dsp_session_t* core,
                                   processing_stop_reason_t* out_reason) {
  if (!core || !core->shared) return false;
  bool req = engine_shared_state_should_stop(core->shared);
  if (req && out_reason) {
    *out_reason = engine_shared_state_get_stop_reason(core->shared);
  }

  // Watchdog Stall Check:
  // Ref: engine_state_management.md - Section 3.4: Watchdog Stall & Recovery Flow
  // Step 1: Query stop_reason safely under stop_reason_mutex (engine_shared_state_get_stop_reason).
  // If the engine state is RUNNING and no stop sequence has been initiated, but the capture
  // thread has not successfully enqueued/read any audio chunk for more than the stall timeout,
  // we transition the state to STALLED. Checking this on the main thread (during poll) prevents
  // lockups when the capture backend read call blocks infinitely in kernel space.
  if (!req && engine_shared_state_get_stop_reason(core->shared).type == STOP_REASON_NONE &&
      engine_shared_state_get_state(core->shared) == PROCESSING_STATE_RUNNING) {
    uint64_t last_capture_time = engine_shared_state_get_last_capture_time(core->shared);
    if (last_capture_time > 0) {
      double elapsed = (double)(cdsp_time_now_ns() - last_capture_time) / 1000000000.0;
      double timeout_sec = 0.5;
      if (elapsed > timeout_sec) {
        engine_shared_state_set_state(core->shared, PROCESSING_STATE_STALLED);
        logger_warn(&g_logger, "Watchdog: capture device stalled (no data for %.3fs)", elapsed);
      }
    }
  }

  return req;
}

// MARK: - Lifecycle (RAII Option A)

dsp_session_t* dsp_session_create_and_start(dsp_config_t* config,
                                            chunk_callback_t on_captured,
                                            void* captured_ctx,
                                            chunk_callback_t on_processed,
                                            void* processed_ctx,
                                            audio_backend_error_t* err) {
  return engine_session_build_and_start(config, on_captured, captured_ctx,
                                        on_processed, processed_ctx, err);
}

processing_state_t dsp_session_get_state(const dsp_session_t* core) {
  return core && core->shared ? engine_shared_state_get_state(core->shared)
                              : PROCESSING_STATE_INACTIVE;
}

processing_stop_reason_t dsp_session_get_stop_reason(
    const dsp_session_t* core) {
  if (core && core->shared) {
    return engine_shared_state_get_stop_reason(core->shared);
  }
  return (processing_stop_reason_t){.type = STOP_REASON_NONE};
}

processing_stop_reason_t dsp_session_stop_and_free(
    dsp_session_t* core, processing_stop_reason_t reason) {
  processing_stop_reason_t final_reason = {.type = STOP_REASON_NONE};
  if (!core) return final_reason;

  logger_info(&g_logger, "Stopping and destroying DSP session");

  // Dispatch in-band AudioMessage stop signal downstream
  if (core->shared) {
    engine_shared_state_request_stop(core->shared, reason);
    if (!core->threads_created) {
      engine_shared_state_shutdown_processed_queue(core->shared);
    }
    final_reason = engine_shared_state_get_stop_reason(core->shared);
  }

  // Ref: engine_state_management.md - Section 1.7.2 Rule 3 & Section 3.6 Step 3
  // Stop backends immediately to abort any blocking OS kernel driver read/write operations
  // in worker threads BEFORE invoking pthread_join().
  if (core->capture) {
    capture_backend_stop(core->capture);
  }
  if (core->playback) {
    playback_backend_stop(core->playback);
  }

  // Wait for all audio threads to finish.
  if (core->threads_created) {
    pthread_join(core->capture_thread, NULL);
    pthread_join(core->processing_thread, NULL);
    pthread_join(core->playback_thread, NULL);
    core->threads_created = false;
  }

  // Drain any chunks left in the lock-free queues before the
  // device handles go away. Prevents stale-chunk pollution.
  if (core->shared) {
    spsc_queue_drain(engine_shared_state_get_captured_queue(core->shared));
    spsc_queue_drain(engine_shared_state_get_processed_queue(core->shared));
  }

  if (core->capture) {
    capture_backend_close(core->capture);
    capture_backend_free(core->capture);
    core->capture = NULL;
  }
  if (core->playback) {
    playback_backend_close(core->playback);
    playback_backend_free(core->playback);
    core->playback = NULL;
  }
  if (core->resampler) {
    resampler_free(core->resampler);
    core->resampler = NULL;
  }
  if (core->resampler_scratch) {
    audio_chunk_free(core->resampler_scratch);
    core->resampler_scratch = NULL;
  }
  if (core->pipeline_scratch) {
    audio_chunk_free(core->pipeline_scratch);
    core->pipeline_scratch = NULL;
  }
  if (core->capture_loop) {
    engine_capture_loop_free(core->capture_loop);
    core->capture_loop = NULL;
  }
  if (core->processing_loop) {
    engine_processing_loop_free(core->processing_loop);
    core->processing_loop = NULL;
    core->pipeline = NULL;
  } else if (core->pipeline) {
    pipeline_free(core->pipeline);
    core->pipeline = NULL;
  }
  if (core->playback_loop) {
    engine_playback_loop_free(core->playback_loop);
    core->playback_loop = NULL;
  }

  if (core->capture_chunk_pool) {
    round_robin_chunk_pool_free(core->capture_chunk_pool);
    core->capture_chunk_pool = NULL;
  }
  if (core->processing_scratch_pool) {
    round_robin_chunk_pool_free(core->processing_scratch_pool);
    core->processing_scratch_pool = NULL;
  }

  pthread_mutex_lock(&core->config_mutex);
  if (core->current_config) {
    dsp_config_free(core->current_config);
    core->current_config = NULL;
  }
  pthread_mutex_unlock(&core->config_mutex);
  pthread_mutex_destroy(&core->config_mutex);

  if (core->processing_params) {
    processing_parameters_free(core->processing_params);
    core->processing_params = NULL;
  }
  if (core->shared) {
    engine_shared_state_set_state(core->shared, PROCESSING_STATE_INACTIVE);
    engine_shared_state_free(core->shared);
    core->shared = NULL;
  }
  if (core->dop_decoder) {
    dop_decoder_free(core->dop_decoder);
    core->dop_decoder = NULL;
  }
  if (core->dsd_encoder) {
    dsd_encoder_free(core->dsd_encoder);
    core->dsd_encoder = NULL;
  }

#ifdef _WIN32
  timeEndPeriod(1);
#endif

  free(core);
  return final_reason;
}

/// Rebuild or update the processing pipeline against `newConfig` without
/// touching the audio devices. The caller is responsible for
/// verifying that `newConfig.devices == currentConfig.devices` —
/// the `DSPEngine` actor does this comparison and falls back to a
/// full teardown when they differ.
bool dsp_session_reload_config(dsp_session_t* core, dsp_config_t* new_config,
                               audio_backend_error_t* err) {
  if (!core || !new_config) {
    if (err) {
      err->type = AUDIO_BACKEND_ERR_COMMAND_SEND;
      strcpy(err->message, "Invalid session or configuration arguments");
    }
    return false;
  }

  pthread_mutex_lock(&core->config_mutex);
  dsp_config_t* old_config = core->current_config;
  pthread_mutex_unlock(&core->config_mutex);

  if (dsp_session_get_state(core) == PROCESSING_STATE_INACTIVE) {
    pthread_mutex_lock(&core->config_mutex);
    if (old_config && old_config != new_config) {
      dsp_config_free(old_config);
    }
    core->current_config = new_config;
    pthread_mutex_unlock(&core->config_mutex);
    return true;
  }

  // 1. Perform configuration diffing.
  // Evaluate the changes between the running config and the new config.
  config_change_t* change = config_change_create();
  if (!change) {
    if (err) {
      err->type = AUDIO_BACKEND_ERR_COMMAND_SEND;
      strcpy(err->message,
             "Failed to allocate memory for configuration diffing");
    }
    return false;
  }
  config_change_type_t change_type =
      config_diff(old_config, new_config, change);

  if (change_type == CONFIG_CHANGE_NONE) {
    logger_info(&g_logger, "No changes in config.");
    dsp_config_free(new_config);  // new config is identical, discard it
    config_change_free(change);
    return true;
  }

  // 3. Fall back to structural change (rebuilding pipeline).
  // If structural changes occurred (e.g., adding or removing filters), we must
  // rebuild the entire pipeline structure. We create the new pipeline and
  // instruct the processing loop thread to swap it. This avoids audio backend
  // restarts.
  config_change_free(change);

  config_error_t cerr;
  config_error_init(&cerr);
  pipeline_t* new_pipeline =
      pipeline_create(new_config, core->processing_params,
                      core->effective_playback_chunk_size, &cerr);
  if (!new_pipeline) {
    if (err) {
      err->type = AUDIO_BACKEND_ERR_COMMAND_SEND;
      strncpy(err->message, cerr.message, sizeof(err->message) - 1);
      err->message[sizeof(err->message) - 1] = '\0';
    }
    return false;
  }

  if (core->processing_loop) {
    engine_processing_loop_set_pipeline(core->processing_loop, new_pipeline);
  } else {
    pipeline_free(new_pipeline);
    if (err) {
      err->type = AUDIO_BACKEND_ERR_COMMAND_SEND;
      strcpy(err->message, "Processing loop unavailable");
    }
    return false;
  }

  pthread_mutex_lock(&core->config_mutex);
  core->current_config = new_config;
  if (old_config && old_config != new_config) {
    dsp_config_free(old_config);
  }
  pthread_mutex_unlock(&core->config_mutex);

  logger_info(&g_logger, "Pipeline rebuilt without audio-device restart");
  return true;
}

void dsp_session_collect_garbage(dsp_session_t* core) {
  if (!core || !core->shared) return;

  pipeline_t* p = NULL;
  while ((p = engine_shared_state_dequeue_garbage_pipeline(core->shared)) !=
         NULL) {
    pipeline_free(p);
  }
}
