#include "Pipeline/pipeline.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Audio/audio_chunk.h"
#include "Audio/processing_parameters.h"
#include "Config/config_error.h"
#include "Config/configuration.h"
#include "Config/engine_config_types.h"
#include "Config/filter_config_types.h"
#include "Config/processor_config_types.h"
#include "Filters/filter.h"
#include "Filters/volume.h"
#include "Logging/app_logger.h"
#include "Mixer/mixer.h"
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
// Internal Data Structures
// ============================================================================

/// A filter chain applied to a single channel in parallel.
typedef struct {
  size_t channel;
  filter_t** filters;
  size_t filters_count;
} parallel_filter_chain_t;

/// A single step in the processing pipeline
typedef enum {
  /// Contiguous filter chains that can be processed in parallel.
  EXEC_STEP_PARALLEL_FILTERS = 0,
  /// Mixer that changes channel routing.
  EXEC_STEP_MIXER,
  /// Audio processor applied to the chunk in-place.
  EXEC_STEP_PROCESSOR
} exec_step_type_t;

/// A single step in the processing pipeline
typedef struct {
  exec_step_type_t type;
  // For EXEC_STEP_PARALLEL_FILTERS:
  parallel_filter_chain_t* chains;
  size_t chains_count;
  // For EXEC_STEP_MIXER:
  mixer_t* mixer;
  // For EXEC_STEP_PROCESSOR:
  dsp_processor_t* processor;
} pipeline_exec_step_t;

/// The main audio processing pipeline.
struct pipeline_s {
  pipeline_exec_step_t* steps;
  size_t steps_count;
  bool multithreaded;
  /// Implicit main volume filter with smooth ramping
  volume_filter_t* master_volume;
  /// Working scratch the pipeline copies the caller's input into at the start
  /// of each `process(...)`. With class-owned `AudioBuffers`, we can no
  /// longer rely on CoW to isolate mutations from the caller's `input`
  /// chunk — so we copy explicitly into this pre-allocated buffer.
  audio_chunk_t* capture_scratch;
  /// Pre-allocated scratch chunks mapped by the sequential step index in
  /// `steps` array to prevent Copy-On-Write allocations on the hot path.
  audio_chunk_t** scratches_for_mixers;
  size_t scratches_for_mixers_count;

  size_t frames_per_chunk;
  int rate;
  size_t expected_in_channels;
  size_t expected_out_channels;

  // For test inspection on error:
  size_t last_error_needed;
  size_t last_error_got;
};

// ============================================================================
// Cleanup and Destruction Helpers
// ============================================================================

static void free_filter_chains(parallel_filter_chain_t* chains, size_t count) {
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

static void free_filter_chains_shallow(parallel_filter_chain_t* chains,
                                       size_t count) {
  if (!chains) return;
  for (size_t i = 0; i < count; i++) {
    if (chains[i].filters) {
      free(chains[i].filters);
    }
  }
  free(chains);
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
// State Transfer (Real-Time Audio Thread Safe - Zero Allocations)
// ============================================================================

/// Transfer filter states between two filter chains matching the same channel.
static void transfer_chain_filters(const parallel_filter_chain_t* dest_chain,
                                   const parallel_filter_chain_t* src_chain,
                                   bool* dest_used, size_t dest_count) {
  if (!dest_chain || !src_chain || src_chain->filters_count == 0 ||
      dest_chain->filters_count == 0) {
    return;
  }

  bool src_used[512] = {false};
  size_t max_src =
      src_chain->filters_count < 512 ? src_chain->filters_count : 512;

  for (size_t i = 0; i < dest_chain->filters_count; i++) {
    if (dest_used && i < dest_count && dest_used[i]) continue;
    filter_t* dest_f = dest_chain->filters[i];
    const char* dname = filter_get_name(dest_f);
    if (!dname || dname[0] == '\0') continue;

    for (size_t j = 0; j < max_src; j++) {
      if (src_used[j]) continue;
      filter_t* src_f = src_chain->filters[j];
      const char* sname = filter_get_name(src_f);
      if (sname && strcmp(dname, sname) == 0) {
        filter_transfer_state(dest_f, src_f);
        src_used[j] = true;
        if (dest_used && i < dest_count) dest_used[i] = true;
        break;
      }
    }
  }
}

/// Transfer state for named audio processors.
static void transfer_named_processors_state(pipeline_t* dest,
                                            const pipeline_t* src) {
  if (!dest || !src || !dest->steps || !src->steps) return;
  bool src_proc_used[128] = {false};
  size_t max_src = src->steps_count < 128 ? src->steps_count : 128;

  for (size_t di = 0; di < dest->steps_count; di++) {
    pipeline_exec_step_t* d_step = &dest->steps[di];
    if (d_step->type != EXEC_STEP_PROCESSOR || !d_step->processor) {
      continue;
    }
    const char* dname = dsp_processor_get_name(d_step->processor);
    if (!dname || dname[0] == '\0') continue;

    for (size_t si = 0; si < max_src; si++) {
      if (src_proc_used[si]) continue;
      pipeline_exec_step_t* s_step = &src->steps[si];
      if (s_step->type != EXEC_STEP_PROCESSOR || !s_step->processor ||
          s_step->processor->type != d_step->processor->type) {
        continue;
      }
      const char* sname = dsp_processor_get_name(s_step->processor);
      if (sname && strcmp(dname, sname) == 0) {
        dsp_processor_transfer_state(d_step->processor, s_step->processor);
        src_proc_used[si] = true;
        break;
      }
    }
  }
}

void pipeline_transfer_state(pipeline_t* dest, const pipeline_t* src) {
  if (!dest || !src) return;

  logger_info(&g_logger, "Starting pipeline state transfer");

  // 1. Transfer Master Volume state
  if (dest->master_volume && src->master_volume) {
    g_volume_vtable.transfer_state(dest->master_volume, src->master_volume);
    logger_info(&g_logger, "Transferred master volume filter state");
  }

  // 2. Transfer all channel filter states
  for (size_t di = 0; di < dest->steps_count; di++) {
    const pipeline_exec_step_t* d_step = &dest->steps[di];
    if (!d_step->chains) continue;

    for (size_t dc = 0; dc < d_step->chains_count; dc++) {
      const parallel_filter_chain_t* d_chain = &d_step->chains[dc];
      bool dest_used[512] = {false};

      for (size_t si = 0; si < src->steps_count; si++) {
        const pipeline_exec_step_t* s_step = &src->steps[si];
        if (!s_step->chains) continue;

        for (size_t sc = 0; sc < s_step->chains_count; sc++) {
          const parallel_filter_chain_t* s_chain = &s_step->chains[sc];
          if (s_chain->channel == d_chain->channel) {
            transfer_chain_filters(d_chain, s_chain, dest_used, 512);
          }
        }
      }
    }
  }

  // 3. Transfer named multi-channel processors
  transfer_named_processors_state(dest, src);

  logger_info(&g_logger, "Completed pipeline state transfer");
}

// ============================================================================
// Parallel Filter Step Lowering Helpers (Split into Positional Runs)
// ============================================================================

static bool append_exec_step(pipeline_exec_step_t** steps, size_t* count,
                             size_t* cap, pipeline_exec_step_t step) {
  if (*count >= *cap) {
    size_t new_cap = (*cap == 0) ? 8 : (*cap * 2);
    pipeline_exec_step_t* grown = (pipeline_exec_step_t*)realloc(
        *steps, new_cap * sizeof(pipeline_exec_step_t));
    if (!grown) return false;
    *steps = grown;
    *cap = new_cap;
  }
  (*steps)[(*count)++] = step;
  return true;
}

static dsp_processor_t* create_biquad_processor(
    const char* name, const parallel_filter_chain_t* chains,
    size_t chains_count) {
  size_t* channels = (size_t*)calloc(chains_count, sizeof(size_t));
  filter_t*** filters = (filter_t***)calloc(chains_count, sizeof(filter_t**));
  size_t* counts = (size_t*)calloc(chains_count, sizeof(size_t));
  if (!channels || !filters || !counts) {
    if (channels) free(channels);
    if (filters) free(filters);
    if (counts) free(counts);
    return NULL;
  }
  for (size_t c = 0; c < chains_count; c++) {
    channels[c] = chains[c].channel;
    filters[c] = chains[c].filters;
    counts[c] = chains[c].filters_count;
  }
  processor_config_t cfg = {
      .type = PROCESSOR_TYPE_BIQUAD,
      .parameters.biquad =
          {
              .channels = channels,
              .channels_count = chains_count,
              .filters = (filter_t* const* const*)filters,
              .filters_counts = counts,
          },
  };
  dsp_processor_t* p = dsp_processor_create(name, &cfg, 0, 0, NULL);
  free(channels);
  free(filters);
  free(counts);
  return p;
}

static parallel_filter_chain_t* slice_chains(const parallel_filter_chain_t* src,
                                             size_t chains_count, size_t start,
                                             size_t len) {
  parallel_filter_chain_t* out = (parallel_filter_chain_t*)calloc(
      chains_count, sizeof(parallel_filter_chain_t));
  if (!out) return NULL;
  for (size_t c = 0; c < chains_count; c++) {
    out[c].channel = src[c].channel;
    if (start < src[c].filters_count) {
      size_t avail = src[c].filters_count - start;
      size_t take = (avail < len) ? avail : len;
      out[c].filters_count = take;
      if (take > 0) {
        out[c].filters = (filter_t**)calloc(take, sizeof(filter_t*));
        if (!out[c].filters) {
          free_filter_chains_shallow(out, chains_count);
          return NULL;
        }
        memcpy(out[c].filters, src[c].filters + start,
               take * sizeof(filter_t*));
      }
    }
  }
  return out;
}

static bool is_biquad_at_pos(const parallel_filter_chain_t* chains,
                             size_t chains_count, size_t pos) {
  for (size_t c = 0; c < chains_count; c++) {
    if (pos >= chains[c].filters_count || !chains[c].filters[pos] ||
        chains[c].filters[pos]->type != FILTER_INSTANCE_BIQUAD) {
      return false;
    }
  }
  return true;
}

static bool split_and_lower_step(pipeline_exec_step_t* old_step,
                                 size_t step_idx,
                                 pipeline_exec_step_t** out_steps,
                                 size_t* out_count, size_t* out_cap) {
  size_t max_depth = 0;
  for (size_t c = 0; c < old_step->chains_count; c++) {
    if (old_step->chains[c].filters_count > max_depth) {
      max_depth = old_step->chains[c].filters_count;
    }
  }
  if (max_depth == 0) {
    return append_exec_step(out_steps, out_count, out_cap, *old_step);
  }

  size_t pos = 0;
  size_t run_idx = 0;
  while (pos < max_depth) {
    bool is_bq =
        is_biquad_at_pos(old_step->chains, old_step->chains_count, pos);
    size_t run_start = pos++;
    while (pos < max_depth &&
           is_biquad_at_pos(old_step->chains, old_step->chains_count, pos) ==
               is_bq) {
      pos++;
    }
    size_t run_len = pos - run_start;

    parallel_filter_chain_t* run_chains = slice_chains(
        old_step->chains, old_step->chains_count, run_start, run_len);
    if (!run_chains) return false;

    pipeline_exec_step_t new_step = {
        .type = EXEC_STEP_PARALLEL_FILTERS,
        .chains = run_chains,
        .chains_count = old_step->chains_count,
    };

    if (is_bq) {
      char name[64];
      const char* f0 =
          (run_chains[0].filters_count > 0 && run_chains[0].filters[0])
              ? filter_get_name(run_chains[0].filters[0])
              : NULL;
      if (f0 && f0[0]) {
        snprintf(name, sizeof(name), "biquad_proc_%s", f0);
      } else {
        snprintf(name, sizeof(name), "biquad_proc_step_%zu_run_%zu", step_idx,
                 run_idx);
      }
      dsp_processor_t* bq_proc =
          create_biquad_processor(name, run_chains, old_step->chains_count);
      if (bq_proc) {
        new_step.type = EXEC_STEP_PROCESSOR;
        new_step.processor = bq_proc;
      }
    }

    if (!append_exec_step(out_steps, out_count, out_cap, new_step)) {
      return false;
    }
    run_idx++;
  }

  for (size_t c = 0; c < old_step->chains_count; c++) {
    if (old_step->chains[c].filters) {
      free(old_step->chains[c].filters);
    }
  }
  free(old_step->chains);
  old_step->chains = NULL;
  old_step->chains_count = 0;
  return true;
}

static bool lower_pipeline_steps(pipeline_t* pipeline, size_t unlowered_count,
                                 config_error_t* err) {
  pipeline_exec_step_t* lowered_steps = NULL;
  size_t lowered_count = 0;
  size_t lowered_cap = 0;

  for (size_t s = 0; s < unlowered_count; s++) {
    pipeline_exec_step_t* step = &pipeline->steps[s];
    if (step->type == EXEC_STEP_PARALLEL_FILTERS && step->chains &&
        step->chains_count > 0) {
      if (!split_and_lower_step(step, s, &lowered_steps, &lowered_count,
                                &lowered_cap)) {
        config_error_set(err, CONFIG_ERR_PARSE,
                         "Memory allocation failure during filter lowering");
        free_exec_steps(lowered_steps, lowered_count);
        return false;
      }
    } else {
      if (!append_exec_step(&lowered_steps, &lowered_count, &lowered_cap,
                            *step)) {
        config_error_set(err, CONFIG_ERR_PARSE,
                         "Memory allocation failure during step appending");
        free_exec_steps(lowered_steps, lowered_count);
        return false;
      }
      step->chains = NULL;
      step->chains_count = 0;
      step->mixer = NULL;
      step->processor = NULL;
    }
  }

  free(pipeline->steps);
  pipeline->steps = lowered_steps;
  pipeline->steps_count = lowered_count;
  return true;
}

// ============================================================================
// Pipeline Step Construction Helpers
// ============================================================================

static void count_pipeline_requirements(const dsp_config_t* config,
                                        size_t* out_total_steps,
                                        size_t* out_num_mixers) {
  *out_total_steps = 0;
  *out_num_mixers = 0;
  if (!config->pipeline || config->pipeline_count == 0) return;

  for (size_t i = 0; i < config->pipeline_count; i++) {
    const pipeline_step_config_t* step = &config->pipeline[i];
    if (step->bypassed) continue;
    if (step->type == PIPELINE_STEP_TYPE_FILTER) {
      (*out_total_steps)++;
    } else if (step->type == PIPELINE_STEP_TYPE_MIXER) {
      (*out_total_steps)++;
      (*out_num_mixers)++;
    } else if (step->type == PIPELINE_STEP_TYPE_PROCESSOR) {
      (*out_total_steps)++;
    }
  }
}

static bool resolve_filter_step_channels(
    const pipeline_step_config_t* step, size_t current_channels,
    size_t** out_channels, size_t* out_count, size_t* out_single_ch,
    bool* out_is_allocated, config_error_t* err) {
  *out_is_allocated = false;
  if (step->channels && step->channels_count > 0) {
    *out_channels = step->channels;
    *out_count = step->channels_count;
    return true;
  }
  if (step->has_channel) {
    *out_single_ch = step->channel;
    *out_channels = out_single_ch;
    *out_count = 1;
    return true;
  }
  if (current_channels > SIZE_MAX / sizeof(size_t)) {
    config_error_set(err, CONFIG_ERR_PARSE,
                     "Integer overflow in channels count");
    return false;
  }
  size_t* all_chs = (size_t*)calloc(current_channels, sizeof(size_t));
  if (!all_chs) {
    config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
    return false;
  }
  for (size_t c = 0; c < current_channels; c++) {
    all_chs[c] = c;
  }
  *out_channels = all_chs;
  *out_count = current_channels;
  *out_is_allocated = true;
  return true;
}

static parallel_filter_chain_t* build_filter_chains(
    const pipeline_step_config_t* step, const dsp_config_t* config, int rate,
    size_t frames_per_chunk, processing_parameters_t* proc_params,
    const size_t* channels, size_t channels_count, config_error_t* err) {
  parallel_filter_chain_t* chains = (parallel_filter_chain_t*)calloc(
      channels_count, sizeof(parallel_filter_chain_t));
  if (!chains) {
    config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
    return NULL;
  }

  for (size_t c = 0; c < channels_count; c++) {
    parallel_filter_chain_t* chain = &chains[c];
    chain->channel = channels[c];
    chain->filters_count = step->names_count;
    chain->filters = (filter_t**)calloc(step->names_count, sizeof(filter_t*));
    if (!chain->filters) {
      config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
      free_filter_chains(chains, channels_count);
      return NULL;
    }

    for (size_t j = 0; j < step->names_count; j++) {
      const filter_config_t* f_cfg =
          dsp_config_get_filter(config, step->names[j]);
      if (!f_cfg) {
        config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                         "Filter '%s' not defined", step->names[j]);
        free_filter_chains(chains, channels_count);
        return NULL;
      }
      filter_t* f = filter_create(step->names[j], f_cfg, rate, frames_per_chunk,
                                  proc_params, err);
      if (!f) {
        free_filter_chains(chains, channels_count);
        return NULL;
      }
      chain->filters[j] = f;
    }
  }
  return chains;
}

static bool merge_parallel_filter_chains(pipeline_exec_step_t* last,
                                         parallel_filter_chain_t* new_chains,
                                         size_t new_chains_count,
                                         config_error_t* err) {
  for (size_t c = 0; c < new_chains_count; c++) {
    parallel_filter_chain_t* new_chain = &new_chains[c];
    int found_idx = -1;
    for (size_t k = 0; k < last->chains_count; k++) {
      if (last->chains[k].channel == new_chain->channel) {
        found_idx = (int)k;
        break;
      }
    }

    if (found_idx != -1) {
      parallel_filter_chain_t* old_chain = &last->chains[found_idx];
      size_t combined_count =
          old_chain->filters_count + new_chain->filters_count;
      filter_t** combined_filters = (filter_t**)realloc(
          old_chain->filters, combined_count * sizeof(filter_t*));
      if (!combined_filters) {
        config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
        return false;
      }
      memcpy(combined_filters + old_chain->filters_count, new_chain->filters,
             new_chain->filters_count * sizeof(filter_t*));
      old_chain->filters = combined_filters;
      old_chain->filters_count = combined_count;
      free(new_chain->filters);
      new_chain->filters = NULL;
    } else {
      parallel_filter_chain_t* merged = (parallel_filter_chain_t*)realloc(
          last->chains,
          (last->chains_count + 1) * sizeof(parallel_filter_chain_t));
      if (!merged) {
        config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
        return false;
      }
      merged[last->chains_count] = *new_chain;
      last->chains = merged;
      last->chains_count += 1;
      new_chain->filters = NULL;
    }
  }
  free(new_chains);
  return true;
}

static bool build_filter_step(const pipeline_step_config_t* step,
                              const dsp_config_t* config, pipeline_t* pipeline,
                              processing_parameters_t* proc_params,
                              size_t current_channels, size_t* inout_exec_idx,
                              config_error_t* err) {
  if (!step->names || step->names_count == 0) {
    config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                     "Filter step missing names");
    return false;
  }

  size_t* channels = NULL;
  size_t channels_count = 0;
  size_t single_ch = 0;
  bool is_allocated = false;

  if (!resolve_filter_step_channels(step, current_channels, &channels,
                                    &channels_count, &single_ch, &is_allocated,
                                    err)) {
    return false;
  }

  parallel_filter_chain_t* chains = build_filter_chains(
      step, config, pipeline->rate, pipeline->frames_per_chunk, proc_params,
      channels, channels_count, err);
  if (is_allocated) {
    free(channels);
  }
  if (!chains) {
    return false;
  }

  size_t exec_idx = *inout_exec_idx;
  if (exec_idx > 0 &&
      pipeline->steps[exec_idx - 1].type == EXEC_STEP_PARALLEL_FILTERS) {
    if (!merge_parallel_filter_chains(&pipeline->steps[exec_idx - 1], chains,
                                      channels_count, err)) {
      free_filter_chains(chains, channels_count);
      return false;
    }
  } else {
    for (size_t c = 0; c < channels_count; c++) {
      for (size_t k = 0; k < c; k++) {
        if (chains[c].channel == chains[k].channel) {
          config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                           "Duplicate channel %zu in parallel filter step",
                           chains[c].channel);
          free_filter_chains(chains, channels_count);
          return false;
        }
      }
    }
    pipeline_exec_step_t* exec = &pipeline->steps[exec_idx];
    exec->type = EXEC_STEP_PARALLEL_FILTERS;
    exec->chains = chains;
    exec->chains_count = channels_count;
    *inout_exec_idx = exec_idx + 1;
  }
  return true;
}

static bool build_mixer_step(const pipeline_step_config_t* step,
                             const dsp_config_t* config, pipeline_t* pipeline,
                             size_t* inout_channels, size_t* inout_exec_idx,
                             size_t* inout_mixer_idx, config_error_t* err) {
  if (!step->has_name || step->name[0] == '\0') {
    config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                     "Mixer step missing name or config");
    return false;
  }
  const mixer_config_t* m_cfg = dsp_config_get_mixer(config, step->name);
  if (!m_cfg) {
    config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                     "Mixer step missing name or config");
    return false;
  }
  mixer_t* m = mixer_create(step->name, m_cfg, pipeline->frames_per_chunk, err);
  if (!m) {
    config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                     "Failed to create mixer '%s'", step->name);
    return false;
  }
  *inout_channels = m_cfg->channels_out;
  audio_chunk_t* scratch =
      audio_chunk_create(pipeline->frames_per_chunk, *inout_channels);
  if (!scratch) {
    mixer_free(m);
    config_error_set(err, CONFIG_ERR_PARSE,
                     "Failed to allocate mixer scratch buffer");
    return false;
  }
  pipeline->scratches_for_mixers[(*inout_mixer_idx)++] = scratch;

  pipeline_exec_step_t* exec = &pipeline->steps[(*inout_exec_idx)++];
  exec->type = EXEC_STEP_MIXER;
  exec->mixer = m;
  logger_debug(&g_logger, "Mixer '%s' added to pipeline", mixer_get_name(m));
  return true;
}

static bool build_processor_step(const pipeline_step_config_t* step,
                                 const dsp_config_t* config,
                                 pipeline_t* pipeline, size_t* inout_exec_idx,
                                 config_error_t* err) {
  if (!step->has_name || step->name[0] == '\0') {
    config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                     "Processor step missing name or config");
    return false;
  }
  const processor_config_t* p_cfg =
      dsp_config_get_processor(config, step->name);
  if (!p_cfg) {
    config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                     "Processor step missing name or config");
    return false;
  }
  dsp_processor_t* p = dsp_processor_create(step->name, p_cfg, pipeline->rate,
                                            pipeline->frames_per_chunk, err);
  if (!p) {
    return false;
  }
  pipeline_exec_step_t* exec = &pipeline->steps[(*inout_exec_idx)++];
  exec->type = EXEC_STEP_PROCESSOR;
  exec->processor = p;
  return true;
}

// ============================================================================
// Pipeline Lifecycle Functions
// ============================================================================

pipeline_t* pipeline_create(const dsp_config_t* config,
                            processing_parameters_t* proc_params,
                            size_t explicit_chunk_size, config_error_t* err) {
  if (pipeline_config_validate(config, err) != 0) return NULL;
  pipeline_t* pipeline = (pipeline_t*)calloc(1, sizeof(pipeline_t));
  if (!pipeline) {
    logger_error(&g_logger,
                 "Pipeline creation failed: Memory allocation failure");
    config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
    return NULL;
  }

  pipeline->frames_per_chunk =
      explicit_chunk_size > 0 ? explicit_chunk_size : config->devices.chunksize;
  pipeline->rate = config->devices.samplerate;
  pipeline->expected_in_channels =
      capture_device_config_get_channels(&config->devices.capture);
  pipeline->multithreaded =
      config->devices.has_multithreaded ? config->devices.multithreaded : false;

  logger_info(&g_logger,
              "Initializing DSP pipeline (sample_rate=%d, chunk_size=%zu, "
              "in_channels=%zu, multithreaded=%d)",
              pipeline->rate, pipeline->frames_per_chunk,
              pipeline->expected_in_channels, pipeline->multithreaded ? 1 : 0);

  // 1. Create the implicit master volume filter
  volume_config_t vol_params = {
      .ramp_time_ms = config->devices.has_volume_ramp_time_ms
                          ? config->devices.volume_ramp_time_ms
                          : 400.0,
      .has_ramp_time_ms = true,
      .limit = config->devices.has_volume_limit ? config->devices.volume_limit
                                                : 50.0,
      .has_limit = true,
      .fader = FADER_MAIN};

  filter_config_t vcfg = {.type = FILTER_TYPE_VOLUME,
                          .parameters.volume = vol_params};
  pipeline->master_volume = (volume_filter_t*)g_volume_vtable.create(
      "master_volume", &vcfg, pipeline->rate, pipeline->frames_per_chunk,
      proc_params, err);
  if (!pipeline->master_volume) {
    logger_error(
        &g_logger,
        "Failed to create master volume filter (rate=%d, chunk=%zu): %s",
        pipeline->rate, pipeline->frames_per_chunk,
        err ? err->message : "unknown error");
    pipeline_free(pipeline);
    return NULL;
  }

  // 2. Pre-allocate the capture scratch buffer
  pipeline->capture_scratch = audio_chunk_create(
      pipeline->frames_per_chunk, pipeline->expected_in_channels);
  if (!pipeline->capture_scratch) {
    logger_error(
        &g_logger,
        "Failed to allocate capture scratch buffer (frames=%zu, channels=%zu)",
        pipeline->frames_per_chunk, pipeline->expected_in_channels);
    config_error_set(err, CONFIG_ERR_PARSE,
                     "Failed to allocate capture scratch buffer");
    pipeline_free(pipeline);
    return NULL;
  }

  // 3. Count steps and allocate step & mixer scratch arrays
  size_t total_exec_steps = 0;
  size_t num_mixers = 0;
  count_pipeline_requirements(config, &total_exec_steps, &num_mixers);

  if (total_exec_steps == 0) {
    pipeline->steps_count = 0;
    pipeline->expected_out_channels = pipeline->expected_in_channels;
    return pipeline;
  }

  pipeline->steps = (pipeline_exec_step_t*)calloc(total_exec_steps,
                                                  sizeof(pipeline_exec_step_t));
  if (!pipeline->steps) {
    config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
    pipeline_free(pipeline);
    return NULL;
  }
  pipeline->steps_count = total_exec_steps;

  if (num_mixers > 0) {
    pipeline->scratches_for_mixers =
        (audio_chunk_t**)calloc(num_mixers, sizeof(audio_chunk_t*));
    if (!pipeline->scratches_for_mixers) {
      config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
      pipeline_free(pipeline);
      return NULL;
    }
    pipeline->scratches_for_mixers_count = num_mixers;
  }

  // 4. Build execution steps sequentially
  size_t current_channels = pipeline->expected_in_channels;
  size_t exec_idx = 0;
  size_t mixer_idx = 0;

  if (config->pipeline && config->pipeline_count > 0) {
    for (size_t i = 0; i < config->pipeline_count; i++) {
      const pipeline_step_config_t* step = &config->pipeline[i];
      if (step->bypassed) continue;

      bool ok = false;
      switch (step->type) {
        case PIPELINE_STEP_TYPE_FILTER:
          ok = build_filter_step(step, config, pipeline, proc_params,
                                 current_channels, &exec_idx, err);
          break;
        case PIPELINE_STEP_TYPE_MIXER:
          ok = build_mixer_step(step, config, pipeline, &current_channels,
                                &exec_idx, &mixer_idx, err);
          break;
        case PIPELINE_STEP_TYPE_PROCESSOR:
          ok = build_processor_step(step, config, pipeline, &exec_idx, err);
          break;
      }
      if (!ok) {
        pipeline_free(pipeline);
        return NULL;
      }
    }
  }

  // 5. Lower biquad sections of parallel filter steps into biquad processors
  if (!lower_pipeline_steps(pipeline, exec_idx, err)) {
    pipeline_free(pipeline);
    return NULL;
  }

  pipeline->expected_out_channels = current_channels;
  return pipeline;
}

// ============================================================================
// Audio Processing Loop
// ============================================================================

#if defined(ENABLE_LIBDISPATCH)
typedef struct {
  audio_chunk_t* current_chunk;
  size_t valid_frames;
  parallel_filter_chain_t* chains;
  int rate;
} dispatch_ctx_t;

static void parallel_filter_worker(void* context, size_t idx) {
  dispatch_ctx_t* ctx = (dispatch_ctx_t*)context;
  parallel_filter_chain_t* chain = &ctx->chains[idx];
  if ((size_t)chain->channel >= audio_chunk_get_channels(ctx->current_chunk)) {
    return;
  }
  mutable_waveform_t buf =
      audio_chunk_get_channel(ctx->current_chunk, chain->channel);
  if (!buf) return;
  for (size_t j = 0; j < chain->filters_count; j++) {
    if (chain->filters[j] && ctx->valid_frames > 0) {
      filter_process(chain->filters[j], buf, ctx->valid_frames);
    }
  }
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
    dispatch_ctx_t dctx = {current_chunk, valid_frames, step->chains,
                           pipeline->rate};
    dispatch_queue_t queue =
        dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_HIGH, 0);
    dispatch_apply_f(step->chains_count, queue, &dctx, parallel_filter_worker);
#elif defined(ENABLE_OPENMP)
#pragma omp parallel num_threads(step->chains_count)
    {
#pragma omp for
      for (size_t idx = 0; idx < step->chains_count; idx++) {
        parallel_filter_chain_t* chain = &step->chains[idx];
        if ((size_t)chain->channel >= audio_chunk_get_channels(current_chunk))
          continue;
        mutable_waveform_t buf =
            audio_chunk_get_channel(current_chunk, chain->channel);
        if (!buf) continue;
        for (size_t j = 0; j < chain->filters_count; j++) {
          if (chain->filters[j] && valid_frames > 0) {
            filter_process(chain->filters[j], buf, valid_frames);
          }
        }
      }
    }
#endif
  } else {
    for (size_t idx = 0; idx < step->chains_count; idx++) {
      parallel_filter_chain_t* chain = &step->chains[idx];
      if ((size_t)chain->channel >= audio_chunk_get_channels(current_chunk))
        continue;
      mutable_waveform_t buf =
          audio_chunk_get_channel(current_chunk, chain->channel);
      if (!buf) continue;
      for (size_t j = 0; j < chain->filters_count; j++) {
        if (chain->filters[j] && valid_frames > 0) {
          filter_process(chain->filters[j], buf, valid_frames);
        }
      }
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

// ============================================================================
// Configuration Validation
// ============================================================================

static bool validate_filter_step(const pipeline_step_config_t* step,
                                 size_t step_idx, size_t num_channels,
                                 const dsp_config_t* config,
                                 config_error_t* err) {
  if (!step->names || step->names_count == 0) {
    config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                     "Filter step %zu must have 'names'", step_idx);
    return false;
  }
  for (size_t j = 0; j < step->names_count; j++) {
    if (!step->names[j] || step->names[j][0] == '\0') {
      config_error_set(
          err, CONFIG_ERR_INVALID_PIPELINE,
          "Filter step %zu has invalid/empty filter name at index %zu",
          step_idx, j);
      return false;
    }
    if (!dsp_config_get_filter(config, step->names[j])) {
      config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                       "Filter '%s' referenced in pipeline but not defined",
                       step->names[j]);
      return false;
    }
  }
  if (step->has_channel) {
    if (step->channel >= num_channels) {
      config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                       "Filter step %zu references channel %zu but "
                       "pipeline only has %zu channel(s) at this point",
                       step_idx, step->channel, num_channels);
      return false;
    }
  }
  for (size_t j = 0; j < step->channels_count; j++) {
    if (step->channels[j] >= num_channels) {
      config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                       "Filter step %zu references channel %zu but "
                       "pipeline only has %zu channel(s) at this point",
                       step_idx, step->channels[j], num_channels);
      return false;
    }
    for (size_t k = 0; k < j; k++) {
      if (step->channels[j] == step->channels[k]) {
        config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                         "Filter step %zu references duplicated channel %zu",
                         step_idx, step->channels[j]);
        return false;
      }
    }
  }
  return true;
}

static bool validate_mixer_step(const pipeline_step_config_t* step,
                                size_t step_idx, size_t* inout_channels,
                                const dsp_config_t* config,
                                config_error_t* err) {
  if (!step->has_name || step->name[0] == '\0') {
    config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                     "Mixer step %zu must have 'name'", step_idx);
    return false;
  }
  const mixer_config_t* mixer = dsp_config_get_mixer(config, step->name);
  if (!mixer) {
    config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                     "Mixer '%s' referenced in pipeline but not defined",
                     step->name);
    return false;
  }
  if (mixer->channels_in != *inout_channels) {
    config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                     "Mixer '%s' expects %zu input channel(s) but "
                     "pipeline has %zu at this point",
                     step->name, mixer->channels_in, *inout_channels);
    return false;
  }
  *inout_channels = mixer->channels_out;
  return true;
}

static bool validate_processor_step(const pipeline_step_config_t* step,
                                    size_t step_idx, size_t num_channels,
                                    const dsp_config_t* config,
                                    config_error_t* err) {
  if (!step->has_name || step->name[0] == '\0') {
    config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                     "Processor step %zu must have 'name'", step_idx);
    return false;
  }
  const processor_config_t* proc = dsp_config_get_processor(config, step->name);
  if (!proc) {
    config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                     "Processor '%s' referenced in pipeline but not defined",
                     step->name);
    return false;
  }
  size_t expected_channels = 0;
  switch (proc->type) {
    case PROCESSOR_TYPE_COMPRESSOR:
      expected_channels = proc->parameters.compressor.channels;
      break;
    case PROCESSOR_TYPE_NOISE_GATE:
      expected_channels = proc->parameters.noise_gate.channels;
      break;
    case PROCESSOR_TYPE_RACE:
      expected_channels = proc->parameters.race.channels;
      break;
    case PROCESSOR_TYPE_LOOKAHEAD_LIMITER:
      expected_channels = proc->parameters.lookahead_limiter.channels;
      break;
    case PROCESSOR_TYPE_BIQUAD:
      expected_channels = proc->parameters.biquad.channels_count;
      break;
    case PROCESSOR_TYPE_INVALID:
      break;
  }
  if (expected_channels != num_channels) {
    config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                     "Processor '%s' expects %zu channel(s) but pipeline "
                     "has %zu at this point",
                     step->name, expected_channels, num_channels);
    return false;
  }
  return true;
}

int pipeline_config_validate(const dsp_config_t* config, config_error_t* err) {
  if (!config) {
    config_error_set(err, CONFIG_ERR_PARSE, "Configuration is null");
    return -1;
  }

  size_t num_channels =
      capture_device_config_get_channels(&config->devices.capture);
  if (num_channels == 0) {
    if (config->devices.capture.type == AUDIO_BACKEND_TYPE_FILE &&
        config->devices.capture.is_wav) {
      config_error_set(
          err, CONFIG_ERR_INVALID_PIPELINE,
          "Failed to open WAV capture file '%s' or parse channels from header",
          config->devices.capture.cfg.wav_file.filename);
    } else {
      config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                       "Invalid capture channel count: %zu", num_channels);
    }
    return -1;
  }

  for (size_t i = 0; i < config->pipeline_count; i++) {
    const pipeline_step_config_t* step = &config->pipeline[i];
    if (step->bypassed) continue;

    bool ok = false;
    switch (step->type) {
      case PIPELINE_STEP_TYPE_FILTER:
        ok = validate_filter_step(step, i, num_channels, config, err);
        break;
      case PIPELINE_STEP_TYPE_MIXER:
        ok = validate_mixer_step(step, i, &num_channels, config, err);
        break;
      case PIPELINE_STEP_TYPE_PROCESSOR:
        ok = validate_processor_step(step, i, num_channels, config, err);
        break;
    }
    if (!ok) {
      return -1;
    }
  }

  size_t playback_channels =
      playback_device_config_get_channels(&config->devices.playback);
  if (num_channels != playback_channels) {
    config_error_set(
        err, CONFIG_ERR_INVALID_PIPELINE,
        "Pipeline outputs %zu channel(s) but playback device expects %zu",
        num_channels, playback_channels);
    return -1;
  }

  return 0;
}
