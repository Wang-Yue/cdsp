// Regression tests for audit report 05 (pipeline core) and the pipeline-side
// cross-report items (02, 04, 07).
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "audio/audio_chunk.h"
#include "audio/processing_parameters.h"
#include "config/config_error.h"
#include "config/config_gen.h"
#include "config/configuration.h"
#include "filters/filter.h"
#include "pipeline/pipeline.h"
#include "pipeline/pipeline_internal.h"
#include "test_support.h"
#include "utils/double_helpers.h"

#define FRAMES 256
#define RATE 48000

static void init_config(dsp_config_t *config, size_t channels) {
  memset(config, 0, sizeof(*config));
  config->devices.samplerate = RATE;
  config->devices.chunksize = FRAMES;
  config->devices.capture.type = AUDIO_BACKEND_TYPE_FILE;
  config->devices.capture.cfg.raw_file.channels = channels;
  config->devices.playback.type = AUDIO_BACKEND_TYPE_FILE;
  config->devices.playback.cfg.raw_file.channels = channels;
  config->devices.volume_ramp_time_ms = 0.0;
  config->devices.has_volume_ramp_time_ms = true;
}

static void fill_chunk(audio_chunk_t *chunk, size_t channels) {
  for (size_t ch = 0; ch < channels; ch++) {
    mutable_waveform_t w = audio_chunk_get_channel(chunk, ch);
    for (size_t t = 0; t < FRAMES; t++)
      w[t] = (double)(ch + 1);
  }
  audio_chunk_set_valid_frames(chunk, FRAMES);
}

// 05-1.1 / 04-1.2.2 / 07-1.3: no input scratch (and no extra copy) when the
// channel count is unchanged and every capture channel is used.
TEST(AuditPipelineNoInputScratchWithoutMixer) {
  dsp_config_t config;
  init_config(&config, 2);
  processing_parameters_t *params = processing_parameters_create(2, 2);
  pipeline_t *p = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(p != NULL);
  ASSERT_TRUE(p->input_scratch == NULL);

  audio_chunk_t *in = audio_chunk_create(FRAMES, 2);
  audio_chunk_t *out = audio_chunk_create(FRAMES, 2);
  fill_chunk(in, 2);
  ASSERT_EQ(PIPELINE_OK, pipeline_process(p, in, out));
  ASSERT_NEAR(1.0, audio_chunk_get_channel(out, 0)[FRAMES - 1], 1e-12);
  ASSERT_NEAR(2.0, audio_chunk_get_channel(out, 1)[0], 1e-12);
  // In place.
  ASSERT_EQ(PIPELINE_OK, pipeline_process(p, in, in));
  ASSERT_NEAR(2.0, audio_chunk_get_channel(in, 1)[7], 1e-12);

  audio_chunk_free(in);
  audio_chunk_free(out);
  pipeline_free(p);
  processing_parameters_free(params);
}

static mixer_source_t g_src_ch0 = {
    .channel = 0, .gain = 0.0, .has_gain = true, .scale = GAIN_SCALE_DB};
static mixer_source_t g_src_ch1 = {
    .channel = 1, .gain = 0.0, .has_gain = true, .scale = GAIN_SCALE_DB};

// 05-1.3: with an unused capture channel the scratch exists, carries the used
// mask, and in-place processing still zeroes/ignores the unused channel.
TEST(AuditPipelineInputScratchWhenCaptureChannelUnused) {
  dsp_config_t config;
  init_config(&config, 2);
  mixer_mapping_t maps[2] = {
      {.dest = 0, .sources_count = 1, .sources = &g_src_ch0},
      {.dest = 1, .sources_count = 1, .sources = &g_src_ch0}};
  named_mixer_config_t mixer_cfg;
  memset(&mixer_cfg, 0, sizeof(mixer_cfg));
  strcpy(mixer_cfg.name, "dup");
  mixer_cfg.mixer.channels_in = 2;
  mixer_cfg.mixer.channels_out = 2;
  mixer_cfg.mixer.mapping_count = 2;
  mixer_cfg.mixer.mapping = maps;
  config.mixers = &mixer_cfg;
  config.mixers_count = 1;
  pipeline_step_config_t step;
  memset(&step, 0, sizeof(step));
  step.type = PIPELINE_STEP_TYPE_MIXER;
  strcpy(step.name, "dup");
  step.has_name = true;
  config.pipeline = &step;
  config.pipeline_count = 1;

  processing_parameters_t *params = processing_parameters_create(2, 2);
  pipeline_t *p = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(p != NULL);
  ASSERT_TRUE(p->input_scratch != NULL);
  ASSERT_TRUE(audio_chunk_get_used_channels(p->input_scratch) != NULL);

  audio_chunk_t *in = audio_chunk_create(FRAMES, 2);
  fill_chunk(in, 2);
  ASSERT_EQ(PIPELINE_OK, pipeline_process(p, in, in));
  ASSERT_NEAR(1.0, audio_chunk_get_channel(in, 0)[3], 1e-12);
  ASSERT_NEAR(1.0, audio_chunk_get_channel(in, 1)[3], 1e-12);

  audio_chunk_free(in);
  pipeline_free(p);
  processing_parameters_free(params);
}

TEST(AuditPipelineSwapMixerAllUsedRunsInOutput) {
  dsp_config_t config;
  init_config(&config, 2);
  mixer_mapping_t maps[2] = {
      {.dest = 0, .sources_count = 1, .sources = &g_src_ch1},
      {.dest = 1, .sources_count = 1, .sources = &g_src_ch0}};
  named_mixer_config_t mixer_cfg;
  memset(&mixer_cfg, 0, sizeof(mixer_cfg));
  strcpy(mixer_cfg.name, "swap");
  mixer_cfg.mixer.channels_in = 2;
  mixer_cfg.mixer.channels_out = 2;
  mixer_cfg.mixer.mapping_count = 2;
  mixer_cfg.mixer.mapping = maps;
  config.mixers = &mixer_cfg;
  config.mixers_count = 1;
  pipeline_step_config_t step;
  memset(&step, 0, sizeof(step));
  step.type = PIPELINE_STEP_TYPE_MIXER;
  strcpy(step.name, "swap");
  step.has_name = true;
  config.pipeline = &step;
  config.pipeline_count = 1;

  processing_parameters_t *params = processing_parameters_create(2, 2);
  pipeline_t *p = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(p != NULL);
  ASSERT_TRUE(p->input_scratch == NULL);

  audio_chunk_t *in = audio_chunk_create(FRAMES, 2);
  fill_chunk(in, 2);
  ASSERT_EQ(PIPELINE_OK, pipeline_process(p, in, in));
  ASSERT_NEAR(2.0, audio_chunk_get_channel(in, 0)[0], 1e-12);
  ASSERT_NEAR(1.0, audio_chunk_get_channel(in, 1)[FRAMES - 1], 1e-12);

  audio_chunk_free(in);
  pipeline_free(p);
  processing_parameters_free(params);
}

// 05-3.1: a filter referenced twice on one channel keeps both delay lines
// apart across a FILTER_PARAMETERS hot reload.
TEST(AuditPipelineTransferKeepsDuplicateFilterStatesApart) {
  dsp_config_t config;
  init_config(&config, 1);
  named_filter_config_t fcfg;
  memset(&fcfg, 0, sizeof(fcfg));
  strcpy(fcfg.name, "d");
  fcfg.filter.type = FILTER_TYPE_DELAY;
  fcfg.filter.parameters.delay.delay = 300.0;
  fcfg.filter.parameters.delay.delay_unit = DELAY_UNIT_SAMPLES;
  config.filters = &fcfg;
  config.filters_count = 1;
  char *names[2] = {"d", "d"};
  pipeline_step_config_t step;
  memset(&step, 0, sizeof(step));
  step.type = PIPELINE_STEP_TYPE_FILTER;
  step.names = names;
  step.names_count = 2;
  config.pipeline = &step;
  config.pipeline_count = 1;

  processing_parameters_t *params = processing_parameters_create(1, 1);
  pipeline_t *a = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(a != NULL);
  audio_chunk_t *in = audio_chunk_create(FRAMES, 1);
  audio_chunk_t *out = audio_chunk_create(FRAMES, 1);
  memset(audio_chunk_get_channel(in, 0), 0, FRAMES * sizeof(double));
  audio_chunk_get_channel(in, 0)[0] = 1.0;
  audio_chunk_set_valid_frames(in, FRAMES);
  ASSERT_EQ(PIPELINE_OK, pipeline_process(a, in, out));

  pipeline_t *b = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(b != NULL);
  pipeline_transfer_state(b, a, true);
  pipeline_free(a);

  audio_chunk_get_channel(in, 0)[0] = 0.0;
  double total = 0.0;
  size_t peak_pos = 0;
  double peak = 0.0;
  for (size_t c = 1; c < 4; c++) {
    ASSERT_EQ(PIPELINE_OK, pipeline_process(b, in, out));
    waveform_t w = audio_chunk_get_channel(out, 0);
    for (size_t t = 0; t < FRAMES; t++) {
      total += fabs(w[t]);
      if (fabs(w[t]) > peak) {
        peak = fabs(w[t]);
        peak_pos = c * FRAMES + t;
      }
    }
  }
  ASSERT_NEAR(1.0, total, 1e-9);
  ASSERT_EQ(600, peak_pos);

  audio_chunk_free(in);
  audio_chunk_free(out);
  pipeline_free(b);
  processing_parameters_free(params);
}

// Upstream `used_capture_channels_include_processors_before_the_mixer`
// (87590f4): a mixer that only reads channel 0, and processors that all read
// channel 1. A processor before the mixer keeps channel 1 captured; after the
// mixer, or bypassed, it does not.
static void set_used_channels_config(dsp_config_t *config,
                                     named_mixer_config_t *mixer_cfg,
                                     mixer_mapping_t *maps,
                                     named_processor_config_t *procs,
                                     size_t procs_count) {
  init_config(config, 2);
  maps[0] = (mixer_mapping_t){.dest = 0, .sources_count = 1,
                              .sources = &g_src_ch0};
  maps[1] = (mixer_mapping_t){.dest = 1, .sources_count = 1,
                              .sources = &g_src_ch0};
  memset(mixer_cfg, 0, sizeof(*mixer_cfg));
  strcpy(mixer_cfg->name, "left");
  mixer_cfg->mixer.channels_in = 2;
  mixer_cfg->mixer.channels_out = 2;
  mixer_cfg->mixer.mapping_count = 2;
  mixer_cfg->mixer.mapping = maps;
  config->mixers = mixer_cfg;
  config->mixers_count = 1;
  config->processors = procs;
  config->processors_count = procs_count;
}

static void set_step(pipeline_step_config_t *step, pipeline_step_type_t type,
                     const char *name, bool bypassed) {
  memset(step, 0, sizeof(*step));
  step->type = type;
  strcpy(step->name, name);
  step->has_name = true;
  step->bypassed = bypassed;
}

TEST(UsedCaptureChannelsIncludeProcessorsBeforeTheMixer) {
  static size_t monitor_ch1[] = {1};
  named_processor_config_t procs[4];
  memset(procs, 0, sizeof(procs));
  strcpy(procs[0].name, "comp");
  procs[0].processor.type = PROCESSOR_TYPE_COMPRESSOR;
  procs[0].processor.parameters.compressor.channels = 2;
  procs[0].processor.parameters.compressor.monitor_channels = monitor_ch1;
  procs[0].processor.parameters.compressor.monitor_channels_count = 1;
  // The noise gate leaves monitor_channels out, so it monitors all channels.
  strcpy(procs[1].name, "gate");
  procs[1].processor.type = PROCESSOR_TYPE_NOISE_GATE;
  procs[1].processor.parameters.noise_gate.channels = 2;
  strcpy(procs[2].name, "limiter");
  procs[2].processor.type = PROCESSOR_TYPE_LOOKAHEAD_LIMITER;
  procs[2].processor.parameters.lookahead_limiter.channels = 2;
  procs[2].processor.parameters.lookahead_limiter.monitor_channels =
      monitor_ch1;
  procs[2].processor.parameters.lookahead_limiter.monitor_channels_count = 1;
  strcpy(procs[3].name, "race");
  procs[3].processor.type = PROCESSOR_TYPE_RACE;
  procs[3].processor.parameters.race.channels = 2;
  procs[3].processor.parameters.race.channel_a = 1;
  procs[3].processor.parameters.race.channel_b = 0;

  dsp_config_t config;
  named_mixer_config_t mixer_cfg;
  mixer_mapping_t maps[2];
  pipeline_step_config_t steps[2];
  bool used[2];

  for (size_t i = 0; i < 4; i++) {
    set_used_channels_config(&config, &mixer_cfg, maps, procs, 4);
    set_step(&steps[0], PIPELINE_STEP_TYPE_PROCESSOR, procs[i].name, false);
    set_step(&steps[1], PIPELINE_STEP_TYPE_MIXER, "left", false);
    config.pipeline = steps;
    config.pipeline_count = 2;
    ASSERT_TRUE(pipeline_compute_used_capture_channels(&config, used, 2));
    ASSERT_TRUE(used[0]);
    ASSERT_TRUE(used[1]);
  }

  // After the mixer, a processor does not read any capture channel.
  set_used_channels_config(&config, &mixer_cfg, maps, procs, 4);
  set_step(&steps[0], PIPELINE_STEP_TYPE_MIXER, "left", false);
  set_step(&steps[1], PIPELINE_STEP_TYPE_PROCESSOR, "comp", false);
  config.pipeline = steps;
  config.pipeline_count = 2;
  ASSERT_TRUE(pipeline_compute_used_capture_channels(&config, used, 2));
  ASSERT_TRUE(used[0]);
  ASSERT_FALSE(used[1]);

  // Bypassed, neither.
  set_used_channels_config(&config, &mixer_cfg, maps, procs, 4);
  set_step(&steps[0], PIPELINE_STEP_TYPE_PROCESSOR, "comp", true);
  set_step(&steps[1], PIPELINE_STEP_TYPE_MIXER, "left", false);
  config.pipeline = steps;
  config.pipeline_count = 2;
  ASSERT_TRUE(pipeline_compute_used_capture_channels(&config, used, 2));
  ASSERT_TRUE(used[0]);
  ASSERT_FALSE(used[1]);
}

TEST_MAIN()
