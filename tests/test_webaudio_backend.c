#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "audio/audio_chunk.h"
#include "backend/audio_backend.h"
#include "backend/backend_error.h"
#include "backend/webaudio_backend.h"
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
  size_t pushed = webaudio_capture_push_samples(cap, cap_ptrs, 128);
  ASSERT_EQ(pushed, 128);

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
  size_t rendered = webaudio_playback_render_samples(play, render_ptrs, 128);
  ASSERT_EQ(rendered, 128);
  ASSERT_NEAR(render_ch0[0], 0.75f, 1e-5f);
  ASSERT_NEAR(render_ch1[0], -0.75f, 1e-5f);

  playback_backend_close(play);
  playback_backend_free(play);
  audio_chunk_free(write_chunk);
}

TEST_MAIN()
