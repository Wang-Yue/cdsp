#include "Pipeline/pipeline.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Audio/audio_chunk.h"
#include "Audio/processing_parameters.h"
#include "Config/config_error.h"
#include "Filters/biquad.h"
#include "Filters/biquad_combo.h"
#include "Filters/filter.h"
#include "Filters/volume.h"
#include "Logging/app_logger.h"
#include "Mixer/mixer.h"
#include "Pipeline/pipeline_internal.h"
#include "Processors/processor.h"
#include "Utils/double_helpers.h"

#if defined(ENABLE_LIBDISPATCH)
#include <dispatch/dispatch.h>
#endif

#if defined(ENABLE_OPENMP)
#include <omp.h>
#endif

static const logger_t g_logger = {"dsp.pipeline"};

const char* pipeline_error_description(pipeline_error_t err) {
  switch (err) {
    case PIPELINE_OK:
      return "No error";
    case PIPELINE_ERR_INPUT_SIZE_MISMATCH:
      return "Input size mismatch";
    case PIPELINE_ERR_OUTPUT_BUFFER_TOO_SMALL:
      return "Output buffer too small";
    case PIPELINE_ERR_CHANNEL_COUNT_MISMATCH:
      return "Channel count mismatch";
    default:
      return "Unknown pipeline error";
  }
}

// ============================================================================
// Cleanup and Destruction Helpers
// ============================================================================

void free_filter_chains(parallel_filter_chain_t* chains, size_t count) {
  if (!chains) return;
  for (size_t i = 0; i < count; i++) {
    if (chains[i].filters) {
      for (size_t j = 0; j < chains[i].filters_count; j++) {
        if (chains[i].filters[j]) {
          filter_free(chains[i].filters[j]);
        }
      }
      free(chains[i].filters);
    }
  }
  free(chains);
}

void biquad_step_free(biquad_step_t* step) {
  if (!step) return;
  if (step->filters) {
    for (size_t c = 0; c < step->channels_count; c++) {
      if (step->filters[c]) {
        for (size_t i = 0; i < step->filters_count; i++) {
          if (step->filters[c][i]) {
            filter_free(step->filters[c][i]);
          }
        }
        free(step->filters[c]);
      }
    }
    free(step->filters);
  }
  if (step->cascades) {
    for (size_t c = 0; c < step->channels_count; c++) {
      free(step->cascades[c]);
    }
    free(step->cascades);
  }
  if (step->channel_of) free(step->channel_of);
  if (step->live) free(step->live);
  if (step->waveforms) free(step->waveforms);
  free(step);
}

static void free_exec_steps(pipeline_exec_step_t* steps, size_t count) {
  if (!steps) return;
  for (size_t i = 0; i < count; i++) {
    pipeline_exec_step_t* step = &steps[i];
    if (step->chains) {
      free_filter_chains(step->chains, step->chains_count);
    }
    if (step->mixer) {
      mixer_free(step->mixer);
    }
    if (step->processor) {
      dsp_processor_free(step->processor);
    }
    if (step->biquad_step) {
      biquad_step_free(step->biquad_step);
    }
  }
  free(steps);
}

void pipeline_free(pipeline_t* pipeline) {
  if (!pipeline) return;
  if (pipeline->master_volume) {
    g_volume_vtable.free(pipeline->master_volume);
  }
  if (pipeline->capture_scratch) {
    audio_chunk_free(pipeline->capture_scratch);
  }
  if (pipeline->scratches_for_mixers) {
    for (size_t i = 0; i < pipeline->scratches_for_mixers_count; i++) {
      if (pipeline->scratches_for_mixers[i]) {
        audio_chunk_free(pipeline->scratches_for_mixers[i]);
      }
    }
    free(pipeline->scratches_for_mixers);
  }
  if (pipeline->steps) {
    free_exec_steps(pipeline->steps, pipeline->steps_count);
  }
  free(pipeline);
}

// ============================================================================
// Audio Processing Loop
// ============================================================================

static void execute_biquad_step(biquad_step_t* step, audio_chunk_t* chunk,
                                size_t valid_frames) {
  if (!step || !chunk || valid_frames == 0 || step->cascade_depth == 0) return;
  size_t chunk_channels = audio_chunk_get_channels(chunk);
  if (chunk_channels > step->waveforms_capacity) {
    return;
  }
  for (size_t ch = 0; ch < chunk_channels; ch++) {
    step->waveforms[ch] = audio_chunk_get_channel(chunk, ch);
  }

  step->live_count = 0;
  for (size_t i = 0; i < step->channels_count; i++) {
    size_t ch = step->channel_of[i];
    if (ch < chunk_channels && step->waveforms[ch] != NULL) {
      step->live[step->live_count++] = i;
    }
  }
  if (step->live_count == 0) return;

  biquad_process_cascades(step->cascades, step->waveforms, step->channel_of,
                          step->live, step->live_count, step->cascade_depth,
                          valid_frames);
}

static inline void process_filter_chain(const parallel_filter_chain_t* chain,
                                        audio_chunk_t* chunk,
                                        size_t valid_frames) {
  if (chain->channel >= audio_chunk_get_channels(chunk)) return;
  mutable_waveform_t buf = audio_chunk_get_channel(chunk, chain->channel);
  if (!buf || valid_frames == 0) return;
  for (size_t j = 0; j < chain->filters_count; j++) {
    if (chain->filters[j]) {
      filter_process(chain->filters[j], buf, valid_frames);
    }
  }
}

#if defined(ENABLE_LIBDISPATCH)
typedef struct {
  audio_chunk_t* current_chunk;
  size_t valid_frames;
  parallel_filter_chain_t* chains;
} dispatch_ctx_t;

static void parallel_filter_worker(void* context, size_t idx) {
  dispatch_ctx_t* ctx = (dispatch_ctx_t*)context;
  process_filter_chain(&ctx->chains[idx], ctx->current_chunk,
                       ctx->valid_frames);
}
#endif

static void execute_parallel_filters(const pipeline_t* pipeline,
                                     const pipeline_exec_step_t* step,
                                     audio_chunk_t* current_chunk,
                                     size_t valid_frames) {
  bool use_multithreading = false;
#if defined(ENABLE_LIBDISPATCH) || defined(ENABLE_OPENMP)
  if (pipeline->multithreaded && step->chains_count > 1) {
    use_multithreading = true;
  }
#endif

  if (use_multithreading) {
#if defined(ENABLE_LIBDISPATCH)
    dispatch_ctx_t dctx = {current_chunk, valid_frames, step->chains};
    dispatch_queue_t queue =
        dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_HIGH, 0);
    dispatch_apply_f(step->chains_count, queue, &dctx, parallel_filter_worker);
#elif defined(ENABLE_OPENMP)
#pragma omp parallel for num_threads(step->chains_count)
    for (size_t idx = 0; idx < step->chains_count; idx++) {
      process_filter_chain(&step->chains[idx], current_chunk, valid_frames);
    }
#endif
  } else {
    for (size_t idx = 0; idx < step->chains_count; idx++) {
      process_filter_chain(&step->chains[idx], current_chunk, valid_frames);
    }
  }
}

static pipeline_error_t execute_mixer_step(pipeline_t* pipeline,
                                           const pipeline_exec_step_t* step,
                                           audio_chunk_t** inout_current_chunk,
                                           size_t* inout_mixer_idx,
                                           size_t valid_frames) {
  size_t mixer_idx = *inout_mixer_idx;
  if (mixer_idx >= pipeline->scratches_for_mixers_count) {
    logger_warn(&g_logger,
                "Mixer scratch buffer index out of bounds: %zu >= %zu",
                mixer_idx, pipeline->scratches_for_mixers_count);
    return PIPELINE_ERR_OUTPUT_BUFFER_TOO_SMALL;
  }
  audio_chunk_t* current_chunk = *inout_current_chunk;
  audio_chunk_t* scratch = pipeline->scratches_for_mixers[mixer_idx];
  mixer_error_t err = mixer_process(step->mixer, current_chunk, scratch);
  if (err != MIXER_OK) {
    if (err == MIXER_ERR_INPUT_SIZE_MISMATCH) {
      pipeline->last_error_needed = pipeline->frames_per_chunk;
      pipeline->last_error_got = valid_frames;
      return PIPELINE_ERR_INPUT_SIZE_MISMATCH;
    }
    if (err == MIXER_ERR_OUTPUT_BUFFER_TOO_SMALL) {
      pipeline->last_error_needed = valid_frames;
      pipeline->last_error_got = audio_chunk_get_frames(scratch);
      return PIPELINE_ERR_OUTPUT_BUFFER_TOO_SMALL;
    }
    size_t current_in_ch = audio_chunk_get_channels(current_chunk);
    size_t mixer_in_ch = mixer_get_channels_in(step->mixer);
    if (current_in_ch != mixer_in_ch) {
      pipeline->last_error_needed = mixer_in_ch;
      pipeline->last_error_got = current_in_ch;
    } else {
      pipeline->last_error_needed = mixer_get_channels_out(step->mixer);
      pipeline->last_error_got = audio_chunk_get_channels(scratch);
    }
    return PIPELINE_ERR_CHANNEL_COUNT_MISMATCH;
  }
  *inout_current_chunk = scratch;
  *inout_mixer_idx = mixer_idx + 1;
  return PIPELINE_OK;
}

pipeline_error_t pipeline_process(pipeline_t* pipeline,
                                  const audio_chunk_t* input,
                                  audio_chunk_t* output) {
  if (!pipeline || !input || !output) return PIPELINE_ERR_INPUT_SIZE_MISMATCH;
  size_t valid_frames = audio_chunk_get_valid_frames(input);

  // 1. Validate input and output buffer shapes/capacities against pipeline
  // configurations.
  if (valid_frames > pipeline->frames_per_chunk) {
    logger_warn(&g_logger,
                "Pipeline input frame size mismatch: needed <= %zu, got %zu",
                pipeline->frames_per_chunk, valid_frames);
    pipeline->last_error_needed = pipeline->frames_per_chunk;
    pipeline->last_error_got = valid_frames;
    return PIPELINE_ERR_INPUT_SIZE_MISMATCH;
  }
  if (audio_chunk_get_channels(input) != pipeline->expected_in_channels) {
    logger_warn(
        &g_logger, "Pipeline input channel mismatch: expected %zu, got %zu",
        pipeline->expected_in_channels, audio_chunk_get_channels(input));
    pipeline->last_error_needed = pipeline->expected_in_channels;
    pipeline->last_error_got = audio_chunk_get_channels(input);
    return PIPELINE_ERR_CHANNEL_COUNT_MISMATCH;
  }
  if (audio_chunk_get_channels(output) != pipeline->expected_out_channels) {
    logger_warn(
        &g_logger, "Pipeline output channel mismatch: expected %zu, got %zu",
        pipeline->expected_out_channels, audio_chunk_get_channels(output));
    pipeline->last_error_needed = pipeline->expected_out_channels;
    pipeline->last_error_got = audio_chunk_get_channels(output);
    return PIPELINE_ERR_CHANNEL_COUNT_MISMATCH;
  }
  if (audio_chunk_get_frames(output) < valid_frames) {
    logger_warn(&g_logger,
                "Pipeline output buffer too small: needed %zu, got %zu",
                valid_frames, audio_chunk_get_frames(output));
    pipeline->last_error_needed = valid_frames;
    pipeline->last_error_got = audio_chunk_get_frames(output);
    return PIPELINE_ERR_OUTPUT_BUFFER_TOO_SMALL;
  }

  // 2. Copy input into our pre-allocated scratch.
  for (size_t ch = 0; ch < pipeline->expected_in_channels; ch++) {
    waveform_t src = audio_chunk_get_channel(input, ch);
    mutable_waveform_t dst =
        audio_chunk_get_channel(pipeline->capture_scratch, ch);
    if (src && dst && valid_frames > 0) {
      memcpy(dst, src, valid_frames * sizeof(double));
    }
  }
  audio_chunk_set_valid_frames(pipeline->capture_scratch, valid_frames);

  audio_chunk_t* current_chunk = pipeline->capture_scratch;

  // 3. Implicit main volume with smooth ramp.
  volume_filter_prepare_chunk(pipeline->master_volume);
  for (size_t ch = 0; ch < audio_chunk_get_channels(current_chunk); ch++) {
    mutable_waveform_t buf = audio_chunk_get_channel(current_chunk, ch);
    if (buf && valid_frames > 0) {
      g_volume_vtable.process(pipeline->master_volume, buf, valid_frames);
    }
  }
  volume_filter_advance_ramp(pipeline->master_volume);

  // 4. Execute pipeline steps sequentially.
  size_t mixer_idx = 0;
  for (size_t i = 0; i < pipeline->steps_count; i++) {
    pipeline_exec_step_t* step = &pipeline->steps[i];
    switch (step->type) {
      case EXEC_STEP_BIQUAD:
        if (step->biquad_step) {
          execute_biquad_step(step->biquad_step, current_chunk, valid_frames);
        }
        break;
      case EXEC_STEP_PARALLEL_FILTERS:
        execute_parallel_filters(pipeline, step, current_chunk, valid_frames);
        break;
      case EXEC_STEP_MIXER: {
        pipeline_error_t err = execute_mixer_step(
            pipeline, step, &current_chunk, &mixer_idx, valid_frames);
        if (err != PIPELINE_OK) {
          return err;
        }
        break;
      }
      case EXEC_STEP_PROCESSOR:
        if (step->processor) {
          dsp_processor_process(step->processor, current_chunk);
        }
        break;
    }
  }

  // 5. Copy the final computed samples from current_chunk to caller-supplied
  // output buffer.
  audio_chunk_set_valid_frames(output, valid_frames);
  size_t current_channels = audio_chunk_get_channels(current_chunk);
  for (size_t ch = 0; ch < pipeline->expected_out_channels; ch++) {
    mutable_waveform_t dst = audio_chunk_get_channel(output, ch);
    if (!dst || valid_frames == 0) continue;
    if (ch < current_channels) {
      waveform_t src = audio_chunk_get_channel(current_chunk, ch);
      if (src) {
        memcpy(dst, src, valid_frames * sizeof(double));
      } else {
        dsp_ops_clear(dst, valid_frames);
      }
    } else {
      dsp_ops_clear(dst, valid_frames);
    }
  }
  return PIPELINE_OK;
}

size_t pipeline_get_last_error_needed(const pipeline_t* pipeline) {
  return pipeline ? pipeline->last_error_needed : 0;
}

size_t pipeline_get_last_error_got(const pipeline_t* pipeline) {
  return pipeline ? pipeline->last_error_got : 0;
}
