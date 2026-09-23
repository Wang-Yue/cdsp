#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <string.h>

#include "audio/audio_chunk.h"
#include "backend/audio_backend.h"
#include "test_support.h"
#include "utils/lock_free_ring_buffer.h"

TEST(CapacityRoundsUpToPowerOfTwo) {
  ASSERT_EQ(1, spsc_round_up_to_power_of_two(1));
  ASSERT_EQ(2, spsc_round_up_to_power_of_two(2));
  ASSERT_EQ(128, spsc_round_up_to_power_of_two(100));
  ASSERT_EQ(1024, spsc_round_up_to_power_of_two(1024));
  ASSERT_EQ(2048, spsc_round_up_to_power_of_two(1025));
}

TEST(SpscByteRingBuffer_CapacityAndAvailable) {
  spsc_byte_ring_buffer_t *ring = spsc_byte_ring_buffer_create(100);
  ASSERT_EQ(128, spsc_byte_ring_buffer_get_capacity(ring));
  ASSERT_EQ(128, spsc_byte_ring_buffer_get_available_to_write(ring));
  ASSERT_EQ(0, spsc_byte_ring_buffer_get_available_to_read(ring));
  spsc_byte_ring_buffer_free(ring);
}

TEST(SpscByteRingBuffer_BasicRoundTrip) {
  spsc_byte_ring_buffer_t *ring = spsc_byte_ring_buffer_create(100);
  uint8_t write_data[64];
  for (int i = 0; i < 64; i++)
    write_data[i] = (uint8_t)(i & 0xFF);

  size_t written = spsc_byte_ring_buffer_write(ring, write_data, 64);
  ASSERT_EQ(64, written);
  ASSERT_EQ(64, spsc_byte_ring_buffer_get_available_to_read(ring));
  ASSERT_EQ(64, spsc_byte_ring_buffer_get_available_to_write(ring));

  uint8_t read_data[64] = {0};
  size_t consumed = spsc_byte_ring_buffer_consume(ring, read_data, 64);
  ASSERT_EQ(64, consumed);
  for (int i = 0; i < 64; i++) {
    ASSERT_EQ((uint8_t)(i & 0xFF), read_data[i]);
  }
  ASSERT_EQ(0, spsc_byte_ring_buffer_get_available_to_read(ring));
  ASSERT_EQ(128, spsc_byte_ring_buffer_get_available_to_write(ring));

  spsc_byte_ring_buffer_free(ring);
}

TEST(SpscByteRingBuffer_WrapAround) {
  spsc_byte_ring_buffer_t *ring = spsc_byte_ring_buffer_create(8);
  ASSERT_EQ(8, spsc_byte_ring_buffer_get_capacity(ring));

  uint8_t first_batch[] = {1, 2, 3, 4, 5, 6};
  size_t w1 = spsc_byte_ring_buffer_write(ring, first_batch, 6);
  ASSERT_EQ(6, w1);

  uint8_t dest[4] = {0};
  size_t r1 = spsc_byte_ring_buffer_consume(ring, dest, 4);
  ASSERT_EQ(4, r1);
  for (int i = 0; i < 4; i++) {
    ASSERT_EQ((uint8_t)(i + 1), dest[i]);
  }

  uint8_t second_batch[] = {7, 8, 9, 10, 11, 12};
  size_t w2 = spsc_byte_ring_buffer_write(ring, second_batch, 6);
  ASSERT_EQ(6, w2);

  uint8_t dest2[8] = {0};
  size_t r2 = spsc_byte_ring_buffer_consume(ring, dest2, 8);
  ASSERT_EQ(8, r2);
  for (int i = 0; i < 8; i++) {
    ASSERT_EQ((uint8_t)(i + 5), dest2[i]);
  }

  spsc_byte_ring_buffer_free(ring);
}

TEST(SpscByteRingBuffer_Drain) {
  spsc_byte_ring_buffer_t *ring = spsc_byte_ring_buffer_create(8);
  ASSERT_TRUE(ring != NULL);
  uint8_t src[] = {1, 2, 3, 4};
  spsc_byte_ring_buffer_write(ring, src, 4);
  ASSERT_EQ(4, spsc_byte_ring_buffer_get_available_to_read(ring));
  spsc_byte_ring_buffer_drain(ring);
  ASSERT_EQ(0, spsc_byte_ring_buffer_get_available_to_read(ring));
  spsc_byte_ring_buffer_free(ring);
}

typedef struct {
  spsc_byte_ring_buffer_t *ring;
  size_t total_bytes;
} byte_thread_ctx_t;

static void *byte_producer_thread(void *arg) {
  byte_thread_ctx_t *ctx = (byte_thread_ctx_t *)arg;
  size_t total = ctx->total_bytes;
  size_t written = 0;
  uint8_t chunk[128];
  while (written < total) {
    size_t to_write = (total - written < 128) ? total - written : 128;
    for (size_t i = 0; i < to_write; i++) {
      chunk[i] = (uint8_t)((written + i) & 0xFF);
    }
    size_t pushed = spsc_byte_ring_buffer_write(ctx->ring, chunk, to_write);
    written += pushed;
    if (pushed == 0)
      sched_yield();
  }
  return NULL;
}

TEST(SpscByteRingBuffer_Concurrent) {
  spsc_byte_ring_buffer_t *ring = spsc_byte_ring_buffer_create(256);
  byte_thread_ctx_t ctx = {.ring = ring, .total_bytes = 100000};
  pthread_t th;
  pthread_create(&th, NULL, byte_producer_thread, &ctx);

  size_t consumed = 0;
  uint8_t chunk[128];
  while (consumed < ctx.total_bytes) {
    size_t to_read =
        (ctx.total_bytes - consumed < 128) ? ctx.total_bytes - consumed : 128;
    size_t n = spsc_byte_ring_buffer_consume(ring, chunk, to_read);
    for (size_t i = 0; i < n; i++) {
      ASSERT_EQ((uint8_t)((consumed + i) & 0xFF), chunk[i]);
    }
    consumed += n;
    if (n == 0)
      sched_yield();
  }

  pthread_join(th, NULL);
  spsc_byte_ring_buffer_free(ring);
}

TEST(SpscQueueRoundTripFifo) {
  spsc_queue_t *queue = spsc_queue_create(8);
  ASSERT_EQ(8, spsc_queue_get_capacity(queue));
  ASSERT_EQ(0, spsc_queue_get_count(queue));
  ASSERT_TRUE(spsc_queue_dequeue(queue) == NULL);
  for (intptr_t i = 1; i <= 5; i++) {
    ASSERT_TRUE(spsc_queue_enqueue(queue, (void *)i));
  }
  ASSERT_EQ(5, spsc_queue_get_count(queue));
  for (intptr_t i = 1; i <= 5; i++) {
    ASSERT_EQ((void *)i, spsc_queue_dequeue(queue));
  }
  ASSERT_EQ(0, spsc_queue_get_count(queue));
  ASSERT_TRUE(spsc_queue_dequeue(queue) == NULL);
  spsc_queue_free(queue);
}

TEST(SpscQueueEnqueueReturnsFalseWhenFull) {
  spsc_queue_t *queue = spsc_queue_create(4);
  for (intptr_t i = 1; i <= 4; i++) {
    ASSERT_TRUE(spsc_queue_enqueue(queue, (void *)i));
  }
  ASSERT_FALSE(spsc_queue_enqueue(queue, (void *)99));
  ASSERT_EQ((void *)1, spsc_queue_dequeue(queue));
  ASSERT_TRUE(spsc_queue_enqueue(queue, (void *)99));
  spsc_queue_free(queue);
}

TEST(SpscQueueWrapsAroundIndices) {
  spsc_queue_t *queue = spsc_queue_create(4);
  for (intptr_t i = 1; i <= 3; i++) {
    ASSERT_TRUE(spsc_queue_enqueue(queue, (void *)i));
  }
  for (intptr_t i = 1; i <= 3; i++) {
    ASSERT_EQ((void *)i, spsc_queue_dequeue(queue));
  }
  for (intptr_t i = 100; i < 104; i++) {
    ASSERT_TRUE(spsc_queue_enqueue(queue, (void *)i));
  }
  for (intptr_t i = 100; i < 104; i++) {
    ASSERT_EQ((void *)i, spsc_queue_dequeue(queue));
  }
  spsc_queue_free(queue);
}

typedef struct {
  int value;
} non_sendable_item_t;

TEST(SpscQueueTransferNonSendable) {
  spsc_queue_t *queue = spsc_queue_create(4);
  non_sendable_item_t item = {42};
  ASSERT_TRUE(spsc_queue_enqueue(queue, &item));
  non_sendable_item_t *popped =
      (non_sendable_item_t *)spsc_queue_dequeue(queue);
  ASSERT_TRUE(popped != NULL);
  ASSERT_EQ(42, popped->value);
  spsc_queue_free(queue);
}

typedef struct {
  spsc_queue_t *queue;
  int total_to_write;
} queue_concurrent_arg_t;

static void *queue_producer_thread(void *arg) {
  queue_concurrent_arg_t *a = (queue_concurrent_arg_t *)arg;
  int i = 0;
  while (i < a->total_to_write) {
    if (spsc_queue_enqueue(a->queue, (void *)(intptr_t)(i + 1))) {
      i++;
    }
  }
  return NULL;
}

TEST(SpscQueueConcurrentNoDataLoss) {
  spsc_queue_t *queue = spsc_queue_create(64);
  int total_to_write = 200000;
  queue_concurrent_arg_t arg = {queue, total_to_write};
  pthread_t th;
  pthread_create(&th, NULL, queue_producer_thread, &arg);

  int last_seen = 0;
  int consumed = 0;
  while (consumed < total_to_write) {
    void *val = spsc_queue_dequeue(queue);
    if (val) {
      int v = (int)(intptr_t)val;
      ASSERT_EQ(last_seen + 1, v);
      last_seen = v;
      consumed++;
    }
  }
  pthread_join(th, NULL);
  spsc_queue_free(queue);
  ASSERT_EQ(total_to_write, consumed);
}

TEST(AtomicDoubleRoundTrip) {
  atomic_double_t value;
  atomic_double_init(&value, 1.5);
  ASSERT_DOUBLE_EQ(1.5, atomic_double_get(&value));
  atomic_double_set(&value, 2.71828);
  ASSERT_DOUBLE_EQ(2.71828, atomic_double_get(&value));
  atomic_double_set(&value, -0.0);
  double d = atomic_double_get(&value);
  uint64_t u1, u2;
  memcpy(&u1, &d, sizeof(uint64_t));
  double minus_zero = -0.0;
  memcpy(&u2, &minus_zero, sizeof(uint64_t));
  ASSERT_EQ(u2, u1);
  atomic_double_set(&value, INFINITY);
  ASSERT_DOUBLE_EQ(INFINITY, atomic_double_get(&value));
}

TEST(AtomicFloatRoundTrip) {
  atomic_float_t value;
  atomic_float_init(&value, 1.5f);
  ASSERT_NEAR(1.5f, atomic_float_get(&value), 1e-6);
  atomic_float_set(&value, 2.71828f);
  ASSERT_NEAR(2.71828f, atomic_float_get(&value), 1e-6);
  atomic_float_set(&value, -0.0f);
  float f = atomic_float_get(&value);
  uint32_t u1, u2;
  memcpy(&u1, &f, sizeof(uint32_t));
  float minus_zero = -0.0f;
  memcpy(&u2, &minus_zero, sizeof(uint32_t));
  ASSERT_EQ(u2, u1);
  atomic_float_set(&value, INFINITY);
  ASSERT_TRUE(isinf(atomic_float_get(&value)));
}

TEST(SpscNullCheck) {
  ASSERT_EQ(0, spsc_queue_get_count(NULL));
  ASSERT_EQ(0, spsc_byte_ring_buffer_get_available_to_read(NULL));
  ASSERT_EQ(0, spsc_byte_ring_buffer_get_available_to_write(NULL));
  ASSERT_EQ(0, spsc_byte_ring_buffer_get_capacity(NULL));
  ASSERT_EQ(0, spsc_planar_ring_buffer_get_available_to_read(NULL));
  ASSERT_EQ(0, spsc_planar_ring_buffer_get_available_to_write(NULL));
  ASSERT_EQ(0, spsc_planar_ring_buffer_get_capacity(NULL));
}

TEST(SpscPlanarRingBuffer_BasicRoundTrip) {
  size_t channels = 2;
  size_t bytes_per_sample = sizeof(float);
  spsc_planar_ring_buffer_t *ring =
      spsc_planar_ring_buffer_create(channels, bytes_per_sample, 64);
  ASSERT_TRUE(ring != NULL);
  ASSERT_EQ(64, spsc_planar_ring_buffer_get_capacity(ring));
  ASSERT_EQ(64, spsc_planar_ring_buffer_get_available_to_write(ring));
  ASSERT_EQ(0, spsc_planar_ring_buffer_get_available_to_read(ring));
  ASSERT_EQ(2, spsc_planar_ring_buffer_get_channels(ring));
  ASSERT_EQ(sizeof(float), spsc_planar_ring_buffer_get_bytes_per_sample(ring));

  float ch0_in[32], ch1_in[32];
  for (int i = 0; i < 32; i++) {
    ch0_in[i] = (float)i * 1.0f;
    ch1_in[i] = (float)i * -2.0f;
  }
  const void *in_ptrs[2] = {ch0_in, ch1_in};

  size_t written = spsc_planar_ring_buffer_write_channels(ring, in_ptrs, 32);
  ASSERT_EQ(32, written);
  ASSERT_EQ(32, spsc_planar_ring_buffer_get_available_to_read(ring));
  ASSERT_EQ(32, spsc_planar_ring_buffer_get_available_to_write(ring));

  float ch0_out[32] = {0}, ch1_out[32] = {0};
  void *out_ptrs[2] = {ch0_out, ch1_out};
  size_t read_count = spsc_planar_ring_buffer_read_channels(ring, out_ptrs, 32);
  ASSERT_EQ(32, read_count);
  for (int i = 0; i < 32; i++) {
    ASSERT_NEAR(ch0_in[i], ch0_out[i], 1e-6);
    ASSERT_NEAR(ch1_in[i], ch1_out[i], 1e-6);
  }
  ASSERT_EQ(0, spsc_planar_ring_buffer_get_available_to_read(ring));
  ASSERT_EQ(64, spsc_planar_ring_buffer_get_available_to_write(ring));

  spsc_planar_ring_buffer_free(ring);
}

TEST(SpscPlanarRingBuffer_WrapAroundSlices) {
  size_t channels = 2;
  size_t bytes_per_sample = sizeof(float);
  spsc_planar_ring_buffer_t *ring =
      spsc_planar_ring_buffer_create(channels, bytes_per_sample, 8);
  ASSERT_TRUE(ring != NULL);
  ASSERT_EQ(8, spsc_planar_ring_buffer_get_capacity(ring));

  float ch0_in[6] = {1, 2, 3, 4, 5, 6};
  float ch1_in[6] = {10, 20, 30, 40, 50, 60};
  const void *in_ptrs[2] = {ch0_in, ch1_in};
  size_t w1 = spsc_planar_ring_buffer_write_channels(ring, in_ptrs, 6);
  ASSERT_EQ(6, w1);

  float ch0_out[4] = {0}, ch1_out[4] = {0};
  void *out_ptrs[2] = {ch0_out, ch1_out};
  size_t r1 = spsc_planar_ring_buffer_read_channels(ring, out_ptrs, 4);
  ASSERT_EQ(4, r1);
  ASSERT_NEAR(1.0f, ch0_out[0], 1e-6);
  ASSERT_NEAR(4.0f, ch0_out[3], 1e-6);
  ASSERT_NEAR(10.0f, ch1_out[0], 1e-6);
  ASSERT_NEAR(40.0f, ch1_out[3], 1e-6);

  // Now ring read_index is 4, write_index is 6. Available to write is 6.
  // Writing 6 frames will wrap around the end of the buffer (capacity 8).
  float ch0_in2[6] = {7, 8, 9, 10, 11, 12};
  float ch1_in2[6] = {70, 80, 90, 100, 110, 120};
  const void *in_ptrs2[2] = {ch0_in2, ch1_in2};
  size_t w2 = spsc_planar_ring_buffer_write_channels(ring, in_ptrs2, 6);
  ASSERT_EQ(6, w2);
  ASSERT_EQ(8, spsc_planar_ring_buffer_get_available_to_read(ring));

  // Read all 8 frames across wrap-around boundary
  float ch0_all[8] = {0}, ch1_all[8] = {0};
  void *all_ptrs[2] = {ch0_all, ch1_all};
  size_t r2 = spsc_planar_ring_buffer_read_channels(ring, all_ptrs, 8);
  ASSERT_EQ(8, r2);

  // First 2 remaining from batch 1 (5, 6 / 50, 60), then 6 from batch 2 (7..12
  // / 70..120)
  ASSERT_NEAR(5.0f, ch0_all[0], 1e-6);
  ASSERT_NEAR(6.0f, ch0_all[1], 1e-6);
  ASSERT_NEAR(7.0f, ch0_all[2], 1e-6);
  ASSERT_NEAR(12.0f, ch0_all[7], 1e-6);

  ASSERT_NEAR(50.0f, ch1_all[0], 1e-6);
  ASSERT_NEAR(60.0f, ch1_all[1], 1e-6);
  ASSERT_NEAR(70.0f, ch1_all[2], 1e-6);
  ASSERT_NEAR(120.0f, ch1_all[7], 1e-6);

  spsc_planar_ring_buffer_drain(ring);
  ASSERT_EQ(0, spsc_planar_ring_buffer_get_available_to_read(ring));

  spsc_planar_ring_buffer_free(ring);
}

TEST(AudioChunk_PlanarEncodeDecode) {
  size_t channels = 2;
  size_t frames = 16;
  audio_chunk_t *chunk = audio_chunk_create(frames, channels);
  ASSERT_TRUE(chunk != NULL);

  double *ch0 = audio_chunk_get_channel(chunk, 0);
  double *ch1 = audio_chunk_get_channel(chunk, 1);
  for (size_t f = 0; f < frames; f++) {
    ch0[f] = 0.25 * (double)f;
    ch1[f] = -0.1 * (double)f;
  }
  audio_chunk_set_valid_frames(chunk, frames);

  // Encode to float32 planar channel arrays
  float enc0[16] = {0}, enc1[16] = {0};
  void *dst_ptrs[2] = {enc0, enc1};
  ASSERT_TRUE(audio_chunk_encode_planar(chunk, BINARY_SAMPLE_FORMAT_F32_LE,
                                        channels, frames, dst_ptrs));

  for (size_t f = 0; f < frames; f++) {
    ASSERT_NEAR((float)(0.25 * (double)f), enc0[f], 1e-6f);
    ASSERT_NEAR((float)(-0.1 * (double)f), enc1[f], 1e-6f);
  }

  // Decode from float32 planar channel arrays into fresh chunk
  audio_chunk_t *chunk_out = audio_chunk_create(frames, channels);
  ASSERT_TRUE(chunk_out != NULL);
  const void *src_ptrs[2] = {enc0, enc1};
  ASSERT_TRUE(audio_chunk_decode_planar(src_ptrs, BINARY_SAMPLE_FORMAT_F32_LE,
                                        channels, frames, chunk_out));

  double *out0 = audio_chunk_get_channel(chunk_out, 0);
  double *out1 = audio_chunk_get_channel(chunk_out, 1);
  for (size_t f = 0; f < frames; f++) {
    ASSERT_NEAR(ch0[f], out0[f], 1e-6);
    ASSERT_NEAR(ch1[f], out1[f], 1e-6);
  }

  audio_chunk_free(chunk);
  audio_chunk_free(chunk_out);
}

TEST(AudioBackend_PlanarRingBuffer_ReadWrite) {
  size_t channels = 2;
  size_t frames = 32;
  spsc_planar_ring_buffer_t *ring =
      spsc_planar_ring_buffer_create(channels, sizeof(float), 64);
  ASSERT_TRUE(ring != NULL);

  audio_chunk_t *chunk_in = audio_chunk_create(frames, channels);
  ASSERT_TRUE(chunk_in != NULL);
  double *in0 = audio_chunk_get_channel(chunk_in, 0);
  double *in1 = audio_chunk_get_channel(chunk_in, 1);
  for (size_t f = 0; f < frames; f++) {
    in0[f] = 0.5 * sin((double)f);
    in1[f] = 0.5 * cos((double)f);
  }
  audio_chunk_set_valid_frames(chunk_in, frames);

  backend_error_t err = {0};
  ASSERT_TRUE(audio_backend_planar_ring_buffer_write(
      ring, chunk_in, BINARY_SAMPLE_FORMAT_F32_LE, channels, 1, 4, NULL, NULL,
      NULL, NULL, &err));
  ASSERT_EQ(32, spsc_planar_ring_buffer_get_available_to_read(ring));

  audio_chunk_t *chunk_out = audio_chunk_create(frames, channels);
  ASSERT_TRUE(chunk_out != NULL);
  ASSERT_TRUE(audio_backend_planar_ring_buffer_read(
      ring, frames, BINARY_SAMPLE_FORMAT_F32_LE, channels, NULL, NULL, NULL,
      chunk_out, &err));
  ASSERT_EQ(frames, audio_chunk_get_valid_frames(chunk_out));

  double *out0 = audio_chunk_get_channel(chunk_out, 0);
  double *out1 = audio_chunk_get_channel(chunk_out, 1);
  for (size_t f = 0; f < frames; f++) {
    ASSERT_NEAR(in0[f], out0[f], 1e-5);
    ASSERT_NEAR(in1[f], out1[f], 1e-5);
  }

  audio_chunk_free(chunk_in);
  audio_chunk_free(chunk_out);
  spsc_planar_ring_buffer_free(ring);
}

TEST(SpscByteRingBuffer_WriteSilence) {
  spsc_byte_ring_buffer_t *ring = spsc_byte_ring_buffer_create(64);
  ASSERT_TRUE(ring != NULL);
  ASSERT_EQ(64, spsc_byte_ring_buffer_get_capacity(ring));

  size_t written = spsc_byte_ring_buffer_write_silence(ring, 32, 0x69);
  ASSERT_EQ(32, written);
  ASSERT_EQ(32, spsc_byte_ring_buffer_get_available_to_read(ring));

  uint8_t out[32] = {0};
  size_t consumed = spsc_byte_ring_buffer_consume(ring, out, 32);
  ASSERT_EQ(32, consumed);
  for (int i = 0; i < 32; i++) {
    ASSERT_EQ(0x69, out[i]);
  }

  spsc_byte_ring_buffer_free(ring);
}

TEST(SpscPlanarRingBuffer_WriteSilence) {
  size_t channels = 2;
  size_t bytes_per_sample = sizeof(float);
  spsc_planar_ring_buffer_t *ring =
      spsc_planar_ring_buffer_create(channels, bytes_per_sample, 64);
  ASSERT_TRUE(ring != NULL);

  size_t written = spsc_planar_ring_buffer_write_silence(ring, 32);
  ASSERT_EQ(32, written);
  ASSERT_EQ(32, spsc_planar_ring_buffer_get_available_to_read(ring));

  float ch0_out[32], ch1_out[32];
  memset(ch0_out, 0xFF, sizeof(ch0_out));
  memset(ch1_out, 0xFF, sizeof(ch1_out));
  void *out_ptrs[2] = {ch0_out, ch1_out};

  size_t read_count = spsc_planar_ring_buffer_read_channels(ring, out_ptrs, 32);
  ASSERT_EQ(32, read_count);
  for (int i = 0; i < 32; i++) {
    ASSERT_EQ(0.0f, ch0_out[i]);
    ASSERT_EQ(0.0f, ch1_out[i]);
  }

  spsc_planar_ring_buffer_free(ring);
}

TEST(SpscByteRingBuffer_ReadWithSilence) {
  spsc_byte_ring_buffer_t *ring = spsc_byte_ring_buffer_create(128);
  ASSERT_TRUE(ring != NULL);

  // Write 10 frames of 2-channel 16-bit PCM (4 bytes per frame) = 40 bytes
  uint8_t audio[40];
  for (size_t i = 0; i < sizeof(audio); i++) {
    audio[i] = (uint8_t)(i + 1);
  }
  ASSERT_EQ(40, spsc_byte_ring_buffer_write(ring, audio, 40));

  // We request 20 frames total:
  // - 5 frames of silence prefix
  // - 10 frames of audio from ring
  // - 5 frames of underrun tail silence
  _Atomic size_t silence_prefix = 5;
  _Atomic bool running = true;
  uint8_t dst[20 * 4];
  memset(dst, 0xEE, sizeof(dst));

  size_t consumed = spsc_byte_ring_buffer_read_with_silence(
      ring, dst, 20, 4, 0x00, &silence_prefix, &running);

  ASSERT_EQ(10, consumed);
  ASSERT_EQ(0, silence_prefix);
  ASSERT_FALSE(running); // Flag set to false on underrun

  // Verify prefix silence (first 5 frames = 20 bytes)
  for (size_t i = 0; i < 20; i++) {
    ASSERT_EQ(0x00, dst[i]);
  }
  // Verify audio frames (next 10 frames = 40 bytes)
  for (size_t i = 0; i < 40; i++) {
    ASSERT_EQ((uint8_t)(i + 1), dst[20 + i]);
  }
  // Verify tail silence (remaining 5 frames = 20 bytes)
  for (size_t i = 0; i < 20; i++) {
    ASSERT_EQ(0x00, dst[60 + i]);
  }

  spsc_byte_ring_buffer_free(ring);
}

TEST_MAIN()
