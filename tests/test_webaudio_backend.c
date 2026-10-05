#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/audio_chunk.h"
#include "backend/audio_backend.h"
#include "backend/backend_error.h"
#include "backend/webaudio_backend.h"
#include "config/config_error.h"
#include "config/configuration.h"
#include "test_support.h"

TEST(WebAudioCaptureBackend_Streaming) {
  backend_error_t err;
  backend_error_init(&err, BACKEND_ERROR_NONE, "");

  capture_device_config_t cap_cfg = {0};
  cap_cfg.type = AUDIO_BACKEND_TYPE_WEB_AUDIO;
  cap_cfg.cfg.webaudio.channels = 2;

  capture_backend_t *cap =
      create_capture_backend(&cap_cfg, 48000, 128, false, NULL, &err);
  ASSERT_TRUE(cap != NULL);
  ASSERT_TRUE(capture_backend_open(cap, &err));

  // Push 128 stereo samples into WebAudio capture
  float push_ch0[128], push_ch1[128];
  for (int i = 0; i < 128; i++) {
    push_ch0[i] = 0.5f;
    push_ch1[i] = -0.5f;
  }
  const float *cap_ptrs[2] = {push_ch0, push_ch1};
  webaudio_device_process(cap_ptrs, 2, NULL, 0, 128);

  // Read samples using capture_backend_read
  audio_chunk_t *read_chunk = audio_chunk_create(128, 2);
  ASSERT_TRUE(read_chunk != NULL);
  ASSERT_TRUE(capture_backend_read(cap, 128, read_chunk, &err));
  ASSERT_EQ(audio_chunk_get_valid_frames(read_chunk), 128);
  ASSERT_NEAR(audio_chunk_get_channel(read_chunk, 0)[0], 0.5, 1e-5);
  ASSERT_NEAR(audio_chunk_get_channel(read_chunk, 1)[0], -0.5, 1e-5);

  capture_backend_close(cap);
  capture_backend_free(cap);
  audio_chunk_free(read_chunk);
}

TEST(WebAudioPlaybackBackend_Streaming) {
  backend_error_t err;
  backend_error_init(&err, BACKEND_ERROR_NONE, "");

  playback_device_config_t play_cfg = {0};
  play_cfg.type = AUDIO_BACKEND_TYPE_WEB_AUDIO;
  play_cfg.cfg.webaudio.channels = 2;

  playback_backend_t *play =
      create_playback_backend(&play_cfg, 48000, 128, false, NULL, &err);
  ASSERT_TRUE(play != NULL);
  ASSERT_TRUE(playback_backend_open(play, &err));

  // Write 128 stereo samples using playback_backend_write
  audio_chunk_t *write_chunk = audio_chunk_create(128, 2);
  ASSERT_TRUE(write_chunk != NULL);
  for (int i = 0; i < 128; i++) {
    audio_chunk_get_channel(write_chunk, 0)[i] = 0.75;
    audio_chunk_get_channel(write_chunk, 1)[i] = -0.75;
  }
  audio_chunk_set_valid_frames(write_chunk, 128);
  ASSERT_TRUE(playback_backend_write(play, write_chunk, &err));

  // Render samples out to WebAudio output buffer
  float render_ch0[128] = {0}, render_ch1[128] = {0};
  float *render_ptrs[2] = {render_ch0, render_ch1};
  webaudio_device_process(NULL, 0, render_ptrs, 2, 128);
  ASSERT_NEAR(render_ch0[0], 0.75f, 1e-5f);
  ASSERT_NEAR(render_ch1[0], -0.75f, 1e-5f);

  playback_backend_close(play);
  playback_backend_free(play);
  audio_chunk_free(write_chunk);

  // Detached device renders silence.
  render_ch0[0] = 1.0f;
  render_ch1[0] = 1.0f;
  webaudio_device_process(NULL, 0, render_ptrs, 2, 128);
  ASSERT_NEAR(render_ch0[0], 0.0f, 0.0f);
  ASSERT_NEAR(render_ch1[0], 0.0f, 0.0f);
}

TEST(WebAudioBackend_RejectsRateOtherThanDevice) {
  backend_error_t err;
  backend_error_init(&err, BACKEND_ERROR_NONE, "");
  webaudio_device_set_sample_rate(44100);

  playback_device_config_t play_cfg = {0};
  play_cfg.type = AUDIO_BACKEND_TYPE_WEB_AUDIO;
  play_cfg.cfg.webaudio.channels = 2;
  playback_backend_t *play =
      create_playback_backend(&play_cfg, 48000, 128, false, NULL, &err);
  ASSERT_TRUE(play != NULL);
  ASSERT_FALSE(playback_backend_open(play, &err));
  playback_backend_free(play);

  webaudio_device_set_sample_rate(0);
}

static int g_requested_rate;
static size_t g_requested_input_channels;
static size_t g_requested_output_channels;
static void record_format_request(int sample_rate, size_t input_channels,
                                  size_t output_channels) {
  g_requested_rate = sample_rate;
  g_requested_input_channels = input_channels;
  g_requested_output_channels = output_channels;
}

TEST(WebAudioBackend_RequestsRateSwitchAndWaitsForIt) {
  backend_error_t err;
  backend_error_init(&err, BACKEND_ERROR_NONE, "");
  webaudio_device_set_sample_rate(48000);
  webaudio_device_set_format_request_hook(record_format_request);
  g_requested_rate = 0;

  // A switchable device publishes every standard rate; a fixed one only its own.
  device_error_t derr;
  audio_device_descriptor_t *desc = webaudio_describe("default", false, &derr);
  ASSERT_TRUE(desc != NULL);
  channel_capability_t *caps = &desc->capability_sets[0].capabilities[0];
  ASSERT_EQ(caps->samplerates_count, 15);
  ASSERT_EQ(caps->samplerates[5].samplerate, 44100);
  ASSERT_EQ(caps->samplerates[8].samplerate, 96000);
  free_audio_device_descriptor(desc);
  webaudio_device_set_format_request_hook(NULL);
  desc = webaudio_describe("default", false, &derr);
  ASSERT_TRUE(desc != NULL);
  caps = &desc->capability_sets[0].capabilities[0];
  ASSERT_EQ(caps->samplerates_count, 1);
  ASSERT_EQ(caps->samplerates[0].samplerate, 48000);
  free_audio_device_descriptor(desc);
  webaudio_device_set_format_request_hook(record_format_request);

  playback_device_config_t play_cfg = {0};
  play_cfg.type = AUDIO_BACKEND_TYPE_WEB_AUDIO;
  play_cfg.cfg.webaudio.channels = 2;
  playback_backend_t *play =
      create_playback_backend(&play_cfg, 44100, 128, false, NULL, &err);
  ASSERT_TRUE(play != NULL);
  ASSERT_TRUE(playback_backend_open(play, &err));
  ASSERT_EQ(g_requested_rate, 44100);

  audio_chunk_t *chunk = audio_chunk_create(128, 2);
  ASSERT_TRUE(chunk != NULL);
  for (int i = 0; i < 128; i++) {
    audio_chunk_get_channel(chunk, 0)[i] = 0.25;
    audio_chunk_get_channel(chunk, 1)[i] = 0.25;
  }
  audio_chunk_set_valid_frames(chunk, 128);
  ASSERT_TRUE(playback_backend_write(play, chunk, &err));

  // Device still at 48 kHz: the backend is not drained, output is silent.
  float out0[128], out1[128];
  float *outs[2] = {out0, out1};
  out0[0] = 1.0f;
  webaudio_device_process(NULL, 0, outs, 2, 128);
  ASSERT_NEAR(out0[0], 0.0f, 0.0f);

  // Device switched: the queued audio plays.
  webaudio_device_set_sample_rate(44100);
  webaudio_device_process(NULL, 0, outs, 2, 128);
  ASSERT_NEAR(out0[0], 0.25f, 1e-6f);

  // Capture must share the playback rate, and out-of-range rates are refused.
  capture_device_config_t cap_cfg = {0};
  cap_cfg.type = AUDIO_BACKEND_TYPE_WEB_AUDIO;
  cap_cfg.cfg.webaudio.channels = 2;
  capture_backend_t *cap =
      create_capture_backend(&cap_cfg, 96000, 128, false, NULL, &err);
  ASSERT_TRUE(cap != NULL);
  ASSERT_FALSE(capture_backend_open(cap, &err));
  capture_backend_free(cap);

  playback_backend_close(play);
  playback_backend_free(play);
  playback_backend_t *too_fast =
      create_playback_backend(&play_cfg, 1000000, 128, false, NULL, &err);
  ASSERT_TRUE(too_fast != NULL);
  ASSERT_FALSE(playback_backend_open(too_fast, &err));
  playback_backend_free(too_fast);

  audio_chunk_free(chunk);
  webaudio_device_set_format_request_hook(NULL);
  webaudio_device_set_sample_rate(0);
}

TEST(WebAudioBackend_FollowsPlaybackAndCaptureChannels) {
  backend_error_t err;
  backend_error_init(&err, BACKEND_ERROR_NONE, "");
  webaudio_device_set_sample_rate(48000);
  webaudio_device_set_output_channels(2, 8);
  webaudio_device_set_format_request_hook(record_format_request);

  // Playback offers 1..8 channels (the hardware maximum), capture 1..32.
  device_error_t derr;
  audio_device_descriptor_t *desc = webaudio_describe("default", false, &derr);
  ASSERT_TRUE(desc != NULL);
  ASSERT_EQ(desc->capability_sets[0].capabilities_count, 8);
  ASSERT_EQ(desc->capability_sets[0].capabilities[0].channels, 1);
  ASSERT_EQ(desc->capability_sets[0].capabilities[7].channels, 8);
  ASSERT_EQ(desc->capability_sets[0].capabilities[7].samplerates_count, 15);
  free_audio_device_descriptor(desc);
  // No device name selects the one "default" device.
  desc = webaudio_describe("", true, &derr);
  ASSERT_TRUE(desc != NULL);
  ASSERT_STR_EQ(desc->name, "default");
  ASSERT_EQ(desc->capability_sets[0].capabilities_count,
            WEBAUDIO_MAX_CHANNELS);
  free_audio_device_descriptor(desc);
  ASSERT_TRUE(webaudio_describe("other", true, &derr) == NULL);

  // 6-channel playback asks for 6 outputs and waits until the device has them.
  playback_device_config_t play_cfg = {0};
  play_cfg.type = AUDIO_BACKEND_TYPE_WEB_AUDIO;
  play_cfg.cfg.webaudio.channels = 6;
  playback_backend_t *play =
      create_playback_backend(&play_cfg, 48000, 128, false, NULL, &err);
  ASSERT_TRUE(play != NULL);
  ASSERT_TRUE(playback_backend_open(play, &err));
  ASSERT_EQ(g_requested_rate, 48000);
  ASSERT_EQ(g_requested_input_channels, 0);
  ASSERT_EQ(g_requested_output_channels, 6);

  audio_chunk_t *chunk = audio_chunk_create(128, 6);
  ASSERT_TRUE(chunk != NULL);
  for (size_t c = 0; c < 6; c++) {
    for (int i = 0; i < 128; i++)
      audio_chunk_get_channel(chunk, c)[i] = 0.1 * (double)(c + 1);
  }
  audio_chunk_set_valid_frames(chunk, 128);
  ASSERT_TRUE(playback_backend_write(play, chunk, &err));

  float out[6][128];
  float *outs[6];
  for (size_t c = 0; c < 6; c++) {
    outs[c] = out[c];
    out[c][0] = 1.0f;
  }
  webaudio_device_process(NULL, 0, outs, 2, 128);
  ASSERT_NEAR(out[0][0], 0.0f, 0.0f);
  ASSERT_NEAR(out[1][0], 0.0f, 0.0f);
  webaudio_device_process(NULL, 0, outs, 6, 128);
  for (size_t c = 0; c < 6; c++)
    ASSERT_NEAR(out[c][0], 0.1f * (float)(c + 1), 1e-6f);
  playback_backend_close(play);
  playback_backend_free(play);
  audio_chunk_free(chunk);

  // More channels than the hardware has are refused.
  play_cfg.cfg.webaudio.channels = 10;
  play = create_playback_backend(&play_cfg, 48000, 128, false, NULL, &err);
  ASSERT_TRUE(play != NULL);
  ASSERT_FALSE(playback_backend_open(play, &err));
  playback_backend_free(play);

  // Capture asks for its channel count; missing inputs read as silence.
  capture_device_config_t cap_cfg = {0};
  cap_cfg.type = AUDIO_BACKEND_TYPE_WEB_AUDIO;
  cap_cfg.cfg.webaudio.channels = 4;
  capture_backend_t *cap =
      create_capture_backend(&cap_cfg, 48000, 128, false, NULL, &err);
  ASSERT_TRUE(cap != NULL);
  ASSERT_TRUE(capture_backend_open(cap, &err));
  ASSERT_EQ(g_requested_input_channels, 4);
  ASSERT_EQ(g_requested_output_channels, 0);
  capture_backend_close(cap);
  capture_backend_free(cap);

  // A fixed device only takes the channel count it renders.
  webaudio_device_set_format_request_hook(NULL);
  desc = webaudio_describe("default", false, &derr);
  ASSERT_TRUE(desc != NULL);
  ASSERT_EQ(desc->capability_sets[0].capabilities_count, 1);
  ASSERT_EQ(desc->capability_sets[0].capabilities[0].channels, 2);
  free_audio_device_descriptor(desc);
  play_cfg.cfg.webaudio.channels = 6;
  play = create_playback_backend(&play_cfg, 48000, 128, false, NULL, &err);
  ASSERT_TRUE(play != NULL);
  ASSERT_FALSE(playback_backend_open(play, &err));
  playback_backend_free(play);

  webaudio_device_set_output_channels(0, 0);
  webaudio_device_set_sample_rate(0);
}

/** Parses a WebAudio-to-WebAudio config with extra "devices" members. */
static int parse_webaudio_config(const char *extra, config_error_t *err) {
  char json[1024];
  snprintf(json, sizeof(json),
           "{\"devices\": {\"samplerate\": 48000, \"chunksize\": 512, %s"
           "\"capture\": {\"type\": \"WebAudio\", \"channels\": 2},"
           "\"playback\": {\"type\": \"WebAudio\", \"channels\": 2}}}",
           extra);
  dsp_config_t *config = NULL;
  config_error_init(err);
  int res = dsp_config_parse_json(json, &config, err);
  dsp_config_free(config);
  return res;
}

TEST(WebAudioConfig_SharedDeviceRejectsResamplerAndRateAdjust) {
  config_error_t err;
  ASSERT_EQ(0, parse_webaudio_config("", &err));

  ASSERT_TRUE(parse_webaudio_config(
                  "\"resampler\": {\"type\": \"Synchronous\"}, "
                  "\"capture_samplerate\": 44100, ",
                  &err) != 0);
  ASSERT_EQ(CONFIG_ERR_INVALID_DEVICE, err.type);
  ASSERT_TRUE(strstr(err.message, "Resampling is not supported") != NULL);

  ASSERT_TRUE(parse_webaudio_config("\"enable_rate_adjust\": true, ", &err) != 0);
  ASSERT_EQ(CONFIG_ERR_INVALID_DEVICE, err.type);
  ASSERT_TRUE(strstr(err.message, "Rate adjust is not supported") != NULL);
}

TEST_MAIN()
