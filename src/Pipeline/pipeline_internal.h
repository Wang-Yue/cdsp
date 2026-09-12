#ifndef CLIB_PIPELINE_PIPELINE_INTERNAL_H
#define CLIB_PIPELINE_PIPELINE_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
#include "Processors/processor.h"

/// A filter chain applied to a single channel in parallel.
typedef struct {
  size_t channel;
  filter_t** filters;
  size_t filters_count;
} parallel_filter_chain_t;

/// A run of biquad-based filters from one config filter step, compiled into
/// one cascade per channel.
typedef struct {
  size_t* channel_of;
  size_t channels_count;
  filter_t*** filters;
  size_t filters_count;
  biquad_filter_t*** cascades;
  size_t cascade_depth;
  int samplerate;
  size_t* live;
  size_t live_count;
  double** waveforms;
  size_t waveforms_capacity;
} biquad_step_t;

/// A single step in the processing pipeline
typedef enum {
  EXEC_STEP_PARALLEL_FILTERS = 0,
  EXEC_STEP_MIXER,
  EXEC_STEP_PROCESSOR,
  EXEC_STEP_BIQUAD
} exec_step_type_t;

/// A single step in the processing pipeline
typedef struct {
  exec_step_type_t type;
  parallel_filter_chain_t* chains;
  size_t chains_count;
  mixer_t* mixer;
  dsp_processor_t* processor;
  biquad_step_t* biquad_step;
} pipeline_exec_step_t;

/// The main audio processing pipeline.
struct pipeline_s {
  pipeline_exec_step_t* steps;
  size_t steps_count;
  bool multithreaded;
  volume_filter_t* master_volume;
  audio_chunk_t* capture_scratch;
  audio_chunk_t** scratches_for_mixers;
  size_t scratches_for_mixers_count;

  size_t frames_per_chunk;
  int rate;
  size_t expected_in_channels;
  size_t expected_out_channels;

  size_t last_error_needed;
  size_t last_error_got;
};

// Cleanup helpers shared between builder and destruction
void free_filter_chains(parallel_filter_chain_t* chains, size_t count);
void biquad_step_free(biquad_step_t* step);

#endif  // CLIB_PIPELINE_PIPELINE_INTERNAL_H
