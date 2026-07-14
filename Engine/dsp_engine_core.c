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

#include "Engine/dsp_engine_core.h"

#ifdef _WIN32
#include <mmsystem.h>
#include <windows.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Config/config_diff.h"
#include "Engine/dsp_engine_core_internal.h"
#include "Engine/engine_session_builder.h"
#include "Logging/app_logger.h"

static const logger_t g_logger = {"dsp.engine.core"};

const dsp_config_t* dsp_engine_core_get_config(const dsp_engine_core_t* core) {
  return core ? core->current_config : NULL;
}

processing_parameters_t* dsp_engine_core_get_processing_params(
    dsp_engine_core_t* core) {
  return core ? core->processing_params : NULL;
}

void dsp_engine_core_set_chunk_callbacks(dsp_engine_core_t* core,
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

bool dsp_engine_core_is_stop_requested(const dsp_engine_core_t* core,
                                       processing_stop_reason_t* out_reason) {
  if (!core || !core->shared) return false;
  bool req = engine_shared_state_should_stop(core->shared);
  if (req && out_reason) {
    const processing_stop_reason_t* r =
        engine_shared_state_get_stop_reason(core->shared);
    if (r) *out_reason = *r;
  }
  return req;
}

// MARK: - Lifecycle (RAII Option A)

dsp_engine_core_t* dsp_engine_core_create_and_start(
    dsp_config_t* config, chunk_callback_t on_captured, void* captured_ctx,
    chunk_callback_t on_processed, void* processed_ctx,
    audio_backend_error_t* err) {
  return engine_session_build_and_start(config, on_captured, captured_ctx,
                                        on_processed, processed_ctx, err);
}

processing_state_t dsp_engine_core_get_state(const dsp_engine_core_t* core) {
  return core && core->shared ? engine_shared_state_get_state(core->shared)
                              : PROCESSING_STATE_INACTIVE;
}

const processing_stop_reason_t* dsp_engine_core_get_stop_reason(
    const dsp_engine_core_t* core) {
  return core && core->shared
             ? engine_shared_state_get_stop_reason(core->shared)
             : NULL;
}

void dsp_engine_core_stop_and_free(dsp_engine_core_t* core,
                                   processing_stop_reason_t reason) {
  if (!core) return;

  logger_info(&g_logger, "Stopping and destroying engine core session");

  // Dispatch in-band AudioMessage stop signal downstream
  if (core->shared) {
    engine_shared_state_request_stop(core->shared, reason);
    if (!core->threads_created) {
      engine_shared_state_shutdown_processed_queue(core->shared);
    }
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
    audio_resampler_free(core->resampler);
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

  if (core->current_config) {
    dsp_config_free(core->current_config);
    core->current_config = NULL;
  }
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
}

/// Rebuild or update the processing pipeline against `newConfig` without
/// touching the audio devices. The caller is responsible for
/// verifying that `newConfig.devices == currentConfig.devices` —
/// the `DSPEngine` actor does this comparison and falls back to a
/// full teardown when they differ.
bool dsp_engine_core_reload_config(dsp_engine_core_t* core,
                                   dsp_config_t* new_config,
                                   audio_backend_error_t* err) {
  if (!core || !new_config) return false;

  dsp_config_t* old_config = core->current_config;

  if (dsp_engine_core_get_state(core) == PROCESSING_STATE_INACTIVE) {
    if (old_config && old_config != new_config) {
      dsp_config_free(old_config);
    }
    core->current_config = new_config;
    return true;
  }

  // 1. Perform configuration diffing.
  // Evaluate the changes between the running config and the new config.
  config_change_t* change = config_change_create();
  if (!change) {
    dsp_config_free(new_config);
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
    dsp_config_free(new_config);
    return false;
  }

  if (core->processing_loop) {
    engine_processing_loop_set_pipeline(core->processing_loop, new_pipeline);
  } else {
    pipeline_free(new_pipeline);
  }

  core->current_config = new_config;
  if (old_config && old_config != new_config) {
    dsp_config_free(old_config);
  }

  logger_info(&g_logger, "Pipeline rebuilt without audio-device restart");
  return true;
}

void dsp_engine_core_collect_garbage(dsp_engine_core_t* core) {
  if (!core || !core->shared) return;

  pipeline_t* p = NULL;
  while ((p = engine_shared_state_dequeue_garbage_pipeline(core->shared)) !=
         NULL) {
    pipeline_free(p);
  }
}
