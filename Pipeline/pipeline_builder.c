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
#include "Filters/biquad.h"
#include "Filters/biquad_combo.h"
#include "Filters/filter.h"
#include "Filters/volume.h"
#include "Logging/app_logger.h"
#include "Mixer/mixer.h"
#include "Pipeline/pipeline.h"
#include "Pipeline/pipeline_internal.h"
#include "Processors/processor.h"

static const logger_t g_logger = {"dsp.pipeline"};

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

typedef struct {
  bool is_biquads;
  size_t start;
  size_t len;
} filter_run_t;

static size_t find_biquad_runs(const char* const* names, size_t names_count,
                               const dsp_config_t* config,
                               filter_run_t* out_runs, size_t max_runs) {
  if (names_count == 0) return 0;
  size_t runs_count = 0;
  for (size_t i = 0; i < names_count; i++) {
    const filter_config_t* f_cfg = dsp_config_get_filter(config, names[i]);
    bool is_bq = filter_config_is_biquad(f_cfg);
    if (runs_count > 0 && out_runs[runs_count - 1].is_biquads == is_bq) {
      out_runs[runs_count - 1].len += 1;
    } else {
      if (runs_count < max_runs) {
        out_runs[runs_count].is_biquads = is_bq;
        out_runs[runs_count].start = i;
        out_runs[runs_count].len = 1;
        runs_count++;
      }
    }
  }
  return runs_count;
}

static biquad_step_t* build_biquad_step(const char* const* names,
                                        size_t names_count,
                                        const dsp_config_t* config, int rate,
                                        const size_t* channels,
                                        size_t channels_count,
                                        config_error_t* err) {
  biquad_step_t* step = (biquad_step_t*)calloc(1, sizeof(biquad_step_t));
  if (!step) {
    config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
    return NULL;
  }

  step->channels_count = channels_count;
  step->filters_count = names_count;
  step->samplerate = rate;

  step->channel_of = (size_t*)calloc(channels_count, sizeof(size_t));
  if (!step->channel_of) {
    config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
    biquad_step_free(step);
    return NULL;
  }
  memcpy(step->channel_of, channels, channels_count * sizeof(size_t));

  step->filters = (filter_t***)calloc(channels_count, sizeof(filter_t**));
  if (!step->filters) {
    config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
    biquad_step_free(step);
    return NULL;
  }

  for (size_t c = 0; c < channels_count; c++) {
    step->filters[c] = (filter_t**)calloc(names_count, sizeof(filter_t*));
    if (!step->filters[c]) {
      config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
      biquad_step_free(step);
      return NULL;
    }
    for (size_t i = 0; i < names_count; i++) {
      const char* name = names[i];
      const filter_config_t* f_cfg = dsp_config_get_filter(config, name);
      if (!f_cfg) {
        config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                         "Filter '%s' not defined", name);
        biquad_step_free(step);
        return NULL;
      }
      if (!filter_config_is_biquad(f_cfg)) {
        config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                         "Filter '%s' is not biquad-based", name);
        biquad_step_free(step);
        return NULL;
      }
      filter_t* f = filter_create(name, f_cfg, rate, 0, NULL, err);
      if (!f) {
        biquad_step_free(step);
        return NULL;
      }
      step->filters[c][i] = f;
    }
  }

  size_t total_stages = 0;
  if (channels_count > 0) {
    for (size_t i = 0; i < names_count; i++) {
      total_stages += filter_get_biquad_stage_count(step->filters[0][i]);
    }
  }
  step->cascade_depth = total_stages;

  step->cascades =
      (biquad_filter_t***)calloc(channels_count, sizeof(biquad_filter_t**));
  if (!step->cascades) {
    config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
    biquad_step_free(step);
    return NULL;
  }

  if (total_stages > 0) {
    for (size_t c = 0; c < channels_count; c++) {
      step->cascades[c] =
          (biquad_filter_t**)calloc(total_stages, sizeof(biquad_filter_t*));
      if (!step->cascades[c]) {
        config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
        biquad_step_free(step);
        return NULL;
      }
      size_t stage_offset = 0;
      for (size_t i = 0; i < names_count; i++) {
        size_t count = filter_get_biquad_stages(
            step->filters[c][i], &step->cascades[c][stage_offset],
            total_stages - stage_offset);
        stage_offset += count;
      }
    }
  }

  step->live = (size_t*)calloc(channels_count, sizeof(size_t));
  if (!step->live) {
    config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
    biquad_step_free(step);
    return NULL;
  }
  step->live_count = 0;
  size_t init_wf_cap = channels_count > 16 ? channels_count : 16;
  step->waveforms = (double**)calloc(init_wf_cap, sizeof(double*));
  if (!step->waveforms) {
    config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
    biquad_step_free(step);
    return NULL;
  }
  step->waveforms_capacity = init_wf_cap;

  return step;
}

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

static parallel_filter_chain_t* build_filter_chains_slice(
    const char* const* names, size_t names_count, const dsp_config_t* config,
    int rate, size_t frames_per_chunk, processing_parameters_t* proc_params,
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
    chain->filters_count = names_count;
    chain->filters = (filter_t**)calloc(names_count, sizeof(filter_t*));
    if (!chain->filters) {
      config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
      free_filter_chains(chains, channels_count);
      return NULL;
    }

    for (size_t j = 0; j < names_count; j++) {
      const filter_config_t* f_cfg = dsp_config_get_filter(config, names[j]);
      if (!f_cfg) {
        config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                         "Filter '%s' not defined", names[j]);
        free_filter_chains(chains, channels_count);
        return NULL;
      }
      filter_t* f = filter_create(names[j], f_cfg, rate, frames_per_chunk,
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
                              size_t current_channels, size_t* inout_cap,
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

  if (channels_count == 0) {
    if (is_allocated) free(channels);
    return true;
  }

  for (size_t c = 0; c < channels_count; c++) {
    for (size_t k = 0; k < c; k++) {
      if (channels[c] == channels[k]) {
        config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                         "Duplicate channel %zu in parallel filter step",
                         channels[c]);
        if (is_allocated) free(channels);
        return false;
      }
    }
  }

  filter_run_t runs[64];
  size_t num_runs = find_biquad_runs((const char* const*)step->names,
                                     step->names_count, config, runs, 64);

  for (size_t r = 0; r < num_runs; r++) {
    const char* const* run_names =
        (const char* const*)&step->names[runs[r].start];
    size_t run_len = runs[r].len;

    if (runs[r].is_biquads) {
      biquad_step_t* bq =
          build_biquad_step(run_names, run_len, config, pipeline->rate,
                            channels, channels_count, err);
      if (!bq) {
        if (is_allocated) free(channels);
        return false;
      }
      pipeline_exec_step_t exec = {.type = EXEC_STEP_BIQUAD, .biquad_step = bq};
      if (!append_exec_step(&pipeline->steps, &pipeline->steps_count, inout_cap,
                            exec)) {
        biquad_step_free(bq);
        if (is_allocated) free(channels);
        config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
        return false;
      }
    } else {
      parallel_filter_chain_t* chains =
          build_filter_chains_slice(run_names, run_len, config, pipeline->rate,
                                    pipeline->frames_per_chunk, proc_params,
                                    channels, channels_count, err);
      if (!chains) {
        if (is_allocated) free(channels);
        return false;
      }

      size_t count = pipeline->steps_count;
      if (count > 0 &&
          pipeline->steps[count - 1].type == EXEC_STEP_PARALLEL_FILTERS) {
        if (!merge_parallel_filter_chains(&pipeline->steps[count - 1], chains,
                                          channels_count, err)) {
          free_filter_chains(chains, channels_count);
          if (is_allocated) free(channels);
          return false;
        }
      } else {
        pipeline_exec_step_t exec = {.type = EXEC_STEP_PARALLEL_FILTERS,
                                     .chains = chains,
                                     .chains_count = channels_count};
        if (!append_exec_step(&pipeline->steps, &pipeline->steps_count,
                              inout_cap, exec)) {
          free_filter_chains(chains, channels_count);
          if (is_allocated) free(channels);
          config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
          return false;
        }
      }
    }
  }

  if (is_allocated) free(channels);
  return true;
}

static bool build_mixer_step(const pipeline_step_config_t* step,
                             const dsp_config_t* config, pipeline_t* pipeline,
                             size_t* inout_channels, size_t* inout_cap,
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

  pipeline_exec_step_t exec = {.type = EXEC_STEP_MIXER, .mixer = m};
  if (!append_exec_step(&pipeline->steps, &pipeline->steps_count, inout_cap,
                        exec)) {
    mixer_free(m);
    config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
    return false;
  }
  logger_debug(&g_logger, "Mixer '%s' added to pipeline", mixer_get_name(m));
  return true;
}

static bool build_processor_step(const pipeline_step_config_t* step,
                                 const dsp_config_t* config,
                                 pipeline_t* pipeline, size_t* inout_cap,
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
  pipeline_exec_step_t exec = {.type = EXEC_STEP_PROCESSOR, .processor = p};
  if (!append_exec_step(&pipeline->steps, &pipeline->steps_count, inout_cap,
                        exec)) {
    dsp_processor_free(p);
    config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
    return false;
  }
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

  // 3. Count steps and allocate mixer scratch arrays
  size_t total_exec_steps = 0;
  size_t num_mixers = 0;
  count_pipeline_requirements(config, &total_exec_steps, &num_mixers);

  if (total_exec_steps == 0) {
    pipeline->steps_count = 0;
    pipeline->expected_out_channels = pipeline->expected_in_channels;
    return pipeline;
  }

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
  size_t mixer_idx = 0;
  size_t steps_cap = total_exec_steps > 0 ? total_exec_steps : 8;
  pipeline->steps =
      (pipeline_exec_step_t*)calloc(steps_cap, sizeof(pipeline_exec_step_t));
  if (!pipeline->steps) {
    config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
    pipeline_free(pipeline);
    return NULL;
  }
  pipeline->steps_count = 0;

  if (config->pipeline && config->pipeline_count > 0) {
    for (size_t i = 0; i < config->pipeline_count; i++) {
      const pipeline_step_config_t* step = &config->pipeline[i];
      if (step->bypassed) continue;

      bool ok = false;
      switch (step->type) {
        case PIPELINE_STEP_TYPE_FILTER:
          ok = build_filter_step(step, config, pipeline, proc_params,
                                 current_channels, &steps_cap, err);
          break;
        case PIPELINE_STEP_TYPE_MIXER:
          ok = build_mixer_step(step, config, pipeline, &current_channels,
                                &steps_cap, &mixer_idx, err);
          break;
        case PIPELINE_STEP_TYPE_PROCESSOR:
          ok = build_processor_step(step, config, pipeline, &steps_cap, err);
          break;
      }
      if (!ok) {
        pipeline_free(pipeline);
        return NULL;
      }
    }
  }

  pipeline->expected_out_channels = current_channels;
  return pipeline;
}
