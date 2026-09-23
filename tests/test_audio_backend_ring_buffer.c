#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "audio/audio_chunk.h"
#include "backend/backend_buffer.h"
#include "backend/backend_error.h"
#include "test_support.h"

TEST(AudioBackendRingBufferRead_BasicRoundTrip) {
  size_t channels = 2;
  size_t frames = 64;
  backend_buffer_t *buf = backend_buffer_create(
      1024, BINARY_SAMPLE_FORMAT_F32_LE, channels, 48000.0, false);
  ASSERT_TRUE(buf != NULL);

  audio_chunk_t *write_chunk = audio_chunk_create(frames, channels);
  audio_chunk_t *read_chunk = audio_chunk_create(frames, channels);

  // Fill write chunk with test data
  double *w_ch0 = audio_chunk_get_channel(write_chunk, 0);
  double *w_ch1 = audio_chunk_get_channel(write_chunk, 1);
  for (size_t i = 0; i < frames; i++) {
    w_ch0[i] = 0.5;
    w_ch1[i] = -0.5;
  }
  audio_chunk_set_valid_frames(write_chunk, frames);

  backend_error_t err;
  backend_error_init(&err, BACKEND_ERROR_NONE, "");

  _Atomic bool running = true;
  _Atomic bool stopped = false;
  _Atomic bool paused = false;
  _Atomic bool rate_changed = false;
  backend_buffer_set_control_flags(buf, &running, &stopped, &paused,
                                   &rate_changed);

  bool write_ok = backend_buffer_write_chunk(buf, write_chunk, 1, 10, &err);
  ASSERT_TRUE(write_ok);

  bool read_ok = backend_buffer_read_chunk(buf, frames, read_chunk, &err);
  ASSERT_TRUE(read_ok);
  ASSERT_EQ(audio_chunk_get_valid_frames(read_chunk), frames);

  const double *r_ch0 = audio_chunk_get_channel(read_chunk, 0);
  const double *r_ch1 = audio_chunk_get_channel(read_chunk, 1);
  for (size_t i = 0; i < frames; i++) {
    ASSERT_NEAR(r_ch0[i], 0.5, 1e-5);
    ASSERT_NEAR(r_ch1[i], -0.5, 1e-5);
  }

  audio_chunk_free(write_chunk);
  audio_chunk_free(read_chunk);
  backend_buffer_free(buf);
}

TEST(AudioBackendRingBufferRead_ZeroFrames) {
  size_t channels = 2;
  backend_buffer_t *buf = backend_buffer_create(
      256, BINARY_SAMPLE_FORMAT_F32_LE, channels, 48000.0, false);
  ASSERT_TRUE(buf != NULL);
  audio_chunk_t *chunk = audio_chunk_create(64, channels);

  backend_error_t err;
  backend_error_init(&err, BACKEND_ERROR_NONE, "");

  bool ok = backend_buffer_read_chunk(buf, 0, chunk, &err);
  ASSERT_TRUE(ok);
  ASSERT_EQ(audio_chunk_get_valid_frames(chunk), 0);

  audio_chunk_free(chunk);
  backend_buffer_free(buf);
}

TEST(AudioBackendRingBufferRead_InsufficientChunkCapacity) {
  size_t channels = 2;
  backend_buffer_t *buf = backend_buffer_create(
      1024, BINARY_SAMPLE_FORMAT_F32_LE, channels, 48000.0, false);
  ASSERT_TRUE(buf != NULL);
  audio_chunk_t *chunk = audio_chunk_create(32, channels);

  backend_error_t err;
  backend_error_init(&err, BACKEND_ERROR_NONE, "");

  // Requesting 64 frames on a chunk of capacity 32 must fail safely
  bool ok = backend_buffer_read_chunk(buf, 64, chunk, &err);
  ASSERT_FALSE(ok);
  ASSERT_EQ(err.type, BACKEND_ERROR_READ_ERROR);

  audio_chunk_free(chunk);
  backend_buffer_free(buf);
}

TEST(AudioBackendRingBufferRead_DrainOnStreamStopped) {
  size_t channels = 2;
  size_t frames = 32;
  backend_buffer_t *buf = backend_buffer_create(
      1024, BINARY_SAMPLE_FORMAT_F32_LE, channels, 48000.0, false);
  ASSERT_TRUE(buf != NULL);
  audio_chunk_t *chunk = audio_chunk_create(frames, channels);

  // Push 32 frames of dummy data
  float raw_data[64] = {0};
  backend_buffer_push(buf, raw_data, frames);

  _Atomic bool stopped = true;
  backend_buffer_set_control_flags(buf, NULL, &stopped, NULL, NULL);

  backend_error_t err;
  backend_error_init(&err, BACKEND_ERROR_NONE, "");

  // First read should succeed because buffered data is present
  bool ok1 = backend_buffer_read_chunk(buf, frames, chunk, &err);
  ASSERT_TRUE(ok1);
  ASSERT_EQ(audio_chunk_get_valid_frames(chunk), frames);

  // Second read should fail with stream stopped error because ring buffer is
  // depleted
  bool ok2 = backend_buffer_read_chunk(buf, frames, chunk, &err);
  ASSERT_FALSE(ok2);
  ASSERT_EQ(err.type, BACKEND_ERROR_READ_ERROR);

  audio_chunk_free(chunk);
  backend_buffer_free(buf);
}

TEST(AudioBackendRingBufferRead_PendingRateChange) {
  size_t channels = 2;
  backend_buffer_t *buf = backend_buffer_create(
      1024, BINARY_SAMPLE_FORMAT_F32_LE, channels, 48000.0, false);
  ASSERT_TRUE(buf != NULL);
  audio_chunk_t *chunk = audio_chunk_create(32, channels);

  _Atomic bool rate_change = true;
  backend_buffer_set_control_flags(buf, NULL, NULL, NULL, &rate_change);

  backend_error_t err;
  backend_error_init(&err, BACKEND_ERROR_NONE, "");

  bool ok = backend_buffer_read_chunk(buf, 32, chunk, &err);
  ASSERT_FALSE(ok);
  ASSERT_EQ(err.type, BACKEND_ERROR_NONE);

  audio_chunk_free(chunk);
  backend_buffer_free(buf);
}

TEST(AudioBackendRingBufferRead_ThreadRunningFalse_RaisesReadError) {
  size_t channels = 2;
  size_t frames = 32;
  backend_buffer_t *buf = backend_buffer_create(
      1024, BINARY_SAMPLE_FORMAT_F32_LE, channels, 48000.0, false);
  ASSERT_TRUE(buf != NULL);
  audio_chunk_t *chunk = audio_chunk_create(frames, channels);

  _Atomic bool thread_running = false;
  backend_buffer_set_control_flags(buf, &thread_running, NULL, NULL, NULL);

  backend_error_t err;
  backend_error_init(&err, BACKEND_ERROR_NONE, "");

  // Read should fail immediately with BACKEND_ERROR_READ_ERROR because
  // thread_running is false
  bool ok = backend_buffer_read_chunk(buf, frames, chunk, &err);
  ASSERT_FALSE(ok);
  ASSERT_EQ(err.type, BACKEND_ERROR_READ_ERROR);

  audio_chunk_free(chunk);
  backend_buffer_free(buf);
}

TEST(AudioBackendRingBufferWrite_ThreadRunningFalse_RaisesWriteError) {
  size_t channels = 2;
  size_t frames = 32;
  backend_buffer_t *buf = backend_buffer_create(
      1024, BINARY_SAMPLE_FORMAT_F32_LE, channels, 48000.0, false);
  ASSERT_TRUE(buf != NULL);
  audio_chunk_t *chunk = audio_chunk_create(frames, channels);
  audio_chunk_set_valid_frames(chunk, frames);

  _Atomic bool thread_running = false;
  backend_buffer_set_control_flags(buf, &thread_running, NULL, NULL, NULL);

  backend_error_t err;
  backend_error_init(&err, BACKEND_ERROR_NONE, "");

  // Write should fail immediately with BACKEND_ERROR_WRITE_ERROR because
  // thread_running is false
  bool ok = backend_buffer_write_chunk(buf, chunk, 1, 10, &err);
  ASSERT_FALSE(ok);
  ASSERT_EQ(err.type, BACKEND_ERROR_WRITE_ERROR);

  audio_chunk_free(chunk);
  backend_buffer_free(buf);
}

TEST(AudioBackendRingBuffer_WrapAroundRoundTrip) {
  size_t channels = 2;
  size_t frames = 64;
  // Small capacity power of 2: 128 frames capacity
  backend_buffer_t *buf = backend_buffer_create(
      128, BINARY_SAMPLE_FORMAT_F32_LE, channels, 48000.0, false);
  ASSERT_TRUE(buf != NULL);

  audio_chunk_t *write_chunk = audio_chunk_create(frames, channels);
  audio_chunk_t *read_chunk = audio_chunk_create(frames, channels);

  backend_error_t err;
  backend_error_init(&err, BACKEND_ERROR_NONE, "");

  // Write and read 5 rounds to force multiple wrap-arounds across ring storage
  // boundaries
  for (size_t round = 0; round < 5; round++) {
    double *w0 = audio_chunk_get_channel(write_chunk, 0);
    double *w1 = audio_chunk_get_channel(write_chunk, 1);
    for (size_t f = 0; f < frames; f++) {
      w0[f] = (double)(round * 100 + f);
      w1[f] = -(double)(round * 100 + f);
    }
    audio_chunk_set_valid_frames(write_chunk, frames);

    bool w_ok = backend_buffer_write_chunk(buf, write_chunk, 1, 10, &err);
    ASSERT_TRUE(w_ok);

    bool r_ok = backend_buffer_read_chunk(buf, frames, read_chunk, &err);
    ASSERT_TRUE(r_ok);
    ASSERT_EQ(audio_chunk_get_valid_frames(read_chunk), frames);

    const double *r0 = audio_chunk_get_channel(read_chunk, 0);
    const double *r1 = audio_chunk_get_channel(read_chunk, 1);
    for (size_t f = 0; f < frames; f++) {
      ASSERT_NEAR(r0[f], (double)(round * 100 + f), 1e-5);
      ASSERT_NEAR(r1[f], -(double)(round * 100 + f), 1e-5);
    }
  }

  audio_chunk_free(write_chunk);
  audio_chunk_free(read_chunk);
  backend_buffer_free(buf);
}

TEST_MAIN()
