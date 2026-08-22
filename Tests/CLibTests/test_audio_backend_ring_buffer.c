#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "Audio/audio_chunk.h"
#include "Backend/audio_backend.h"
#include "Backend/backend_error.h"
#include "Utils/lock_free_ring_buffer.h"
#include "test_support.h"
#include "Config/engine_config_types.h"

TEST(AudioBackendRingBufferRead_BasicRoundTrip) {
  size_t channels = 2;
  size_t frames = 64;
  size_t blockalign = channels * sizeof(float);
  size_t scratch_cap = 1024 * blockalign;
  uint8_t* scratch_buf = (uint8_t*)malloc(scratch_cap);
  spsc_byte_ring_buffer_t* ring = spsc_byte_ring_buffer_create(scratch_cap);

  audio_chunk_t* write_chunk = audio_chunk_create(frames, channels);
  audio_chunk_t* read_chunk = audio_chunk_create(frames, channels);

  // Fill write chunk with test data
  double* w_ch0 = audio_chunk_get_channel(write_chunk, 0);
  double* w_ch1 = audio_chunk_get_channel(write_chunk, 1);
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

  bool write_ok = audio_backend_ring_buffer_write(
      ring, scratch_buf, scratch_cap, blockalign, write_chunk,
      BINARY_SAMPLE_FORMAT_F32_LE, channels, 1, 10, &running, &stopped, &paused,
      &rate_changed, &err);
  ASSERT_TRUE(write_ok);

  bool read_ok = audio_backend_ring_buffer_read(
      ring, scratch_buf, scratch_cap, blockalign, frames,
      BINARY_SAMPLE_FORMAT_F32_LE, channels, &running, &stopped, &rate_changed,
      read_chunk, &err);
  ASSERT_TRUE(read_ok);
  ASSERT_EQ(audio_chunk_get_valid_frames(read_chunk), frames);

  const double* r_ch0 = audio_chunk_get_channel(read_chunk, 0);
  const double* r_ch1 = audio_chunk_get_channel(read_chunk, 1);
  for (size_t i = 0; i < frames; i++) {
    ASSERT_NEAR(r_ch0[i], 0.5, 1e-5);
    ASSERT_NEAR(r_ch1[i], -0.5, 1e-5);
  }

  audio_chunk_free(write_chunk);
  audio_chunk_free(read_chunk);
  spsc_byte_ring_buffer_free(ring);
  free(scratch_buf);
}

TEST(AudioBackendRingBufferRead_ZeroFrames) {
  size_t channels = 2;
  size_t blockalign = channels * sizeof(float);
  size_t scratch_cap = 256;
  uint8_t scratch_buf[256];
  spsc_byte_ring_buffer_t* ring = spsc_byte_ring_buffer_create(scratch_cap);
  audio_chunk_t* chunk = audio_chunk_create(64, channels);

  backend_error_t err;
  backend_error_init(&err, BACKEND_ERROR_NONE, "");

  bool ok = audio_backend_ring_buffer_read(
      ring, scratch_buf, sizeof(scratch_buf), blockalign, 0,
      BINARY_SAMPLE_FORMAT_F32_LE, channels, NULL, NULL, NULL, chunk, &err);
  ASSERT_TRUE(ok);
  ASSERT_EQ(audio_chunk_get_valid_frames(chunk), 0);

  audio_chunk_free(chunk);
  spsc_byte_ring_buffer_free(ring);
}

TEST(AudioBackendRingBufferRead_InsufficientChunkCapacity) {
  size_t channels = 2;
  size_t blockalign = channels * sizeof(float);
  size_t scratch_cap = 1024;
  uint8_t scratch_buf[1024];
  spsc_byte_ring_buffer_t* ring = spsc_byte_ring_buffer_create(scratch_cap);
  audio_chunk_t* chunk = audio_chunk_create(32, channels);

  backend_error_t err;
  backend_error_init(&err, BACKEND_ERROR_NONE, "");

  // Requesting 64 frames on a chunk of capacity 32 must fail safely
  bool ok = audio_backend_ring_buffer_read(
      ring, scratch_buf, sizeof(scratch_buf), blockalign, 64,
      BINARY_SAMPLE_FORMAT_F32_LE, channels, NULL, NULL, NULL, chunk, &err);
  ASSERT_FALSE(ok);
  ASSERT_EQ(err.type, BACKEND_ERROR_READ_ERROR);

  audio_chunk_free(chunk);
  spsc_byte_ring_buffer_free(ring);
}

TEST(AudioBackendRingBufferRead_DrainOnStreamStopped) {
  size_t channels = 2;
  size_t frames = 32;
  size_t blockalign = channels * sizeof(float);
  size_t scratch_cap = 1024;
  uint8_t scratch_buf[1024];
  spsc_byte_ring_buffer_t* ring = spsc_byte_ring_buffer_create(scratch_cap);
  audio_chunk_t* chunk = audio_chunk_create(frames, channels);

  // Push 32 frames of dummy data
  float raw_data[64] = {0};
  spsc_byte_ring_buffer_write(ring, (const uint8_t*)raw_data,
                              frames * blockalign);

  _Atomic bool stopped = true;
  backend_error_t err;
  backend_error_init(&err, BACKEND_ERROR_NONE, "");

  // First read should succeed because buffered data is present
  bool ok1 = audio_backend_ring_buffer_read(
      ring, scratch_buf, sizeof(scratch_buf), blockalign, frames,
      BINARY_SAMPLE_FORMAT_F32_LE, channels, NULL, &stopped, NULL, chunk, &err);
  ASSERT_TRUE(ok1);
  ASSERT_EQ(audio_chunk_get_valid_frames(chunk), frames);

  // Second read should fail with stream stopped error because ring buffer is
  // depleted
  bool ok2 = audio_backend_ring_buffer_read(
      ring, scratch_buf, sizeof(scratch_buf), blockalign, frames,
      BINARY_SAMPLE_FORMAT_F32_LE, channels, NULL, &stopped, NULL, chunk, &err);
  ASSERT_FALSE(ok2);
  ASSERT_EQ(err.type, BACKEND_ERROR_READ_ERROR);

  audio_chunk_free(chunk);
  spsc_byte_ring_buffer_free(ring);
}

TEST(AudioBackendRingBufferRead_PendingRateChange) {
  size_t channels = 2;
  size_t blockalign = channels * sizeof(float);
  size_t scratch_cap = 1024;
  uint8_t scratch_buf[1024];
  spsc_byte_ring_buffer_t* ring = spsc_byte_ring_buffer_create(scratch_cap);
  audio_chunk_t* chunk = audio_chunk_create(32, channels);

  _Atomic bool rate_change = true;
  backend_error_t err;
  backend_error_init(&err, BACKEND_ERROR_NONE, "");

  bool ok = audio_backend_ring_buffer_read(
      ring, scratch_buf, sizeof(scratch_buf), blockalign, 32,
      BINARY_SAMPLE_FORMAT_F32_LE, channels, NULL, NULL, &rate_change, chunk,
      &err);
  ASSERT_FALSE(ok);
  ASSERT_EQ(err.type, BACKEND_ERROR_NONE);

  audio_chunk_free(chunk);
  spsc_byte_ring_buffer_free(ring);
}

TEST_MAIN()
