// Processing thread body. Drains the capture→processing SPSC queue,
// runs each chunk through the (optional) resampler and the pipeline,
// then enqueues the result on the processing→playback queue.
//
// State ownership
// ---------------
// The pre-allocated scratch chunks (`resamplerScratch`,
// `pipelineScratch`) are owned by this loop and only mutated here.
// The resampler's own internal state is also single-threaded: the
// playback thread publishes a relative ratio via the shared atomic,
// and the processing thread consumes it once per chunk through
// `setRelativeRatio`. No cross-thread mutation of resampler state.
//
// Audio-thread invariants
// -----------------------
//   * No allocations in the steady state. Output chunks are obtained
//     from a pre-allocated `RoundRobinChunkPool`, and the resampler
//     scratch chunk is pre-allocated at init.
//   * No locks. The shared SPSC queues + semaphores carry chunks
//     and wakeups; the resampler ratio is an atomic Double.
//   * The thread sets a real-time scheduling policy on entry so the
//     OS prefers it over background work.
#include "engine_processing_loop.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include "Utils/cdsp_time.h"

struct pending_update {
  dsp_config_t* config;
  char** filters;
  size_t filters_count;
  char** mixers;
  size_t mixers_count;
  char** processors;
  size_t processors_count;
};

struct engine_processing_loop {
  engine_shared_state_t* shared;
  processing_parameters_t* processing_params;
  size_t pipeline_rate;
  resampler_t* resampler;
  pipeline_t* active_pipeline;
  dsd_encoder_t* dsd_encoder;
  _Atomic(pipeline_t*) next_pipeline;
  audio_chunk_t* resampler_scratch;
  audio_chunk_t* pipeline_scratch;
  round_robin_chunk_pool_t* scratch_pool;

  chunk_callback_t on_chunk_captured;
  void* on_chunk_captured_ctx;
  chunk_callback_t on_chunk_processed;
  void* on_chunk_processed_ctx;
  bool is_realtime;
  uint64_t processed_drop_counter;
};

#include <string.h>
#include <time.h>

#include "Logging/app_logger.h"
#include "thread_priority.h"

static const logger_t g_logger = {"dsp.processing"};

#include "Utils/cdsp_time.h"

engine_processing_loop_t* engine_processing_loop_create(
    const engine_processing_loop_config_t* config) {
  if (!config) return NULL;

  engine_processing_loop_t* loop =
      (engine_processing_loop_t*)calloc(1, sizeof(engine_processing_loop_t));
  if (!loop) return NULL;
  loop->shared = config->shared;
  loop->processing_params = config->processing_params;
  loop->pipeline_rate = config->pipeline_rate;
  loop->resampler = config->resampler;
  loop->active_pipeline = config->pipeline;
  loop->dsd_encoder = config->dsd_encoder;
  loop->resampler_scratch = config->resampler_scratch;
  loop->pipeline_scratch = config->pipeline_scratch;
  loop->scratch_pool = config->scratch_pool;
  loop->on_chunk_captured = config->on_chunk_captured;
  loop->on_chunk_captured_ctx = config->on_chunk_captured_ctx;
  loop->on_chunk_processed = config->on_chunk_processed;
  loop->on_chunk_processed_ctx = config->on_chunk_processed_ctx;
  loop->is_realtime = config->is_realtime;
  loop->processed_drop_counter = 0;

  atomic_store(&loop->next_pipeline, NULL);

  return loop;
}

void engine_processing_loop_free(engine_processing_loop_t* loop) {
  if (!loop) return;
  pipeline_t* p = atomic_exchange(&loop->next_pipeline, NULL);
  if (p) {
    pipeline_free(p);
  }
  if (loop->active_pipeline) {
    pipeline_free(loop->active_pipeline);
  }
  free(loop);
}

void engine_processing_loop_set_pipeline(engine_processing_loop_t* loop,
                                         pipeline_t* new_pipeline) {
  if (loop) {
    pipeline_t* old = atomic_exchange(&loop->next_pipeline, new_pipeline);
    if (old) {
      pipeline_free(old);
    }
  }
}

/**
 * @brief Resamples an audio chunk if resampling is configured.
 *
 * @param loop Pointer to the processing loop context.
 * @param chunk Input audio chunk.
 * @param out_res_start Output timestamp for start of resampling.
 * @param out_res_end Output timestamp for end of resampling.
 * @param out_err Set to true if resampling encountered a fatal error.
 * @return Resampled audio chunk (resampler_scratch) or original chunk if no
 * resampler set.
 */
static audio_chunk_t* processing_loop_resample(engine_processing_loop_t* loop,
                                               audio_chunk_t* chunk,
                                               uint64_t* out_res_start,
                                               uint64_t* out_res_end,
                                               bool* out_err) {
  *out_err = false;
  if (!loop->resampler) return chunk;

  // Resample if configured. The desired ratio is published
  // by the rate-adjust controller via `shared.resamplerRatio`;
  // we sync the resampler to it once per chunk. The
  // resampler's internal state is otherwise owned exclusively
  // by this thread, so no lock is required.
  double ratio = engine_shared_state_get_resampler_ratio(loop->shared);
  resampler_set_relative_ratio(loop->resampler, ratio);

  // Write into the pre-sized output scratch (sized to
  // `resampler.maxOutputFrames`), then make that scratch
  // our working chunk. We can't `swap` here — a non-1:1
  // resampler has different input/output chunk sizes, so
  // swapping would leave scratch holding a too-small array
  // on the next iteration.
  *out_res_start = cdsp_time_now_ns();
  resampler_error_t rerr =
      resampler_process(loop->resampler, chunk, loop->resampler_scratch);
  *out_res_end = cdsp_time_now_ns();

  if (rerr != RESAMPLER_OK) {
    logger_error(&g_logger, "Processing error: resampler error %d", rerr);
    processing_stop_reason_t reason = {.type = STOP_REASON_UNKNOWN_ERROR};
    snprintf(reason.message, sizeof(reason.message), "Resampler error %d",
             rerr);
    engine_shared_state_request_stop(loop->shared, reason);
    *out_err = true;
    return NULL;
  }
  return loop->resampler_scratch;
}

/**
 * @brief Checks if a hot-reloaded pipeline has been queued and atomically swaps
 * it in.
 *
 * Transfers active filter state to the new pipeline and enqueues the old
 * pipeline for off-thread garbage collection.
 *
 * @param loop Pointer to the processing loop context.
 */
static void processing_loop_check_pipeline_swap(
    engine_processing_loop_t* loop) {
  pipeline_t* next_pipeline = atomic_exchange(&loop->next_pipeline, NULL);
  if (next_pipeline) {
    if (loop->active_pipeline) {
      pipeline_transfer_state(next_pipeline, loop->active_pipeline);
      if (!engine_shared_state_enqueue_garbage_pipeline(
              loop->shared, loop->active_pipeline)) {
        pipeline_free(loop->active_pipeline);
      }
    }
    loop->active_pipeline = next_pipeline;
  }
}

/**
 * @brief Calculates DSP processing/resampler CPU load and counts clipped
 * samples.
 *
 * @param loop Pointer to the processing loop context.
 * @param chunk Processed audio chunk.
 * @param pipe_start Start timestamp of DSP pipeline execution.
 * @param pipe_end End timestamp of DSP pipeline execution.
 * @param res_start Start timestamp of resampler execution.
 * @param res_end End timestamp of resampler execution.
 */
static void processing_loop_record_metrics(engine_processing_loop_t* loop,
                                           const audio_chunk_t* chunk,
                                           uint64_t pipe_start,
                                           uint64_t pipe_end,
                                           uint64_t res_start,
                                           uint64_t res_end) {
  if (!loop->processing_params) return;

  // Calculate CPU load of the DSP pipeline and resampler.
  // The load is the ratio of processing time (in nanoseconds) to the
  // physical duration of the audio chunk. A load > 1.0 means we cannot
  // process in real-time and will cause dropouts.
  size_t frames = audio_chunk_get_valid_frames(chunk);
  if (frames > 0) {
    uint64_t chunk_duration_ns =
        (uint64_t)frames * 1000000000ULL / loop->pipeline_rate;
    if (chunk_duration_ns > 0) {
      double p_load =
          (double)(pipe_end - pipe_start) / (double)chunk_duration_ns;
      processing_parameters_set_processing_load(loop->processing_params,
                                                p_load);

      if (loop->resampler) {
        double r_load =
            (double)(res_end - res_start) / (double)chunk_duration_ns;
        processing_parameters_set_resampler_load(loop->processing_params,
                                                 r_load);
      } else {
        processing_parameters_set_resampler_load(loop->processing_params, 0.0);
      }
    }
  }

  // Scan the output chunk for clipped samples (outside [-1.0, 1.0] range).
  // This is done before DoP encoding.
  size_t channels = audio_chunk_get_channels(chunk);
  size_t c_frames = audio_chunk_get_valid_frames(chunk);
  uint64_t clipped = 0;
  for (size_t c = 0; c < channels; c++) {
    mutable_waveform_t data = audio_chunk_get_channel(chunk, c);
    for (size_t f = 0; f < c_frames; f++) {
      if (data[f] > 1.0 || data[f] < -1.0) {
        clipped++;
      }
    }
  }
  processing_parameters_add_clipped_samples(loop->processing_params, clipped);
}

void engine_processing_loop_run(engine_processing_loop_t* loop) {
  if (!loop) return;
  logger_info(&g_logger, "Processing thread started");

  set_realtime_thread_priority("Processing",
                               audio_chunk_get_frames(loop->pipeline_scratch),
                               loop->pipeline_rate);

  audio_chunk_t* chunk = NULL;

  while ((chunk = engine_shared_state_dequeue_captured_blocking(
              loop->shared)) != NULL) {
    uint64_t res_start = 0;
    uint64_t res_end = 0;
    if (engine_shared_state_get_state(loop->shared) ==
        PROCESSING_STATE_PAUSED) {
      continue;
    }

    // 1. Resample if configured
    bool resamp_err = false;
    chunk = processing_loop_resample(loop, chunk, &res_start, &res_end,
                                     &resamp_err);
    if (resamp_err) break;

    // 2. Pre-processing tap for visualisation.
    if (loop->on_chunk_captured) {
      loop->on_chunk_captured(loop->on_chunk_captured_ctx, chunk);
    }

    // 3. Hot-reload pipeline swap if requested
    processing_loop_check_pipeline_swap(loop);

    // 4. Retrieve a pre-allocated scratch chunk from the round-robin pool.
    // We must use a pool because the pipeline output chunk is passed down the
    // queue to the playback thread, and we cannot reuse it immediately.
    audio_chunk_t* current_scratch =
        round_robin_chunk_pool_next(loop->scratch_pool);

    // 5. Execute DSP filtering and mixing pipeline
    uint64_t pipe_start = cdsp_time_now_ns();
    pipeline_error_t perr =
        pipeline_process(loop->active_pipeline, chunk, current_scratch);
    uint64_t pipe_end = cdsp_time_now_ns();

    if (perr != PIPELINE_OK) {
      logger_error(&g_logger, "Processing error: pipeline error %d", perr);
      processing_stop_reason_t reason = {.type = STOP_REASON_UNKNOWN_ERROR};
      snprintf(reason.message, sizeof(reason.message), "Pipeline error %d",
               perr);
      engine_shared_state_request_stop(loop->shared, reason);
      break;
    }
    chunk = current_scratch;

    // 6. Metrics and clipping stats calculation
    processing_loop_record_metrics(loop, chunk, pipe_start, pipe_end, res_start,
                                   res_end);

    // 7. Update level meters with output levels
    processing_parameters_update_playback_levels(loop->processing_params,
                                                 chunk);

    if (loop->on_chunk_processed) {
      loop->on_chunk_processed(loop->on_chunk_processed_ctx, chunk);
    }

    // 8. Encode PCM to DSD (DoP / Native DSD) in place if enabled
    if (loop->dsd_encoder) {
      dsd_encoder_encode(loop->dsd_encoder, chunk);
    }

    // 9. Enqueue the processed chunk to the playback queue.
    if (loop->is_realtime) {
      // Real-time hardware stream: non-blocking single-try push to avoid audio
      // hiccups.
      if (!engine_shared_state_enqueue_processed(loop->shared, chunk)) {
        loop->processed_drop_counter++;
        logger_warn(&g_logger, "Processed chunk dropped (playback queue full)");
      }
    } else {
      // Non-real-time file conversion: nanosleep wait while queue is full to
      // preserve every sample frame.
      while (!engine_shared_state_enqueue_processed(loop->shared, chunk)) {
        if (engine_shared_state_should_stop(loop->shared)) {
          const processing_stop_reason_t* reason =
              engine_shared_state_get_stop_reason(loop->shared);
          if (reason && reason->type != STOP_REASON_DONE) {
            break;
          }
        }
        cdsp_sleep_ms(1);
      }
    }
  }

  if (loop->shared) {
    engine_shared_state_shutdown_processed_queue(loop->shared);
  }
  if (loop->processed_drop_counter > 0) {
    logger_warn(
        &g_logger,
        "Processing thread stopped. Total dropped processed chunks: %llu",
        (unsigned long long)loop->processed_drop_counter);
  } else {
    logger_info(&g_logger, "Processing thread stopped");
  }
}
