#if defined(__linux__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "audio/sample_format.h"
#include "backend/audio_backend.h"
#include "config/config_error.h"
#include "config/config_gen.h"
#include "config/configuration.h"
#include "mixer/mixer.h"
#include "test_support.h"

static void set_test_channels(dsp_config_t *config, int cap_chs, int play_chs) {
  config->devices.capture.type = AUDIO_BACKEND_TYPE_FILE;
  snprintf(config->devices.capture.cfg.raw_file.filename,
           sizeof(config->devices.capture.cfg.raw_file.filename), "/dev/null");
  config->devices.capture.cfg.raw_file.has_filename = true;
  config->devices.capture.cfg.raw_file.channels = cap_chs;
  config->devices.playback.type = AUDIO_BACKEND_TYPE_FILE;
  config->devices.playback.cfg.raw_file.channels = play_chs;
}

TEST(ParseValidConfig) {
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
  int res = dsp_config_parse_json(json, &config, &err);
  ASSERT_EQ(0, res);
  ASSERT_TRUE(config != NULL);
  ASSERT_EQ(44100, config->devices.samplerate);
  ASSERT_EQ(1024, config->devices.chunksize);
  ASSERT_EQ(2, capture_device_config_get_channels(&config->devices.capture));
  ASSERT_EQ(2, playback_device_config_get_channels(&config->devices.playback));
  dsp_config_free(config);
}

TEST(ParseResamplerConfigProfile) {
  const char *json = "{\n"
                     "    \"devices\": {\n"
                     "        \"samplerate\": 48000,\n"
                     "        \"chunksize\": 1024,\n"
                     "        \"capture_samplerate\": 44100,\n"
                     "        \"resampler\": {\n"
                     "            \"type\": \"AsyncSinc\",\n"
                     "            \"profile\": \"Balanced\"\n"
                     "        },\n"
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
  int res = dsp_config_parse_json(json, &config, &err);
  ASSERT_EQ(0, res);
  ASSERT_TRUE(config != NULL);
  ASSERT_EQ(48000, config->devices.samplerate);
  ASSERT_EQ(44100, config->devices.capture_samplerate);
  ASSERT_TRUE(config->devices.has_resampler);
  ASSERT_EQ(RESAMPLER_TYPE_ASYNC_SINC, config->devices.resampler.type);
  ASSERT_TRUE(config->devices.resampler.has_profile);
  ASSERT_STR_EQ("Balanced", config->devices.resampler.profile);
  ASSERT_FALSE(config->devices.resampler.has_interpolation);
  ASSERT_FALSE(config->devices.resampler.has_window);
  dsp_config_free(config);
}

TEST(ParseResamplerConfigFree) {
  const char *json = "{\n"
                     "    \"devices\": {\n"
                     "        \"samplerate\": 48000,\n"
                     "        \"chunksize\": 1024,\n"
                     "        \"capture_samplerate\": 44100,\n"
                     "        \"resampler\": {\n"
                     "            \"type\": \"AsyncSinc\",\n"
                     "            \"interpolation\": \"Cubic\",\n"
                     "            \"sinc_len\": 256,\n"
                     "            \"oversampling_factor\": 512,\n"
                     "            \"window\": \"BlackmanHarris2\",\n"
                     "            \"f_cutoff\": 0.95\n"
                     "        },\n"
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
  int res = dsp_config_parse_json(json, &config, &err);
  ASSERT_EQ(0, res);
  ASSERT_TRUE(config != NULL);
  ASSERT_EQ(48000, config->devices.samplerate);
  ASSERT_EQ(44100, config->devices.capture_samplerate);
  ASSERT_TRUE(config->devices.has_resampler);
  ASSERT_EQ(RESAMPLER_TYPE_ASYNC_SINC, config->devices.resampler.type);
  ASSERT_FALSE(config->devices.resampler.has_profile);
  ASSERT_TRUE(config->devices.resampler.has_interpolation);
  ASSERT_STR_EQ("Cubic", config->devices.resampler.interpolation);
  ASSERT_EQ(256, config->devices.resampler.sinc_len);
  ASSERT_EQ(512, config->devices.resampler.oversampling_factor);
  ASSERT_TRUE(config->devices.resampler.has_window);
  ASSERT_STR_EQ("BlackmanHarris2", config->devices.resampler.window);
  ASSERT_NEAR(0.95, config->devices.resampler.f_cutoff, 1e-6);
  dsp_config_free(config);
}

TEST(ParseInvalidJSON) {
  const char *json = "{\n"
                     "    \"devices\": {\n"
                     "        \"samplerate\": 44100,\n"
                     "        \"chunksize\": 1024,\n"
                     "        \"capture\": {\n"
                     "            \"type\": \"RawFile\",\n"
                     "            \"channels\": 2\n";
  dsp_config_t *config = NULL;
  config_error_t err;
  config_error_init(&err);
  int res = dsp_config_parse_json(json, &config, &err);
  ASSERT_NE(0, res);
  ASSERT_EQ(CONFIG_ERR_PARSE, err.type);
  if (config)
    dsp_config_free(config);
}

TEST(ValidateSampleRate) {
  dsp_config_t config;
  memset(&config, 0, sizeof(config));
  config.devices.samplerate = 0;
  config.devices.chunksize = 1024;
  set_test_channels(&config, 2, 2);
  config_error_t err;
  config_error_init(&err);
  int res = dsp_config_validate(&config, &err);
  ASSERT_NE(0, res);
  ASSERT_EQ(CONFIG_ERR_INVALID_DEVICE, err.type);
  ASSERT_TRUE(strstr(err.message, "Sample rate must be positive") != NULL);
}

TEST(ValidateChunkSize) {
  dsp_config_t config;
  memset(&config, 0, sizeof(config));
  config.devices.samplerate = 44100;
  config.devices.chunksize = 0;
  set_test_channels(&config, 2, 2);
  config_error_t err;
  config_error_init(&err);
  int res = dsp_config_validate(&config, &err);
  ASSERT_NE(0, res);
  ASSERT_EQ(CONFIG_ERR_INVALID_DEVICE, err.type);
  ASSERT_TRUE(strstr(err.message, "Chunk size must be positive") != NULL);
}

TEST(ValidateChannels) {
  dsp_config_t config;
  memset(&config, 0, sizeof(config));
  config.devices.samplerate = 44100;
  config.devices.chunksize = 1024;
  set_test_channels(&config, 0, 2);
  config_error_t err;
  config_error_init(&err);
  int res = dsp_config_validate(&config, &err);
  ASSERT_NE(0, res);
  ASSERT_EQ(CONFIG_ERR_INVALID_DEVICE, err.type);
  ASSERT_TRUE(strstr(err.message, "Capture channels must be positive") != NULL);

  set_test_channels(&config, 2, 0);
  res = dsp_config_validate(&config, &err);
  ASSERT_NE(0, res);
  ASSERT_EQ(CONFIG_ERR_INVALID_DEVICE, err.type);
  ASSERT_TRUE(strstr(err.message, "Playback channels must be positive") !=
              NULL);
}

TEST(ValidateRawFileCapture_NonexistentFile) {
  dsp_config_t config;
  memset(&config, 0, sizeof(config));
  config.devices.samplerate = 44100;
  config.devices.chunksize = 1024;
  config.devices.capture.type = AUDIO_BACKEND_TYPE_FILE;
  snprintf(config.devices.capture.cfg.raw_file.filename,
           sizeof(config.devices.capture.cfg.raw_file.filename),
           "/nonexistent/file/path/that/does/not/exist.raw");
  config.devices.capture.cfg.raw_file.has_filename = true;
  config.devices.capture.cfg.raw_file.channels = 2;
  config.devices.playback.type = AUDIO_BACKEND_TYPE_FILE;
  config.devices.playback.cfg.raw_file.channels = 2;
  config_error_t err;
  config_error_init(&err);
  int res = dsp_config_validate(&config, &err);
  ASSERT_NE(0, res);
  ASSERT_EQ(CONFIG_ERR_INVALID_DEVICE, err.type);
  ASSERT_TRUE(strstr(err.message, "Could not open input file") != NULL);
}

TEST(ValidatePipelineFilterMissingNames) {
  dsp_config_t config;
  memset(&config, 0, sizeof(config));
  config.devices.samplerate = 44100;
  config.devices.chunksize = 1024;
  set_test_channels(&config, 2, 2);

  pipeline_step_config_t step;
  memset(&step, 0, sizeof(step));
  step.type = PIPELINE_STEP_TYPE_FILTER;
  step.channel = 0;
  step.has_channel = true;

  config.pipeline = &step;
  config.pipeline_count = 1;

  config_error_t err;
  config_error_init(&err);
  int res = dsp_config_validate(&config, &err);
  ASSERT_NE(0, res);
  ASSERT_EQ(CONFIG_ERR_INVALID_PIPELINE, err.type);
  ASSERT_TRUE(strstr(err.message, "must have 'names'") != NULL);
}

TEST(ValidatePipelineFilterUndefined) {
  dsp_config_t config;
  memset(&config, 0, sizeof(config));
  config.devices.samplerate = 44100;
  config.devices.chunksize = 1024;
  set_test_channels(&config, 2, 2);

  char *name = strdup("undefined_filter");
  pipeline_step_config_t step;
  memset(&step, 0, sizeof(step));
  step.type = PIPELINE_STEP_TYPE_FILTER;
  step.channel = 0;
  step.has_channel = true;
  step.names = &name;
  step.names_count = 1;

  config.pipeline = &step;
  config.pipeline_count = 1;

  config_error_t err;
  config_error_init(&err);
  int res = dsp_config_validate(&config, &err);
  free(name);
  ASSERT_NE(0, res);
  ASSERT_EQ(CONFIG_ERR_INVALID_PIPELINE, err.type);
  ASSERT_TRUE(strstr(err.message, "referenced in pipeline but not defined") !=
              NULL);
}

TEST(ValidatePipelineFilterChannelOutOfRange) {
  dsp_config_t config;
  memset(&config, 0, sizeof(config));
  config.devices.samplerate = 44100;
  config.devices.chunksize = 1024;
  set_test_channels(&config, 2, 2);

  named_filter_config_t nf;
  memset(&nf, 0, sizeof(nf));
  strcpy(nf.name, "myfilter");
  nf.filter.type = FILTER_TYPE_GAIN;
  nf.filter.parameters.gain.gain = 0.0;
  nf.filter.parameters.gain.has_gain = true;

  config.filters = &nf;
  config.filters_count = 1;

  char *name = strdup("myfilter");
  pipeline_step_config_t step;
  memset(&step, 0, sizeof(step));
  step.type = PIPELINE_STEP_TYPE_FILTER;
  step.channel = 2;
  step.has_channel = true;
  step.names = &name;
  step.names_count = 1;

  config.pipeline = &step;
  config.pipeline_count = 1;

  config_error_t err;
  config_error_init(&err);
  int res = dsp_config_validate(&config, &err);
  free(name);
  ASSERT_NE(0, res);
  ASSERT_EQ(CONFIG_ERR_INVALID_PIPELINE, err.type);
  ASSERT_TRUE(strstr(err.message,
                     "references channel 2 but pipeline only has 2") != NULL);
}

TEST(ValidatePipelineMixerMissingName) {
  dsp_config_t config;
  memset(&config, 0, sizeof(config));
  config.devices.samplerate = 44100;
  config.devices.chunksize = 1024;
  set_test_channels(&config, 2, 2);

  pipeline_step_config_t step;
  memset(&step, 0, sizeof(step));
  step.type = PIPELINE_STEP_TYPE_MIXER;

  config.pipeline = &step;
  config.pipeline_count = 1;

  config_error_t err;
  config_error_init(&err);
  int res = dsp_config_validate(&config, &err);
  ASSERT_NE(0, res);
  ASSERT_EQ(CONFIG_ERR_INVALID_PIPELINE, err.type);
  ASSERT_TRUE(strstr(err.message, "must have 'name'") != NULL);
}

TEST(ValidatePipelineMixerUndefined) {
  dsp_config_t config;
  memset(&config, 0, sizeof(config));
  config.devices.samplerate = 44100;
  config.devices.chunksize = 1024;
  set_test_channels(&config, 2, 2);

  pipeline_step_config_t step;
  memset(&step, 0, sizeof(step));
  step.type = PIPELINE_STEP_TYPE_MIXER;
  strcpy(step.name, "undefined_mixer");
  step.has_name = true;

  config.pipeline = &step;
  config.pipeline_count = 1;

  config_error_t err;
  config_error_init(&err);
  int res = dsp_config_validate(&config, &err);
  ASSERT_NE(0, res);
  ASSERT_EQ(CONFIG_ERR_INVALID_PIPELINE, err.type);
  ASSERT_TRUE(strstr(err.message, "referenced in pipeline but not defined") !=
              NULL);
}

TEST(ValidatePipelineMixerInputMismatch) {
  dsp_config_t config;
  memset(&config, 0, sizeof(config));
  config.devices.samplerate = 44100;
  config.devices.chunksize = 1024;
  set_test_channels(&config, 2, 2);

  named_mixer_config_t nm;
  memset(&nm, 0, sizeof(nm));
  strcpy(nm.name, "mymixer");
  nm.mixer.channels_in = 3;
  nm.mixer.channels_out = 2;

  config.mixers = &nm;
  config.mixers_count = 1;

  pipeline_step_config_t step;
  memset(&step, 0, sizeof(step));
  step.type = PIPELINE_STEP_TYPE_MIXER;
  strcpy(step.name, "mymixer");
  step.has_name = true;

  config.pipeline = &step;
  config.pipeline_count = 1;

  config_error_t err;
  config_error_init(&err);
  int res = dsp_config_validate(&config, &err);
  ASSERT_NE(0, res);
  ASSERT_EQ(CONFIG_ERR_INVALID_PIPELINE, err.type);
  ASSERT_TRUE(strstr(err.message,
                     "expects 3 input channel(s) but pipeline has 2") != NULL);
}

TEST(ValidatePipelineOutputMismatch) {
  dsp_config_t config;
  memset(&config, 0, sizeof(config));
  config.devices.samplerate = 44100;
  config.devices.chunksize = 1024;
  set_test_channels(&config, 2, 2);

  named_mixer_config_t nm;
  memset(&nm, 0, sizeof(nm));
  strcpy(nm.name, "mymixer");
  nm.mixer.channels_in = 2;
  nm.mixer.channels_out = 3;

  config.mixers = &nm;
  config.mixers_count = 1;

  pipeline_step_config_t step;
  memset(&step, 0, sizeof(step));
  step.type = PIPELINE_STEP_TYPE_MIXER;
  strcpy(step.name, "mymixer");
  step.has_name = true;

  config.pipeline = &step;
  config.pipeline_count = 1;

  config_error_t err;
  config_error_init(&err);
  int res = dsp_config_validate(&config, &err);
  ASSERT_NE(0, res);
  ASSERT_EQ(CONFIG_ERR_INVALID_PIPELINE, err.type);
  ASSERT_TRUE(strstr(err.message,
                     "outputs 3 channel(s) but playback device expects 2") !=
              NULL);
}

TEST(ValidatePipelineBypassedStep) {
  dsp_config_t config;
  memset(&config, 0, sizeof(config));
  config.devices.samplerate = 44100;
  config.devices.chunksize = 1024;
  set_test_channels(&config, 2, 2);

  named_filter_config_t nf;
  memset(&nf, 0, sizeof(nf));
  strcpy(nf.name, "myfilter");
  nf.filter.type = FILTER_TYPE_GAIN;
  nf.filter.parameters.gain.gain = 0.0;
  nf.filter.parameters.gain.has_gain = true;

  config.filters = &nf;
  config.filters_count = 1;

  char *name = strdup("myfilter");
  pipeline_step_config_t step;
  memset(&step, 0, sizeof(step));
  step.type = PIPELINE_STEP_TYPE_FILTER;
  step.channel = 2;
  step.has_channel = true;
  step.names = &name;
  step.names_count = 1;
  step.bypassed = true;

  config.pipeline = &step;
  config.pipeline_count = 1;

  config_error_t err;
  config_error_init(&err);
  int res = dsp_config_validate(&config, &err);
  free(name);
  ASSERT_EQ(0, res);
}

TEST(ConfigErrorDescription) {
  config_error_t err;
  char buf[256];

  config_error_set(&err, CONFIG_ERR_PARSE, "test");
  config_error_description(&err, buf, sizeof(buf));
  ASSERT_STR_EQ("Parse error: test", buf);

  config_error_set(&err, CONFIG_ERR_VALIDATION, "test");
  config_error_description(&err, buf, sizeof(buf));
  ASSERT_STR_EQ("Validation error: test", buf);

  config_error_set(&err, CONFIG_ERR_INVALID_FILTER, "test");
  config_error_description(&err, buf, sizeof(buf));
  ASSERT_STR_EQ("Invalid filter: test", buf);

  config_error_set(&err, CONFIG_ERR_INVALID_MIXER, "test");
  config_error_description(&err, buf, sizeof(buf));
  ASSERT_STR_EQ("Invalid mixer: test", buf);

  config_error_set(&err, CONFIG_ERR_INVALID_PIPELINE, "test");
  config_error_description(&err, buf, sizeof(buf));
  ASSERT_STR_EQ("Invalid pipeline: test", buf);
}

TEST(MixerValidatorDestOutOfRange) {
  mixer_mapping_t mapping;
  memset(&mapping, 0, sizeof(mapping));
  mapping.dest = 2;

  mixer_config_t mixer;
  memset(&mixer, 0, sizeof(mixer));
  mixer.channels_in = 2;
  mixer.channels_out = 2;
  mixer.mapping = &mapping;
  mixer.mapping_count = 1;

  config_error_t err;
  config_error_init(&err);
  int res = mixer_config_validate(&mixer, &err);
  ASSERT_NE(0, res);
  ASSERT_EQ(CONFIG_ERR_INVALID_MIXER, err.type);
  ASSERT_TRUE(strstr(err.message, "mixer dest 2 >= channels_out 2") != NULL);
}

TEST(MixerValidatorDuplicateDest) {
  mixer_mapping_t mappings[2];
  memset(mappings, 0, sizeof(mappings));
  mappings[0].dest = 0;
  mappings[1].dest = 0;

  mixer_config_t mixer;
  memset(&mixer, 0, sizeof(mixer));
  mixer.channels_in = 2;
  mixer.channels_out = 2;
  mixer.mapping = mappings;
  mixer.mapping_count = 2;

  config_error_t err;
  config_error_init(&err);
  int res = mixer_config_validate(&mixer, &err);
  ASSERT_NE(0, res);
  ASSERT_EQ(CONFIG_ERR_INVALID_MIXER, err.type);
  ASSERT_TRUE(strstr(err.message, "mixer dest 0 mapped more than once") !=
              NULL);
}

TEST(MixerValidatorSourceOutOfRange) {
  mixer_source_t src;
  memset(&src, 0, sizeof(src));
  src.channel = 2;

  mixer_mapping_t mapping;
  memset(&mapping, 0, sizeof(mapping));
  mapping.dest = 0;
  mapping.sources = &src;
  mapping.sources_count = 1;

  mixer_config_t mixer;
  memset(&mixer, 0, sizeof(mixer));
  mixer.channels_in = 2;
  mixer.channels_out = 2;
  mixer.mapping = &mapping;
  mixer.mapping_count = 1;

  config_error_t err;
  config_error_init(&err);
  int res = mixer_config_validate(&mixer, &err);
  ASSERT_NE(0, res);
  ASSERT_EQ(CONFIG_ERR_INVALID_MIXER, err.type);
  ASSERT_TRUE(strstr(err.message, "mixer source channel 2 >= channels_in 2") !=
              NULL);
}

// Upstream's duplicate-source check in validate_mixer is dead code (it never
// pushes to `input_channels`), while `Mixer::from_config` pushes both sources
// and sums them. A config that lists an input channel twice therefore loads
// and plays in real CamillaDSP, so the port must accept it too.
TEST(MixerValidatorDuplicateSourceAccepted) {
  mixer_source_t srcs[2];
  memset(srcs, 0, sizeof(srcs));
  srcs[0].channel = 0;
  srcs[1].channel = 0;

  mixer_mapping_t mapping;
  memset(&mapping, 0, sizeof(mapping));
  mapping.dest = 0;
  mapping.sources = srcs;
  mapping.sources_count = 2;

  mixer_config_t mixer;
  memset(&mixer, 0, sizeof(mixer));
  mixer.channels_in = 2;
  mixer.channels_out = 2;
  mixer.mapping = &mapping;
  mixer.mapping_count = 1;

  config_error_t err;
  config_error_init(&err);
  ASSERT_EQ(0, mixer_config_validate(&mixer, &err));
  ASSERT_EQ(CONFIG_ERR_NONE, err.type);
}

TEST(ValidateInvalidFilterConfig) {
  named_filter_config_t nf;
  memset(&nf, 0, sizeof(nf));
  strcpy(nf.name, "mygain");
  nf.filter.type = FILTER_TYPE_GAIN;
  nf.filter.parameters.gain.gain = 200.0;
  nf.filter.parameters.gain.has_gain = true;

  dsp_config_t config;
  memset(&config, 0, sizeof(config));
  config.devices.samplerate = 44100;
  config.devices.chunksize = 1024;
  set_test_channels(&config, 2, 2);
  config.filters = &nf;
  config.filters_count = 1;

  config_error_t err;
  config_error_init(&err);

  // 1. Unused invalid filter in config is ignored during validation (matches
  // upstream)
  int res = dsp_config_validate(&config, &err);
  ASSERT_EQ(0, res);

  // 2. Active filter step referencing invalid filter fails validation
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

  config_error_init(&err);
  res = dsp_config_validate(&config, &err);
  ASSERT_NE(0, res);
  ASSERT_EQ(CONFIG_ERR_INVALID_FILTER, err.type);
  ASSERT_TRUE(strstr(err.message, "Gain must be less than +150 dB") != NULL);

  // 3. Bypassed filter step referencing invalid filter succeeds
  step.bypassed = true;
  config_error_init(&err);
  res = dsp_config_validate(&config, &err);
  ASSERT_EQ(0, res);

  free(filter_name);
}

TEST(ValidateInvalidMixerConfig) {
  mixer_mapping_t mapping;
  memset(&mapping, 0, sizeof(mapping));
  mapping.dest = 5;

  named_mixer_config_t nm;
  memset(&nm, 0, sizeof(nm));
  strcpy(nm.name, "mymixer");
  nm.mixer.channels_in = 2;
  nm.mixer.channels_out = 2;
  nm.mixer.mapping = &mapping;
  nm.mixer.mapping_count = 1;

  dsp_config_t config;
  memset(&config, 0, sizeof(config));
  config.devices.samplerate = 44100;
  config.devices.chunksize = 1024;
  set_test_channels(&config, 2, 2);
  config.mixers = &nm;
  config.mixers_count = 1;

  config_error_t err;
  config_error_init(&err);

  // 1. Unused invalid mixer is ignored during validation (matches upstream)
  int res = dsp_config_validate(&config, &err);
  ASSERT_EQ(0, res);

  // 2. Active mixer step referencing invalid mixer fails validation
  pipeline_step_config_t step;
  memset(&step, 0, sizeof(step));
  step.type = PIPELINE_STEP_TYPE_MIXER;
  strcpy(step.name, "mymixer");
  step.has_name = true;
  config.pipeline = &step;
  config.pipeline_count = 1;

  config_error_init(&err);
  res = dsp_config_validate(&config, &err);
  ASSERT_NE(0, res);
  ASSERT_EQ(CONFIG_ERR_INVALID_MIXER, err.type);
  ASSERT_TRUE(strstr(err.message, "mixer dest 5 >= channels_out 2") != NULL);

  // 3. Bypassed mixer step referencing invalid mixer succeeds
  step.bypassed = true;
  config_error_init(&err);
  res = dsp_config_validate(&config, &err);
  ASSERT_EQ(0, res);
}

TEST(ParseFullConfigWithMixerAndFilter) {
  const char *json =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 48000,\n"
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
      "    },\n"
      "    \"filters\": {\n"
      "        \"mygain\": {\n"
      "            \"type\": \"Gain\",\n"
      "            \"parameters\": {\n"
      "                \"gain\": -6.0\n"
      "            }\n"
      "        }\n"
      "    },\n"
      "    \"mixers\": {\n"
      "        \"balance\": {\n"
      "            \"channels\": {\n"
      "                \"in\": 2,\n"
      "                \"out\": 2\n"
      "            },\n"
      "            \"mapping\": [\n"
      "                {\n"
      "                    \"dest\": 0,\n"
      "                    \"sources\": [\n"
      "                        { \"channel\": 0, \"gain\": 0.0 }\n"
      "                    ]\n"
      "                },\n"
      "                {\n"
      "                    \"dest\": 1,\n"
      "                    \"sources\": [\n"
      "                        { \"channel\": 1, \"gain\": -3.0 }\n"
      "                    ]\n"
      "                }\n"
      "            ]\n"
      "        }\n"
      "    },\n"
      "    \"pipeline\": [\n"
      "        {\n"
      "            \"type\": \"Mixer\",\n"
      "            \"name\": \"balance\"\n"
      "        },\n"
      "        {\n"
      "            \"type\": \"Filter\",\n"
      "            \"channels\": [0],\n"
      "            \"names\": [\"mygain\"]\n"
      "        }\n"
      "    ]\n"
      "}";

  dsp_config_t *config = NULL;
  config_error_t err;
  config_error_init(&err);
  int res = dsp_config_parse_json(json, &config, &err);

  ASSERT_EQ(0, res);
  ASSERT_TRUE(config != NULL);

  // Validate devices
  ASSERT_EQ(48000, config->devices.samplerate);
  ASSERT_EQ(1024, config->devices.chunksize);
  ASSERT_EQ(2, capture_device_config_get_channels(&config->devices.capture));

  // Validate filters
  ASSERT_EQ(1, config->filters_count);
  ASSERT_STR_EQ("mygain", config->filters[0].name);
  ASSERT_EQ(FILTER_TYPE_GAIN, config->filters[0].filter.type);
  ASSERT_DOUBLE_EQ(-6.0, config->filters[0].filter.parameters.gain.gain);

  // Validate mixers
  ASSERT_EQ(1, config->mixers_count);
  ASSERT_STR_EQ("balance", config->mixers[0].name);
  ASSERT_EQ(2, config->mixers[0].mixer.channels_in);
  ASSERT_EQ(2, config->mixers[0].mixer.channels_out);
  ASSERT_EQ(2, config->mixers[0].mixer.mapping_count);
  ASSERT_EQ(0, config->mixers[0].mixer.mapping[0].dest);
  ASSERT_EQ(0, config->mixers[0].mixer.mapping[0].sources[0].channel);
  ASSERT_DOUBLE_EQ(0.0, config->mixers[0].mixer.mapping[0].sources[0].gain);
  ASSERT_EQ(1, config->mixers[0].mixer.mapping[1].dest);
  ASSERT_EQ(1, config->mixers[0].mixer.mapping[1].sources[0].channel);
  ASSERT_DOUBLE_EQ(-3.0, config->mixers[0].mixer.mapping[1].sources[0].gain);

  // Validate pipeline
  ASSERT_EQ(2, config->pipeline_count);
  ASSERT_EQ(PIPELINE_STEP_TYPE_MIXER, config->pipeline[0].type);
  ASSERT_STR_EQ("balance", config->pipeline[0].name);
  ASSERT_EQ(PIPELINE_STEP_TYPE_FILTER, config->pipeline[1].type);
  ASSERT_EQ(0, config->pipeline[1].channel);
  ASSERT_EQ(1, config->pipeline[1].names_count);
  ASSERT_STR_EQ("mygain", config->pipeline[1].names[0]);

  dsp_config_free(config);
}

TEST(ParseChannelLabels) {
  const char *json =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\n"
      "            \"type\": \"RawFile\",\n"
      "            \"filename\": \"/dev/null\",\n"
      "            \"format\": \"S16_LE\",\n"
      "            \"channels\": 4,\n"
      "            \"labels\": [\"Left\", \"Right\", null, \"Center\"]\n"
      "        },\n"
      "        \"playback\": {\n"
      "            \"type\": \"File\",\n"
      "            \"filename\": \"/dev/null\",\n"
      "            \"format\": \"S16_LE\",\n"
      "            \"channels\": 4,\n"
      "            \"labels\": [\"OutputLeft\", null, \"OutputRight\", null]\n"
      "        }\n"
      "    }\n"
      "}";
  dsp_config_t *config = NULL;
  config_error_t err;
  config_error_init(&err);
  int res = dsp_config_parse_json(json, &config, &err);
  if (res != 0) {
    printf("PARSE ERROR: %s\n", err.message);
  }
  ASSERT_EQ(0, res);
  ASSERT_TRUE(config != NULL);

  // Verify capture labels
  ASSERT_TRUE(config->devices.capture.has_labels);
  ASSERT_EQ(4, config->devices.capture.labels_count);
  ASSERT_STR_EQ("Left", config->devices.capture.labels[0]);
  ASSERT_STR_EQ("Right", config->devices.capture.labels[1]);
  ASSERT_TRUE(config->devices.capture.labels[2] == NULL);
  ASSERT_STR_EQ("Center", config->devices.capture.labels[3]);

  // Verify playback labels
  ASSERT_TRUE(config->devices.playback.has_labels);
  ASSERT_EQ(4, config->devices.playback.labels_count);
  ASSERT_STR_EQ("OutputLeft", config->devices.playback.labels[0]);
  ASSERT_TRUE(config->devices.playback.labels[1] == NULL);
  ASSERT_STR_EQ("OutputRight", config->devices.playback.labels[2]);
  ASSERT_TRUE(config->devices.playback.labels[3] == NULL);

  dsp_config_free(config);
}

TEST(RejectWavS24_4_RJ) {
  const char *json = "{\n"
                     "    \"devices\": {\n"
                     "        \"samplerate\": 48000,\n"
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
                     "            \"channels\": 2,\n"
                     "            \"format\": \"S24_4_RJ_LE\",\n"
                     "            \"wav_header\": true\n"
                     "        }\n"
                     "    }\n"
                     "}";
  dsp_config_t *config = NULL;
  config_error_t err;
  config_error_init(&err);
  int res = dsp_config_parse_json(json, &config, &err);
  ASSERT_EQ(-1, res);
  ASSERT_STR_EQ("Wav files do not support the S24_4_RJ_LE sample format",
                err.message);
  ASSERT_TRUE(config == NULL);
}

TEST(RejectFileWavHeaderS24_4_RJ_LE) {
  const char *json = "{\n"
                     "    \"devices\": {\n"
                     "        \"samplerate\": 48000,\n"
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
                     "            \"channels\": 2,\n"
                     "            \"format\": \"S24_4_RJ_LE\",\n"
                     "            \"wav_header\": true\n"
                     "        }\n"
                     "    }\n"
                     "}";
  dsp_config_t *config = NULL;
  config_error_t err;
  config_error_init(&err);
  int res = dsp_config_parse_json(json, &config, &err);
  ASSERT_EQ(-1, res);
  ASSERT_STR_EQ("Wav files do not support the S24_4_RJ_LE sample format",
                err.message);
  ASSERT_TRUE(config == NULL);
}

TEST(AcceptMissingResamplerWhenRatesDiffer) {
  const char *json = "{\n"
                     "    \"devices\": {\n"
                     "        \"samplerate\": 48000,\n"
                     "        \"capture_samplerate\": 44100,\n"
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
  int res = dsp_config_parse_json(json, &config, &err);
  ASSERT_EQ(0, res);
  ASSERT_TRUE(config != NULL);
  dsp_config_free(config);
}

TEST(WavFileOverrideWithoutResampler) {
  char wav_path[256];
  snprintf(wav_path, sizeof(wav_path), "/tmp/test_ovr_96k_%d.wav", getpid());
  remove(wav_path);

  // Write a minimal 96kHz 2-channel 16-bit WAV header
  FILE *f = fopen(wav_path, "wb");
  ASSERT_TRUE(f != NULL);
  uint8_t header[44] = {'R', 'I',  'F',  'F',  36,  0,   0,    0,    'W',
                        'A', 'V',  'E',  'f',  'm', 't', ' ',  16,   0,
                        0,   0,    1,    0,    2,   0,   0x00, 0x77, 0x01,
                        0,   0x00, 0xdc, 0x05, 0,   4,   0,    16,   0,
                        'd', 'a',  't',  'a',  0,   0,   0,    0};
  fwrite(header, 1, 44, f);
  fclose(f);

  char json[1024];
  snprintf(json, sizeof(json),
           "{\n"
           "    \"devices\": {\n"
           "        \"samplerate\": 48000,\n"
           "        \"chunksize\": 1024,\n"
           "        \"capture\": {\n"
           "            \"type\": \"WavFile\",\n"
           "            \"filename\": \"%s\"\n"
           "        },\n"
           "        \"playback\": {\n"
           "            \"type\": \"File\",\n"
           "            \"filename\": \"/dev/null\",\n"
           "            \"format\": \"S16_LE\",\n"
           "            \"channels\": 2\n"
           "        }\n"
           "    }\n"
           "}",
           wav_path);

  dsp_config_t *config = NULL;
  config_error_t err;
  config_error_init(&err);
  int res = dsp_config_parse_json(json, &config, &err);
  ASSERT_EQ(0, res);
  ASSERT_TRUE(config != NULL);
  // Overrides samplerate 48000 -> 96000 and scales chunksize 1024 -> 2048
  ASSERT_EQ(96000, (int)config->devices.samplerate);
  ASSERT_EQ(2048, (int)config->devices.chunksize);
  ASSERT_EQ(2, (int)config->devices.capture.cfg.wav_file.channels);
  dsp_config_free(config);
  remove(wav_path);
}

TEST(WavFileOverrideWithResampler) {
  char wav_path[256];
  snprintf(wav_path, sizeof(wav_path), "/tmp/test_ovr_44k_%d.wav", getpid());
  remove(wav_path);

  // Write a minimal 44.1kHz 2-channel 16-bit WAV header
  FILE *f = fopen(wav_path, "wb");
  ASSERT_TRUE(f != NULL);
  uint8_t header[44] = {'R', 'I',  'F',  'F',  36,  0,   0,    0,    'W',
                        'A', 'V',  'E',  'f',  'm', 't', ' ',  16,   0,
                        0,   0,    1,    0,    2,   0,   0x44, 0xac, 0x00,
                        0,   0x10, 0xb1, 0x02, 0,   4,   0,    16,   0,
                        'd', 'a',  't',  'a',  0,   0,   0,    0};
  fwrite(header, 1, 44, f);
  fclose(f);

  char json[1024];
  snprintf(json, sizeof(json),
           "{\n"
           "    \"devices\": {\n"
           "        \"samplerate\": 352800,\n"
           "        \"chunksize\": 2048,\n"
           "        \"resampler\": {\n"
           "            \"type\": \"Synchronous\"\n"
           "        },\n"
           "        \"capture\": {\n"
           "            \"type\": \"WavFile\",\n"
           "            \"filename\": \"%s\"\n"
           "        },\n"
           "        \"playback\": {\n"
           "            \"type\": \"File\",\n"
           "            \"filename\": \"/dev/null\",\n"
           "            \"format\": \"S16_LE\",\n"
           "            \"channels\": 2\n"
           "        }\n"
           "    }\n"
           "}",
           wav_path);

  dsp_config_t *config = NULL;
  config_error_t err;
  config_error_init(&err);
  int res = dsp_config_parse_json(json, &config, &err);
  ASSERT_EQ(0, res);
  ASSERT_TRUE(config != NULL);
  // Resampler configured: samplerate stays 352800, capture_samplerate set to
  // 44100
  ASSERT_EQ(352800, (int)config->devices.samplerate);
  ASSERT_EQ(44100, (int)config->devices.capture_samplerate);
  ASSERT_TRUE(config->devices.has_resampler);
  dsp_config_free(config);
  remove(wav_path);
}

#if defined(ENABLE_ALSA)
TEST(AlsaThreadedOptionParsing) {
  const char *json_threaded_false = "{\n"
                                    "    \"devices\": {\n"
                                    "        \"samplerate\": 48000,\n"
                                    "        \"chunksize\": 1024,\n"
                                    "        \"capture\": {\n"
                                    "            \"type\": \"Alsa\",\n"
                                    "            \"channels\": 2,\n"
                                    "            \"device\": \"hw:0,0\",\n"
                                    "            \"threaded\": false\n"
                                    "        },\n"
                                    "        \"playback\": {\n"
                                    "            \"type\": \"Alsa\",\n"
                                    "            \"channels\": 2,\n"
                                    "            \"device\": \"hw:0,0\",\n"
                                    "            \"threaded\": false\n"
                                    "        }\n"
                                    "    }\n"
                                    "}";

  dsp_config_t *config = NULL;
  config_error_t err;
  config_error_init(&err);
  int res = dsp_config_parse_json(json_threaded_false, &config, &err);
  ASSERT_EQ(0, res);
  ASSERT_TRUE(config != NULL);
  ASSERT_FALSE(config->devices.capture.cfg.alsa.threaded);
  ASSERT_TRUE(config->devices.capture.cfg.alsa.has_threaded);
  ASSERT_FALSE(config->devices.playback.cfg.alsa.threaded);
  ASSERT_TRUE(config->devices.playback.cfg.alsa.has_threaded);
  dsp_config_free(config);

  const char *json_threaded_default = "{\n"
                                      "    \"devices\": {\n"
                                      "        \"samplerate\": 48000,\n"
                                      "        \"chunksize\": 1024,\n"
                                      "        \"capture\": {\n"
                                      "            \"type\": \"Alsa\",\n"
                                      "            \"channels\": 2,\n"
                                      "            \"device\": \"hw:0,0\"\n"
                                      "        },\n"
                                      "        \"playback\": {\n"
                                      "            \"type\": \"Alsa\",\n"
                                      "            \"channels\": 2,\n"
                                      "            \"device\": \"hw:0,0\"\n"
                                      "        }\n"
                                      "    }\n"
                                      "}";

  config = NULL;
  res = dsp_config_parse_json(json_threaded_default, &config, &err);
  ASSERT_EQ(0, res);
  ASSERT_TRUE(config != NULL);
  ASSERT_FALSE(config->devices.capture.cfg.alsa.threaded);
  ASSERT_FALSE(config->devices.playback.cfg.alsa.threaded);
  dsp_config_free(config);
}
#endif

TEST(StrictValidationRejectFileCapture) {
  const char *json = "{\n"
                     "    \"devices\": {\n"
                     "        \"samplerate\": 44100,\n"
                     "        \"chunksize\": 1024,\n"
                     "        \"capture\": {\n"
                     "            \"type\": \"File\",\n"
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
  int res = dsp_config_parse_json(json, &config, &err);
  ASSERT_NE(0, res);
  ASSERT_EQ(CONFIG_ERR_PARSE, err.type);
  ASSERT_TRUE(strstr(err.message, "unknown variant 'File'") != NULL);
  ASSERT_TRUE(config == NULL);
}

TEST(StrictValidationRejectRawFilePlayback) {
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
                     "            \"type\": \"RawFile\",\n"
                     "            \"filename\": \"/dev/null\",\n"
                     "            \"format\": \"S16_LE\",\n"
                     "            \"channels\": 2\n"
                     "        }\n"
                     "    }\n"
                     "}";
  dsp_config_t *config = NULL;
  config_error_t err;
  config_error_init(&err);
  int res = dsp_config_parse_json(json, &config, &err);
  ASSERT_NE(0, res);
  ASSERT_EQ(CONFIG_ERR_PARSE, err.type);
  ASSERT_TRUE(strstr(err.message, "unknown variant 'RawFile'") != NULL);
  ASSERT_TRUE(config == NULL);
}

TEST(StrictValidationRejectMissingRawFileFilename) {
  const char *json = "{\n"
                     "    \"devices\": {\n"
                     "        \"samplerate\": 44100,\n"
                     "        \"chunksize\": 1024,\n"
                     "        \"capture\": {\n"
                     "            \"type\": \"RawFile\",\n"
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
  int res = dsp_config_parse_json(json, &config, &err);
  ASSERT_NE(0, res);
  ASSERT_EQ(CONFIG_ERR_PARSE, err.type);
  ASSERT_TRUE(strstr(err.message, "missing field 'filename'") != NULL);
  ASSERT_TRUE(config == NULL);
}

TEST(StrictValidationRejectMissingRawFileFormat) {
  const char *json = "{\n"
                     "    \"devices\": {\n"
                     "        \"samplerate\": 44100,\n"
                     "        \"chunksize\": 1024,\n"
                     "        \"capture\": {\n"
                     "            \"type\": \"RawFile\",\n"
                     "            \"filename\": \"/dev/null\",\n"
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
  int res = dsp_config_parse_json(json, &config, &err);
  ASSERT_NE(0, res);
  ASSERT_EQ(CONFIG_ERR_PARSE, err.type);
  ASSERT_TRUE(strstr(err.message, "missing field 'format'") != NULL);
  ASSERT_TRUE(config == NULL);
}

TEST(StrictValidationRejectMissingFilePlaybackFilename) {
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
                     "            \"format\": \"S16_LE\",\n"
                     "            \"channels\": 2\n"
                     "        }\n"
                     "    }\n"
                     "}";
  dsp_config_t *config = NULL;
  config_error_t err;
  config_error_init(&err);
  int res = dsp_config_parse_json(json, &config, &err);
  ASSERT_NE(0, res);
  ASSERT_EQ(CONFIG_ERR_PARSE, err.type);
  ASSERT_TRUE(strstr(err.message, "missing field 'filename'") != NULL);
  ASSERT_TRUE(config == NULL);
}

TEST(StrictValidationRejectMissingSamplerate) {
  const char *json = "{\n"
                     "    \"devices\": {\n"
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
  int res = dsp_config_parse_json(json, &config, &err);
  ASSERT_NE(0, res);
  ASSERT_EQ(CONFIG_ERR_PARSE, err.type);
  ASSERT_TRUE(strstr(err.message, "samplerate") != NULL);
  ASSERT_TRUE(config == NULL);
}

TEST(StrictValidationRejectUnknownRootField) {
  const char *json =
      "{\n"
      "    \"unknown_root\": 123,\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\"type\": \"RawFile\", \"filename\": "
      "\"/dev/null\", \"format\": \"S16_LE\", \"channels\": 2},\n"
      "        \"playback\": {\"type\": \"File\", \"filename\": \"/dev/null\", "
      "\"format\": \"S16_LE\", \"channels\": 2}\n"
      "    }\n"
      "}";
  dsp_config_t *config = NULL;
  config_error_t err;
  config_error_init(&err);
  int res = dsp_config_parse_json(json, &config, &err);
  ASSERT_NE(0, res);
  ASSERT_EQ(CONFIG_ERR_PARSE, err.type);
  ASSERT_TRUE(strstr(err.message, "unknown field 'unknown_root'") != NULL);
  ASSERT_TRUE(config == NULL);
}

TEST(StrictValidationRejectUnknownDevicesField) {
  const char *json =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 1024,\n"
      "        \"invalid_device_key\": true,\n"
      "        \"capture\": {\"type\": \"RawFile\", \"filename\": "
      "\"/dev/null\", \"format\": \"S16_LE\", \"channels\": 2},\n"
      "        \"playback\": {\"type\": \"File\", \"filename\": \"/dev/null\", "
      "\"format\": \"S16_LE\", \"channels\": 2}\n"
      "    }\n"
      "}";
  dsp_config_t *config = NULL;
  config_error_t err;
  config_error_init(&err);
  int res = dsp_config_parse_json(json, &config, &err);
  ASSERT_NE(0, res);
  ASSERT_EQ(CONFIG_ERR_PARSE, err.type);
  ASSERT_TRUE(strstr(err.message, "unknown field 'invalid_device_key'") !=
              NULL);
  ASSERT_TRUE(config == NULL);
}

TEST(StrictValidationRejectUnknownResamplerField) {
  const char *json =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 1024,\n"
      "        \"resampler\": {\"type\": \"AsyncPoly\", \"profile\": "
      "\"Fast\"},\n"
      "        \"capture\": {\"type\": \"RawFile\", \"filename\": "
      "\"/dev/null\", \"format\": \"S16_LE\", \"channels\": 2},\n"
      "        \"playback\": {\"type\": \"File\", \"filename\": \"/dev/null\", "
      "\"format\": \"S16_LE\", \"channels\": 2}\n"
      "    }\n"
      "}";
  dsp_config_t *config = NULL;
  config_error_t err;
  config_error_init(&err);
  int res = dsp_config_parse_json(json, &config, &err);
  ASSERT_NE(0, res);
  ASSERT_EQ(CONFIG_ERR_PARSE, err.type);
  ASSERT_TRUE(strstr(err.message, "unknown field 'profile'") != NULL);
  ASSERT_TRUE(config == NULL);
}

TEST(StrictValidationRejectUnknownFilterField) {
  const char *json =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\"type\": \"RawFile\", \"filename\": "
      "\"/dev/null\", \"format\": \"S16_LE\", \"channels\": 2},\n"
      "        \"playback\": {\"type\": \"File\", \"filename\": \"/dev/null\", "
      "\"format\": \"S16_LE\", \"channels\": 2}\n"
      "    },\n"
      "    \"filters\": {\n"
      "        \"g1\": {\"type\": \"Gain\", \"parameters\": {\"gain\": 0.0, "
      "\"invalid_gain_param\": 123}}\n"
      "    }\n"
      "}";
  dsp_config_t *config = NULL;
  config_error_t err;
  config_error_init(&err);
  int res = dsp_config_parse_json(json, &config, &err);
  ASSERT_NE(0, res);
  ASSERT_EQ(CONFIG_ERR_PARSE, err.type);
  ASSERT_TRUE(strstr(err.message, "unknown field 'invalid_gain_param'") !=
              NULL);
  ASSERT_TRUE(config == NULL);
}

TEST(StrictValidationRejectUnknownPipelineField) {
  const char *json =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\"type\": \"RawFile\", \"filename\": "
      "\"/dev/null\", \"format\": \"S16_LE\", \"channels\": 2},\n"
      "        \"playback\": {\"type\": \"File\", \"filename\": \"/dev/null\", "
      "\"format\": \"S16_LE\", \"channels\": 2}\n"
      "    },\n"
      "    \"filters\": {\"g1\": {\"type\": \"Gain\", \"parameters\": "
      "{\"gain\": 0.0}}},\n"
      "    \"pipeline\": [{\"type\": \"Filter\", \"names\": [\"g1\"], "
      "\"channels\": [0], \"invalid_pipe_field\": 1}]\n"
      "}";
  dsp_config_t *config = NULL;
  config_error_t err;
  config_error_init(&err);
  int res = dsp_config_parse_json(json, &config, &err);
  ASSERT_NE(0, res);
  ASSERT_EQ(CONFIG_ERR_PARSE, err.type);
  ASSERT_TRUE(strstr(err.message, "unknown field 'invalid_pipe_field'") !=
              NULL);
  ASSERT_TRUE(config == NULL);
}

/* --- Strict tagged-enum parsing of filter parameters (audit 01-1, 09-5) --- */

/**
 * @brief Builds a config with a single filter definition spliced in.
 */
static void build_filter_config_json(char *buf, size_t buf_len,
                                     const char *filter_json) {
  snprintf(buf, buf_len,
           "{\n"
           "    \"devices\": {\n"
           "        \"samplerate\": 44100,\n"
           "        \"chunksize\": 1024,\n"
           "        \"capture\": {\"type\": \"RawFile\", \"filename\": "
           "\"/dev/null\", \"format\": \"S16_LE\", \"channels\": 2},\n"
           "        \"playback\": {\"type\": \"File\", \"filename\": "
           "\"/dev/null\", \"format\": \"S16_LE\", \"channels\": 2}\n"
           "    },\n"
           "    \"filters\": {\"f1\": %s}\n"
           "}",
           filter_json);
}

/**
 * @brief Parses a filter definition and returns the parse result.
 */
static int parse_filter_json(const char *filter_json, config_error_t *err) {
  char json[1024];
  build_filter_config_json(json, sizeof(json), filter_json);
  dsp_config_t *config = NULL;
  config_error_init(err);
  int res = dsp_config_parse_json(json, &config, err);
  if (config)
    dsp_config_free(config);
  return res;
}

TEST(FilterRejectsUnknownBiquadType) {
  config_error_t err;
  int res = parse_filter_json(
      "{\"type\": \"Biquad\", \"parameters\": {\"type\": \"Lowpas\", "
      "\"freq\": 1000, \"q\": 0.7}}",
      &err);
  ASSERT_NE(0, res);
  ASSERT_EQ(CONFIG_ERR_PARSE, err.type);
  ASSERT_TRUE(strstr(err.message, "unknown variant 'Lowpas'") != NULL);
}

TEST(FilterRejectsMissingBiquadType) {
  config_error_t err;
  int res = parse_filter_json(
      "{\"type\": \"Biquad\", \"parameters\": {\"freq\": 1000, \"q\": 0.7}}",
      &err);
  ASSERT_NE(0, res);
  ASSERT_EQ(CONFIG_ERR_PARSE, err.type);
  ASSERT_TRUE(strstr(err.message, "missing field 'type'") != NULL);
}

TEST(FilterRejectsMissingBiquadFreq) {
  config_error_t err;
  int res = parse_filter_json(
      "{\"type\": \"Biquad\", \"parameters\": {\"type\": \"Lowpass\", "
      "\"q\": 0.7}}",
      &err);
  ASSERT_NE(0, res);
  ASSERT_EQ(CONFIG_ERR_PARSE, err.type);
  ASSERT_TRUE(strstr(err.message, "missing field 'freq'") != NULL);
}

TEST(FilterRejectsPeakingWithoutWidth) {
  config_error_t err;
  int res = parse_filter_json(
      "{\"type\": \"Biquad\", \"parameters\": {\"type\": \"Peaking\", "
      "\"freq\": 1000, \"gain\": 3.0}}",
      &err);
  ASSERT_NE(0, res);
  ASSERT_EQ(CONFIG_ERR_PARSE, err.type);
  ASSERT_TRUE(strstr(err.message, "'q' or 'bandwidth'") != NULL);
}

TEST(FilterAcceptsPeakingWithBandwidth) {
  config_error_t err;
  int res = parse_filter_json(
      "{\"type\": \"Biquad\", \"parameters\": {\"type\": \"Peaking\", "
      "\"freq\": 1000, \"gain\": 3.0, \"bandwidth\": 1.0}}",
      &err);
  ASSERT_EQ(0, res);
}

TEST(FilterRejectsMissingParameters) {
  config_error_t err;
  int res = parse_filter_json("{\"type\": \"Gain\"}", &err);
  ASSERT_NE(0, res);
  ASSERT_EQ(CONFIG_ERR_PARSE, err.type);
  ASSERT_TRUE(strstr(err.message, "missing 'parameters'") != NULL);
}

TEST(FilterRejectsMissingFilterType) {
  config_error_t err;
  int res = parse_filter_json("{\"parameters\": {\"gain\": 0.0}}", &err);
  ASSERT_NE(0, res);
  ASSERT_EQ(CONFIG_ERR_PARSE, err.type);
  ASSERT_TRUE(strstr(err.message, "missing or non-string 'type'") != NULL);
}

TEST(FilterRejectsDitherWithoutBits) {
  config_error_t err;
  int res = parse_filter_json(
      "{\"type\": \"Dither\", \"parameters\": {\"type\": \"Shibata441\"}}",
      &err);
  ASSERT_NE(0, res);
  ASSERT_EQ(CONFIG_ERR_PARSE, err.type);
  ASSERT_TRUE(strstr(err.message, "missing field 'bits'") != NULL);
}

TEST(FilterRejectsConvWithoutFilename) {
  config_error_t err;
  int res = parse_filter_json(
      "{\"type\": \"Conv\", \"parameters\": {\"type\": \"Wav\"}}", &err);
  ASSERT_NE(0, res);
  ASSERT_EQ(CONFIG_ERR_PARSE, err.type);
  ASSERT_TRUE(strstr(err.message, "missing field 'filename'") != NULL);
}

TEST(FilterRejectsBiquadComboWithoutOrder) {
  config_error_t err;
  int res = parse_filter_json(
      "{\"type\": \"BiquadCombo\", \"parameters\": {\"type\": "
      "\"ButterworthHighpass\", \"freq\": 100}}",
      &err);
  ASSERT_NE(0, res);
  ASSERT_EQ(CONFIG_ERR_PARSE, err.type);
  ASSERT_TRUE(strstr(err.message, "missing field 'order'") != NULL);
}

/* --- Strict resampler configuration (audit 09-4) --- */

/**
 * @brief Parses a config whose devices section carries the given resampler.
 */
static int parse_resampler_json(const char *resampler_json,
                                config_error_t *err) {
  char json[1024];
  snprintf(json, sizeof(json),
           "{\n"
           "    \"devices\": {\n"
           "        \"samplerate\": 44100,\n"
           "        \"chunksize\": 1024,\n"
           "        \"resampler\": %s,\n"
           "        \"capture\": {\"type\": \"RawFile\", \"filename\": "
           "\"/dev/null\", \"format\": \"S16_LE\", \"channels\": 2},\n"
           "        \"playback\": {\"type\": \"File\", \"filename\": "
           "\"/dev/null\", \"format\": \"S16_LE\", \"channels\": 2}\n"
           "    }\n"
           "}",
           resampler_json);
  dsp_config_t *config = NULL;
  config_error_init(err);
  int res = dsp_config_parse_json(json, &config, err);
  if (config)
    dsp_config_free(config);
  return res;
}

TEST(ResamplerRejectsUnknownType) {
  config_error_t err;
  int res = parse_resampler_json("{\"type\": \"AsyncSync\"}", &err);
  ASSERT_NE(0, res);
  ASSERT_EQ(CONFIG_ERR_PARSE, err.type);
  ASSERT_TRUE(strstr(err.message, "unknown variant 'AsyncSync'") != NULL);
}

TEST(ResamplerRejectsUnknownProfile) {
  config_error_t err;
  int res = parse_resampler_json(
      "{\"type\": \"AsyncSinc\", \"profile\": \"Balenced\"}", &err);
  ASSERT_NE(0, res);
  ASSERT_EQ(CONFIG_ERR_PARSE, err.type);
  ASSERT_TRUE(strstr(err.message, "unknown variant 'Balenced'") != NULL);
}

TEST(ResamplerRejectsAsyncPolyWithoutInterpolation) {
  config_error_t err;
  int res = parse_resampler_json("{\"type\": \"AsyncPoly\"}", &err);
  ASSERT_NE(0, res);
  ASSERT_EQ(CONFIG_ERR_PARSE, err.type);
  ASSERT_TRUE(strstr(err.message, "missing field 'interpolation'") != NULL);
}

TEST(ResamplerRejectsIncompleteAsyncSinc) {
  config_error_t err;
  int res = parse_resampler_json(
      "{\"type\": \"AsyncSinc\", \"sinc_len\": 128, \"interpolation\": "
      "\"Cubic\"}",
      &err);
  ASSERT_NE(0, res);
  ASSERT_EQ(CONFIG_ERR_PARSE, err.type);
  ASSERT_TRUE(strstr(err.message, "missing field 'window'") != NULL);
}

TEST(ResamplerAcceptsAsyncSincProfile) {
  config_error_t err;
  int res = parse_resampler_json(
      "{\"type\": \"AsyncSinc\", \"profile\": \"Balanced\"}", &err);
  ASSERT_EQ(0, res);
}

/* --- Channel indices must be non-negative integers (audit 09-9) --- */

TEST(PipelineRejectsNegativeChannelInList) {
  const char *json =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\"type\": \"RawFile\", \"filename\": "
      "\"/dev/null\", \"format\": \"S16_LE\", \"channels\": 2},\n"
      "        \"playback\": {\"type\": \"File\", \"filename\": \"/dev/null\", "
      "\"format\": \"S16_LE\", \"channels\": 2}\n"
      "    },\n"
      "    \"filters\": {\"g1\": {\"type\": \"Gain\", \"parameters\": "
      "{\"gain\": 0.0}}},\n"
      "    \"pipeline\": [{\"type\": \"Filter\", \"names\": [\"g1\"], "
      "\"channels\": [-1, 1]}]\n"
      "}";
  dsp_config_t *config = NULL;
  config_error_t err;
  config_error_init(&err);
  int res = dsp_config_parse_json(json, &config, &err);
  ASSERT_NE(0, res);
  ASSERT_EQ(CONFIG_ERR_PARSE, err.type);
  ASSERT_TRUE(strstr(err.message, "must be a non-negative integer") != NULL);
  ASSERT_TRUE(config == NULL);
}

TEST(PipelineRejectsFractionalChannel) {
  const char *json =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\"type\": \"RawFile\", \"filename\": "
      "\"/dev/null\", \"format\": \"S16_LE\", \"channels\": 2},\n"
      "        \"playback\": {\"type\": \"File\", \"filename\": \"/dev/null\", "
      "\"format\": \"S16_LE\", \"channels\": 2}\n"
      "    },\n"
      "    \"filters\": {\"g1\": {\"type\": \"Gain\", \"parameters\": "
      "{\"gain\": 0.0}}},\n"
      "    \"pipeline\": [{\"type\": \"Filter\", \"names\": [\"g1\"], "
      "\"channels\": [1.5]}]\n"
      "}";
  dsp_config_t *config = NULL;
  config_error_t err;
  config_error_init(&err);
  int res = dsp_config_parse_json(json, &config, &err);
  ASSERT_NE(0, res);
  ASSERT_EQ(CONFIG_ERR_PARSE, err.type);
  ASSERT_TRUE(strstr(err.message, "must be a non-negative integer") != NULL);
  ASSERT_TRUE(config == NULL);
}

TEST(MixerRejectsNegativeSourceChannel) {
  const char *json =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\"type\": \"RawFile\", \"filename\": "
      "\"/dev/null\", \"format\": \"S16_LE\", \"channels\": 2},\n"
      "        \"playback\": {\"type\": \"File\", \"filename\": \"/dev/null\", "
      "\"format\": \"S16_LE\", \"channels\": 2}\n"
      "    },\n"
      "    \"mixers\": {\"m1\": {\"channels\": {\"in\": 2, \"out\": 2}, "
      "\"mapping\": [{\"dest\": 0, \"sources\": [{\"channel\": -1}]}]}}\n"
      "}";
  dsp_config_t *config = NULL;
  config_error_t err;
  config_error_init(&err);
  int res = dsp_config_parse_json(json, &config, &err);
  ASSERT_NE(0, res);
  ASSERT_EQ(CONFIG_ERR_PARSE, err.type);
  ASSERT_TRUE(strstr(err.message, "must be a non-negative integer") != NULL);
  ASSERT_TRUE(config == NULL);
}

TEST(WavFileUnconditionallyUpdatesOverrides) {
  char wav_filename[256];
  snprintf(wav_filename, sizeof(wav_filename),
           "/tmp/test_config_wav_override_%d.wav", getpid());
  remove(wav_filename);

  FILE *f = fopen(wav_filename, "wb");
  ASSERT_TRUE(f != NULL);
  uint8_t wav_header[44] = {'R',  'I',  'F',  'F',  36,   0,   0,    0,    'W',
                            'A',  'V',  'E',  'f',  'm',  't', ' ',  16,   0,
                            0,    0,    1,    0,    2,    0,   0x44, 0xAC, 0x00,
                            0x00, 0x10, 0xB1, 0x02, 0x00, 4,   0,    16,   0,
                            'd',  'a',  't',  'a',  0,    0,   0,    0};
  fwrite(wav_header, 1, sizeof(wav_header), f);
  fclose(f);

  char json[1024];
  snprintf(
      json, sizeof(json),
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 96000,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\"type\": \"WavFile\", \"filename\": \"%s\"},\n"
      "        \"playback\": {\"type\": \"File\", \"filename\": \"/dev/null\", "
      "\"format\": \"S16_LE\", \"channels\": 2}\n"
      "    }\n"
      "}",
      wav_filename);

  dsp_config_overrides_t overrides;
  memset(&overrides, 0, sizeof(overrides));
  overrides.samplerate = 88200;
  overrides.channels = 4;
  overrides.sample_format = BINARY_SAMPLE_FORMAT_S32_LE;
  overrides.has_sample_format = true;

  dsp_config_t *config = NULL;
  config_error_t err;
  config_error_init(&err);
  int res = dsp_config_parse_json_with_dir_and_overrides(json, NULL, &overrides,
                                                         &config, &err);
  ASSERT_EQ(0, res);
  ASSERT_TRUE(config != NULL);
  // WAV header has 44100 Hz, 2 channels, S16_LE. It should override the input
  // overrides!
  ASSERT_EQ(44100, config->devices.samplerate);
  dsp_config_free(config);

  remove(wav_filename);
}

#if defined(ENABLE_COREAUDIO)
TEST(UnmappableSampleFormatOverrideFailsCoreAudio) {
  const char *json =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\"type\": \"CoreAudio\", \"channels\": 2, "
      "\"format\": \"S16\"},\n"
      "        \"playback\": {\"type\": \"File\", \"filename\": \"/dev/null\", "
      "\"format\": \"S16_LE\", \"channels\": 2}\n"
      "    }\n"
      "}";

  dsp_config_overrides_t overrides;
  memset(&overrides, 0, sizeof(overrides));
  overrides.samplerate = -1;
  overrides.channels = -1;
  overrides.extra_samples = -1;
  overrides.has_sample_format = true;
  overrides.sample_format = BINARY_SAMPLE_FORMAT_F64_LE;

  dsp_config_t *config = NULL;
  config_error_t err;
  config_error_init(&err);
  int res = dsp_config_parse_json_with_dir_and_overrides(json, NULL, &overrides,
                                                         &config, &err);
  ASSERT_NE(0, res);
  ASSERT_EQ(CONFIG_ERR_PARSE, err.type);
  ASSERT_TRUE(
      strstr(
          err.message,
          "CoreAudio does not have a sample format corresponding to F64_LE") !=
      NULL);
  ASSERT_TRUE(config == NULL);
}
#endif

TEST(RelativePathResolvedAfterTokenSubstitution) {
  char test_dir[256];
  snprintf(test_dir, sizeof(test_dir), "/tmp/cdsp_test_token_path_%d",
           getpid());
#ifdef _WIN32
  mkdir(test_dir);
#else
  mkdir(test_dir, 0755);
#endif

  char coeff_file[512];
  snprintf(coeff_file, sizeof(coeff_file), "%s/coeffs_44100.raw", test_dir);
  FILE *f = fopen(coeff_file, "wb");
  ASSERT_TRUE(f != NULL);
  float val = 1.0f;
  fwrite(&val, sizeof(float), 1, f);
  fclose(f);

  const char *json =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\"type\": \"RawFile\", \"filename\": "
      "\"/dev/null\", \"format\": \"S16_LE\", \"channels\": 2},\n"
      "        \"playback\": {\"type\": \"File\", \"filename\": \"/dev/null\", "
      "\"format\": \"S16_LE\", \"channels\": 2}\n"
      "    },\n"
      "    \"filters\": {\n"
      "        \"conv1\": {\n"
      "            \"type\": \"Conv\",\n"
      "            \"parameters\": {\n"
      "                \"type\": \"Raw\",\n"
      "                \"filename\": \"coeffs_$samplerate$.raw\",\n"
      "                \"format\": \"F32_LE\"\n"
      "            }\n"
      "        }\n"
      "    }\n"
      "}";

  dsp_config_t *config = NULL;
  config_error_t err;
  config_error_init(&err);
  int res = dsp_config_parse_json_with_dir(json, test_dir, &config, &err);
  ASSERT_EQ(0, res);
  ASSERT_TRUE(config != NULL);
  ASSERT_EQ(1, config->filters_count);
  ASSERT_STR_EQ(coeff_file, config->filters[0].filter.parameters.conv.filename);
  dsp_config_free(config);

  remove(coeff_file);
  rmdir(test_dir);
}

TEST(VolumeFaderParsingAndValidation) {
  // 1. Valid Volume with Aux1
  const char *json_ok =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\"type\": \"RawFile\", \"filename\": "
      "\"/dev/null\", \"format\": \"S16_LE\", \"channels\": 2},\n"
      "        \"playback\": {\"type\": \"File\", \"filename\": \"/dev/null\", "
      "\"format\": \"S16_LE\", \"channels\": 2}\n"
      "    },\n"
      "    \"filters\": {\n"
      "        \"v1\": {\"type\": \"Volume\", \"parameters\": {\"fader\": "
      "\"Aux1\"}}\n"
      "    }\n"
      "}";
  dsp_config_t *config = NULL;
  config_error_t err;
  config_error_init(&err);
  int res = dsp_config_parse_json(json_ok, &config, &err);
  ASSERT_EQ(0, res);
  ASSERT_TRUE(config != NULL);
  ASSERT_EQ(FADER_AUX1, config->filters[0].filter.parameters.volume.fader);
  dsp_config_free(config);

  // 2. Volume missing fader -> rejected
  const char *json_no_fader =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\"type\": \"RawFile\", \"filename\": "
      "\"/dev/null\", \"format\": \"S16_LE\", \"channels\": 2},\n"
      "        \"playback\": {\"type\": \"File\", \"filename\": \"/dev/null\", "
      "\"format\": \"S16_LE\", \"channels\": 2}\n"
      "    },\n"
      "    \"filters\": {\n"
      "        \"v1\": {\"type\": \"Volume\", \"parameters\": "
      "{\"ramp_time_ms\": 200.0}}\n"
      "    }\n"
      "}";
  config_error_init(&err);
  res = dsp_config_parse_json(json_no_fader, &config, &err);
  ASSERT_NE(0, res);
  ASSERT_TRUE(strstr(err.message, "missing field 'fader'") != NULL);

  // 3. Volume with fader Main -> rejected (Main is not valid for Volume)
  const char *json_main =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\"type\": \"RawFile\", \"filename\": "
      "\"/dev/null\", \"format\": \"S16_LE\", \"channels\": 2},\n"
      "        \"playback\": {\"type\": \"File\", \"filename\": \"/dev/null\", "
      "\"format\": \"S16_LE\", \"channels\": 2}\n"
      "    },\n"
      "    \"filters\": {\n"
      "        \"v1\": {\"type\": \"Volume\", \"parameters\": {\"fader\": "
      "\"Main\"}}\n"
      "    }\n"
      "}";
  config_error_init(&err);
  res = dsp_config_parse_json(json_main, &config, &err);
  ASSERT_NE(0, res);
  ASSERT_TRUE(strstr(err.message, "unknown variant 'Main'") != NULL);

  // 4. Volume with invalid fader -> rejected
  const char *json_invalid =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\"type\": \"RawFile\", \"filename\": "
      "\"/dev/null\", \"format\": \"S16_LE\", \"channels\": 2},\n"
      "        \"playback\": {\"type\": \"File\", \"filename\": \"/dev/null\", "
      "\"format\": \"S16_LE\", \"channels\": 2}\n"
      "    },\n"
      "    \"filters\": {\n"
      "        \"v1\": {\"type\": \"Volume\", \"parameters\": {\"fader\": "
      "\"Aux5\"}}\n"
      "    }\n"
      "}";
  config_error_init(&err);
  res = dsp_config_parse_json(json_invalid, &config, &err);
  ASSERT_NE(0, res);
  ASSERT_TRUE(strstr(err.message, "unknown variant 'Aux5'") != NULL);
}

TEST(LoudnessFaderParsingAndValidation) {
  // 1. Loudness without fader -> defaults to Main
  const char *json_def =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\"type\": \"RawFile\", \"filename\": "
      "\"/dev/null\", \"format\": \"S16_LE\", \"channels\": 2},\n"
      "        \"playback\": {\"type\": \"File\", \"filename\": \"/dev/null\", "
      "\"format\": \"S16_LE\", \"channels\": 2}\n"
      "    },\n"
      "    \"filters\": {\n"
      "        \"loud\": {\"type\": \"Loudness\", \"parameters\": "
      "{\"reference_level\": -20.0}}\n"
      "    }\n"
      "}";
  dsp_config_t *config = NULL;
  config_error_t err;
  config_error_init(&err);
  int res = dsp_config_parse_json(json_def, &config, &err);
  ASSERT_EQ(0, res);
  ASSERT_TRUE(config != NULL);
  ASSERT_EQ(FADER_MAIN, config->filters[0].filter.parameters.loudness.fader);
  dsp_config_free(config);

  // 2. Loudness with fader Aux2 -> accepted
  const char *json_aux =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\"type\": \"RawFile\", \"filename\": "
      "\"/dev/null\", \"format\": \"S16_LE\", \"channels\": 2},\n"
      "        \"playback\": {\"type\": \"File\", \"filename\": \"/dev/null\", "
      "\"format\": \"S16_LE\", \"channels\": 2}\n"
      "    },\n"
      "    \"filters\": {\n"
      "        \"loud\": {\"type\": \"Loudness\", \"parameters\": "
      "{\"reference_level\": -20.0, \"fader\": \"Aux2\"}}\n"
      "    }\n"
      "}";
  config_error_init(&err);
  res = dsp_config_parse_json(json_aux, &config, &err);
  ASSERT_EQ(0, res);
  ASSERT_TRUE(config != NULL);
  ASSERT_EQ(FADER_AUX2, config->filters[0].filter.parameters.loudness.fader);
  dsp_config_free(config);

  // 3. Loudness with invalid fader -> rejected
  const char *json_bad =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\"type\": \"RawFile\", \"filename\": "
      "\"/dev/null\", \"format\": \"S16_LE\", \"channels\": 2},\n"
      "        \"playback\": {\"type\": \"File\", \"filename\": \"/dev/null\", "
      "\"format\": \"S16_LE\", \"channels\": 2}\n"
      "    },\n"
      "    \"filters\": {\n"
      "        \"loud\": {\"type\": \"Loudness\", \"parameters\": "
      "{\"reference_level\": -20.0, \"fader\": \"Aux9\"}}\n"
      "    }\n"
      "}";
  config_error_init(&err);
  res = dsp_config_parse_json(json_bad, &config, &err);
  ASSERT_NE(0, res);
  ASSERT_TRUE(strstr(err.message, "unknown variant 'Aux9'") != NULL);
}

TEST(Biquad_WidthFieldPrecedence_QOverSlope) {
  const char *json =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\"type\": \"RawFile\", \"filename\": "
      "\"/dev/null\", \"format\": \"S16_LE\", \"channels\": 2},\n"
      "        \"playback\": {\"type\": \"File\", \"filename\": \"/dev/null\", "
      "\"format\": \"S16_LE\", \"channels\": 2}\n"
      "    },\n"
      "    \"filters\": {\n"
      "        \"shelf\": {\"type\": \"Biquad\", \"parameters\": {\"type\": "
      "\"Highshelf\", \"freq\": 1000.0, \"gain\": 3.0, \"q\": 0.707, "
      "\"slope\": 1.5}}\n"
      "    }\n"
      "}";
  dsp_config_t *config = NULL;
  config_error_t err;
  config_error_init(&err);
  int res = dsp_config_parse_json(json, &config, &err);
  ASSERT_EQ(0, res);
  ASSERT_TRUE(config != NULL);
  ASSERT_EQ(1, config->filters_count);
  ASSERT_EQ(STEEPNESS_TYPE_Q,
            config->filters[0].filter.parameters.biquad.steepness_type);
  ASSERT_NEAR(0.707, config->filters[0].filter.parameters.biquad.q, 1e-4);
  dsp_config_free(config);
}

TEST(Pipeline_EmptyNamesList_AcceptedAsNoOp) {
  const char *json =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\"type\": \"RawFile\", \"filename\": "
      "\"/dev/null\", \"format\": \"S16_LE\", \"channels\": 2},\n"
      "        \"playback\": {\"type\": \"File\", \"filename\": \"/dev/null\", "
      "\"format\": \"S16_LE\", \"channels\": 2}\n"
      "    },\n"
      "    \"pipeline\": [\n"
      "        {\"type\": \"Filter\", \"channels\": [0, 1], \"names\": []}\n"
      "    ]\n"
      "}";
  dsp_config_t *config = NULL;
  config_error_t err;
  config_error_init(&err);
  int res = dsp_config_parse_json(json, &config, &err);
  ASSERT_EQ(0, res);
  ASSERT_TRUE(config != NULL);
  ASSERT_EQ(1, config->pipeline_count);
  ASSERT_TRUE(config->pipeline[0].has_names);
  ASSERT_EQ(0, config->pipeline[0].names_count);

  // Validation must pass (no-op filter step matching upstream)
  res = dsp_config_validate(config, &err);
  ASSERT_EQ(0, res);
  dsp_config_free(config);
}

TEST(Devices_TargetLevelZero_PreservedAndNegativeRejected) {
  // 1. target_level: 0 is explicitly preserved
  const char *json_zero =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 1024,\n"
      "        \"target_level\": 0,\n"
      "        \"capture\": {\"type\": \"RawFile\", \"filename\": "
      "\"/dev/null\", \"format\": \"S16_LE\", \"channels\": 2},\n"
      "        \"playback\": {\"type\": \"File\", \"filename\": \"/dev/null\", "
      "\"format\": \"S16_LE\", \"channels\": 2}\n"
      "    }\n"
      "}";
  dsp_config_t *config = NULL;
  config_error_t err;
  config_error_init(&err);
  int res = dsp_config_parse_json(json_zero, &config, &err);
  ASSERT_EQ(0, res);
  ASSERT_TRUE(config != NULL);
  ASSERT_TRUE(config->devices.has_target_level);
  ASSERT_EQ(0, config->devices.target_level);
  dsp_config_free(config);

  // 2. target_level: -10 is strictly rejected
  const char *json_neg =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 1024,\n"
      "        \"target_level\": -10,\n"
      "        \"capture\": {\"type\": \"RawFile\", \"filename\": "
      "\"/dev/null\", \"format\": \"S16_LE\", \"channels\": 2},\n"
      "        \"playback\": {\"type\": \"File\", \"filename\": \"/dev/null\", "
      "\"format\": \"S16_LE\", \"channels\": 2}\n"
      "    }\n"
      "}";
  config_error_init(&err);
  res = dsp_config_parse_json(json_neg, &config, &err);
  ASSERT_NE(0, res);
  ASSERT_TRUE(strstr(err.message, "must be a non-negative integer") != NULL);
}

TEST(Processor_Definition_Requires_Type_And_Params) {
  // Missing 'type' field in processor definition
  const char *json_no_type =
      "{\n"
      "    \"devices\": {\"samplerate\": 44100, \"chunksize\": 1024, "
      "\"capture\": {\"type\": \"RawFile\", \"filename\": \"/dev/null\", "
      "\"format\": \"S16_LE\", \"channels\": 2}, "
      "\"playback\": {\"type\": \"File\", \"filename\": \"/dev/null\", "
      "\"format\": \"S16_LE\", \"channels\": 2}},\n"
      "    \"processors\": {\"my_proc\": {\"parameters\": {}}}\n"
      "}";
  dsp_config_t *config = NULL;
  config_error_t err;
  config_error_init(&err);
  int res = dsp_config_parse_json(json_no_type, &config, &err);
  ASSERT_NE(0, res);
  ASSERT_TRUE(strstr(err.message, "missing or non-string 'type'") != NULL);

  // Missing 'parameters' field in processor definition
  const char *json_no_params =
      "{\n"
      "    \"devices\": {\"samplerate\": 44100, \"chunksize\": 1024, "
      "\"capture\": {\"type\": \"RawFile\", \"filename\": \"/dev/null\", "
      "\"format\": \"S16_LE\", \"channels\": 2}, "
      "\"playback\": {\"type\": \"File\", \"filename\": \"/dev/null\", "
      "\"format\": \"S16_LE\", \"channels\": 2}},\n"
      "    \"processors\": {\"my_proc\": {\"type\": \"Compressor\"}}\n"
      "}";
  config_error_init(&err);
  res = dsp_config_parse_json(json_no_params, &config, &err);
  ASSERT_NE(0, res);
  ASSERT_TRUE(strstr(err.message, "missing 'parameters' object") != NULL);
}

TEST(Devices_Queuelimit_LargeValuesAccepted) {
  const char *json =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 1024,\n"
      "        \"queuelimit\": 2000,\n"
      "        \"capture\": {\"type\": \"RawFile\", \"filename\": "
      "\"/dev/null\", \"format\": \"S16_LE\", \"channels\": 2},\n"
      "        \"playback\": {\"type\": \"File\", \"filename\": \"/dev/null\", "
      "\"format\": \"S16_LE\", \"channels\": 2}\n"
      "    }\n"
      "}";
  dsp_config_t *config = NULL;
  config_error_t err;
  config_error_init(&err);
  int res = dsp_config_parse_json(json, &config, &err);
  ASSERT_EQ(0, res);
  ASSERT_TRUE(config != NULL);
  ASSERT_TRUE(config->devices.has_queuelimit);
  ASSERT_EQ(2000, config->devices.queuelimit);
  dsp_config_free(config);
}

TEST_MAIN()
