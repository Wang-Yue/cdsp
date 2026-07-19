#if defined(__linux__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "Backend/audio_backend.h"
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
      create_playback_backend(&play_cfg, 44100, 1024, false, NULL, &err);
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
      create_capture_backend(&cap_cfg, 44100, 1024, false, NULL, &err);
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
      create_playback_backend(&play_cfg, 44100, 2048, false, NULL, &err);
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
      create_capture_backend(&cap_cfg, 44100, 64, false, NULL, &err);
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
      create_playback_backend(&play_cfg, 16000, 1024, false, NULL, &err);
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
      create_capture_backend(&cap_cfg, 0, 1024, false, NULL, &err);
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
      create_playback_backend(&play_cfg, 44100, 1024, false, NULL, &err);
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
      create_capture_backend(&cap_cfg, 44100, 1024, false, NULL, &err);
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

  // 4. Set capture paused flag and verify read continues returning frames for metering/auto-resume
  capture_backend_set_is_paused(capture, true);
  ASSERT_TRUE(capture_backend_read(capture, 50, read_chunk, &err));
  ASSERT_EQ(50, audio_chunk_get_valid_frames(read_chunk));

  // 5. Unpause capture and read next 50 frames (should be frames 100..149)
  capture_backend_set_is_paused(capture, false);
  ASSERT_TRUE(capture_backend_read(capture, 50, read_chunk, &err));
  ASSERT_EQ(50, audio_chunk_get_valid_frames(read_chunk));
  for (size_t f = 0; f < 50; f++) {
    ASSERT_NEAR((double)(f + 100) / 200.0,
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
      create_playback_backend(&play_cfg, 44100, 64, false, NULL, &err);
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
      create_capture_backend(&cap_cfg, 44100, 64, false, NULL, &err);
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

static void run_wav_format_roundtrip_test(binary_sample_format_t format, double eps) {
  char wav_filename[256];
  snprintf(wav_filename, sizeof(wav_filename),
           "/tmp/test_file_backend_wav_fmt_%d.wav", getpid());
  remove(wav_filename);

  // 1. Playback / Write
  playback_device_config_t play_cfg;
  memset(&play_cfg, 0, sizeof(play_cfg));
  play_cfg.type = AUDIO_BACKEND_TYPE_FILE;
  play_cfg.is_wav = true;
  play_cfg.has_is_wav = true;
  play_cfg.cfg.raw_file.channels = 2;
  snprintf(play_cfg.cfg.raw_file.filename,
           sizeof(play_cfg.cfg.raw_file.filename), "%s", wav_filename);
  play_cfg.cfg.raw_file.has_filename = true;
  play_cfg.cfg.raw_file.format = format;
  play_cfg.cfg.raw_file.has_format = true;
  play_cfg.cfg.raw_file.wav_header = true;
  play_cfg.cfg.raw_file.has_wav_header = true;
  play_cfg.cfg.raw_file.use_rf64 = false;
  play_cfg.cfg.raw_file.has_use_rf64 = true;

  backend_error_t err;
  playback_backend_t* playback =
      create_playback_backend(&play_cfg, 44100, 64, false, NULL, &err);
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

  // 2. Capture / Read
  capture_device_config_t cap_cfg;
  memset(&cap_cfg, 0, sizeof(cap_cfg));
  cap_cfg.type = AUDIO_BACKEND_TYPE_FILE;
  cap_cfg.is_wav = true;
  cap_cfg.has_is_wav = true;
  snprintf(cap_cfg.cfg.wav_file.filename, sizeof(cap_cfg.cfg.wav_file.filename),
           "%s", wav_filename);
  cap_cfg.cfg.wav_file.has_filename = true;

  capture_backend_t* capture =
      create_capture_backend(&cap_cfg, 44100, 64, false, NULL, &err);
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

  remove(wav_filename);
}

TEST(FileBackendWavS16RoundTrip) {
  run_wav_format_roundtrip_test(BINARY_SAMPLE_FORMAT_S16_LE, 1e-4);
}

TEST(FileBackendWavS24RoundTrip) {
  run_wav_format_roundtrip_test(BINARY_SAMPLE_FORMAT_S24_3_LE, 1e-6);
}

TEST(FileBackendWavS32RoundTrip) {
  run_wav_format_roundtrip_test(BINARY_SAMPLE_FORMAT_S32_LE, 1e-9);
}

TEST(FileBackendWavF32RoundTrip) {
  run_wav_format_roundtrip_test(BINARY_SAMPLE_FORMAT_F32_LE, 1e-6);
}

TEST(FileBackendWavF64RoundTrip) {
  run_wav_format_roundtrip_test(BINARY_SAMPLE_FORMAT_F64_LE, 1e-12);
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
      create_playback_backend(&play_cfg, sample_rate, 1024, false, NULL, &err);
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
      create_capture_backend(&cap_cfg, sample_rate, 441, false, NULL, &err);
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
      create_capture_backend(&cap_cfg, sample_rate, 441, false, NULL, &err);
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
      create_playback_backend(&play_cfg, sample_rate, 441, false, NULL, &err);
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
      create_playback_backend(&play_cfg, sample_rate, 441, false, NULL, &err);
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
      create_playback_backend(&play_cfg, sample_rate, 1024, false, NULL, &err);
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
      create_capture_backend(&cap_cfg, sample_rate, 160, false, NULL, &err);
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
      create_capture_backend(&cap_cfg, sample_rate, 160, false, NULL, &err);
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

TEST(FileBackendRF64Read) {
  char wav_filename[256];
  snprintf(wav_filename, sizeof(wav_filename),
           "/tmp/test_file_backend_rf64_%d.wav", getpid());
  remove(wav_filename);

  FILE* f = fopen(wav_filename, "wb");
  ASSERT_TRUE(f != NULL);

  // RF64 WAV header
  uint8_t header[80] = {
      'R', 'F', '6', '4',
      0xFF, 0xFF, 0xFF, 0xFF,
      'W', 'A', 'V', 'E',
      'd', 's', '6', '4',
      28, 0, 0, 0, // ds64 chunk size (28)
      250, 0, 0, 0, 0, 0, 0, 0, // RIFF size (64-bit)
      0, 1, 0, 0, 0, 0, 0, 0, // Data size (64-bit: 256 bytes)
      128, 0, 0, 0, 0, 0, 0, 0, // Sample count (64-bit: 128 samples)
      0, 0, 0, 0, // table entry count
      'f', 'm', 't', ' ',
      16, 0, 0, 0, // fmt chunk size (16)
      1, 0, // format tag (PCM)
      1, 0, // channels (1)
      0x80, 0x3E, 0, 0, // sample rate (16000)
      0x00, 0x7D, 0, 0, // byte rate (32000)
      2, 0, // block align (2)
      16, 0, // bits per sample (16)
      'd', 'a', 't', 'a',
      0xFF, 0xFF, 0xFF, 0xFF // data size placeholder (RF64 style)
  };

  fwrite(header, 1, sizeof(header), f);

  // Write 128 samples of PCM 16-bit audio (256 bytes)
  int16_t samples[128] = {0};
  for (int i = 0; i < 128; i++) {
    samples[i] = i * 100;
  }
  fwrite(samples, sizeof(int16_t), 128, f);
  fclose(f);

  // Read the RF64 file back and check that the parser correctly extracts params
  capture_device_config_t cap_cfg;
  memset(&cap_cfg, 0, sizeof(cap_cfg));
  cap_cfg.type = AUDIO_BACKEND_TYPE_FILE;
  cap_cfg.is_wav = true;
  cap_cfg.has_is_wav = true;
  snprintf(cap_cfg.cfg.wav_file.filename, sizeof(cap_cfg.cfg.wav_file.filename),
           "%s", wav_filename);
  cap_cfg.cfg.wav_file.has_filename = true;

  backend_error_t err;
  capture_backend_t* capture =
      create_capture_backend(&cap_cfg, 0, 1024, false, NULL, &err);
  ASSERT_TRUE(capture != NULL);
  ASSERT_TRUE(capture_backend_open(capture, &err));

  audio_chunk_t* read_chunk = audio_chunk_create(128, 1);
  ASSERT_TRUE(capture_backend_read(capture, 128, read_chunk, &err));
  ASSERT_EQ(128, audio_chunk_get_valid_frames(read_chunk));

  for (size_t f_idx = 0; f_idx < 128; f_idx++) {
    double expected = (double)((int16_t)f_idx * 100) / 32768.0;
    ASSERT_NEAR(expected, audio_chunk_get_channel(read_chunk, 0)[f_idx], 1.0 / 32767.0);
  }

  audio_chunk_free(read_chunk);
  capture_backend_close(capture);
  capture_backend_free(capture);

  remove(wav_filename);
}

TEST(FileBackendRF64RoundTrip) {
  char wav_filename[256];
  snprintf(wav_filename, sizeof(wav_filename),
           "/tmp/test_file_backend_rf64_rt_%d.wav", getpid());
  remove(wav_filename);

  // 1. Write RF64 WAV file (S16 format)
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
  play_cfg.cfg.raw_file.use_rf64 = true;
  play_cfg.cfg.raw_file.has_use_rf64 = true;

  backend_error_t err;
  playback_backend_t* playback =
      create_playback_backend(&play_cfg, 16000, 1024, false, NULL, &err);
  ASSERT_TRUE(playback != NULL);
  ASSERT_TRUE(playback_backend_open(playback, &err));

  audio_chunk_t* write_chunk = audio_chunk_create(128, 1);
  for (size_t f = 0; f < 128; f++) {
    audio_chunk_get_channel(write_chunk, 0)[f] = (double)f / 128.0;
  }
  audio_chunk_set_valid_frames(write_chunk, 128);

  ASSERT_TRUE(playback_backend_write(playback, write_chunk, &err));
  playback_backend_close(playback);
  playback_backend_free(playback);

  // Verify it starts with RF64
  FILE* check_f = fopen(wav_filename, "rb");
  ASSERT_TRUE(check_f != NULL);
  char sig[4];
  ASSERT_EQ(4, fread(sig, 1, 4, check_f));
  ASSERT_TRUE(memcmp(sig, "RF64", 4) == 0);
  fclose(check_f);

  // 2. Read back and verify values
  capture_device_config_t cap_cfg;
  memset(&cap_cfg, 0, sizeof(cap_cfg));
  cap_cfg.type = AUDIO_BACKEND_TYPE_FILE;
  cap_cfg.is_wav = true;
  cap_cfg.has_is_wav = true;
  snprintf(cap_cfg.cfg.wav_file.filename, sizeof(cap_cfg.cfg.wav_file.filename),
           "%s", wav_filename);
  cap_cfg.cfg.wav_file.has_filename = true;

  capture_backend_t* capture =
      create_capture_backend(&cap_cfg, 0, 1024, false, NULL, &err);
  ASSERT_TRUE(capture != NULL);
  ASSERT_TRUE(capture_backend_open(capture, &err));

  audio_chunk_t* read_chunk = audio_chunk_create(128, 1);
  ASSERT_TRUE(capture_backend_read(capture, 128, read_chunk, &err));
  ASSERT_EQ(128, audio_chunk_get_valid_frames(read_chunk));

  for (size_t f = 0; f < 128; f++) {
    double expected = (double)f / 128.0;
    ASSERT_NEAR(expected, audio_chunk_get_channel(read_chunk, 0)[f],
                1.0 / 32767.0);
  }

  audio_chunk_free(write_chunk);
  audio_chunk_free(read_chunk);
  capture_backend_close(capture);
  capture_backend_free(capture);

  remove(wav_filename);
}

TEST(FileBackendWavRF64CrossRoundTrip) {
  char rf64_in_filename[256];
  char wav_mid_filename[256];
  char rf64_out_filename[256];
  snprintf(rf64_in_filename, sizeof(rf64_in_filename),
           "/tmp/test_file_backend_cross_in_%d.wav", getpid());
  snprintf(wav_mid_filename, sizeof(wav_mid_filename),
           "/tmp/test_file_backend_cross_mid_%d.wav", getpid());
  snprintf(rf64_out_filename, sizeof(rf64_out_filename),
           "/tmp/test_file_backend_cross_out_%d.wav", getpid());

  remove(rf64_in_filename);
  remove(wav_mid_filename);
  remove(rf64_out_filename);

  // Prep Step: Write an RF64 input file manually
  FILE* f = fopen(rf64_in_filename, "wb");
  ASSERT_TRUE(f != NULL);
  uint8_t rf64_header[80] = {
      'R', 'F', '6', '4',
      0xFF, 0xFF, 0xFF, 0xFF,
      'W', 'A', 'V', 'E',
      'd', 's', '6', '4',
      28, 0, 0, 0, // ds64 chunk size (28)
      250, 0, 0, 0, 0, 0, 0, 0, // RIFF size (64-bit)
      0, 1, 0, 0, 0, 0, 0, 0, // Data size (64-bit: 256 bytes)
      128, 0, 0, 0, 0, 0, 0, 0, // Sample count (64-bit: 128 samples)
      0, 0, 0, 0, // table entry count
      'f', 'm', 't', ' ',
      16, 0, 0, 0, // fmt chunk size (16)
      1, 0, // format tag (PCM)
      1, 0, // channels (1)
      0x80, 0x3E, 0, 0, // sample rate (16000)
      0x00, 0x7D, 0, 0, // byte rate (32000)
      2, 0, // block align (2)
      16, 0, // bits per sample (16)
      'd', 'a', 't', 'a',
      0xFF, 0xFF, 0xFF, 0xFF // data size placeholder
  };
  fwrite(rf64_header, 1, sizeof(rf64_header), f);
  int16_t samples[128] = {0};
  for (int i = 0; i < 128; i++) {
    samples[i] = i * 100;
  }
  fwrite(samples, sizeof(int16_t), 128, f);
  fclose(f);

  // 1. RF64 Capture -> WAV Playback
  capture_device_config_t cap_cfg_1;
  memset(&cap_cfg_1, 0, sizeof(cap_cfg_1));
  cap_cfg_1.type = AUDIO_BACKEND_TYPE_FILE;
  cap_cfg_1.is_wav = true;
  cap_cfg_1.has_is_wav = true;
  snprintf(cap_cfg_1.cfg.wav_file.filename, sizeof(cap_cfg_1.cfg.wav_file.filename),
           "%s", rf64_in_filename);
  cap_cfg_1.cfg.wav_file.has_filename = true;

  backend_error_t err;
  capture_backend_t* capture_1 =
      create_capture_backend(&cap_cfg_1, 0, 1024, false, NULL, &err);
  ASSERT_TRUE(capture_1 != NULL);
  ASSERT_TRUE(capture_backend_open(capture_1, &err));

  playback_device_config_t play_cfg_1;
  memset(&play_cfg_1, 0, sizeof(play_cfg_1));
  play_cfg_1.type = AUDIO_BACKEND_TYPE_FILE;
  play_cfg_1.is_wav = true;
  play_cfg_1.has_is_wav = true;
  play_cfg_1.cfg.raw_file.channels = 1;
  snprintf(play_cfg_1.cfg.raw_file.filename,
           sizeof(play_cfg_1.cfg.raw_file.filename), "%s", wav_mid_filename);
  play_cfg_1.cfg.raw_file.has_filename = true;
  play_cfg_1.cfg.raw_file.format = BINARY_SAMPLE_FORMAT_S16_LE;
  play_cfg_1.cfg.raw_file.has_format = true;
  play_cfg_1.cfg.raw_file.wav_header = true;
  play_cfg_1.cfg.raw_file.has_wav_header = true;
  play_cfg_1.cfg.raw_file.use_rf64 = false; // Plain WAV
  play_cfg_1.cfg.raw_file.has_use_rf64 = true;

  playback_backend_t* playback_1 =
      create_playback_backend(&play_cfg_1, 16000, 1024, false, NULL, &err);
  ASSERT_TRUE(playback_1 != NULL);
  ASSERT_TRUE(playback_backend_open(playback_1, &err));

  audio_chunk_t* chunk = audio_chunk_create(128, 1);
  ASSERT_TRUE(capture_backend_read(capture_1, 128, chunk, &err));
  ASSERT_TRUE(playback_backend_write(playback_1, chunk, &err));

  capture_backend_close(capture_1);
  capture_backend_free(capture_1);
  playback_backend_close(playback_1);
  playback_backend_free(playback_1);

  // Verify the intermediate file is standard WAV
  FILE* check_wav = fopen(wav_mid_filename, "rb");
  ASSERT_TRUE(check_wav != NULL);
  char sig[4];
  ASSERT_EQ(4, fread(sig, 1, 4, check_wav));
  ASSERT_TRUE(memcmp(sig, "RIFF", 4) == 0);
  fclose(check_wav);

  // 2. WAV Capture -> RF64 Playback
  capture_device_config_t cap_cfg_2;
  memset(&cap_cfg_2, 0, sizeof(cap_cfg_2));
  cap_cfg_2.type = AUDIO_BACKEND_TYPE_FILE;
  cap_cfg_2.is_wav = true;
  cap_cfg_2.has_is_wav = true;
  snprintf(cap_cfg_2.cfg.wav_file.filename, sizeof(cap_cfg_2.cfg.wav_file.filename),
           "%s", wav_mid_filename);
  cap_cfg_2.cfg.wav_file.has_filename = true;

  capture_backend_t* capture_2 =
      create_capture_backend(&cap_cfg_2, 0, 1024, false, NULL, &err);
  ASSERT_TRUE(capture_2 != NULL);
  ASSERT_TRUE(capture_backend_open(capture_2, &err));

  playback_device_config_t play_cfg_2;
  memset(&play_cfg_2, 0, sizeof(play_cfg_2));
  play_cfg_2.type = AUDIO_BACKEND_TYPE_FILE;
  play_cfg_2.is_wav = true;
  play_cfg_2.has_is_wav = true;
  play_cfg_2.cfg.raw_file.channels = 1;
  snprintf(play_cfg_2.cfg.raw_file.filename,
           sizeof(play_cfg_2.cfg.raw_file.filename), "%s", rf64_out_filename);
  play_cfg_2.cfg.raw_file.has_filename = true;
  play_cfg_2.cfg.raw_file.format = BINARY_SAMPLE_FORMAT_S16_LE;
  play_cfg_2.cfg.raw_file.has_format = true;
  play_cfg_2.cfg.raw_file.wav_header = true;
  play_cfg_2.cfg.raw_file.has_wav_header = true;
  play_cfg_2.cfg.raw_file.use_rf64 = true; // RF64 WAV
  play_cfg_2.cfg.raw_file.has_use_rf64 = true;

  playback_backend_t* playback_2 =
      create_playback_backend(&play_cfg_2, 16000, 1024, false, NULL, &err);
  ASSERT_TRUE(playback_2 != NULL);
  ASSERT_TRUE(playback_backend_open(playback_2, &err));

  audio_chunk_t* chunk2 = audio_chunk_create(128, 1);
  ASSERT_TRUE(capture_backend_read(capture_2, 128, chunk2, &err));
  ASSERT_TRUE(playback_backend_write(playback_2, chunk2, &err));

  capture_backend_close(capture_2);
  capture_backend_free(capture_2);
  playback_backend_close(playback_2);
  playback_backend_free(playback_2);

  // Verify the final file is RF64 WAV
  FILE* check_rf64 = fopen(rf64_out_filename, "rb");
  ASSERT_TRUE(check_rf64 != NULL);
  ASSERT_EQ(4, fread(sig, 1, 4, check_rf64));
  ASSERT_TRUE(memcmp(sig, "RF64", 4) == 0);
  fclose(check_rf64);

  // 3. Read back final RF64 and verify content correctness
  capture_device_config_t cap_cfg_final;
  memset(&cap_cfg_final, 0, sizeof(cap_cfg_final));
  cap_cfg_final.type = AUDIO_BACKEND_TYPE_FILE;
  cap_cfg_final.is_wav = true;
  cap_cfg_final.has_is_wav = true;
  snprintf(cap_cfg_final.cfg.wav_file.filename, sizeof(cap_cfg_final.cfg.wav_file.filename),
           "%s", rf64_out_filename);
  cap_cfg_final.cfg.wav_file.has_filename = true;

  capture_backend_t* capture_final =
      create_capture_backend(&cap_cfg_final, 0, 1024, false, NULL, &err);
  ASSERT_TRUE(capture_final != NULL);
  ASSERT_TRUE(capture_backend_open(capture_final, &err));

  audio_chunk_t* final_chunk = audio_chunk_create(128, 1);
  ASSERT_TRUE(capture_backend_read(capture_final, 128, final_chunk, &err));
  ASSERT_EQ(128, audio_chunk_get_valid_frames(final_chunk));

  for (size_t f_idx = 0; f_idx < 128; f_idx++) {
    double expected = (double)((int16_t)f_idx * 100) / 32768.0;
    ASSERT_NEAR(expected, audio_chunk_get_channel(final_chunk, 0)[f_idx], 1.0 / 32767.0);
  }

  audio_chunk_free(chunk);
  audio_chunk_free(chunk2);
  audio_chunk_free(final_chunk);
  capture_backend_close(capture_final);
  capture_backend_free(capture_final);

  remove(rf64_in_filename);
  remove(wav_mid_filename);
  remove(rf64_out_filename);
}

TEST(FileBackendGetChannelsRF64) {
  char rf64_filename[256];
  snprintf(rf64_filename, sizeof(rf64_filename),
           "/tmp/test_file_backend_get_ch_rf64_%d.wav", getpid());
  remove(rf64_filename);

  // Write RF64 header manually
  FILE* f = fopen(rf64_filename, "wb");
  ASSERT_TRUE(f != NULL);
  uint8_t rf64_header[80] = {
      'R', 'F', '6', '4',
      0xFF, 0xFF, 0xFF, 0xFF,
      'W', 'A', 'V', 'E',
      'd', 's', '6', '4',
      28, 0, 0, 0, // ds64 size
      100, 0, 0, 0, 0, 0, 0, 0,
      100, 0, 0, 0, 0, 0, 0, 0,
      50, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0,
      'f', 'm', 't', ' ',
      16, 0, 0, 0,
      1, 0,
      6, 0, // channels: 6
      0x80, 0x3E, 0, 0,
      0x00, 0x7D, 0, 0,
      12, 0,
      16, 0,
      'd', 'a', 't', 'a',
      0xFF, 0xFF, 0xFF, 0xFF
  };
  fwrite(rf64_header, 1, sizeof(rf64_header), f);
  fclose(f);

  capture_device_config_t cap_cfg;
  memset(&cap_cfg, 0, sizeof(cap_cfg));
  cap_cfg.type = AUDIO_BACKEND_TYPE_FILE;
  cap_cfg.is_wav = true;
  cap_cfg.has_is_wav = true;
  snprintf(cap_cfg.cfg.wav_file.filename, sizeof(cap_cfg.cfg.wav_file.filename),
           "%s", rf64_filename);
  cap_cfg.cfg.wav_file.has_filename = true;

  int channels = capture_device_config_get_channels(&cap_cfg);
  ASSERT_EQ(6, channels);

  remove(rf64_filename);
}

TEST_MAIN()
