
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "audio/audio_chunk.h"
#include "audio/processing_parameters.h"
#include "audio/sample_format.h"
#include "config/config_error.h"
#include "config/config_gen.h"
#include "config/configuration.h"
#include "filters/filter.h"
#include "filters/loudness.h"
#include "filters/volume.h"
#include "pipeline/config_loader.h"
#include "pipeline/pipeline.h"
#include "pipeline/pipeline_faders.h"
#include "pipeline/pipeline_internal.h"
#include "test_support.h"
#include "utils/double_helpers.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void init_default_config(dsp_config_t *config) {
  memset(config, 0, sizeof(dsp_config_t));
  config->devices.samplerate = 44100;
  config->devices.chunksize = 1024;
#if defined(ENABLE_COREAUDIO)
  config->devices.capture.type = AUDIO_BACKEND_TYPE_CORE_AUDIO;
  config->devices.capture.cfg.coreaudio.channels = 2;
  config->devices.playback.type = AUDIO_BACKEND_TYPE_CORE_AUDIO;
  config->devices.playback.cfg.coreaudio.channels = 2;
#elif defined(ENABLE_ALSA)
  config->devices.capture.type = AUDIO_BACKEND_TYPE_ALSA;
  config->devices.capture.cfg.alsa.channels = 2;
  config->devices.playback.type = AUDIO_BACKEND_TYPE_ALSA;
  config->devices.playback.cfg.alsa.channels = 2;
#elif defined(ENABLE_WASAPI)
  config->devices.capture.type = AUDIO_BACKEND_TYPE_WASAPI;
  config->devices.capture.cfg.wasapi.channels = 2;
  config->devices.playback.type = AUDIO_BACKEND_TYPE_WASAPI;
  config->devices.playback.cfg.wasapi.channels = 2;
#else
  config->devices.capture.type = AUDIO_BACKEND_TYPE_FILE;
  snprintf(config->devices.capture.cfg.raw_file.filename,
           sizeof(config->devices.capture.cfg.raw_file.filename), "/dev/null");
  config->devices.capture.cfg.raw_file.channels = 2;
  config->devices.playback.type = AUDIO_BACKEND_TYPE_FILE;
  config->devices.playback.cfg.raw_file.channels = 2;
#endif
}

TEST(PipelineInitEmpty) {
  dsp_config_t config;
  init_default_config(&config);
  processing_parameters_t *params = processing_parameters_create(2, 2);
  config_error_t err;
  config_error_init(&err);
  pipeline_t *pipeline = pipeline_create(&config, params, 0, &err);
  ASSERT_TRUE(pipeline != NULL);
  ASSERT_EQ(CONFIG_ERR_NONE, err.type);

  pipeline_free(pipeline);
  processing_parameters_free(params);
}

TEST(PipelineProcessPassthrough) {
  dsp_config_t config;
  init_default_config(&config);
  processing_parameters_t *params = processing_parameters_create(2, 2);
  pipeline_t *pipeline = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(pipeline != NULL);

  audio_chunk_t *chunk = audio_chunk_create(1024, 2);
  for (size_t ch = 0; ch < 2; ch++) {
    mutable_waveform_t buf = audio_chunk_get_channel(chunk, ch);
    for (size_t t = 0; t < 1024; t++) {
      buf[t] = sin(2.0 * M_PI * 1000.0 * (double)t / 44100.0);
    }
  }
  audio_chunk_set_valid_frames(chunk, 1024);

  audio_chunk_t *output = audio_chunk_create(1024, 2);
  pipeline_error_t err = pipeline_process(pipeline, chunk, output);
  ASSERT_EQ(PIPELINE_OK, err);
  ASSERT_EQ(1024, audio_chunk_get_valid_frames(output));
  ASSERT_EQ(2, audio_chunk_get_channels(output));

  for (size_t ch = 0; ch < 2; ch++) {
    waveform_t in_buf = audio_chunk_get_channel(chunk, ch);
    waveform_t out_buf = audio_chunk_get_channel(output, ch);
    for (size_t t = 0; t < 1024; t++) {
      ASSERT_NEAR(in_buf[t], out_buf[t], 1e-9);
    }
  }

  audio_chunk_free(chunk);
  audio_chunk_free(output);
  pipeline_free(pipeline);
  processing_parameters_free(params);
}

TEST(PipelineMultithreadedCorrectness) {
  dsp_config_t config;
  memset(&config, 0, sizeof(dsp_config_t));
  config.devices.samplerate = 48000;
  config.devices.chunksize = 1024;
  config.devices.capture.type = AUDIO_BACKEND_TYPE_FILE;
  config.devices.capture.cfg.raw_file.channels = 4;
  config.devices.playback.type = AUDIO_BACKEND_TYPE_FILE;
  config.devices.playback.cfg.raw_file.channels = 4;

  named_filter_config_t filters[4];
  memset(filters, 0, sizeof(filters));

  for (int i = 0; i < 4; i++) {
    snprintf(filters[i].name, sizeof(filters[i].name), "gain_%d", i + 1);
    filters[i].filter.type = FILTER_TYPE_GAIN;
    filters[i].filter.parameters.gain.gain = -3.0 * (i + 1);
    filters[i].filter.parameters.gain.scale = GAIN_SCALE_DB;
  }

  config.filters = filters;
  config.filters_count = 4;

  pipeline_step_config_t step;
  memset(&step, 0, sizeof(step));
  step.type = PIPELINE_STEP_TYPE_FILTER;
  step.has_channel = false;
  char *filter_names[4] = {"gain_1", "gain_2", "gain_3", "gain_4"};
  step.names = filter_names;
  step.names_count = 4;

  config.pipeline = &step;
  config.pipeline_count = 1;

  processing_parameters_t *params = processing_parameters_create(4, 4);
  ASSERT_TRUE(params != NULL);

  // 1. Run single-threaded
  config.devices.multithreaded = false;
  config.devices.has_multithreaded = true;
  pipeline_t *pipeline_single = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(pipeline_single != NULL);

  audio_chunk_t *input = audio_chunk_create(1024, 4);
  for (size_t ch = 0; ch < 4; ch++) {
    mutable_waveform_t w = audio_chunk_get_channel(input, ch);
    for (size_t t = 0; t < 1024; t++) {
      w[t] = 0.1 * (double)(t % 10 + 1);
    }
  }
  audio_chunk_set_valid_frames(input, 1024);

  audio_chunk_t *output_single = audio_chunk_create(1024, 4);
  pipeline_error_t err =
      pipeline_process(pipeline_single, input, output_single);
  ASSERT_EQ(PIPELINE_OK, err);

  // 2. Run multi-threaded
  config.devices.multithreaded = true;
  pipeline_t *pipeline_multi = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(pipeline_multi != NULL);

  audio_chunk_t *output_multi = audio_chunk_create(1024, 4);
  err = pipeline_process(pipeline_multi, input, output_multi);
  ASSERT_EQ(PIPELINE_OK, err);

  // 3. Compare outputs
  for (size_t ch = 0; ch < 4; ch++) {
    waveform_t w_single = audio_chunk_get_channel(output_single, ch);
    waveform_t w_multi = audio_chunk_get_channel(output_multi, ch);
    for (size_t t = 0; t < 1024; t++) {
      ASSERT_NEAR(w_single[t], w_multi[t], 1e-9);
    }
  }

  audio_chunk_free(input);
  audio_chunk_free(output_single);
  audio_chunk_free(output_multi);
  pipeline_free(pipeline_single);
  pipeline_free(pipeline_multi);
  processing_parameters_free(params);
}

TEST(PipelineWithFilter) {
  dsp_config_t config;
  init_default_config(&config);

  named_filter_config_t filter_cfg;
  memset(&filter_cfg, 0, sizeof(filter_cfg));
  strcpy(filter_cfg.name, "mygain");
  filter_cfg.filter.type = FILTER_TYPE_GAIN;
  filter_cfg.filter.parameters.gain.gain = -6.0;
  filter_cfg.filter.parameters.gain.scale = GAIN_SCALE_DB;
  config.filters = &filter_cfg;
  config.filters_count = 1;

  char *filter_name = strdup("mygain");
  pipeline_step_config_t step;
  memset(&step, 0, sizeof(step));
  step.type = PIPELINE_STEP_TYPE_FILTER;
  step.channel = 0;
  step.has_channel = true;
  step.names = &filter_name;
  step.names_count = 1;
  config.pipeline = &step;
  config.pipeline_count = 1;

  processing_parameters_t *params = processing_parameters_create(2, 2);
  pipeline_t *pipeline = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(pipeline != NULL);

  audio_chunk_t *chunk = audio_chunk_create(1024, 2);
  for (size_t ch = 0; ch < 2; ch++) {
    mutable_waveform_t buf = audio_chunk_get_channel(chunk, ch);
    for (size_t t = 0; t < 1024; t++) {
      buf[t] = 1.0;
    }
  }
  audio_chunk_set_valid_frames(chunk, 1024);

  audio_chunk_t *output = audio_chunk_create(1024, 2);
  pipeline_error_t err = pipeline_process(pipeline, chunk, output);
  ASSERT_EQ(PIPELINE_OK, err);

  waveform_t out0 = audio_chunk_get_channel(output, 0);
  waveform_t out1 = audio_chunk_get_channel(output, 1);
  ASSERT_NEAR(double_from_db(-6.0), out0[0], 1e-5);
  ASSERT_NEAR(1.0, out1[0], 1e-5);

  free(filter_name);
  audio_chunk_free(chunk);
  audio_chunk_free(output);
  pipeline_free(pipeline);
  processing_parameters_free(params);
}

TEST(PipelineWithMixer) {
  dsp_config_t config;
  init_default_config(&config);

  mixer_source_t src0 = {.channel = 1,
                         .gain = 0.0,
                         .has_gain = true,
                         .scale = GAIN_SCALE_DB,
                         .inverted = false,
                         .mute = false};
  mixer_source_t src1 = {.channel = 0,
                         .gain = 0.0,
                         .has_gain = true,
                         .scale = GAIN_SCALE_DB,
                         .inverted = false,
                         .mute = false};
  mixer_mapping_t maps[2] = {
      {.dest = 0, .sources_count = 1, .sources = &src0, .mute = false},
      {.dest = 1, .sources_count = 1, .sources = &src1, .mute = false}};
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
  pipeline_t *pipeline = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(pipeline != NULL);

  audio_chunk_t *chunk = audio_chunk_create(1024, 2);
  mutable_waveform_t ch0 = audio_chunk_get_channel(chunk, 0);
  mutable_waveform_t ch1 = audio_chunk_get_channel(chunk, 1);
  for (size_t t = 0; t < 1024; t++) {
    ch0[t] = 1.0;
    ch1[t] = 2.0;
  }
  audio_chunk_set_valid_frames(chunk, 1024);

  audio_chunk_t *output = audio_chunk_create(1024, 2);
  pipeline_error_t err = pipeline_process(pipeline, chunk, output);
  ASSERT_EQ(PIPELINE_OK, err);

  waveform_t out0 = audio_chunk_get_channel(output, 0);
  waveform_t out1 = audio_chunk_get_channel(output, 1);
  ASSERT_NEAR(2.0, out0[0], 1e-5);
  ASSERT_NEAR(1.0, out1[0], 1e-5);

  audio_chunk_free(chunk);
  audio_chunk_free(output);
  pipeline_free(pipeline);
  processing_parameters_free(params);
}

// DELIBERATE DIVERGENCE FROM UPSTREAM (audit finding 04-4).
//
// Upstream's `Mixer::update_parameters` (mixer.rs:80-101) is a copy of
// `from_config` with the two `is_mute()` checks missing, and it is what the
// hot-reload path calls (pipeline.rs:598-601). So in CamillaDSP, any config
// change touching a mixer silently un-mutes every muted mapping and source of
// that mixer until the process restarts.
//
// That is a copy-paste omission, not a design choice, and its failure mode is
// unsafe: audio appears on a channel the user deliberately muted. The port has
// no in-place mixer update at all -- it always rebuilds through mixer_create --
// so matching upstream would mean writing the bug into a code path that does
// not otherwise exist. This test pins the port's (correct) behaviour so the
// divergence is intentional and visible rather than accidental.
TEST(PipelineMixerMuteSurvivesReload) {
  dsp_config_t config;
  init_default_config(&config);

  mixer_source_t src0 = {.channel = 0,
                         .gain = 0.0,
                         .has_gain = true,
                         .scale = GAIN_SCALE_DB,
                         .mute = false};
  mixer_source_t src1 = {.channel = 1,
                         .gain = 0.0,
                         .has_gain = true,
                         .scale = GAIN_SCALE_DB,
                         .mute = false};
  // Destination 1 is muted; destination 0 passes through.
  mixer_mapping_t maps[2] = {
      {.dest = 0, .sources_count = 1, .sources = &src0, .mute = false},
      {.dest = 1, .sources_count = 1, .sources = &src1, .mute = true}};
  named_mixer_config_t mixer_cfg;
  memset(&mixer_cfg, 0, sizeof(mixer_cfg));
  strcpy(mixer_cfg.name, "muted");
  mixer_cfg.mixer.channels_in = 2;
  mixer_cfg.mixer.channels_out = 2;
  mixer_cfg.mixer.mapping_count = 2;
  mixer_cfg.mixer.mapping = maps;
  config.mixers = &mixer_cfg;
  config.mixers_count = 1;

  pipeline_step_config_t step;
  memset(&step, 0, sizeof(step));
  step.type = PIPELINE_STEP_TYPE_MIXER;
  strcpy(step.name, "muted");
  step.has_name = true;
  config.pipeline = &step;
  config.pipeline_count = 1;

  processing_parameters_t *params = processing_parameters_create(2, 2);

  audio_chunk_t *chunk = audio_chunk_create(1024, 2);
  mutable_waveform_t in0 = audio_chunk_get_channel(chunk, 0);
  mutable_waveform_t in1 = audio_chunk_get_channel(chunk, 1);
  for (size_t t = 0; t < 1024; t++) {
    in0[t] = 1.0;
    in1[t] = 1.0;
  }
  audio_chunk_set_valid_frames(chunk, 1024);

  pipeline_t *pipeline1 = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(pipeline1 != NULL);
  audio_chunk_t *out1 = audio_chunk_create(1024, 2);
  ASSERT_EQ(PIPELINE_OK, pipeline_process(pipeline1, chunk, out1));
  ASSERT_NEAR(1.0, audio_chunk_get_channel(out1, 0)[0], 1e-9);
  ASSERT_NEAR(0.0, audio_chunk_get_channel(out1, 1)[0], 1e-9);

  // Reload with the mixer changed (dest 0 attenuated) but dest 1 still muted.
  src0.gain = -6.020600;
  pipeline_t *pipeline2 = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(pipeline2 != NULL);
  audio_chunk_t *out2 = audio_chunk_create(1024, 2);
  ASSERT_EQ(PIPELINE_OK, pipeline_process(pipeline2, chunk, out2));

  // The gain change took effect...
  ASSERT_NEAR(0.5, audio_chunk_get_channel(out2, 0)[0], 1e-5);
  // ...and the mute did NOT get dropped along the way (upstream would
  // emit 1.0).
  ASSERT_NEAR(0.0, audio_chunk_get_channel(out2, 1)[0], 1e-9);

  audio_chunk_free(chunk);
  audio_chunk_free(out1);
  audio_chunk_free(out2);
  pipeline_free(pipeline1);
  pipeline_free(pipeline2);
  processing_parameters_free(params);
}

TEST(PipelineBypassedFilter) {
  dsp_config_t config;
  init_default_config(&config);

  named_filter_config_t filter_cfg;
  memset(&filter_cfg, 0, sizeof(filter_cfg));
  strcpy(filter_cfg.name, "mygain");
  filter_cfg.filter.type = FILTER_TYPE_GAIN;
  filter_cfg.filter.parameters.gain.gain = -6.0;
  filter_cfg.filter.parameters.gain.scale = GAIN_SCALE_DB;
  config.filters = &filter_cfg;
  config.filters_count = 1;

  char *filter_name = strdup("mygain");
  pipeline_step_config_t step;
  memset(&step, 0, sizeof(step));
  step.type = PIPELINE_STEP_TYPE_FILTER;
  step.channel = 0;
  step.has_channel = true;
  step.names = &filter_name;
  step.names_count = 1;
  step.bypassed = true;
  config.pipeline = &step;
  config.pipeline_count = 1;

  processing_parameters_t *params = processing_parameters_create(2, 2);
  pipeline_t *pipeline = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(pipeline != NULL);

  audio_chunk_t *chunk = audio_chunk_create(1024, 2);
  for (size_t ch = 0; ch < 2; ch++) {
    mutable_waveform_t buf = audio_chunk_get_channel(chunk, ch);
    for (size_t t = 0; t < 1024; t++) {
      buf[t] = 1.0;
    }
  }
  audio_chunk_set_valid_frames(chunk, 1024);

  audio_chunk_t *output = audio_chunk_create(1024, 2);
  pipeline_error_t err = pipeline_process(pipeline, chunk, output);
  ASSERT_EQ(PIPELINE_OK, err);

  waveform_t out0 = audio_chunk_get_channel(output, 0);
  ASSERT_NEAR(1.0, out0[0], 1e-5);

  free(filter_name);
  audio_chunk_free(chunk);
  audio_chunk_free(output);
  pipeline_free(pipeline);
  processing_parameters_free(params);
}

TEST(PipelineEmptyChannelsListIsNoOp) {
  const char *json =
      "{\n"
      "  \"devices\": {\n"
      "    \"samplerate\": 44100,\n"
      "    \"chunksize\": 1024,\n"
      "    \"capture\": {\"type\": \"RawFile\", \"channels\": 2, \"filename\": "
      "\"/dev/null\", \"format\": \"S16_LE\"},\n"
      "    \"playback\": {\"type\": \"File\", \"channels\": 2, \"filename\": "
      "\"/dev/null\", \"format\": \"S16_LE\"}\n"
      "  },\n"
      "  \"filters\": {\n"
      "    \"mygain\": {\"type\": \"Gain\", \"parameters\": {\"gain\": -6.0}}\n"
      "  },\n"
      "  \"pipeline\": [\n"
      "    {\"type\": \"Filter\", \"channels\": [], \"names\": [\"mygain\"]}\n"
      "  ]\n"
      "}";

  dsp_config_t *config = NULL;
  config_error_t cfg_err;
  config_error_init(&cfg_err);
  ASSERT_EQ(0, dsp_config_parse_json(json, &config, &cfg_err));
  ASSERT_TRUE(config != NULL);
  ASSERT_TRUE(config->pipeline[0].has_channels);
  ASSERT_EQ(0, config->pipeline[0].channels_count);

  processing_parameters_t *params = processing_parameters_create(2, 2);
  pipeline_t *pipeline = pipeline_create(config, params, 0, &cfg_err);
  ASSERT_TRUE(pipeline != NULL);
  ASSERT_EQ(0, pipeline->steps_count);

  audio_chunk_t *chunk = audio_chunk_create(1024, 2);
  for (size_t ch = 0; ch < 2; ch++) {
    mutable_waveform_t buf = audio_chunk_get_channel(chunk, ch);
    for (size_t t = 0; t < 1024; t++) {
      buf[t] = 1.0;
    }
  }
  audio_chunk_set_valid_frames(chunk, 1024);

  audio_chunk_t *output = audio_chunk_create(1024, 2);
  pipeline_error_t err = pipeline_process(pipeline, chunk, output);
  ASSERT_EQ(PIPELINE_OK, err);

  // Both channels must NOT have gain applied (remains 1.0)
  for (size_t ch = 0; ch < 2; ch++) {
    waveform_t buf = audio_chunk_get_channel(output, ch);
    for (size_t t = 0; t < 1024; t++) {
      ASSERT_NEAR(1.0, buf[t], 1e-6);
    }
  }

  audio_chunk_free(chunk);
  audio_chunk_free(output);
  pipeline_free(pipeline);
  processing_parameters_free(params);
  dsp_config_free(config);
}

TEST(PipelineFilterChannelOutOfBounds) {
  dsp_config_t config;
  init_default_config(&config);

  named_filter_config_t filter_cfg;
  memset(&filter_cfg, 0, sizeof(filter_cfg));
  strcpy(filter_cfg.name, "mygain");
  filter_cfg.filter.type = FILTER_TYPE_GAIN;
  filter_cfg.filter.parameters.gain.gain = -6.0;
  filter_cfg.filter.parameters.gain.scale = GAIN_SCALE_DB;
  config.filters = &filter_cfg;
  config.filters_count = 1;

  char *filter_name = strdup("mygain");
  pipeline_step_config_t step;
  memset(&step, 0, sizeof(step));
  step.type = PIPELINE_STEP_TYPE_FILTER;
  step.channel = 2;
  step.has_channel = true;
  step.names = &filter_name;
  step.names_count = 1;
  config.pipeline = &step;
  config.pipeline_count = 1;

  processing_parameters_t *params = processing_parameters_create(2, 2);
  config_error_t cerr;
  config_error_init(&cerr);
  pipeline_t *pipeline = pipeline_create(&config, params, 0, &cerr);
  ASSERT_TRUE(pipeline == NULL);
  ASSERT_EQ(CONFIG_ERR_INVALID_PIPELINE, cerr.type);

  free(filter_name);
  processing_parameters_free(params);
}

TEST(PipelineVolumeChange) {
  dsp_config_t config;
  init_default_config(&config);
  config.devices.volume_ramp_time_ms = 0.0;
  config.devices.has_volume_ramp_time_ms = true;

  processing_parameters_t *params = processing_parameters_create(2, 2);
  pipeline_t *pipeline = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(pipeline != NULL);

  processing_parameters_set_target_volume(params, -10.0);

  audio_chunk_t *chunk = audio_chunk_create(1024, 2);
  for (size_t ch = 0; ch < 2; ch++) {
    mutable_waveform_t buf = audio_chunk_get_channel(chunk, ch);
    for (size_t t = 0; t < 1024; t++) {
      buf[t] = 1.0;
    }
  }
  audio_chunk_set_valid_frames(chunk, 1024);

  audio_chunk_t *output = audio_chunk_create(1024, 2);
  pipeline_error_t err = pipeline_process(pipeline, chunk, output);
  ASSERT_EQ(PIPELINE_OK, err);

  waveform_t out0 = audio_chunk_get_channel(output, 0);
  ASSERT_NEAR(double_from_db(-10.0), out0[0], 1e-5);
  ASSERT_NEAR(double_from_db(-10.0), out0[1023], 1e-5);

  audio_chunk_free(chunk);
  audio_chunk_free(output);
  pipeline_free(pipeline);
  processing_parameters_free(params);
}

TEST(PipelineMute) {
  dsp_config_t config;
  init_default_config(&config);
  config.devices.volume_ramp_time_ms = 0.0;
  config.devices.has_volume_ramp_time_ms = true;

  processing_parameters_t *params = processing_parameters_create(2, 2);
  pipeline_t *pipeline = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(pipeline != NULL);

  processing_parameters_set_muted(params, true);

  audio_chunk_t *chunk = audio_chunk_create(1024, 2);
  for (size_t ch = 0; ch < 2; ch++) {
    mutable_waveform_t buf = audio_chunk_get_channel(chunk, ch);
    for (size_t t = 0; t < 1024; t++) {
      buf[t] = 1.0;
    }
  }
  audio_chunk_set_valid_frames(chunk, 1024);

  audio_chunk_t *output = audio_chunk_create(1024, 2);
  pipeline_error_t err = pipeline_process(pipeline, chunk, output);
  ASSERT_EQ(PIPELINE_OK, err);

  waveform_t out0 = audio_chunk_get_channel(output, 0);
  ASSERT_NEAR(0.0, out0[0], 1e-5);
  ASSERT_NEAR(0.0, out0[1023], 1e-5);

  audio_chunk_free(chunk);
  audio_chunk_free(output);
  pipeline_free(pipeline);
  processing_parameters_free(params);
}

TEST(PipelineVolumePresetBeforeBuild) {
  dsp_config_t config;
  init_default_config(&config);

  processing_parameters_t *params = processing_parameters_create(2, 2);
  processing_parameters_set_target_volume(params, -100.0);

  pipeline_t *pipeline = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(pipeline != NULL);

  audio_chunk_t *chunk = audio_chunk_create(1024, 2);
  for (size_t ch = 0; ch < 2; ch++) {
    mutable_waveform_t buf = audio_chunk_get_channel(chunk, ch);
    for (size_t t = 0; t < 1024; t++) {
      buf[t] = 1.0;
    }
  }
  audio_chunk_set_valid_frames(chunk, 1024);

  audio_chunk_t *output = audio_chunk_create(1024, 2);
  pipeline_error_t err = pipeline_process(pipeline, chunk, output);
  ASSERT_EQ(PIPELINE_OK, err);

  for (size_t ch = 0; ch < 2; ch++) {
    waveform_t out_buf = audio_chunk_get_channel(output, ch);
    for (size_t t = 0; t < 1024; t++) {
      ASSERT_TRUE(out_buf[t] < 1e-4);
    }
  }

  audio_chunk_free(chunk);
  audio_chunk_free(output);
  pipeline_free(pipeline);
  processing_parameters_free(params);
}

TEST(PipelineInitFilterMissingNames) {
  dsp_config_t config;
  init_default_config(&config);

  pipeline_step_config_t step;
  memset(&step, 0, sizeof(step));
  step.type = PIPELINE_STEP_TYPE_FILTER;
  step.channel = 0;
  step.has_channel = true;
  config.pipeline = &step;
  config.pipeline_count = 1;

  processing_parameters_t *params = processing_parameters_create(2, 2);
  config_error_t err;
  config_error_init(&err);
  pipeline_t *pipeline = pipeline_create(&config, params, 0, &err);
  ASSERT_TRUE(pipeline == NULL);
  ASSERT_EQ(CONFIG_ERR_INVALID_PIPELINE, err.type);
  ASSERT_TRUE(strstr(err.message, "must have 'names'") != NULL);

  processing_parameters_free(params);
}

TEST(PipelineInitFilterChannels) {
  dsp_config_t config;
  init_default_config(&config);

  named_filter_config_t filter_cfg;
  memset(&filter_cfg, 0, sizeof(filter_cfg));
  strcpy(filter_cfg.name, "mygain");
  filter_cfg.filter.type = FILTER_TYPE_GAIN;
  filter_cfg.filter.parameters.gain.gain = -6.0;
  config.filters = &filter_cfg;
  config.filters_count = 1;

  size_t chs[2] = {0, 1};
  char *filter_name = strdup("mygain");
  pipeline_step_config_t step;
  memset(&step, 0, sizeof(step));
  step.type = PIPELINE_STEP_TYPE_FILTER;
  step.channels = chs;
  step.channels_count = 2;
  step.names = &filter_name;
  step.names_count = 1;
  config.pipeline = &step;
  config.pipeline_count = 1;

  processing_parameters_t *params = processing_parameters_create(2, 2);
  pipeline_t *pipeline = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(pipeline != NULL);

  free(filter_name);
  pipeline_free(pipeline);
  processing_parameters_free(params);
}

TEST(PipelineInitFilterAllChannels) {
  dsp_config_t config;
  init_default_config(&config);

  named_filter_config_t filter_cfg;
  memset(&filter_cfg, 0, sizeof(filter_cfg));
  strcpy(filter_cfg.name, "mygain");
  filter_cfg.filter.type = FILTER_TYPE_GAIN;
  filter_cfg.filter.parameters.gain.gain = -6.0;
  config.filters = &filter_cfg;
  config.filters_count = 1;

  char *filter_name = strdup("mygain");
  pipeline_step_config_t step;
  memset(&step, 0, sizeof(step));
  step.type = PIPELINE_STEP_TYPE_FILTER;
  step.names = &filter_name;
  step.names_count = 1;
  config.pipeline = &step;
  config.pipeline_count = 1;

  processing_parameters_t *params = processing_parameters_create(2, 2);
  pipeline_t *pipeline = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(pipeline != NULL);

  free(filter_name);
  pipeline_free(pipeline);
  processing_parameters_free(params);
}

TEST(PipelineInitFilterUndefined) {
  dsp_config_t config;
  init_default_config(&config);

  char *filter_name = strdup("undefined_filter");
  pipeline_step_config_t step;
  memset(&step, 0, sizeof(step));
  step.type = PIPELINE_STEP_TYPE_FILTER;
  step.channel = 0;
  step.has_channel = true;
  step.names = &filter_name;
  step.names_count = 1;
  config.pipeline = &step;
  config.pipeline_count = 1;

  processing_parameters_t *params = processing_parameters_create(2, 2);
  config_error_t err;
  config_error_init(&err);
  pipeline_t *pipeline = pipeline_create(&config, params, 0, &err);
  ASSERT_TRUE(pipeline == NULL);
  ASSERT_EQ(CONFIG_ERR_INVALID_PIPELINE, err.type);
  ASSERT_TRUE(strstr(err.message, "not defined") != NULL);

  free(filter_name);
  processing_parameters_free(params);
}

TEST(PipelineInitMixerMissingName) {
  dsp_config_t config;
  init_default_config(&config);

  pipeline_step_config_t step;
  memset(&step, 0, sizeof(step));
  step.type = PIPELINE_STEP_TYPE_MIXER;
  config.pipeline = &step;
  config.pipeline_count = 1;

  processing_parameters_t *params = processing_parameters_create(2, 2);
  config_error_t err;
  config_error_init(&err);
  pipeline_t *pipeline = pipeline_create(&config, params, 0, &err);
  ASSERT_TRUE(pipeline == NULL);
  ASSERT_EQ(CONFIG_ERR_INVALID_PIPELINE, err.type);
  ASSERT_TRUE(strstr(err.message, "must have 'name'") != NULL);

  processing_parameters_free(params);
}

TEST(PipelineWithLoudnessFilters) {
  dsp_config_t config;
  init_default_config(&config);

  named_filter_config_t filter_cfg;
  memset(&filter_cfg, 0, sizeof(filter_cfg));
  strcpy(filter_cfg.name, "myloud");
  filter_cfg.filter.type = FILTER_TYPE_LOUDNESS;
  filter_cfg.filter.parameters.loudness.reference_level = -20.0;
  filter_cfg.filter.parameters.loudness.has_reference_level = true;
  filter_cfg.filter.parameters.loudness.fader = FADER_MAIN;
  config.filters = &filter_cfg;
  config.filters_count = 1;

  char *filter_name = strdup("myloud");
  pipeline_step_config_t step;
  memset(&step, 0, sizeof(step));
  step.type = PIPELINE_STEP_TYPE_FILTER;
  step.channel = 0;
  step.has_channel = true;
  step.names = &filter_name;
  step.names_count = 1;
  config.pipeline = &step;
  config.pipeline_count = 1;

  processing_parameters_t *params = processing_parameters_create(2, 2);
  pipeline_t *pipeline = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(pipeline != NULL);

  free(filter_name);
  pipeline_free(pipeline);
  processing_parameters_free(params);
}

TEST(PipelineSequentialMixersZeroAllocationRecovery) {
  dsp_config_t config;
  init_default_config(&config);

  mixer_source_t src_2to4_0 = {
      .channel = 0, .gain = 0.0, .has_gain = true, .scale = GAIN_SCALE_DB};
  mixer_source_t src_2to4_1 = {
      .channel = 1, .gain = 0.0, .has_gain = true, .scale = GAIN_SCALE_DB};
  mixer_mapping_t map_2to4[4] = {
      {.dest = 0, .sources_count = 1, .sources = &src_2to4_0},
      {.dest = 1, .sources_count = 1, .sources = &src_2to4_0},
      {.dest = 2, .sources_count = 1, .sources = &src_2to4_1},
      {.dest = 3, .sources_count = 1, .sources = &src_2to4_1}};
  named_mixer_config_t mixer_cfgs[2];
  memset(mixer_cfgs, 0, sizeof(mixer_cfgs));
  strcpy(mixer_cfgs[0].name, "2to4");
  mixer_cfgs[0].mixer.channels_in = 2;
  mixer_cfgs[0].mixer.channels_out = 4;
  mixer_cfgs[0].mixer.mapping_count = 4;
  mixer_cfgs[0].mixer.mapping = map_2to4;

  mixer_source_t src_4to2_0[2] = {
      {.channel = 0, .gain = 0.0, .has_gain = true, .scale = GAIN_SCALE_DB},
      {.channel = 2, .gain = 0.0, .has_gain = true, .scale = GAIN_SCALE_DB}};
  mixer_source_t src_4to2_1[2] = {
      {.channel = 1, .gain = 0.0, .has_gain = true, .scale = GAIN_SCALE_DB},
      {.channel = 3, .gain = 0.0, .has_gain = true, .scale = GAIN_SCALE_DB}};
  mixer_mapping_t map_4to2[2] = {
      {.dest = 0, .sources_count = 2, .sources = src_4to2_0},
      {.dest = 1, .sources_count = 2, .sources = src_4to2_1}};
  strcpy(mixer_cfgs[1].name, "4to2");
  mixer_cfgs[1].mixer.channels_in = 4;
  mixer_cfgs[1].mixer.channels_out = 2;
  mixer_cfgs[1].mixer.mapping_count = 2;
  mixer_cfgs[1].mixer.mapping = map_4to2;

  config.mixers = mixer_cfgs;
  config.mixers_count = 2;

  pipeline_step_config_t steps[2];
  memset(steps, 0, sizeof(steps));
  steps[0].type = PIPELINE_STEP_TYPE_MIXER;
  strcpy(steps[0].name, "2to4");
  steps[0].has_name = true;
  steps[1].type = PIPELINE_STEP_TYPE_MIXER;
  strcpy(steps[1].name, "4to2");
  steps[1].has_name = true;
  config.pipeline = steps;
  config.pipeline_count = 2;

  processing_parameters_t *params = processing_parameters_create(2, 2);
  pipeline_t *pipeline = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(pipeline != NULL);

  audio_chunk_t *chunk = audio_chunk_create(1024, 2);
  mutable_waveform_t ch0 = audio_chunk_get_channel(chunk, 0);
  mutable_waveform_t ch1 = audio_chunk_get_channel(chunk, 1);
  for (size_t t = 0; t < 1024; t++) {
    ch0[t] = 1.0;
    ch1[t] = 2.0;
  }
  audio_chunk_set_valid_frames(chunk, 1024);

  audio_chunk_t *output1 = audio_chunk_create(1024, 2);
  pipeline_error_t err = pipeline_process(pipeline, chunk, output1);
  ASSERT_EQ(PIPELINE_OK, err);
  ASSERT_EQ(2, audio_chunk_get_channels(output1));
  waveform_t out1_0 = audio_chunk_get_channel(output1, 0);
  waveform_t out1_1 = audio_chunk_get_channel(output1, 1);
  ASSERT_NEAR(3.0, out1_0[0], 1e-5);
  ASSERT_NEAR(3.0, out1_1[0], 1e-5);

  audio_chunk_t *chunk2 = audio_chunk_create(1024, 2);
  mutable_waveform_t ch2_0 = audio_chunk_get_channel(chunk2, 0);
  mutable_waveform_t ch2_1 = audio_chunk_get_channel(chunk2, 1);
  for (size_t t = 0; t < 1024; t++) {
    ch2_0[t] = 3.0;
    ch2_1[t] = 4.0;
  }
  audio_chunk_set_valid_frames(chunk2, 1024);

  audio_chunk_t *output2 = audio_chunk_create(1024, 2);
  err = pipeline_process(pipeline, chunk2, output2);
  ASSERT_EQ(PIPELINE_OK, err);
  ASSERT_EQ(2, audio_chunk_get_channels(output2));
  waveform_t out2_0 = audio_chunk_get_channel(output2, 0);
  ASSERT_NEAR(7.0, out2_0[0], 1e-5);

  audio_chunk_free(chunk);
  audio_chunk_free(output1);
  audio_chunk_free(chunk2);
  audio_chunk_free(output2);
  pipeline_free(pipeline);
  processing_parameters_free(params);
}

TEST(PipelineProcessValidationThrows) {
  dsp_config_t config;
  init_default_config(&config);
  processing_parameters_t *params = processing_parameters_create(2, 2);
  pipeline_t *pipeline = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(pipeline != NULL);

  audio_chunk_t *input = audio_chunk_create(1024, 2);
  audio_chunk_set_valid_frames(input, 1024);
  audio_chunk_t *output = audio_chunk_create(1024, 2);

  // 1. inputSizeMismatch
  audio_chunk_t *tooLargeInput = audio_chunk_create(2048, 2);
  audio_chunk_set_valid_frames(tooLargeInput, 2048);
  pipeline_error_t err = pipeline_process(pipeline, tooLargeInput, output);
  ASSERT_EQ(PIPELINE_ERR_INPUT_SIZE_MISMATCH, err);
  ASSERT_EQ(1024, pipeline_get_last_error_needed(pipeline));
  ASSERT_EQ(2048, pipeline_get_last_error_got(pipeline));

  // 2. input channel Count mismatch
  audio_chunk_t *wrongInputChannels = audio_chunk_create(1024, 1);
  audio_chunk_set_valid_frames(wrongInputChannels, 1024);
  err = pipeline_process(pipeline, wrongInputChannels, output);
  ASSERT_EQ(PIPELINE_ERR_CHANNEL_COUNT_MISMATCH, err);
  ASSERT_EQ(2, pipeline_get_last_error_needed(pipeline));
  ASSERT_EQ(1, pipeline_get_last_error_got(pipeline));

  // 3. output channel Count mismatch
  audio_chunk_t *wrongOutputChannels = audio_chunk_create(1024, 3);
  err = pipeline_process(pipeline, input, wrongOutputChannels);
  ASSERT_EQ(PIPELINE_ERR_CHANNEL_COUNT_MISMATCH, err);
  ASSERT_EQ(2, pipeline_get_last_error_needed(pipeline));
  ASSERT_EQ(3, pipeline_get_last_error_got(pipeline));

  // 4. output capacity too small
  audio_chunk_t *tooSmallOutput = audio_chunk_create(512, 2);
  err = pipeline_process(pipeline, input, tooSmallOutput);
  ASSERT_EQ(PIPELINE_ERR_OUTPUT_BUFFER_TOO_SMALL, err);
  ASSERT_EQ(1024, pipeline_get_last_error_needed(pipeline));
  ASSERT_EQ(512, pipeline_get_last_error_got(pipeline));

  audio_chunk_free(input);
  audio_chunk_free(output);
  audio_chunk_free(tooLargeInput);
  audio_chunk_free(wrongInputChannels);
  audio_chunk_free(wrongOutputChannels);
  audio_chunk_free(tooSmallOutput);
  pipeline_free(pipeline);
  processing_parameters_free(params);
}

TEST(ConfigLoaderParseAndValidate) {
  const char *json = "{\n"
                     "    \"devices\": {\n"
                     "        \"samplerate\": 44100,\n"
                     "        \"chunksize\": 1024,\n"
                     "        \"capture\": {\n"
                     "            \"type\": \"RawFile\",\n"
                     "            \"filename\": \"/dev/null\",\n"
                     "            \"format\": \"S16_LE\",\n"
                     "            \"channels\": 2\n"
                     "        },\n"
                     "        \"playback\": {\n"
                     "            \"type\": \"File\",\n"
                     "            \"filename\": \"/dev/null\",\n"
                     "            \"format\": \"S16_LE\",\n"
                     "            \"channels\": 2\n"
                     "        }\n"
                     "    }\n"
                     "}";
  dsp_config_t *config = NULL;
  config_error_t err;
  config_error_init(&err);
  int res = config_loader_parse(json, &config, &err);
  ASSERT_EQ(0, res);
  ASSERT_TRUE(config != NULL);

  res = dsp_config_validate(config, &err);
  ASSERT_EQ(0, res);
  ASSERT_EQ(CONFIG_ERR_NONE, err.type);

  dsp_config_free(config);

  dsp_config_t invalid_config;
  init_default_config(&invalid_config);
  invalid_config.devices.samplerate = 0;
  res = dsp_config_validate(&invalid_config, &err);
  ASSERT_NE(0, res);
  ASSERT_EQ(CONFIG_ERR_INVALID_DEVICE, err.type);
}

TEST(PipelineReload_StatePreserved) {
  dsp_config_t config;
  init_default_config(&config);

  // 1. Setup Peak EQ Biquad Filter config
  named_filter_config_t filter_cfg;
  memset(&filter_cfg, 0, sizeof(filter_cfg));
  strcpy(filter_cfg.name, "mybiquad");
  filter_cfg.filter.type = FILTER_TYPE_BIQUAD;
  filter_cfg.filter.parameters.biquad.type = BIQUAD_TYPE_PEAKING;
  filter_cfg.filter.parameters.biquad.freq = 1000.0;
  filter_cfg.filter.parameters.biquad.gain = 12.0;
  filter_cfg.filter.parameters.biquad.q = 1.0;
  filter_cfg.filter.parameters.biquad.steepness_type = STEEPNESS_TYPE_Q;
  config.filters = &filter_cfg;
  config.filters_count = 1;

  char *filter_name = strdup("mybiquad");
  pipeline_step_config_t step;
  memset(&step, 0, sizeof(step));
  step.type = PIPELINE_STEP_TYPE_FILTER;
  step.channel = 0;
  step.has_channel = true;
  step.names = &filter_name;
  step.names_count = 1;
  config.pipeline = &step;
  config.pipeline_count = 1;

  processing_parameters_t *params = processing_parameters_create(2, 2);

  // 2. Create pipeline 1
  pipeline_t *pipeline1 = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(pipeline1 != NULL);

  // 3. Prepare an impulse chunk (1.0 at index 0, then zeros)
  audio_chunk_t *impulse_chunk = audio_chunk_create(1024, 2);
  for (size_t ch = 0; ch < 2; ch++) {
    mutable_waveform_t buf = audio_chunk_get_channel(impulse_chunk, ch);
    buf[0] = 1.0;
    for (size_t t = 1; t < 1024; t++) {
      buf[t] = 0.0;
    }
  }
  audio_chunk_set_valid_frames(impulse_chunk, 1024);

  // 4. Prepare a zero chunk (for decay)
  audio_chunk_t *zero_chunk = audio_chunk_create(1024, 2);
  for (size_t ch = 0; ch < 2; ch++) {
    mutable_waveform_t buf = audio_chunk_get_channel(zero_chunk, ch);
    for (size_t t = 0; t < 1024; t++) {
      buf[t] = 0.0;
    }
  }
  audio_chunk_set_valid_frames(zero_chunk, 1024);

  // 5. Process impulse through pipeline 1 (this updates internal z1, z2 state)
  audio_chunk_t *output_chunk1_1 = audio_chunk_create(1024, 2);
  pipeline_error_t perr =
      pipeline_process(pipeline1, impulse_chunk, output_chunk1_1);
  ASSERT_EQ(PIPELINE_OK, perr);

  // 6. Process zero chunk through pipeline 1 (natural decay output)
  audio_chunk_t *output_chunk1_2 = audio_chunk_create(1024, 2);
  perr = pipeline_process(pipeline1, zero_chunk, output_chunk1_2);
  ASSERT_EQ(PIPELINE_OK, perr);

  // Read first decay sample
  waveform_t dec_out1_2 = audio_chunk_get_channel(output_chunk1_2, 0);
  double expected_first_decay_sample = dec_out1_2[0];
  // Ensure it's not zero (otherwise the test is trivial/faulty)
  ASSERT_NE(0.0, expected_first_decay_sample);

  // 7. Create pipeline 2 (a rebuilt pipeline with same config)
  pipeline_t *pipeline2 = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(pipeline2 != NULL);

  // 8. Process zero chunk through pipeline 2 (without state transfer)
  audio_chunk_t *output_chunk2 = audio_chunk_create(1024, 2);
  perr = pipeline_process(pipeline2, zero_chunk, output_chunk2);
  ASSERT_EQ(PIPELINE_OK, perr);

  // Without state transfer, the output should be exactly 0.0
  waveform_t out_chunk2 = audio_chunk_get_channel(output_chunk2, 0);
  ASSERT_EQ(0.0, out_chunk2[0]);

  // 9. Create pipeline 3 (another rebuilt pipeline)
  pipeline_t *pipeline3 = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(pipeline3 != NULL);

  // 10. Transfer state from pipeline1 (after it processed chunk 1) to pipeline3
  pipeline_transfer_state(pipeline3, pipeline1, true);

  // 11. Process zero chunk through pipeline 3 (should seamlessly continue
  // decay)
  audio_chunk_t *output_chunk3 = audio_chunk_create(1024, 2);
  perr = pipeline_process(pipeline3, zero_chunk, output_chunk3);
  ASSERT_EQ(PIPELINE_OK, perr);

  waveform_t out_chunk3 = audio_chunk_get_channel(output_chunk3, 0);
  // Output should match the expected decay value!
  ASSERT_NEAR(expected_first_decay_sample, out_chunk3[0], 1e-12);

  // Cleanup
  free(filter_name);
  pipeline_free(pipeline1);
  pipeline_free(pipeline2);
  pipeline_free(pipeline3);
  audio_chunk_free(impulse_chunk);
  audio_chunk_free(zero_chunk);
  audio_chunk_free(output_chunk1_1);
  audio_chunk_free(output_chunk1_2);
  audio_chunk_free(output_chunk2);
  audio_chunk_free(output_chunk3);
  processing_parameters_free(params);
}

TEST(Pipeline_TransferState_ParallelStepToBiquadProcessor) {
  // Config A: Step with Biquad + Gain -> Unlowered parallel filter step
  dsp_config_t config_a;
  init_default_config(&config_a);

  named_filter_config_t filters_a[2];
  memset(filters_a, 0, sizeof(filters_a));
  strcpy(filters_a[0].name, "mybiquad");
  filters_a[0].filter.type = FILTER_TYPE_BIQUAD;
  filters_a[0].filter.parameters.biquad.type = BIQUAD_TYPE_LOWPASS;
  filters_a[0].filter.parameters.biquad.freq = 1000.0;
  filters_a[0].filter.parameters.biquad.q = 5.0;

  strcpy(filters_a[1].name, "mygain");
  filters_a[1].filter.type = FILTER_TYPE_GAIN;
  filters_a[1].filter.parameters.gain.gain = 0.0;
  filters_a[1].filter.parameters.gain.scale = GAIN_SCALE_DB;

  config_a.filters = filters_a;
  config_a.filters_count = 2;

  char *step_names_a[2] = {strdup("mybiquad"), strdup("mygain")};
  pipeline_step_config_t step_a;
  memset(&step_a, 0, sizeof(step_a));
  step_a.type = PIPELINE_STEP_TYPE_FILTER;
  step_a.channel = 0;
  step_a.has_channel = true;
  step_a.names = step_names_a;
  step_a.names_count = 2;
  config_a.pipeline = &step_a;
  config_a.pipeline_count = 1;

  // Config B: Step with only Biquad -> Lowered to internal Biquad Processor
  dsp_config_t config_b;
  init_default_config(&config_b);

  named_filter_config_t filter_b;
  memset(&filter_b, 0, sizeof(filter_b));
  strcpy(filter_b.name, "mybiquad");
  filter_b.filter.type = FILTER_TYPE_BIQUAD;
  filter_b.filter.parameters.biquad.type = BIQUAD_TYPE_LOWPASS;
  filter_b.filter.parameters.biquad.freq = 1000.0;
  filter_b.filter.parameters.biquad.q = 5.0;

  config_b.filters = &filter_b;
  config_b.filters_count = 1;

  char *step_name_b = strdup("mybiquad");
  pipeline_step_config_t step_b;
  memset(&step_b, 0, sizeof(step_b));
  step_b.type = PIPELINE_STEP_TYPE_FILTER;
  step_b.channel = 0;
  step_b.has_channel = true;
  step_b.names = &step_name_b;
  step_b.names_count = 1;
  config_b.pipeline = &step_b;
  config_b.pipeline_count = 1;

  processing_parameters_t *params = processing_parameters_create(2, 2);
  pipeline_t *pipe_a = pipeline_create(&config_a, params, 0, NULL);
  ASSERT_TRUE(pipe_a != NULL);

  audio_chunk_t *impulse = audio_chunk_create(1024, 2);
  audio_chunk_get_channel(impulse, 0)[0] = 1.0;
  audio_chunk_set_valid_frames(impulse, 1024);

  audio_chunk_t *zero_chunk = audio_chunk_create(1024, 2);
  audio_chunk_set_valid_frames(zero_chunk, 1024);

  audio_chunk_t *out_a1 = audio_chunk_create(1024, 2);
  pipeline_process(pipe_a, impulse, out_a1);

  audio_chunk_t *out_a2 = audio_chunk_create(1024, 2);
  pipeline_process(pipe_a, zero_chunk, out_a2);
  double expected_decay = audio_chunk_get_channel(out_a2, 0)[0];
  ASSERT_NE(0.0, expected_decay);

  // Pipe B created fresh
  pipeline_t *pipe_b = pipeline_create(&config_b, params, 0, NULL);
  ASSERT_TRUE(pipe_b != NULL);

  // Re-run chunk 1 on pipe_a to restore state at chunk boundary
  pipeline_t *pipe_a_fresh = pipeline_create(&config_a, params, 0, NULL);
  audio_chunk_t *dummy = audio_chunk_create(1024, 2);
  pipeline_process(pipe_a_fresh, impulse, dummy);

  // Transfer state from unlowered parallel step in pipe_a_fresh to lowered
  // biquad processor in pipe_b
  pipeline_transfer_state(pipe_b, pipe_a_fresh, true);

  audio_chunk_t *out_b = audio_chunk_create(1024, 2);
  pipeline_process(pipe_b, zero_chunk, out_b);
  double actual_decay = audio_chunk_get_channel(out_b, 0)[0];
  ASSERT_NEAR(expected_decay, actual_decay, 1e-12);

  free(step_names_a[0]);
  free(step_names_a[1]);
  free(step_name_b);
  pipeline_free(pipe_a);
  pipeline_free(pipe_a_fresh);
  pipeline_free(pipe_b);
  audio_chunk_free(impulse);
  audio_chunk_free(zero_chunk);
  audio_chunk_free(out_a1);
  audio_chunk_free(out_a2);
  audio_chunk_free(dummy);
  audio_chunk_free(out_b);
  processing_parameters_free(params);
}

TEST(Pipeline_TransferState_MultipleBiquadSteps) {
  dsp_config_t config;
  init_default_config(&config);

  named_filter_config_t filters[2];
  memset(filters, 0, sizeof(filters));
  strcpy(filters[0].name, "bq_pre");
  filters[0].filter.type = FILTER_TYPE_BIQUAD;
  filters[0].filter.parameters.biquad.type = BIQUAD_TYPE_HIGHSHELF;
  filters[0].filter.parameters.biquad.freq = 4000.0;
  filters[0].filter.parameters.biquad.gain = 4.0;
  filters[0].filter.parameters.biquad.q = 1.0;

  strcpy(filters[1].name, "bq_post");
  filters[1].filter.type = FILTER_TYPE_BIQUAD;
  filters[1].filter.parameters.biquad.type = BIQUAD_TYPE_LOWPASS;
  filters[1].filter.parameters.biquad.freq = 1500.0;
  filters[1].filter.parameters.biquad.q = 3.0;

  config.filters = filters;
  config.filters_count = 2;

  char *step0_name = strdup("bq_pre");
  char *step1_name = strdup("bq_post");
  pipeline_step_config_t steps[2];
  memset(steps, 0, sizeof(steps));
  steps[0].type = PIPELINE_STEP_TYPE_FILTER;
  steps[0].channel = 0;
  steps[0].has_channel = true;
  steps[0].names = &step0_name;
  steps[0].names_count = 1;

  steps[1].type = PIPELINE_STEP_TYPE_FILTER;
  steps[1].channel = 0;
  steps[1].has_channel = true;
  steps[1].names = &step1_name;
  steps[1].names_count = 1;

  config.pipeline = steps;
  config.pipeline_count = 2;

  processing_parameters_t *params = processing_parameters_create(2, 2);
  pipeline_t *pipe1 = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(pipe1 != NULL);

  audio_chunk_t *impulse = audio_chunk_create(1024, 2);
  audio_chunk_get_channel(impulse, 0)[0] = 1.0;
  audio_chunk_set_valid_frames(impulse, 1024);

  audio_chunk_t *zero_chunk = audio_chunk_create(1024, 2);
  audio_chunk_set_valid_frames(zero_chunk, 1024);

  audio_chunk_t *out1_1 = audio_chunk_create(1024, 2);
  pipeline_process(pipe1, impulse, out1_1);

  audio_chunk_t *out1_2 = audio_chunk_create(1024, 2);
  pipeline_process(pipe1, zero_chunk, out1_2);
  double expected_decay = audio_chunk_get_channel(out1_2, 0)[0];
  ASSERT_NE(0.0, expected_decay);

  pipeline_t *pipe2 = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(pipe2 != NULL);

  pipeline_t *pipe1_fresh = pipeline_create(&config, params, 0, NULL);
  audio_chunk_t *dummy = audio_chunk_create(1024, 2);
  pipeline_process(pipe1_fresh, impulse, dummy);

  pipeline_transfer_state(pipe2, pipe1_fresh, true);

  audio_chunk_t *out2 = audio_chunk_create(1024, 2);
  pipeline_process(pipe2, zero_chunk, out2);
  double actual_decay = audio_chunk_get_channel(out2, 0)[0];
  ASSERT_NEAR(expected_decay, actual_decay, 1e-12);

  free(step0_name);
  free(step1_name);
  pipeline_free(pipe1);
  pipeline_free(pipe1_fresh);
  pipeline_free(pipe2);
  audio_chunk_free(impulse);
  audio_chunk_free(zero_chunk);
  audio_chunk_free(out1_1);
  audio_chunk_free(out1_2);
  audio_chunk_free(dummy);
  audio_chunk_free(out2);
  processing_parameters_free(params);
}

TEST(PipelineProcessPartialChunk) {
  dsp_config_t config;
  init_default_config(&config);
  processing_parameters_t *params = processing_parameters_create(2, 2);
  pipeline_t *pipeline = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(pipeline != NULL);

  audio_chunk_t *chunk = audio_chunk_create(1024, 2);
  for (size_t ch = 0; ch < 2; ch++) {
    mutable_waveform_t buf = audio_chunk_get_channel(chunk, ch);
    for (size_t t = 0; t < 1024; t++) {
      buf[t] = sin(2.0 * M_PI * 1000.0 * (double)t / 44100.0);
    }
  }
  // Configured chunk size is 1024, but we only pass 500 frames of valid data.
  size_t partial_frames = 500;
  audio_chunk_set_valid_frames(chunk, partial_frames);

  audio_chunk_t *output = audio_chunk_create(1024, 2);
  pipeline_error_t err = pipeline_process(pipeline, chunk, output);
  ASSERT_EQ(PIPELINE_OK, err);
  ASSERT_EQ(partial_frames, audio_chunk_get_valid_frames(output));
  ASSERT_EQ(2, audio_chunk_get_channels(output));

  for (size_t ch = 0; ch < 2; ch++) {
    waveform_t in_buf = audio_chunk_get_channel(chunk, ch);
    waveform_t out_buf = audio_chunk_get_channel(output, ch);
    for (size_t t = 0; t < partial_frames; t++) {
      ASSERT_NEAR(in_buf[t], out_buf[t], 1e-9);
    }
  }

  audio_chunk_free(chunk);
  audio_chunk_free(output);
  pipeline_free(pipeline);
  processing_parameters_free(params);
}

TEST(RaggedPipelineMatchesFiltersRunOneAtATime) {
  // Build a 2-channel pipeline where ch0 has 2 biquads, ch1 has 4 biquads
  dsp_config_t config;
  init_default_config(&config);
  config.devices.samplerate = 48000;
  config.devices.chunksize = 256;

  named_filter_config_t filters[6];
  memset(filters, 0, sizeof(filters));
  for (int i = 0; i < 6; i++) {
    char name[32];
    snprintf(name, sizeof(name), "bq%d", i);
    strncpy(filters[i].name, name, sizeof(filters[i].name) - 1);
    filters[i].filter.type = FILTER_TYPE_BIQUAD;
    filters[i].filter.parameters.biquad.type = BIQUAD_TYPE_PEAKING;
    filters[i].filter.parameters.biquad.freq = 500.0 + 100.0 * (double)i;
    filters[i].filter.parameters.biquad.q = 1.0;
    filters[i].filter.parameters.biquad.gain = (i % 2 == 0) ? 3.0 : -3.0;
  }
  config.filters = filters;
  config.filters_count = 6;

  char *step0_names[] = {filters[0].name, filters[1].name};
  char *step1_names[] = {filters[2].name, filters[3].name, filters[4].name,
                         filters[5].name};

  pipeline_step_config_t steps[2];
  memset(steps, 0, sizeof(steps));
  steps[0].type = PIPELINE_STEP_TYPE_FILTER;
  steps[0].has_channel = true;
  steps[0].channel = 0;
  steps[0].names = step0_names;
  steps[0].names_count = 2;

  steps[1].type = PIPELINE_STEP_TYPE_FILTER;
  steps[1].has_channel = true;
  steps[1].channel = 1;
  steps[1].names = step1_names;
  steps[1].names_count = 4;

  config.pipeline = steps;
  config.pipeline_count = 2;

  processing_parameters_t *params = processing_parameters_create(2, 2);
  pipeline_t *pipe = pipeline_create(&config, params, 256, NULL);
  ASSERT_TRUE(pipe != NULL);

  // Standalone filter references
  filter_t *ref_ch0[2];
  for (int i = 0; i < 2; i++) {
    ref_ch0[i] = filter_create(filters[i].name, &filters[i].filter, 48000, 256,
                               params, NULL);
  }
  filter_t *ref_ch1[4];
  for (int i = 0; i < 4; i++) {
    ref_ch1[i] = filter_create(filters[2 + i].name, &filters[2 + i].filter,
                               48000, 256, params, NULL);
  }

  audio_chunk_t *in_chunk = audio_chunk_create(256, 2);
  audio_chunk_t *out_chunk = audio_chunk_create(256, 2);

  double ref_w0[256];
  double ref_w1[256];
  for (size_t i = 0; i < 256; i++) {
    double v0 = sin(0.015 * (double)i) * 0.4;
    double v1 = cos(0.022 * (double)i) * 0.4;
    audio_chunk_get_channel(in_chunk, 0)[i] = v0;
    audio_chunk_get_channel(in_chunk, 1)[i] = v1;
    ref_w0[i] = v0;
    ref_w1[i] = v1;
  }
  audio_chunk_set_valid_frames(in_chunk, 256);

  pipeline_error_t perr = pipeline_process(pipe, in_chunk, out_chunk);
  ASSERT_EQ(PIPELINE_OK, perr);

  // Run reference filters
  for (int i = 0; i < 2; i++)
    filter_process(ref_ch0[i], ref_w0, 256);
  for (int i = 0; i < 4; i++)
    filter_process(ref_ch1[i], ref_w1, 256);

  waveform_t pipe_out0 = audio_chunk_get_channel(out_chunk, 0);
  waveform_t pipe_out1 = audio_chunk_get_channel(out_chunk, 1);

  for (size_t i = 0; i < 256; i++) {
    ASSERT_NEAR(ref_w0[i], pipe_out0[i], 1e-12);
    ASSERT_NEAR(ref_w1[i], pipe_out1[i], 1e-12);
  }

  // Cleanup
  for (int i = 0; i < 2; i++)
    filter_free(ref_ch0[i]);
  for (int i = 0; i < 4; i++)
    filter_free(ref_ch1[i]);
  audio_chunk_free(in_chunk);
  audio_chunk_free(out_chunk);
  pipeline_free(pipe);
  processing_parameters_free(params);
}

TEST(InterleavedPipelineMatchesParallelPipeline) {
  dsp_config_t config;
  init_default_config(&config);
  config.devices.samplerate = 48000;
  config.devices.chunksize = 256;
  config.devices.has_multithreaded = true;
  config.devices.multithreaded = true;

  named_filter_config_t filter;
  memset(&filter, 0, sizeof(filter));
  strncpy(filter.name, "peq", sizeof(filter.name) - 1);
  filter.filter.type = FILTER_TYPE_BIQUAD;
  filter.filter.parameters.biquad.type = BIQUAD_TYPE_PEAKING;
  filter.filter.parameters.biquad.freq = 1000.0;
  filter.filter.parameters.biquad.q = 1.0;
  filter.filter.parameters.biquad.gain = 4.0;

  config.filters = &filter;
  config.filters_count = 1;

  char *step_names[] = {filter.name};
  pipeline_step_config_t step;
  memset(&step, 0, sizeof(step));
  step.type = PIPELINE_STEP_TYPE_FILTER;
  step.names = step_names;
  step.names_count = 1;

  config.pipeline = &step;
  config.pipeline_count = 1;

  processing_parameters_t *params = processing_parameters_create(2, 2);
  pipeline_t *pipe = pipeline_create(&config, params, 256, NULL);
  ASSERT_TRUE(pipe != NULL);

  audio_chunk_t *in_chunk = audio_chunk_create(256, 2);
  audio_chunk_t *out_chunk = audio_chunk_create(256, 2);

  for (size_t ch = 0; ch < 2; ch++) {
    mutable_waveform_t w = audio_chunk_get_channel(in_chunk, ch);
    for (size_t i = 0; i < 256; i++) {
      w[i] = 0.3 * sin(0.01 * (double)(i + ch * 20));
    }
  }
  audio_chunk_set_valid_frames(in_chunk, 256);

  pipeline_error_t perr = pipeline_process(pipe, in_chunk, out_chunk);
  ASSERT_EQ(PIPELINE_OK, perr);

  // Standalone reference
  filter_t *ref_filter0 =
      filter_create(filter.name, &filter.filter, 48000, 256, params, NULL);
  filter_t *ref_filter1 =
      filter_create(filter.name, &filter.filter, 48000, 256, params, NULL);

  double ref_wave0[256];
  double ref_wave1[256];
  memcpy(ref_wave0, audio_chunk_get_channel(in_chunk, 0), 256 * sizeof(double));
  memcpy(ref_wave1, audio_chunk_get_channel(in_chunk, 1), 256 * sizeof(double));
  filter_process(ref_filter0, ref_wave0, 256);
  filter_process(ref_filter1, ref_wave1, 256);

  waveform_t out_w0 = audio_chunk_get_channel(out_chunk, 0);
  waveform_t out_w1 = audio_chunk_get_channel(out_chunk, 1);
  for (size_t i = 0; i < 256; i++) {
    ASSERT_NEAR(ref_wave0[i], out_w0[i], 1e-12);
    ASSERT_NEAR(ref_wave1[i], out_w1[i], 1e-12);
  }

  filter_free(ref_filter0);
  filter_free(ref_filter1);
  audio_chunk_free(in_chunk);
  audio_chunk_free(out_chunk);
  pipeline_free(pipe);
  processing_parameters_free(params);
}

TEST(Pipeline_ArbitraryChannels_Biquad) {
  const size_t num_chans = 96;
  dsp_config_t config;
  memset(&config, 0, sizeof(config));
  config.devices.samplerate = 48000;
  config.devices.chunksize = 256;
  config.devices.capture.type = AUDIO_BACKEND_TYPE_FILE;
  config.devices.capture.cfg.raw_file.channels = (int)num_chans;
  config.devices.playback.type = AUDIO_BACKEND_TYPE_FILE;
  config.devices.playback.cfg.raw_file.channels = (int)num_chans;

  named_filter_config_t filter;
  memset(&filter, 0, sizeof(filter));
  strcpy(filter.name, "bq96");
  filter.filter.type = FILTER_TYPE_BIQUAD;
  filter.filter.parameters.biquad.type = BIQUAD_TYPE_PEAKING;
  filter.filter.parameters.biquad.freq = 1000.0;
  filter.filter.parameters.biquad.q = 1.0;
  filter.filter.parameters.biquad.gain = 6.0;

  config.filters = &filter;
  config.filters_count = 1;

  char *step_names[] = {filter.name};
  pipeline_step_config_t step;
  memset(&step, 0, sizeof(step));
  step.type = PIPELINE_STEP_TYPE_FILTER;
  step.names = step_names;
  step.names_count = 1;

  config.pipeline = &step;
  config.pipeline_count = 1;

  processing_parameters_t *params =
      processing_parameters_create(num_chans, num_chans);
  pipeline_t *pipe = pipeline_create(&config, params, 256, NULL);
  ASSERT_TRUE(pipe != NULL);

  audio_chunk_t *in_chunk = audio_chunk_create(256, num_chans);
  audio_chunk_t *out_chunk = audio_chunk_create(256, num_chans);

  for (size_t ch = 0; ch < num_chans; ch++) {
    mutable_waveform_t w = audio_chunk_get_channel(in_chunk, ch);
    for (size_t i = 0; i < 256; i++) {
      w[i] = 0.2 * sin(0.01 * (double)(i + ch * 3));
    }
  }
  audio_chunk_set_valid_frames(in_chunk, 256);

  pipeline_error_t perr = pipeline_process(pipe, in_chunk, out_chunk);
  ASSERT_EQ(PIPELINE_OK, perr);

  for (size_t ch = 0; ch < num_chans; ch++) {
    filter_t *ref_filter =
        filter_create(filter.name, &filter.filter, 48000, 256, params, NULL);
    double ref_wave[256];
    memcpy(ref_wave, audio_chunk_get_channel(in_chunk, ch),
           256 * sizeof(double));
    filter_process(ref_filter, ref_wave, 256);

    waveform_t out_w = audio_chunk_get_channel(out_chunk, ch);
    for (size_t i = 0; i < 256; i++) {
      ASSERT_NEAR(ref_wave[i], out_w[i], 1e-12);
    }
    filter_free(ref_filter);
  }
  audio_chunk_free(in_chunk);
  audio_chunk_free(out_chunk);
  pipeline_free(pipe);
  processing_parameters_free(params);
}

TEST(Pipeline_ArbitraryCascade_Biquad) {
  const size_t num_filters = 144;
  dsp_config_t config;
  memset(&config, 0, sizeof(config));
  config.devices.samplerate = 48000;
  config.devices.chunksize = 128;
  config.devices.capture.type = AUDIO_BACKEND_TYPE_FILE;
  config.devices.capture.cfg.raw_file.channels = 2;
  config.devices.playback.type = AUDIO_BACKEND_TYPE_FILE;
  config.devices.playback.cfg.raw_file.channels = 2;

  named_filter_config_t *filters =
      calloc(num_filters, sizeof(named_filter_config_t));
  char **step_names = calloc(num_filters, sizeof(char *));

  for (size_t i = 0; i < num_filters; i++) {
    snprintf(filters[i].name, sizeof(filters[i].name), "bq_%zu", i);
    filters[i].filter.type = FILTER_TYPE_BIQUAD;
    filters[i].filter.parameters.biquad.type = BIQUAD_TYPE_PEAKING;
    filters[i].filter.parameters.biquad.freq = 500.0 + (double)(i % 20) * 50.0;
    filters[i].filter.parameters.biquad.q = 0.707;
    filters[i].filter.parameters.biquad.gain =
        0.05 * (double)((int)(i % 5) - 2);
    step_names[i] = filters[i].name;
  }

  config.filters = filters;
  config.filters_count = num_filters;

  pipeline_step_config_t step;
  memset(&step, 0, sizeof(step));
  step.type = PIPELINE_STEP_TYPE_FILTER;
  step.names = step_names;
  step.names_count = num_filters;

  config.pipeline = &step;
  config.pipeline_count = 1;

  processing_parameters_t *params = processing_parameters_create(2, 2);
  config_error_t err;
  config_error_init(&err);
  pipeline_t *pipe = pipeline_create(&config, params, 128, &err);
  ASSERT_TRUE(pipe != NULL);

  audio_chunk_t *in_chunk = audio_chunk_create(128, 2);
  audio_chunk_t *out_chunk = audio_chunk_create(128, 2);
  for (size_t ch = 0; ch < 2; ch++) {
    mutable_waveform_t w = audio_chunk_get_channel(in_chunk, ch);
    for (size_t i = 0; i < 128; i++) {
      w[i] = 0.1 * sin(0.02 * (double)(i + ch * 10));
    }
  }
  audio_chunk_set_valid_frames(in_chunk, 128);

  pipeline_error_t perr = pipeline_process(pipe, in_chunk, out_chunk);
  ASSERT_EQ(PIPELINE_OK, perr);

  double ref_wave[128];
  memcpy(ref_wave, audio_chunk_get_channel(in_chunk, 0), 128 * sizeof(double));
  for (size_t i = 0; i < num_filters; i++) {
    filter_t *f = filter_create(filters[i].name, &filters[i].filter, 48000, 128,
                                params, NULL);
    filter_process(f, ref_wave, 128);
    filter_free(f);
  }

  waveform_t out_w0 = audio_chunk_get_channel(out_chunk, 0);
  for (size_t i = 0; i < 128; i++) {
    ASSERT_NEAR(ref_wave[i], out_w0[i], 1e-10);
  }

  audio_chunk_free(in_chunk);
  audio_chunk_free(out_chunk);
  pipeline_free(pipe);
  processing_parameters_free(params);
  free(filters);
  free(step_names);
}

TEST(PipelineFiltersWithMoreThan64Runs) {
  const size_t num_filters =
      70; // 70 alternating runs (biquad, gain, biquad, gain...)
  dsp_config_t config;
  init_default_config(&config);
  config.devices.samplerate = 48000;
  config.devices.chunksize = 128;
  config.devices.capture.type = AUDIO_BACKEND_TYPE_FILE;
  config.devices.capture.cfg.raw_file.channels = 2;
  config.devices.playback.type = AUDIO_BACKEND_TYPE_FILE;
  config.devices.playback.cfg.raw_file.channels = 2;

  named_filter_config_t *filters =
      calloc(num_filters, sizeof(named_filter_config_t));
  char **step_names = calloc(num_filters, sizeof(char *));

  for (size_t i = 0; i < num_filters; i++) {
    snprintf(filters[i].name, sizeof(filters[i].name), "f_%zu", i);
    if (i % 2 == 0) {
      filters[i].filter.type = FILTER_TYPE_BIQUAD;
      filters[i].filter.parameters.biquad.type = BIQUAD_TYPE_PEAKING;
      filters[i].filter.parameters.biquad.freq = 1000.0;
      filters[i].filter.parameters.biquad.q = 0.707;
      filters[i].filter.parameters.biquad.gain = 0.0;
    } else {
      filters[i].filter.type = FILTER_TYPE_GAIN;
      filters[i].filter.parameters.gain.gain = 0.0;
      filters[i].filter.parameters.gain.inverted = false;
      filters[i].filter.parameters.gain.mute = false;
    }
    step_names[i] = filters[i].name;
  }

  config.filters = filters;
  config.filters_count = num_filters;

  pipeline_step_config_t step;
  memset(&step, 0, sizeof(step));
  step.type = PIPELINE_STEP_TYPE_FILTER;
  step.names = step_names;
  step.names_count = num_filters;

  config.pipeline = &step;
  config.pipeline_count = 1;

  processing_parameters_t *params = processing_parameters_create(2, 2);
  config_error_t err;
  config_error_init(&err);
  pipeline_t *pipe = pipeline_create(&config, params, 128, &err);
  ASSERT_TRUE(pipe != NULL);
  ASSERT_EQ(CONFIG_ERR_NONE, err.type);

  audio_chunk_t *in_chunk = audio_chunk_create(128, 2);
  audio_chunk_t *out_chunk = audio_chunk_create(128, 2);
  for (size_t ch = 0; ch < 2; ch++) {
    mutable_waveform_t w = audio_chunk_get_channel(in_chunk, ch);
    for (size_t i = 0; i < 128; i++) {
      w[i] = 1.0;
    }
  }
  audio_chunk_set_valid_frames(in_chunk, 128);

  pipeline_error_t perr = pipeline_process(pipe, in_chunk, out_chunk);
  ASSERT_EQ(PIPELINE_OK, perr);

  waveform_t out_w0 = audio_chunk_get_channel(out_chunk, 0);
  for (size_t i = 0; i < 128; i++) {
    ASSERT_NEAR(1.0, out_w0[i], 1e-6);
  }

  audio_chunk_free(in_chunk);
  audio_chunk_free(out_chunk);
  pipeline_free(pipe);
  processing_parameters_free(params);
  free(filters);
  free(step_names);
}

TEST(PipelineSingleThreadedPreservesFilterSteps) {
  dsp_config_t config;
  init_default_config(&config);
  config.devices.samplerate = 48000;
  config.devices.chunksize = 256;

  named_filter_config_t filters[2];
  memset(filters, 0, sizeof(filters));
  strncpy(filters[0].name, "gain1", sizeof(filters[0].name) - 1);
  filters[0].filter.type = FILTER_TYPE_GAIN;
  filters[0].filter.parameters.gain.gain = -1.0;
  filters[0].filter.parameters.gain.scale = GAIN_SCALE_DB;

  strncpy(filters[1].name, "gain2", sizeof(filters[1].name) - 1);
  filters[1].filter.type = FILTER_TYPE_GAIN;
  filters[1].filter.parameters.gain.gain = -2.0;
  filters[1].filter.parameters.gain.scale = GAIN_SCALE_DB;

  config.filters = filters;
  config.filters_count = 2;

  char *step1_names[] = {filters[0].name};
  char *step2_names[] = {filters[1].name};
  pipeline_step_config_t steps[2];
  memset(steps, 0, sizeof(steps));
  steps[0].type = PIPELINE_STEP_TYPE_FILTER;
  steps[0].names = step1_names;
  steps[0].names_count = 1;
  steps[0].has_names = true;
  steps[0].has_channel = true;
  steps[0].channel = 0;

  steps[1].type = PIPELINE_STEP_TYPE_FILTER;
  steps[1].names = step2_names;
  steps[1].names_count = 1;
  steps[1].has_names = true;
  steps[1].has_channel = true;
  steps[1].channel = 0;

  config.pipeline = steps;
  config.pipeline_count = 2;

  // Single-threaded: steps are not merged (remains 2 steps)
  config.devices.has_multithreaded = true;
  config.devices.multithreaded = false;
  processing_parameters_t *params1 = processing_parameters_create(2, 2);
  pipeline_t *pipe_st = pipeline_create(&config, params1, 256, NULL);
  ASSERT_TRUE(pipe_st != NULL);
  ASSERT_EQ(2, pipe_st->steps_count);
  pipeline_free(pipe_st);
  processing_parameters_free(params1);

  // Multithreaded: adjacent parallel filter steps are merged into 1 step
  config.devices.multithreaded = true;
  processing_parameters_t *params2 = processing_parameters_create(2, 2);
  pipeline_t *pipe_mt = pipeline_create(&config, params2, 256, NULL);
  ASSERT_TRUE(pipe_mt != NULL);
  ASSERT_EQ(1, pipe_mt->steps_count);
  pipeline_free(pipe_mt);
  processing_parameters_free(params2);
}

/* ------------------------------------------------------------------------- */
/* Regression tests from audit (core pipeline & faders)                      */
/* ------------------------------------------------------------------------- */

#define AUDIT_CORE_FRAMES 256
#define AUDIT_CORE_RATE 48000

static void audit_core_init_config(dsp_config_t *config, size_t channels) {
  memset(config, 0, sizeof(*config));
  config->devices.samplerate = AUDIT_CORE_RATE;
  config->devices.chunksize = AUDIT_CORE_FRAMES;
  config->devices.capture.type = AUDIO_BACKEND_TYPE_FILE;
  config->devices.capture.cfg.raw_file.channels = channels;
  config->devices.playback.type = AUDIO_BACKEND_TYPE_FILE;
  config->devices.playback.cfg.raw_file.channels = channels;
  config->devices.volume_ramp_time_ms = 0.0;
  config->devices.has_volume_ramp_time_ms = true;
}

static void audit_core_fill_chunk(audio_chunk_t *chunk, size_t channels) {
  for (size_t ch = 0; ch < channels; ch++) {
    mutable_waveform_t w = audio_chunk_get_channel(chunk, ch);
    for (size_t t = 0; t < AUDIT_CORE_FRAMES; t++)
      w[t] = (double)(ch + 1);
  }
  audio_chunk_set_valid_frames(chunk, AUDIT_CORE_FRAMES);
}

// 05-1.1 / 04-1.2.2 / 07-1.3: no input scratch (and no extra copy) when the
// channel count is unchanged and every capture channel is used.
TEST(AuditPipelineNoInputScratchWithoutMixer) {
  dsp_config_t config;
  audit_core_init_config(&config, 2);
  processing_parameters_t *params = processing_parameters_create(2, 2);
  pipeline_t *p = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(p != NULL);
  ASSERT_TRUE(p->input_scratch == NULL);

  audio_chunk_t *in = audio_chunk_create(AUDIT_CORE_FRAMES, 2);
  audio_chunk_t *out = audio_chunk_create(AUDIT_CORE_FRAMES, 2);
  audit_core_fill_chunk(in, 2);
  ASSERT_EQ(PIPELINE_OK, pipeline_process(p, in, out));
  ASSERT_NEAR(1.0, audio_chunk_get_channel(out, 0)[AUDIT_CORE_FRAMES - 1], 1e-12);
  ASSERT_NEAR(2.0, audio_chunk_get_channel(out, 1)[0], 1e-12);
  // In place.
  ASSERT_EQ(PIPELINE_OK, pipeline_process(p, in, in));
  ASSERT_NEAR(2.0, audio_chunk_get_channel(in, 1)[7], 1e-12);

  audio_chunk_free(in);
  audio_chunk_free(out);
  pipeline_free(p);
  processing_parameters_free(params);
}

static mixer_source_t g_audit_src_ch0 = {
    .channel = 0, .gain = 0.0, .has_gain = true, .scale = GAIN_SCALE_DB};
static mixer_source_t g_audit_src_ch1 = {
    .channel = 1, .gain = 0.0, .has_gain = true, .scale = GAIN_SCALE_DB};

// 05-1.3: with an unused capture channel the scratch exists, carries the used
// mask, and in-place processing still zeroes/ignores the unused channel.
TEST(AuditPipelineInputScratchWhenCaptureChannelUnused) {
  dsp_config_t config;
  audit_core_init_config(&config, 2);
  mixer_mapping_t maps[2] = {
      {.dest = 0, .sources_count = 1, .sources = &g_audit_src_ch0},
      {.dest = 1, .sources_count = 1, .sources = &g_audit_src_ch0}};
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

  audio_chunk_t *in = audio_chunk_create(AUDIT_CORE_FRAMES, 2);
  audit_core_fill_chunk(in, 2);
  ASSERT_EQ(PIPELINE_OK, pipeline_process(p, in, in));
  ASSERT_NEAR(1.0, audio_chunk_get_channel(in, 0)[3], 1e-12);
  ASSERT_NEAR(1.0, audio_chunk_get_channel(in, 1)[3], 1e-12);

  audio_chunk_free(in);
  pipeline_free(p);
  processing_parameters_free(params);
}

TEST(AuditPipelineSwapMixerAllUsedRunsInOutput) {
  dsp_config_t config;
  audit_core_init_config(&config, 2);
  mixer_mapping_t maps[2] = {
      {.dest = 0, .sources_count = 1, .sources = &g_audit_src_ch1},
      {.dest = 1, .sources_count = 1, .sources = &g_audit_src_ch0}};
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

  audio_chunk_t *in = audio_chunk_create(AUDIT_CORE_FRAMES, 2);
  audit_core_fill_chunk(in, 2);
  ASSERT_EQ(PIPELINE_OK, pipeline_process(p, in, in));
  ASSERT_NEAR(2.0, audio_chunk_get_channel(in, 0)[0], 1e-12);
  ASSERT_NEAR(1.0, audio_chunk_get_channel(in, 1)[AUDIT_CORE_FRAMES - 1], 1e-12);

  audio_chunk_free(in);
  pipeline_free(p);
  processing_parameters_free(params);
}

// 05-3.1: a filter referenced twice on one channel keeps both delay lines
// apart across a FILTER_PARAMETERS hot reload.
TEST(AuditPipelineTransferKeepsDuplicateFilterStatesApart) {
  dsp_config_t config;
  audit_core_init_config(&config, 1);
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
  audio_chunk_t *in = audio_chunk_create(AUDIT_CORE_FRAMES, 1);
  audio_chunk_t *out = audio_chunk_create(AUDIT_CORE_FRAMES, 1);
  memset(audio_chunk_get_channel(in, 0), 0, AUDIT_CORE_FRAMES * sizeof(double));
  audio_chunk_get_channel(in, 0)[0] = 1.0;
  audio_chunk_set_valid_frames(in, AUDIT_CORE_FRAMES);
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
    for (size_t t = 0; t < AUDIT_CORE_FRAMES; t++) {
      total += fabs(w[t]);
      if (fabs(w[t]) > peak) {
        peak = fabs(w[t]);
        peak_pos = c * AUDIT_CORE_FRAMES + t;
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
static void audit_set_used_channels_config(dsp_config_t *config,
                                           named_mixer_config_t *mixer_cfg,
                                           mixer_mapping_t *maps,
                                           named_processor_config_t *procs,
                                           size_t procs_count) {
  audit_core_init_config(config, 2);
  maps[0] = (mixer_mapping_t){.dest = 0, .sources_count = 1,
                              .sources = &g_audit_src_ch0};
  maps[1] = (mixer_mapping_t){.dest = 1, .sources_count = 1,
                              .sources = &g_audit_src_ch0};
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

static void audit_set_step(pipeline_step_config_t *step, pipeline_step_type_t type,
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
    audit_set_used_channels_config(&config, &mixer_cfg, maps, procs, 4);
    audit_set_step(&steps[0], PIPELINE_STEP_TYPE_PROCESSOR, procs[i].name, false);
    audit_set_step(&steps[1], PIPELINE_STEP_TYPE_MIXER, "left", false);
    config.pipeline = steps;
    config.pipeline_count = 2;
    ASSERT_TRUE(pipeline_compute_used_capture_channels(&config, used, 2));
    ASSERT_TRUE(used[0]);
    ASSERT_TRUE(used[1]);
  }

  // After the mixer, a processor does not read any capture channel.
  audit_set_used_channels_config(&config, &mixer_cfg, maps, procs, 4);
  audit_set_step(&steps[0], PIPELINE_STEP_TYPE_MIXER, "left", false);
  audit_set_step(&steps[1], PIPELINE_STEP_TYPE_PROCESSOR, "comp", false);
  config.pipeline = steps;
  config.pipeline_count = 2;
  ASSERT_TRUE(pipeline_compute_used_capture_channels(&config, used, 2));
  ASSERT_TRUE(used[0]);
  ASSERT_FALSE(used[1]);

  // Bypassed, neither.
  audit_set_used_channels_config(&config, &mixer_cfg, maps, procs, 4);
  audit_set_step(&steps[0], PIPELINE_STEP_TYPE_PROCESSOR, "comp", true);
  audit_set_step(&steps[1], PIPELINE_STEP_TYPE_MIXER, "left", false);
  config.pipeline = steps;
  config.pipeline_count = 2;
  ASSERT_TRUE(pipeline_compute_used_capture_channels(&config, used, 2));
  ASSERT_TRUE(used[0]);
  ASSERT_FALSE(used[1]);
}

static int audit_validate_volume(double ramp, bool has_ramp, double limit,
                                 bool has_limit) {
  filter_config_t cfg = {.type = FILTER_TYPE_VOLUME};
  cfg.parameters.volume.fader = FADER_AUX1;
  cfg.parameters.volume.ramp_time_ms = ramp;
  cfg.parameters.volume.has_ramp_time_ms = has_ramp;
  cfg.parameters.volume.limit = limit;
  cfg.parameters.volume.has_limit = has_limit;
  config_error_t err;
  config_error_init(&err);
  return g_volume_vtable.validate(&cfg, 48000, &err);
}

// 02-1.6: non-finite ramp_time_ms / limit are rejected (upstream FiniteF32).
TEST(AuditVolumeRejectsNonFiniteParameters) {
  ASSERT_EQ(0, audit_validate_volume(200.0, true, -10.0, true));
  ASSERT_EQ(-1, audit_validate_volume(NAN, true, 0.0, false));
  ASSERT_EQ(-1, audit_validate_volume(INFINITY, true, 0.0, false));
  ASSERT_EQ(-1, audit_validate_volume(-1.0, true, 0.0, false));
  ASSERT_EQ(-1, audit_validate_volume(0.0, false, NAN, true));
  ASSERT_EQ(-1, audit_validate_volume(0.0, false, -INFINITY, true));
  ASSERT_EQ(-1, audit_validate_volume(0.0, false, INFINITY, true));
}

static int audit_validate_loudness(loudness_config_t lp) {
  filter_config_t cfg = {.type = FILTER_TYPE_LOUDNESS};
  cfg.parameters.loudness = lp;
  config_error_t err;
  config_error_init(&err);
  return g_loudness_vtable.validate(&cfg, 48000, &err);
}

// 02-2.1: NaN in any loudness field is rejected.
TEST(AuditLoudnessRejectsNonFiniteParameters) {
  loudness_config_t base = {.has_reference_level = true,
                            .reference_level = -20.0};
  ASSERT_EQ(0, audit_validate_loudness(base));

  loudness_config_t lp = base;
  lp.reference_level = NAN;
  ASSERT_EQ(-1, audit_validate_loudness(lp));
  lp = base;
  lp.has_high_boost = true;
  lp.high_boost = NAN;
  ASSERT_EQ(-1, audit_validate_loudness(lp));
  lp = base;
  lp.has_low_boost = true;
  lp.low_boost = NAN;
  ASSERT_EQ(-1, audit_validate_loudness(lp));
  lp = base;
  lp.has_low_freq = true;
  lp.low_freq = NAN;
  ASSERT_EQ(-1, audit_validate_loudness(lp));
  lp = base;
  lp.has_high_freq = true;
  lp.high_freq = NAN;
  ASSERT_EQ(-1, audit_validate_loudness(lp));
  lp = base;
  lp.has_low_q = true;
  lp.low_q = NAN;
  ASSERT_EQ(-1, audit_validate_loudness(lp));
  lp = base;
  lp.has_high_q = true;
  lp.high_q = NAN;
  ASSERT_EQ(-1, audit_validate_loudness(lp));
}

#define AUDIT_FADER_FR 256
#define AUDIT_FADER_SR 48000
#define AUDIT_FADER_CHUNK_MS (1000.0 * AUDIT_FADER_FR / AUDIT_FADER_SR)

static void audit_fader_base_config(dsp_config_t *config, size_t channels) {
  memset(config, 0, sizeof(*config));
  config->devices.samplerate = AUDIT_FADER_SR;
  config->devices.chunksize = AUDIT_FADER_FR;
  config->devices.capture.type = AUDIO_BACKEND_TYPE_FILE;
  config->devices.capture.cfg.raw_file.channels = channels;
  config->devices.playback.type = AUDIO_BACKEND_TYPE_FILE;
  config->devices.playback.cfg.raw_file.channels = channels;
  config->devices.volume_ramp_time_ms = 0.0;
  config->devices.has_volume_ramp_time_ms = true;
}

static void audit_fader_fill_sine(audio_chunk_t *chunk, size_t channels, double freq,
                                  size_t chunk_idx) {
  for (size_t ch = 0; ch < channels; ch++) {
    mutable_waveform_t w = audio_chunk_get_channel(chunk, ch);
    for (size_t t = 0; t < AUDIT_FADER_FR; t++) {
      double n = (double)(chunk_idx * AUDIT_FADER_FR + t);
      w[t] = 0.5 * sin(2.0 * M_PI * freq * n / AUDIT_FADER_SR);
    }
  }
  audio_chunk_set_valid_frames(chunk, AUDIT_FADER_FR);
}

static double audit_fader_rms(waveform_t w, size_t n) {
  double acc = 0.0;
  for (size_t i = 0; i < n; i++)
    acc += w[i] * w[i];
  return sqrt(acc / (double)n);
}

// 05-2.1 / 02-1.1(2): an Aux fader without a Volume filter still tracks its
// target, so a Loudness filter on it responds to volume changes.
TEST(AuditFaderUnusedAuxTracksTargetAndDrivesLoudness) {
  dsp_config_t config;
  audit_fader_base_config(&config, 1);
  named_filter_config_t fcfg;
  memset(&fcfg, 0, sizeof(fcfg));
  strcpy(fcfg.name, "loud");
  fcfg.filter.type = FILTER_TYPE_LOUDNESS;
  fcfg.filter.parameters.loudness.has_reference_level = true;
  fcfg.filter.parameters.loudness.reference_level = -20.0;
  fcfg.filter.parameters.loudness.fader = FADER_AUX1;
  config.filters = &fcfg;
  config.filters_count = 1;
  char *names[1] = {"loud"};
  pipeline_step_config_t step;
  memset(&step, 0, sizeof(step));
  step.type = PIPELINE_STEP_TYPE_FILTER;
  step.names = names;
  step.names_count = 1;
  config.pipeline = &step;
  config.pipeline_count = 1;

  processing_parameters_t *params = processing_parameters_create(1, 1);
  pipeline_t *p = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(p != NULL);
  audio_chunk_t *in = audio_chunk_create(AUDIT_FADER_FR, 1);
  audio_chunk_t *out = audio_chunk_create(AUDIT_FADER_FR, 1);

  audit_fader_fill_sine(in, 1, 40.0, 0);
  ASSERT_EQ(PIPELINE_OK, pipeline_process(p, in, out));
  double rms_flat = audit_fader_rms(audio_chunk_get_channel(out, 0), AUDIT_FADER_FR);

  processing_parameters_set_target_volume_for_fader(params, -45.0, FADER_AUX1);
  double boosted = 0.0;
  for (size_t c = 1; c < 20; c++) {
    audit_fader_fill_sine(in, 1, 40.0, c);
    ASSERT_EQ(PIPELINE_OK, pipeline_process(p, in, out));
    boosted = audit_fader_rms(audio_chunk_get_channel(out, 0), AUDIT_FADER_FR);
  }
  ASSERT_NEAR(
      -45.0,
      processing_parameters_get_current_volume_for_fader(params, FADER_AUX1),
      1e-9);
  // Full 10 dB low-shelf boost at 40 Hz: well above the flat level.
  ASSERT_TRUE(boosted > 2.0 * rms_flat);

  audio_chunk_free(in);
  audio_chunk_free(out);
  pipeline_free(p);
  processing_parameters_free(params);
}

// 05-2.2 / 02-1.1(1,3): Loudness placed before Volume on the same Aux fader,
// on two channels, multithreaded: both channels see the same level for the
// same chunk, so identical inputs give identical outputs.
TEST(AuditFaderAllChannelsSeeTheSameChunk) {
  dsp_config_t config;
  audit_fader_base_config(&config, 2);
  config.devices.has_multithreaded = true;
  config.devices.multithreaded = true;
  named_filter_config_t fcfg[2];
  memset(fcfg, 0, sizeof(fcfg));
  strcpy(fcfg[0].name, "loud");
  fcfg[0].filter.type = FILTER_TYPE_LOUDNESS;
  fcfg[0].filter.parameters.loudness.has_reference_level = true;
  fcfg[0].filter.parameters.loudness.reference_level = -10.0;
  fcfg[0].filter.parameters.loudness.fader = FADER_AUX1;
  strcpy(fcfg[1].name, "vol");
  fcfg[1].filter.type = FILTER_TYPE_VOLUME;
  fcfg[1].filter.parameters.volume.fader = FADER_AUX1;
  fcfg[1].filter.parameters.volume.has_ramp_time_ms = true;
  fcfg[1].filter.parameters.volume.ramp_time_ms = 6.0 * AUDIT_FADER_CHUNK_MS;
  config.filters = fcfg;
  config.filters_count = 2;
  char *names[2] = {"loud", "vol"};
  pipeline_step_config_t step;
  memset(&step, 0, sizeof(step));
  step.type = PIPELINE_STEP_TYPE_FILTER;
  step.names = names;
  step.names_count = 2;
  config.pipeline = &step;
  config.pipeline_count = 1;

  processing_parameters_t *params = processing_parameters_create(2, 2);
  pipeline_t *p = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(p != NULL);
  audio_chunk_t *in = audio_chunk_create(AUDIT_FADER_FR, 2);
  audio_chunk_t *out = audio_chunk_create(AUDIT_FADER_FR, 2);

  audit_fader_fill_sine(in, 2, 60.0, 0);
  ASSERT_EQ(PIPELINE_OK, pipeline_process(p, in, out));
  processing_parameters_set_target_volume_for_fader(params, -40.0, FADER_AUX1);
  for (size_t c = 1; c < 10; c++) {
    audit_fader_fill_sine(in, 2, 60.0, c);
    ASSERT_EQ(PIPELINE_OK, pipeline_process(p, in, out));
    waveform_t a = audio_chunk_get_channel(out, 0);
    waveform_t b = audio_chunk_get_channel(out, 1);
    for (size_t t = 0; t < AUDIT_FADER_FR; t++)
      ASSERT_NEAR(a[t], b[t], 1e-15);
  }
  audio_chunk_free(in);
  audio_chunk_free(out);
  pipeline_free(p);
  processing_parameters_free(params);
}

// 05-2.3 / 02-1.2: Loudness follows the mute ramp instead of snapping to the
// mute level on the first muted chunk. With the main fader ramping 0 -> -100
// dB over 10 chunks and a reference of -20 dB, the first muted chunk ends at
// -10 dB, so loudness must still be inactive: output == volume ramp alone.
TEST(AuditFaderLoudnessFollowsMuteRamp) {
  named_filter_config_t fcfg;
  memset(&fcfg, 0, sizeof(fcfg));
  strcpy(fcfg.name, "loud");
  fcfg.filter.type = FILTER_TYPE_LOUDNESS;
  fcfg.filter.parameters.loudness.has_reference_level = true;
  fcfg.filter.parameters.loudness.reference_level = -20.0;
  fcfg.filter.parameters.loudness.attenuate_mid = true;
  fcfg.filter.parameters.loudness.fader = FADER_MAIN;
  char *names[1] = {"loud"};
  pipeline_step_config_t step;
  memset(&step, 0, sizeof(step));
  step.type = PIPELINE_STEP_TYPE_FILTER;
  step.names = names;
  step.names_count = 1;

  dsp_config_t with;
  audit_fader_base_config(&with, 1);
  with.devices.volume_ramp_time_ms = 10.0 * AUDIT_FADER_CHUNK_MS;
  with.filters = &fcfg;
  with.filters_count = 1;
  with.pipeline = &step;
  with.pipeline_count = 1;
  dsp_config_t without;
  audit_fader_base_config(&without, 1);
  without.devices.volume_ramp_time_ms = 10.0 * AUDIT_FADER_CHUNK_MS;

  processing_parameters_t *pa = processing_parameters_create(1, 1);
  processing_parameters_t *pb = processing_parameters_create(1, 1);
  pipeline_t *a = pipeline_create(&with, pa, 0, NULL);
  pipeline_t *b = pipeline_create(&without, pb, 0, NULL);
  ASSERT_TRUE(a && b);
  audio_chunk_t *in = audio_chunk_create(AUDIT_FADER_FR, 1);
  audio_chunk_t *oa = audio_chunk_create(AUDIT_FADER_FR, 1);
  audio_chunk_t *ob = audio_chunk_create(AUDIT_FADER_FR, 1);
  audit_fader_fill_sine(in, 1, 1000.0, 0);
  ASSERT_EQ(PIPELINE_OK, pipeline_process(a, in, oa));
  ASSERT_EQ(PIPELINE_OK, pipeline_process(b, in, ob));

  processing_parameters_set_muted(pa, true);
  processing_parameters_set_muted(pb, true);
  audit_fader_fill_sine(in, 1, 1000.0, 1);
  ASSERT_EQ(PIPELINE_OK, pipeline_process(a, in, oa));
  ASSERT_EQ(PIPELINE_OK, pipeline_process(b, in, ob));
  waveform_t wa = audio_chunk_get_channel(oa, 0);
  waveform_t wb = audio_chunk_get_channel(ob, 0);
  for (size_t t = 0; t < AUDIT_FADER_FR; t++)
    ASSERT_NEAR(wb[t], wa[t], 1e-12);
  ASSERT_NEAR(-10.0, a->faders.levels.faders[FADER_MAIN].end_db, 1e-9);

  audio_chunk_free(in);
  audio_chunk_free(oa);
  audio_chunk_free(ob);
  pipeline_free(a);
  pipeline_free(b);
  processing_parameters_free(pa);
  processing_parameters_free(pb);
}

// 05-2.4 / 02-1.3: a ramp processed over fewer samples than the chunk size
// still ends on the chunk's end level.
TEST(AuditVolumeRampUsesProcessedCount) {
  processing_parameters_t *params = processing_parameters_create(1, 1);
  filter_config_t cfg = {.type = FILTER_TYPE_VOLUME};
  cfg.parameters.volume.fader = FADER_MAIN;
  cfg.parameters.volume.has_ramp_time_ms = true;
  cfg.parameters.volume.ramp_time_ms = 2.0 * 1000.0 * 8.0 / AUDIT_FADER_SR;
  volume_filter_t *f =
      (volume_filter_t *)g_volume_vtable.create("v", &cfg, AUDIT_FADER_SR, 8, params, NULL);
  ASSERT_TRUE(f != NULL);
  double w[8];
  for (int i = 0; i < 8; i++)
    w[i] = 1.0;
  g_volume_vtable.process(f, w, 8);
  processing_parameters_set_target_volume_for_fader(params, -20.0, FADER_MAIN);
  for (int i = 0; i < 4; i++)
    w[i] = 1.0;
  g_volume_vtable.process(f, w, 4);
  // First of two ramp chunks: 0 -> -10 dB laid out over the 4 samples.
  ASSERT_NEAR(1.0, w[0], 1e-12);
  ASSERT_NEAR(pow(10.0, -7.5 / 20.0), w[3], 1e-12);
  g_volume_vtable.free(f);
  processing_parameters_free(params);
}

// 05-3.2(1) / 02-1.5(1): a self-driven Volume filter does not take over the
// state of a filter on a different fader.
TEST(AuditVolumeTransferIgnoresDifferentFader) {
  processing_parameters_t *params = processing_parameters_create(1, 1);
  processing_parameters_set_target_volume_for_fader(params, -30.0, FADER_AUX1);
  processing_parameters_set_current_volume_for_fader(params, -30.0, FADER_AUX1);
  filter_config_t c1 = {.type = FILTER_TYPE_VOLUME};
  c1.parameters.volume.fader = FADER_AUX1;
  c1.parameters.volume.has_ramp_time_ms = true;
  c1.parameters.volume.ramp_time_ms = 0.0;
  filter_config_t c2 = c1;
  c2.parameters.volume.fader = FADER_AUX2;
  volume_filter_t *src =
      (volume_filter_t *)g_volume_vtable.create("v", &c1, AUDIT_FADER_SR, 8, params, NULL);
  volume_filter_t *dst =
      (volume_filter_t *)g_volume_vtable.create("v", &c2, AUDIT_FADER_SR, 8, params, NULL);
  g_volume_vtable.transfer_state(dst, src);
  double w[8];
  for (int i = 0; i < 8; i++)
    w[i] = 1.0;
  g_volume_vtable.process(dst, w, 8);
  ASSERT_NEAR(1.0, w[7], 1e-12);
  ASSERT_NEAR(
      0.0,
      processing_parameters_get_current_volume_for_fader(params, FADER_AUX2),
      1e-12);
  g_volume_vtable.free(src);
  g_volume_vtable.free(dst);
  processing_parameters_free(params);
}

// 05-3.2(2) / 02-1.5(2): ramp time change mid-ramp restarts from the current
// level at the new speed, or settles at once when ramping is disabled
// (upstream Faders::update_parameters).
TEST(AuditFaderTransferRampTimeChange) {
  processing_parameters_t *params = processing_parameters_create(1, 1);
  fader_settings_t slow[FADER_COUNT], none[FADER_COUNT], fast[FADER_COUNT];
  for (int i = 0; i < FADER_COUNT; i++) {
    slow[i] = (fader_settings_t){.ramp_time_ms = 10.0 * AUDIT_FADER_CHUNK_MS, .limit = 50};
    none[i] = (fader_settings_t){.ramp_time_ms = 0.0, .limit = 50};
    fast[i] = (fader_settings_t){.ramp_time_ms = 4.0 * AUDIT_FADER_CHUNK_MS, .limit = 50};
  }
  pipeline_faders_t a, b, c;
  pipeline_faders_init(&a, slow, params, AUDIT_FADER_FR, AUDIT_FADER_SR);
  processing_parameters_set_target_volume(params, -20.0);
  for (int i = 0; i < 3; i++)
    pipeline_faders_prepare_chunk(&a);
  ASSERT_NEAR(-6.0, a.faders[0].current_db, 1e-9);

  pipeline_faders_init(&b, none, params, AUDIT_FADER_FR, AUDIT_FADER_SR);
  pipeline_faders_transfer(&b, &a);
  ASSERT_EQ(0, b.faders[0].ramp_step);
  ASSERT_NEAR(-20.0, b.faders[0].current_db, 1e-9);

  pipeline_faders_init(&c, fast, params, AUDIT_FADER_FR, AUDIT_FADER_SR);
  pipeline_faders_transfer(&c, &a);
  ASSERT_EQ(1, c.faders[0].ramp_step);
  ASSERT_NEAR(-6.0, c.faders[0].ramp_start, 1e-9);
  pipeline_faders_prepare_chunk(&c);
  ASSERT_NEAR(-6.0, c.levels.faders[0].start_db, 1e-9);
  ASSERT_NEAR(-9.5, c.levels.faders[0].end_db, 1e-9);
  processing_parameters_free(params);
}

// 07-1.1: a structural reload during a ramp starts the new pipeline settled at
// the target instead of restarting a full-length ramp.
TEST(AuditFaderStructuralReloadStartsSettled) {
  dsp_config_t config;
  audit_fader_base_config(&config, 1);
  config.devices.volume_ramp_time_ms = 20.0 * AUDIT_FADER_CHUNK_MS;
  processing_parameters_t *params = processing_parameters_create(1, 1);
  pipeline_t *a = pipeline_create(&config, params, 0, NULL);
  audio_chunk_t *in = audio_chunk_create(AUDIT_FADER_FR, 1);
  audio_chunk_t *out = audio_chunk_create(AUDIT_FADER_FR, 1);
  for (size_t t = 0; t < AUDIT_FADER_FR; t++)
    audio_chunk_get_channel(in, 0)[t] = 1.0;
  audio_chunk_set_valid_frames(in, AUDIT_FADER_FR);
  ASSERT_EQ(PIPELINE_OK, pipeline_process(a, in, out));
  processing_parameters_set_target_volume(params, -20.0);
  for (int i = 0; i < 3; i++)
    ASSERT_EQ(PIPELINE_OK, pipeline_process(a, in, out));
  ASSERT_TRUE(processing_parameters_get_current_volume(params) > -19.0);

  pipeline_t *b = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(b != NULL);
  pipeline_transfer_state(b, a, false);
  pipeline_free(a);
  ASSERT_EQ(PIPELINE_OK, pipeline_process(b, in, out));
  waveform_t w = audio_chunk_get_channel(out, 0);
  ASSERT_NEAR(pow(10.0, -1.0), w[0], 1e-12);
  ASSERT_NEAR(pow(10.0, -1.0), w[AUDIT_FADER_FR - 1], 1e-12);

  audio_chunk_free(in);
  audio_chunk_free(out);
  pipeline_free(b);
  processing_parameters_free(params);
}

// 05-2.5: the bank seeds limited levels; the published current volume never
// exceeds the limit.
TEST(AuditFaderSeedRespectsLimit) {
  dsp_config_t config;
  audit_fader_base_config(&config, 1);
  config.devices.has_volume_limit = true;
  config.devices.volume_limit = -6.0;
  processing_parameters_t *params = processing_parameters_create(1, 1);
  processing_parameters_set_target_volume(params, 0.0);
  pipeline_t *p = pipeline_create(&config, params, 0, NULL);
  ASSERT_NEAR(-6.0, p->faders.levels.faders[0].end_db, 1e-12);
  audio_chunk_t *in = audio_chunk_create(AUDIT_FADER_FR, 1);
  for (size_t t = 0; t < AUDIT_FADER_FR; t++)
    audio_chunk_get_channel(in, 0)[t] = 1.0;
  audio_chunk_set_valid_frames(in, AUDIT_FADER_FR);
  ASSERT_EQ(PIPELINE_OK, pipeline_process(p, in, in));
  ASSERT_NEAR(pow(10.0, -6.0 / 20.0), audio_chunk_get_channel(in, 0)[0], 1e-12);
  ASSERT_NEAR(-6.0, processing_parameters_get_current_volume(params), 1e-12);
  audio_chunk_free(in);
  pipeline_free(p);
  processing_parameters_free(params);
}

TEST_MAIN()
