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
  webaudio_device_set_capture_format(44100, 0);
  webaudio_device_set_playback_format(44100, 2);

  playback_device_config_t play_cfg = {0};
  play_cfg.type = AUDIO_BACKEND_TYPE_WEB_AUDIO;
  play_cfg.cfg.webaudio.channels = 2;
  playback_backend_t *play =
      create_playback_backend(&play_cfg, 48000, 128, false, NULL, &err);
  ASSERT_TRUE(play != NULL);
  ASSERT_FALSE(playback_backend_open(play, &err));
  playback_backend_free(play);

  webaudio_device_set_capture_format(0, 0);
  webaudio_device_set_playback_format(0, 0);
}

TEST(WebAudioBackend_DescribesOnlyNativeFormat) {
  device_error_t derr;
  // Unknown format: the defaults.
  webaudio_device_set_capture_format(0, 0);
  webaudio_device_set_playback_format(0, 0);
  audio_device_descriptor_t *desc = webaudio_describe("", true, &derr);
  ASSERT_TRUE(desc != NULL);
  ASSERT_STR_EQ(desc->name, "default");
  ASSERT_EQ(desc->capability_sets[0].capabilities_count, 1);
  channel_capability_t *caps = &desc->capability_sets[0].capabilities[0];
  ASSERT_EQ(caps->channels, WEBAUDIO_DEFAULT_CHANNELS);
  ASSERT_EQ(caps->samplerates_count, 1);
  ASSERT_EQ(caps->samplerates[0].samplerate, WEBAUDIO_DEFAULT_SAMPLE_RATE);
  free_audio_device_descriptor(desc);
  ASSERT_TRUE(webaudio_describe("other", true, &derr) == NULL);

  webaudio_device_set_capture_format(44100, 1);
  webaudio_device_set_playback_format(44100, 6);
  desc = webaudio_describe("default", false, &derr);
  ASSERT_TRUE(desc != NULL);
  ASSERT_EQ(desc->capability_sets[0].capabilities_count, 1);
  caps = &desc->capability_sets[0].capabilities[0];
  ASSERT_EQ(caps->channels, 6);
  ASSERT_EQ(caps->samplerates_count, 1);
  ASSERT_EQ(caps->samplerates[0].samplerate, 44100);
  free_audio_device_descriptor(desc);
  desc = webaudio_describe("default", true, &derr);
  ASSERT_TRUE(desc != NULL);
  ASSERT_EQ(desc->capability_sets[0].capabilities[0].channels, 1);
  free_audio_device_descriptor(desc);

  webaudio_device_set_capture_format(0, 0);
  webaudio_device_set_playback_format(0, 0);
}

TEST(WebAudioBackend_RefusesOtherChannelCounts) {
  backend_error_t err;
  backend_error_init(&err, BACKEND_ERROR_NONE, "");
  webaudio_device_set_capture_format(48000, 2);
  webaudio_device_set_playback_format(48000, 6);

  playback_device_config_t play_cfg = {0};
  play_cfg.type = AUDIO_BACKEND_TYPE_WEB_AUDIO;
  play_cfg.cfg.webaudio.channels = 2;
  playback_backend_t *play =
      create_playback_backend(&play_cfg, 48000, 128, false, NULL, &err);
  ASSERT_TRUE(play != NULL);
  ASSERT_FALSE(playback_backend_open(play, &err));
  playback_backend_free(play);

  capture_device_config_t cap_cfg = {0};
  cap_cfg.type = AUDIO_BACKEND_TYPE_WEB_AUDIO;
  cap_cfg.cfg.webaudio.channels = 4;
  capture_backend_t *cap =
      create_capture_backend(&cap_cfg, 48000, 128, false, NULL, &err);
  ASSERT_TRUE(cap != NULL);
  ASSERT_FALSE(capture_backend_open(cap, &err));
  capture_backend_free(cap);

  // 6-channel playback in the native format renders all six outputs.
  play_cfg.cfg.webaudio.channels = 6;
  play = create_playback_backend(&play_cfg, 48000, 128, false, NULL, &err);
  ASSERT_TRUE(play != NULL);
  ASSERT_TRUE(playback_backend_open(play, &err));
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
  for (size_t c = 0; c < 6; c++)
    outs[c] = out[c];
  webaudio_device_process(NULL, 0, outs, 6, 128);
  for (size_t c = 0; c < 6; c++)
    ASSERT_NEAR(out[c][0], 0.1f * (float)(c + 1), 1e-6f);
  playback_backend_close(play);
  playback_backend_free(play);
  audio_chunk_free(chunk);

  webaudio_device_set_capture_format(0, 0);
  webaudio_device_set_playback_format(0, 0);
}

TEST(WebAudioBackend_FormatChangeStopsBothSides) {
  backend_error_t err;
  backend_error_init(&err, BACKEND_ERROR_NONE, "");
  webaudio_device_set_capture_format(48000, 0);
  webaudio_device_set_playback_format(48000, 2);

  capture_device_config_t cap_cfg = {0};
  cap_cfg.type = AUDIO_BACKEND_TYPE_WEB_AUDIO;
  cap_cfg.cfg.webaudio.channels = 2;
  capture_backend_t *cap =
      create_capture_backend(&cap_cfg, 48000, 128, false, NULL, &err);
  ASSERT_TRUE(cap != NULL);
  ASSERT_TRUE(capture_backend_open(cap, &err));
  playback_device_config_t play_cfg = {0};
  play_cfg.type = AUDIO_BACKEND_TYPE_WEB_AUDIO;
  play_cfg.cfg.webaudio.channels = 2;
  playback_backend_t *play =
      create_playback_backend(&play_cfg, 48000, 128, false, NULL, &err);
  ASSERT_TRUE(play != NULL);
  ASSERT_TRUE(playback_backend_open(play, &err));

  double rate = 0.0;
  // Learning the captured stream's count (unknown -> 2) is not a change.
  webaudio_device_set_capture_format(48000, 2);
  webaudio_device_set_playback_format(48000, 2);
  ASSERT_FALSE(capture_backend_get_pending_rate_change(cap, &rate));
  ASSERT_FALSE(playback_backend_get_pending_rate_change(play, &rate));

  // A different captured channel count flags both sides.
  webaudio_device_set_capture_format(48000, 1);
  webaudio_device_set_playback_format(48000, 2);
  ASSERT_TRUE(capture_backend_get_pending_rate_change(cap, &rate));
  ASSERT_NEAR(rate, 48000.0, 0.0);
  ASSERT_TRUE(playback_backend_get_pending_rate_change(play, &rate));
  ASSERT_FALSE(playback_backend_get_pending_rate_change(play, &rate));

  // A new output rate reports the device's new rate.
  webaudio_device_set_capture_format(44100, 1);
  webaudio_device_set_playback_format(44100, 2);
  ASSERT_TRUE(capture_backend_get_pending_rate_change(cap, &rate));
  ASSERT_NEAR(rate, 44100.0, 0.0);
  ASSERT_TRUE(playback_backend_get_pending_rate_change(play, &rate));
  ASSERT_NEAR(rate, 44100.0, 0.0);

  capture_backend_close(cap);
  capture_backend_free(cap);
  playback_backend_close(play);
  playback_backend_free(play);
  webaudio_device_set_capture_format(0, 0);
  webaudio_device_set_playback_format(0, 0);
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

TEST(WebAudioConfig_AllowsResamplerAndRateAdjust) {
  // Capture and playback run on separate contexts with independent clocks.
  config_error_t err;
  ASSERT_EQ(0, parse_webaudio_config("", &err));
  ASSERT_EQ(0, parse_webaudio_config(
                   "\"resampler\": {\"type\": \"AsyncSinc\", "
                   "\"profile\": \"Balanced\"}, "
                   "\"capture_samplerate\": 44100, "
                   "\"enable_rate_adjust\": true, ",
                   &err));
}

TEST(WebAudioBackend_DirectionsHaveIndependentRates) {
  backend_error_t err;
  backend_error_init(&err, BACKEND_ERROR_NONE, "");
  webaudio_device_set_capture_format(44100, 2);
  webaudio_device_set_playback_format(48000, 2);

  capture_device_config_t cap_cfg = {0};
  cap_cfg.type = AUDIO_BACKEND_TYPE_WEB_AUDIO;
  cap_cfg.cfg.webaudio.channels = 2;
  capture_backend_t *cap =
      create_capture_backend(&cap_cfg, 44100, 128, false, NULL, &err);
  ASSERT_TRUE(cap != NULL);
  ASSERT_TRUE(capture_backend_open(cap, &err));
  playback_device_config_t play_cfg = {0};
  play_cfg.type = AUDIO_BACKEND_TYPE_WEB_AUDIO;
  play_cfg.cfg.webaudio.channels = 2;
  playback_backend_t *play =
      create_playback_backend(&play_cfg, 48000, 128, false, NULL, &err);
  ASSERT_TRUE(play != NULL);
  ASSERT_TRUE(playback_backend_open(play, &err));

  // A capture-only render call does not touch playback, and vice versa.
  float in0[128], in1[128];
  for (int i = 0; i < 128; i++) {
    in0[i] = 0.5f;
    in1[i] = -0.5f;
  }
  const float *ins[2] = {in0, in1};
  webaudio_device_process(ins, 2, NULL, 0, 128);
  audio_chunk_t *chunk = audio_chunk_create(128, 2);
  ASSERT_TRUE(chunk != NULL);
  ASSERT_TRUE(capture_backend_read(cap, 128, chunk, &err));
  ASSERT_NEAR(audio_chunk_get_channel(chunk, 0)[0], 0.5, 1e-6);
  ASSERT_TRUE(playback_backend_write(play, chunk, &err));
  float out0[128], out1[128];
  float *outs[2] = {out0, out1};
  webaudio_device_process(NULL, 0, outs, 2, 128);
  ASSERT_NEAR(out1[0], -0.5f, 1e-6f);

  // A new playback rate reports it on playback, capture keeps its own.
  double rate = 0.0;
  webaudio_device_set_playback_format(44100, 2);
  ASSERT_TRUE(playback_backend_get_pending_rate_change(play, &rate));
  ASSERT_NEAR(rate, 44100.0, 0.0);
  ASSERT_TRUE(capture_backend_get_pending_rate_change(cap, &rate));
  ASSERT_NEAR(rate, 44100.0, 0.0);
  webaudio_device_set_capture_format(96000, 2);
  ASSERT_TRUE(capture_backend_get_pending_rate_change(cap, &rate));
  ASSERT_NEAR(rate, 96000.0, 0.0);

  capture_backend_close(cap);
  capture_backend_free(cap);
  playback_backend_close(play);
  playback_backend_free(play);
  audio_chunk_free(chunk);
  webaudio_device_set_capture_format(0, 0);
  webaudio_device_set_playback_format(0, 0);
}

TEST_MAIN()
