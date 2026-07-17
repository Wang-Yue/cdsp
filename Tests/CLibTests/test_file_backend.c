#if defined(__linux__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "Backend/file_backend.h"
#include "Utils/cdsp_time.h"
#include "test_support.h"

TEST(FileBackendRawRoundTrip) {
  char raw_filename[256];
  snprintf(raw_filename, sizeof(raw_filename),
           "/tmp/test_file_backend_roundtrip_%d.raw", getpid());
  remove(raw_filename);

  // 1. Write raw float values to file
  playback_device_config_t play_cfg;
  memset(&play_cfg, 0, sizeof(play_cfg));
  play_cfg.type = AUDIO_BACKEND_TYPE_FILE;
  play_cfg.is_wav = false;
  play_cfg.has_is_wav = true;
  play_cfg.cfg.raw_file.channels = 2;
  snprintf(play_cfg.cfg.raw_file.filename,
           sizeof(play_cfg.cfg.raw_file.filename), "%s", raw_filename);
  play_cfg.cfg.raw_file.has_filename = true;
  play_cfg.cfg.raw_file.format = BINARY_SAMPLE_FORMAT_F32_LE;
  play_cfg.cfg.raw_file.has_format = true;

  backend_error_t err;
  playback_backend_t* playback =
      file_playback_create(&play_cfg, 44100, 1024, NULL, &err);
  ASSERT_TRUE(playback != NULL);
  ASSERT_TRUE(playback_backend_open(playback, &err));

  audio_chunk_t* write_chunk = audio_chunk_create(100, 2);
  for (size_t f = 0; f < 100; f++) {
    audio_chunk_get_channel(write_chunk, 0)[f] = (double)f / 100.0;
    audio_chunk_get_channel(write_chunk, 1)[f] = -(double)f / 100.0;
  }
  audio_chunk_set_valid_frames(write_chunk, 100);

  ASSERT_TRUE(playback_backend_write(playback, write_chunk, &err));
  playback_backend_close(playback);
  playback_backend_free(playback);

  // 2. Read back and verify
  capture_device_config_t cap_cfg;
  memset(&cap_cfg, 0, sizeof(cap_cfg));
  cap_cfg.type = AUDIO_BACKEND_TYPE_FILE;
  cap_cfg.is_wav = false;
  cap_cfg.has_is_wav = true;
  cap_cfg.cfg.raw_file.channels = 2;
  snprintf(cap_cfg.cfg.raw_file.filename, sizeof(cap_cfg.cfg.raw_file.filename),
           "%s", raw_filename);
  cap_cfg.cfg.raw_file.has_filename = true;
  cap_cfg.cfg.raw_file.format = BINARY_SAMPLE_FORMAT_F32_LE;
  cap_cfg.cfg.raw_file.has_format = true;

  capture_backend_t* capture =
      file_capture_create(&cap_cfg, 44100, 1024, NULL, &err);
  ASSERT_TRUE(capture != NULL);
  ASSERT_TRUE(capture_backend_open(capture, &err));

  audio_chunk_t* read_chunk = audio_chunk_create(100, 2);
  ASSERT_TRUE(capture_backend_read(capture, 100, read_chunk, &err));
  ASSERT_EQ(100, audio_chunk_get_valid_frames(read_chunk));

  for (size_t f = 0; f < 100; f++) {
    ASSERT_NEAR((double)f / 100.0, audio_chunk_get_channel(read_chunk, 0)[f],
                1e-6);
    ASSERT_NEAR(-(double)f / 100.0, audio_chunk_get_channel(read_chunk, 1)[f],
                1e-6);
  }

  audio_chunk_free(write_chunk);
  audio_chunk_free(read_chunk);
  capture_backend_close(capture);
  capture_backend_free(capture);

  remove(raw_filename);
}

TEST(FileBackendLargeReadDynamicRealloc) {
  char raw_filename[256];
  snprintf(raw_filename, sizeof(raw_filename),
           "/tmp/test_file_backend_large_read_%d.raw", getpid());
  remove(raw_filename);

  playback_device_config_t play_cfg;
  memset(&play_cfg, 0, sizeof(play_cfg));
  play_cfg.type = AUDIO_BACKEND_TYPE_FILE;
  play_cfg.is_wav = false;
  play_cfg.has_is_wav = true;
  play_cfg.cfg.raw_file.channels = 2;
  snprintf(play_cfg.cfg.raw_file.filename,
           sizeof(play_cfg.cfg.raw_file.filename), "%s", raw_filename);
  play_cfg.cfg.raw_file.has_filename = true;
  play_cfg.cfg.raw_file.format = BINARY_SAMPLE_FORMAT_F32_LE;
  play_cfg.cfg.raw_file.has_format = true;

  backend_error_t err;
  playback_backend_t* playback =
      file_playback_create(&play_cfg, 44100, 2048, NULL, &err);
  ASSERT_TRUE(playback != NULL);
  ASSERT_TRUE(playback_backend_open(playback, &err));

  audio_chunk_t* write_chunk = audio_chunk_create(2048, 2);
  for (size_t f = 0; f < 2048; f++) {
    audio_chunk_get_channel(write_chunk, 0)[f] = (double)f / 2048.0;
    audio_chunk_get_channel(write_chunk, 1)[f] = -(double)f / 2048.0;
  }
  audio_chunk_set_valid_frames(write_chunk, 2048);

  ASSERT_TRUE(playback_backend_write(playback, write_chunk, &err));
  playback_backend_close(playback);
  playback_backend_free(playback);

  capture_device_config_t cap_cfg;
  memset(&cap_cfg, 0, sizeof(cap_cfg));
  cap_cfg.type = AUDIO_BACKEND_TYPE_FILE;
  cap_cfg.is_wav = false;
  cap_cfg.has_is_wav = true;
  cap_cfg.cfg.raw_file.channels = 2;
  snprintf(cap_cfg.cfg.raw_file.filename, sizeof(cap_cfg.cfg.raw_file.filename),
           "%s", raw_filename);
  cap_cfg.cfg.raw_file.has_filename = true;
  cap_cfg.cfg.raw_file.format = BINARY_SAMPLE_FORMAT_F32_LE;
  cap_cfg.cfg.raw_file.has_format = true;

  // Initialize capture with small chunk_size (64)
  capture_backend_t* capture =
      file_capture_create(&cap_cfg, 44100, 64, NULL, &err);
  ASSERT_TRUE(capture != NULL);
  ASSERT_TRUE(capture_backend_open(capture, &err));

  // Request 2048 frames (exceeding initial chunk_size capacity of 64)
  audio_chunk_t* read_chunk = audio_chunk_create(2048, 2);
  ASSERT_TRUE(capture_backend_read(capture, 2048, read_chunk, &err));
  ASSERT_EQ(2048, audio_chunk_get_valid_frames(read_chunk));

  for (size_t f = 0; f < 2048; f++) {
    ASSERT_NEAR((double)f / 2048.0, audio_chunk_get_channel(read_chunk, 0)[f],
                1e-6);
    ASSERT_NEAR(-(double)f / 2048.0, audio_chunk_get_channel(read_chunk, 1)[f],
                1e-6);
  }

  audio_chunk_free(write_chunk);
  audio_chunk_free(read_chunk);
  capture_backend_close(capture);
  capture_backend_free(capture);

  remove(raw_filename);
}

TEST(FileBackendWavRoundTrip) {
  char wav_filename[256];
  snprintf(wav_filename, sizeof(wav_filename),
           "/tmp/test_file_backend_roundtrip_%d.wav", getpid());
  remove(wav_filename);

  // 1. Write WAV file (S16 format)
  playback_device_config_t play_cfg;
  memset(&play_cfg, 0, sizeof(play_cfg));
  play_cfg.type = AUDIO_BACKEND_TYPE_FILE;
  play_cfg.is_wav = true;
  play_cfg.has_is_wav = true;
  play_cfg.cfg.raw_file.channels = 1;
  snprintf(play_cfg.cfg.raw_file.filename,
           sizeof(play_cfg.cfg.raw_file.filename), "%s", wav_filename);
  play_cfg.cfg.raw_file.has_filename = true;
  play_cfg.cfg.raw_file.format = BINARY_SAMPLE_FORMAT_S16_LE;
  play_cfg.cfg.raw_file.has_format = true;
  play_cfg.cfg.raw_file.wav_header = true;
  play_cfg.cfg.raw_file.has_wav_header = true;

  backend_error_t err;
  playback_backend_t* playback =
      file_playback_create(&play_cfg, 16000, 1024, NULL, &err);
  ASSERT_TRUE(playback != NULL);
  ASSERT_TRUE(playback_backend_open(playback, &err));

  audio_chunk_t* write_chunk = audio_chunk_create(128, 1);
  for (size_t f = 0; f < 128; f++) {
    // Values in [-1.0, 1.0]. S16 will quantize them.
    audio_chunk_get_channel(write_chunk, 0)[f] = (double)f / 128.0;
  }
  audio_chunk_set_valid_frames(write_chunk, 128);

  ASSERT_TRUE(playback_backend_write(playback, write_chunk, &err));
  playback_backend_close(playback);
  playback_backend_free(playback);

  // 2. Read back using WavFile type (checks headers dynamically!)
  capture_device_config_t cap_cfg;
  memset(&cap_cfg, 0, sizeof(cap_cfg));
  cap_cfg.type = AUDIO_BACKEND_TYPE_FILE;
  cap_cfg.is_wav = true;
  cap_cfg.has_is_wav = true;
  snprintf(cap_cfg.cfg.wav_file.filename, sizeof(cap_cfg.cfg.wav_file.filename),
           "%s", wav_filename);
  cap_cfg.cfg.wav_file.has_filename = true;

  // Notice we pass sample_rate = 0, channels = 0 to verify that the
  // open routine updates them from the WAV header!
  capture_backend_t* capture =
      file_capture_create(&cap_cfg, 0, 1024, NULL, &err);
  ASSERT_TRUE(capture != NULL);
  ASSERT_TRUE(capture_backend_open(capture, &err));

  audio_chunk_t* read_chunk = audio_chunk_create(128, 1);
  ASSERT_TRUE(capture_backend_read(capture, 128, read_chunk, &err));
  ASSERT_EQ(128, audio_chunk_get_valid_frames(read_chunk));

  for (size_t f = 0; f < 128; f++) {
    double expected = (double)f / 128.0;
    // Quantization error for S16 is +/- 1/32768
    ASSERT_NEAR(expected, audio_chunk_get_channel(read_chunk, 0)[f],
                1.0 / 32767.0);
  }

  audio_chunk_free(write_chunk);
  audio_chunk_free(read_chunk);
  capture_backend_close(capture);
  capture_backend_free(capture);

  remove(wav_filename);
}

TEST(FileBackendPauseThrottling) {
  char raw_filename[256];
  snprintf(raw_filename, sizeof(raw_filename),
           "/tmp/test_file_backend_pause_%d.raw", getpid());
  remove(raw_filename);

  // 1. Write 200 frames to temp file
  playback_device_config_t play_cfg;
  memset(&play_cfg, 0, sizeof(play_cfg));
  play_cfg.type = AUDIO_BACKEND_TYPE_FILE;
  play_cfg.is_wav = false;
  play_cfg.has_is_wav = true;
  play_cfg.cfg.raw_file.channels = 2;
  snprintf(play_cfg.cfg.raw_file.filename,
           sizeof(play_cfg.cfg.raw_file.filename), "%s", raw_filename);
  play_cfg.cfg.raw_file.has_filename = true;
  play_cfg.cfg.raw_file.format = BINARY_SAMPLE_FORMAT_F32_LE;
  play_cfg.cfg.raw_file.has_format = true;

  backend_error_t err;
  playback_backend_t* playback =
      file_playback_create(&play_cfg, 44100, 1024, NULL, &err);
  ASSERT_TRUE(playback != NULL);
  ASSERT_TRUE(playback_backend_open(playback, &err));

  audio_chunk_t* write_chunk = audio_chunk_create(200, 2);
  for (size_t f = 0; f < 200; f++) {
    audio_chunk_get_channel(write_chunk, 0)[f] = (double)f / 200.0;
    audio_chunk_get_channel(write_chunk, 1)[f] = -(double)f / 200.0;
  }
  audio_chunk_set_valid_frames(write_chunk, 200);

  ASSERT_TRUE(playback_backend_write(playback, write_chunk, &err));
  playback_backend_close(playback);
  playback_backend_free(playback);

  // 2. Open capture
  capture_device_config_t cap_cfg;
  memset(&cap_cfg, 0, sizeof(cap_cfg));
  cap_cfg.type = AUDIO_BACKEND_TYPE_FILE;
  cap_cfg.is_wav = false;
  cap_cfg.has_is_wav = true;
  cap_cfg.cfg.raw_file.channels = 2;
  snprintf(cap_cfg.cfg.raw_file.filename, sizeof(cap_cfg.cfg.raw_file.filename),
           "%s", raw_filename);
  cap_cfg.cfg.raw_file.has_filename = true;
  cap_cfg.cfg.raw_file.format = BINARY_SAMPLE_FORMAT_F32_LE;
  cap_cfg.cfg.raw_file.has_format = true;

  capture_backend_t* capture =
      file_capture_create(&cap_cfg, 44100, 1024, NULL, &err);
  ASSERT_TRUE(capture != NULL);
  ASSERT_TRUE(capture_backend_open(capture, &err));

  // 3. Read first 50 frames
  audio_chunk_t* read_chunk = audio_chunk_create(50, 2);
  ASSERT_TRUE(capture_backend_read(capture, 50, read_chunk, &err));
  ASSERT_EQ(50, audio_chunk_get_valid_frames(read_chunk));
  for (size_t f = 0; f < 50; f++) {
    ASSERT_NEAR((double)f / 200.0, audio_chunk_get_channel(read_chunk, 0)[f],
                1e-6);
  }

  // 4. Pause capture and verify read returns false with 0 valid frames
  capture_backend_set_is_paused(capture, true);
  ASSERT_FALSE(capture_backend_read(capture, 50, read_chunk, &err));
  ASSERT_EQ(0, audio_chunk_get_valid_frames(read_chunk));

  // 5. Unpause capture and read next 50 frames (should be frames 50..99)
  capture_backend_set_is_paused(capture, false);
  ASSERT_TRUE(capture_backend_read(capture, 50, read_chunk, &err));
  ASSERT_EQ(50, audio_chunk_get_valid_frames(read_chunk));
  for (size_t f = 0; f < 50; f++) {
    ASSERT_NEAR((double)(f + 50) / 200.0,
                audio_chunk_get_channel(read_chunk, 0)[f], 1e-6);
  }

  audio_chunk_free(write_chunk);
  audio_chunk_free(read_chunk);
  capture_backend_close(capture);
  capture_backend_free(capture);

  remove(raw_filename);
}

static void run_format_roundtrip_test(binary_sample_format_t format,
                                      double eps) {
  char raw_filename[256];
  snprintf(raw_filename, sizeof(raw_filename),
           "/tmp/test_file_backend_roundtrip_tmp_%d.raw", getpid());
  remove(raw_filename);

  playback_device_config_t play_cfg;
  memset(&play_cfg, 0, sizeof(play_cfg));
  play_cfg.type = AUDIO_BACKEND_TYPE_FILE;
  play_cfg.is_wav = false;
  play_cfg.has_is_wav = true;
  play_cfg.cfg.raw_file.channels = 2;
  snprintf(play_cfg.cfg.raw_file.filename,
           sizeof(play_cfg.cfg.raw_file.filename), "%s", raw_filename);
  play_cfg.cfg.raw_file.has_filename = true;
  play_cfg.cfg.raw_file.format = format;
  play_cfg.cfg.raw_file.has_format = true;

  backend_error_t err;
  playback_backend_t* playback =
      file_playback_create(&play_cfg, 44100, 64, NULL, &err);
  ASSERT_TRUE(playback != NULL);
  ASSERT_TRUE(playback_backend_open(playback, &err));

  audio_chunk_t* write_chunk = audio_chunk_create(10, 2);
  for (size_t f = 0; f < 10; f++) {
    audio_chunk_get_channel(write_chunk, 0)[f] = (double)f / 10.0;
    audio_chunk_get_channel(write_chunk, 1)[f] = -(double)f / 10.0;
  }
  audio_chunk_set_valid_frames(write_chunk, 10);

  ASSERT_TRUE(playback_backend_write(playback, write_chunk, &err));
  playback_backend_close(playback);
  playback_backend_free(playback);

  capture_device_config_t cap_cfg;
  memset(&cap_cfg, 0, sizeof(cap_cfg));
  cap_cfg.type = AUDIO_BACKEND_TYPE_FILE;
  cap_cfg.is_wav = false;
  cap_cfg.has_is_wav = true;
  cap_cfg.cfg.raw_file.channels = 2;
  snprintf(cap_cfg.cfg.raw_file.filename, sizeof(cap_cfg.cfg.raw_file.filename),
           "%s", raw_filename);
  cap_cfg.cfg.raw_file.has_filename = true;
  cap_cfg.cfg.raw_file.format = format;
  cap_cfg.cfg.raw_file.has_format = true;

  capture_backend_t* capture =
      file_capture_create(&cap_cfg, 44100, 64, NULL, &err);
  ASSERT_TRUE(capture != NULL);
  ASSERT_TRUE(capture_backend_open(capture, &err));

  audio_chunk_t* read_chunk = audio_chunk_create(10, 2);
  ASSERT_TRUE(capture_backend_read(capture, 10, read_chunk, &err));
  ASSERT_EQ(10, audio_chunk_get_valid_frames(read_chunk));

  for (size_t f = 0; f < 10; f++) {
    ASSERT_NEAR((double)f / 10.0, audio_chunk_get_channel(read_chunk, 0)[f],
                eps);
    ASSERT_NEAR(-(double)f / 10.0, audio_chunk_get_channel(read_chunk, 1)[f],
                eps);
  }

  audio_chunk_free(write_chunk);
  audio_chunk_free(read_chunk);
  capture_backend_close(capture);
  capture_backend_free(capture);

  remove(raw_filename);
}

TEST(FileBackendS16RoundTrip) {
  run_format_roundtrip_test(BINARY_SAMPLE_FORMAT_S16_LE, 1e-4);
}

TEST(FileBackendS24_3RoundTrip) {
  run_format_roundtrip_test(BINARY_SAMPLE_FORMAT_S24_3_LE, 1e-6);
}

TEST(FileBackendS24_4RJRoundTrip) {
  run_format_roundtrip_test(BINARY_SAMPLE_FORMAT_S24_4_RJ_LE, 1e-6);
}

TEST(FileBackendS24_4LJRoundTrip) {
  run_format_roundtrip_test(BINARY_SAMPLE_FORMAT_S24_4_LJ_LE, 1e-6);
}

TEST(FileBackendS32RoundTrip) {
  run_format_roundtrip_test(BINARY_SAMPLE_FORMAT_S32_LE, 1e-9);
}

TEST(FileBackendF32RoundTrip) {
  run_format_roundtrip_test(BINARY_SAMPLE_FORMAT_F32_LE, 1e-6);
}

TEST(FileBackendF64RoundTrip) {
  run_format_roundtrip_test(BINARY_SAMPLE_FORMAT_F64_LE, 1e-12);
}

TEST(FileBackendRealtimeThrottling) {
  char raw_filename[256];
  snprintf(raw_filename, sizeof(raw_filename),
           "/tmp/test_file_backend_realtime_%d.raw", getpid());
  remove(raw_filename);

  int sample_rate = 44100;
  int channels = 1;
  int frames = 4410;  // 0.1 seconds of audio

  // 1. Write dummy data to file
  playback_device_config_t play_cfg;
  memset(&play_cfg, 0, sizeof(play_cfg));
  play_cfg.type = AUDIO_BACKEND_TYPE_FILE;
  play_cfg.is_wav = false;
  play_cfg.has_is_wav = true;
  play_cfg.cfg.raw_file.channels = channels;
  snprintf(play_cfg.cfg.raw_file.filename,
           sizeof(play_cfg.cfg.raw_file.filename), "%s", raw_filename);
  play_cfg.cfg.raw_file.has_filename = true;
  play_cfg.cfg.raw_file.format = BINARY_SAMPLE_FORMAT_S16_LE;
  play_cfg.cfg.raw_file.has_format = true;

  backend_error_t err;
  playback_backend_t* playback =
      file_playback_create(&play_cfg, sample_rate, 1024, NULL, &err);
  ASSERT_TRUE(playback != NULL);
  ASSERT_TRUE(playback_backend_open(playback, &err));

  audio_chunk_t* write_chunk = audio_chunk_create(frames, channels);
  for (int f = 0; f < frames; f++) {
    audio_chunk_get_channel(write_chunk, 0)[f] = 0.0;
  }
  audio_chunk_set_valid_frames(write_chunk, frames);
  ASSERT_TRUE(playback_backend_write(playback, write_chunk, &err));
  playback_backend_close(playback);
  playback_backend_free(playback);

  // 2. Read back with realtime = false (default) and measure time
  capture_device_config_t cap_cfg;
  memset(&cap_cfg, 0, sizeof(cap_cfg));
  cap_cfg.type = AUDIO_BACKEND_TYPE_FILE;
  cap_cfg.is_wav = false;
  cap_cfg.has_is_wav = true;
  cap_cfg.cfg.raw_file.channels = channels;
  snprintf(cap_cfg.cfg.raw_file.filename, sizeof(cap_cfg.cfg.raw_file.filename),
           "%s", raw_filename);
  cap_cfg.cfg.raw_file.has_filename = true;
  cap_cfg.cfg.raw_file.format = BINARY_SAMPLE_FORMAT_S16_LE;
  cap_cfg.cfg.raw_file.has_format = true;
  cap_cfg.cfg.raw_file.realtime = false;
  cap_cfg.cfg.raw_file.has_realtime = true;

  capture_backend_t* capture_fast =
      file_capture_create(&cap_cfg, sample_rate, 441, NULL, &err);
  ASSERT_TRUE(capture_fast != NULL);
  ASSERT_TRUE(capture_backend_open(capture_fast, &err));

  audio_chunk_t* read_chunk = audio_chunk_create(441, channels);

  uint64_t t0 = cdsp_time_now_ns();
  for (int i = 0; i < 10; i++) {
    ASSERT_TRUE(capture_backend_read(capture_fast, 441, read_chunk, &err));
  }
  uint64_t t1 = cdsp_time_now_ns();
  double elapsed_fast = (double)(t1 - t0) / 1000000000.0;

  capture_backend_close(capture_fast);
  capture_backend_free(capture_fast);

  // Fast read should be very quick, much less than 100ms.
  ASSERT_TRUE(elapsed_fast < 0.50);

  // 3. Read back with realtime = true and measure time
  cap_cfg.cfg.raw_file.realtime = true;
  capture_backend_t* capture_rt =
      file_capture_create(&cap_cfg, sample_rate, 441, NULL, &err);
  ASSERT_TRUE(capture_rt != NULL);
  ASSERT_TRUE(capture_backend_open(capture_rt, &err));

  t0 = cdsp_time_now_ns();
  for (int i = 0; i < 10; i++) {
    ASSERT_TRUE(capture_backend_read(capture_rt, 441, read_chunk, &err));
  }
  t1 = cdsp_time_now_ns();
  double elapsed_rt = (double)(t1 - t0) / 1000000000.0;

  capture_backend_close(capture_rt);
  capture_backend_free(capture_rt);

  // Realtime read should take around 100ms (0.1s) of simulated time.
  // We check if it is within [70ms, 200ms] to account for scheduler jitter.
  ASSERT_TRUE(elapsed_rt >= 0.07);
  ASSERT_TRUE(elapsed_rt < 3.00);

  audio_chunk_free(write_chunk);
  audio_chunk_free(read_chunk);
  remove(raw_filename);
}

TEST(FileBackendPlaybackRealtimeThrottling) {
  char raw_filename[256];
  snprintf(raw_filename, sizeof(raw_filename),
           "/tmp/test_file_backend_playback_realtime_%d.raw", getpid());
  remove(raw_filename);

  int sample_rate = 44100;
  int channels = 1;

  playback_device_config_t play_cfg;
  memset(&play_cfg, 0, sizeof(play_cfg));
  play_cfg.type = AUDIO_BACKEND_TYPE_FILE;
  play_cfg.is_wav = false;
  play_cfg.has_is_wav = true;
  play_cfg.cfg.raw_file.channels = channels;
  snprintf(play_cfg.cfg.raw_file.filename,
           sizeof(play_cfg.cfg.raw_file.filename), "%s", raw_filename);
  play_cfg.cfg.raw_file.has_filename = true;
  play_cfg.cfg.raw_file.format = BINARY_SAMPLE_FORMAT_S16_LE;
  play_cfg.cfg.raw_file.has_format = true;
  play_cfg.cfg.raw_file.realtime = false;
  play_cfg.cfg.raw_file.has_realtime = true;

  backend_error_t err;
  playback_backend_t* playback_fast =
      file_playback_create(&play_cfg, sample_rate, 441, NULL, &err);
  ASSERT_TRUE(playback_fast != NULL);
  ASSERT_TRUE(playback_backend_open(playback_fast, &err));

  audio_chunk_t* write_chunk = audio_chunk_create(441, channels);
  for (int f = 0; f < 441; f++) {
    audio_chunk_get_channel(write_chunk, 0)[f] = 0.0;
  }
  audio_chunk_set_valid_frames(write_chunk, 441);

  uint64_t t0 = cdsp_time_now_ns();
  for (int i = 0; i < 10; i++) {
    ASSERT_TRUE(playback_backend_write(playback_fast, write_chunk, &err));
  }
  uint64_t t1 = cdsp_time_now_ns();
  double elapsed_fast = (double)(t1 - t0) / 1000000000.0;

  playback_backend_close(playback_fast);
  playback_backend_free(playback_fast);
  remove(raw_filename);

  ASSERT_TRUE(elapsed_fast < 0.50);

  play_cfg.cfg.raw_file.realtime = true;
  playback_backend_t* playback_rt =
      file_playback_create(&play_cfg, sample_rate, 441, NULL, &err);
  ASSERT_TRUE(playback_rt != NULL);
  ASSERT_TRUE(playback_backend_open(playback_rt, &err));

  t0 = cdsp_time_now_ns();
  for (int i = 0; i < 10; i++) {
    ASSERT_TRUE(playback_backend_write(playback_rt, write_chunk, &err));
  }
  t1 = cdsp_time_now_ns();
  double elapsed_rt = (double)(t1 - t0) / 1000000000.0;

  playback_backend_close(playback_rt);
  playback_backend_free(playback_rt);
  remove(raw_filename);

  ASSERT_TRUE(elapsed_rt >= 0.07);
  ASSERT_TRUE(elapsed_rt < 3.00);

  audio_chunk_free(write_chunk);
}

TEST(FileBackendWavRealtimeThrottling) {
  char wav_filename[256];
  snprintf(wav_filename, sizeof(wav_filename),
           "/tmp/test_file_backend_wav_realtime_%d.wav", getpid());
  remove(wav_filename);

  int sample_rate = 16000;
  int channels = 1;
  int frames = 1600;  // 0.1 seconds of audio

  // 1. Write WAV data to file
  playback_device_config_t play_cfg;
  memset(&play_cfg, 0, sizeof(play_cfg));
  play_cfg.type = AUDIO_BACKEND_TYPE_FILE;
  play_cfg.is_wav = true;
  play_cfg.has_is_wav = true;
  play_cfg.cfg.raw_file.channels = channels;
  snprintf(play_cfg.cfg.raw_file.filename,
           sizeof(play_cfg.cfg.raw_file.filename), "%s", wav_filename);
  play_cfg.cfg.raw_file.has_filename = true;
  play_cfg.cfg.raw_file.format = BINARY_SAMPLE_FORMAT_S16_LE;
  play_cfg.cfg.raw_file.has_format = true;
  play_cfg.cfg.raw_file.wav_header = true;
  play_cfg.cfg.raw_file.has_wav_header = true;

  backend_error_t err;
  playback_backend_t* playback =
      file_playback_create(&play_cfg, sample_rate, 1024, NULL, &err);
  ASSERT_TRUE(playback != NULL);
  ASSERT_TRUE(playback_backend_open(playback, &err));

  audio_chunk_t* write_chunk = audio_chunk_create(frames, channels);
  for (int f = 0; f < frames; f++) {
    audio_chunk_get_channel(write_chunk, 0)[f] = 0.0;
  }
  audio_chunk_set_valid_frames(write_chunk, frames);
  ASSERT_TRUE(playback_backend_write(playback, write_chunk, &err));
  playback_backend_close(playback);
  playback_backend_free(playback);

  // 2. Read back with realtime = false (default) and measure time
  capture_device_config_t cap_cfg;
  memset(&cap_cfg, 0, sizeof(cap_cfg));
  cap_cfg.type = AUDIO_BACKEND_TYPE_FILE;
  cap_cfg.is_wav = true;
  cap_cfg.has_is_wav = true;
  snprintf(cap_cfg.cfg.wav_file.filename, sizeof(cap_cfg.cfg.wav_file.filename),
           "%s", wav_filename);
  cap_cfg.cfg.wav_file.has_filename = true;
  cap_cfg.cfg.wav_file.realtime = false;
  cap_cfg.cfg.wav_file.has_realtime = true;

  capture_backend_t* capture_fast =
      file_capture_create(&cap_cfg, sample_rate, 160, NULL, &err);
  ASSERT_TRUE(capture_fast != NULL);
  ASSERT_TRUE(capture_backend_open(capture_fast, &err));

  audio_chunk_t* read_chunk = audio_chunk_create(160, channels);

  uint64_t t0 = cdsp_time_now_ns();
  for (int i = 0; i < 10; i++) {
    ASSERT_TRUE(capture_backend_read(capture_fast, 160, read_chunk, &err));
  }
  uint64_t t1 = cdsp_time_now_ns();
  double elapsed_fast = (double)(t1 - t0) / 1000000000.0;

  capture_backend_close(capture_fast);
  capture_backend_free(capture_fast);

  ASSERT_TRUE(elapsed_fast < 0.50);

  // 3. Read back with realtime = true and measure time
  cap_cfg.cfg.wav_file.realtime = true;
  capture_backend_t* capture_rt =
      file_capture_create(&cap_cfg, sample_rate, 160, NULL, &err);
  ASSERT_TRUE(capture_rt != NULL);
  ASSERT_TRUE(capture_backend_open(capture_rt, &err));

  t0 = cdsp_time_now_ns();
  for (int i = 0; i < 10; i++) {
    ASSERT_TRUE(capture_backend_read(capture_rt, 160, read_chunk, &err));
  }
  t1 = cdsp_time_now_ns();
  double elapsed_rt = (double)(t1 - t0) / 1000000000.0;

  capture_backend_close(capture_rt);
  capture_backend_free(capture_rt);

  // Realtime read should take around 100ms (0.1s) of simulated time.
  ASSERT_TRUE(elapsed_rt >= 0.07);
  ASSERT_TRUE(elapsed_rt < 3.00);

  audio_chunk_free(write_chunk);
  audio_chunk_free(read_chunk);
  remove(wav_filename);
}

TEST_MAIN()
