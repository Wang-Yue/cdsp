#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>

#include "Audio/audio_chunk.h"
#include "Audio/audio_history_buffer.h"
#include "test_support.h"

TEST(Reset) {
  audio_history_buffer_t* buffer = audio_history_buffer_create();
  ASSERT_TRUE(buffer != NULL);
  ASSERT_EQ(0, audio_history_buffer_get_channels(buffer));

  audio_history_buffer_reset(buffer, 2);
  ASSERT_EQ(2, audio_history_buffer_get_channels(buffer));
  audio_history_buffer_free(buffer);
}

TEST(AppendAndRead) {
  audio_history_buffer_t* buffer = audio_history_buffer_create();
  audio_history_buffer_reset(buffer, 2);

  audio_chunk_t* chunk = audio_chunk_create(1024, 2);
  for (size_t t = 0; t < 1024; t++) {
    audio_chunk_get_channel(chunk, 0)[t] = (double)t;
    audio_chunk_get_channel(chunk, 1)[t] = (double)(t * 2);
  }
  audio_chunk_set_valid_frames(chunk, 1024);

  audio_history_buffer_append(buffer, chunk);

  float* dest = (float*)calloc(1024, sizeof(float));
  bool enough_data = false;

  // Read channel 0
  const size_t ch0 = 0;
  audio_history_buffer_status_t status0 =
      audio_history_buffer_read_latest(buffer, dest, 1024, &ch0, &enough_data);
  ASSERT_EQ(AUDIO_HISTORY_BUFFER_OK, status0);
  ASSERT_TRUE(enough_data);
  ASSERT_FLOAT_EQ(0.0f, dest[0]);
  ASSERT_FLOAT_EQ(1023.0f, dest[1023]);

  // Read channel 1
  const size_t ch1 = 1;
  audio_history_buffer_status_t status1 =
      audio_history_buffer_read_latest(buffer, dest, 1024, &ch1, &enough_data);
  ASSERT_EQ(AUDIO_HISTORY_BUFFER_OK, status1);
  ASSERT_TRUE(enough_data);
  ASSERT_FLOAT_EQ(0.0f, dest[0]);
  ASSERT_FLOAT_EQ(2046.0f, dest[1023]);

  free(dest);
  audio_chunk_free(chunk);
  audio_history_buffer_free(buffer);
}

TEST(ReadLatestAverageChannels) {
  audio_history_buffer_t* buffer = audio_history_buffer_create();
  audio_history_buffer_reset(buffer, 2);

  audio_chunk_t* chunk = audio_chunk_create(1024, 2);
  for (size_t t = 0; t < 1024; t++) {
    audio_chunk_get_channel(chunk, 0)[t] = 1.0;
    audio_chunk_get_channel(chunk, 1)[t] = 3.0;
  }
  audio_chunk_set_valid_frames(chunk, 1024);

  audio_history_buffer_append(buffer, chunk);

  float* dest = (float*)calloc(1024, sizeof(float));
  bool enough_data = false;

  audio_history_buffer_status_t status =
      audio_history_buffer_read_latest(buffer, dest, 1024, NULL, &enough_data);
  ASSERT_EQ(AUDIO_HISTORY_BUFFER_OK, status);
  ASSERT_TRUE(enough_data);
  ASSERT_NEAR(2.0f, dest[0], 1e-5);
  ASSERT_NEAR(2.0f, dest[1023], 1e-5);

  free(dest);
  audio_chunk_free(chunk);
  audio_history_buffer_free(buffer);
}

TEST(ReadLatestEmpty) {
  audio_history_buffer_t* buffer = audio_history_buffer_create();
  float* dest = (float*)calloc(1024, sizeof(float));
  bool enough_data = false;

  audio_history_buffer_status_t status =
      audio_history_buffer_read_latest(buffer, dest, 1024, NULL, &enough_data);
  ASSERT_EQ(AUDIO_HISTORY_BUFFER_ERROR_EMPTY, status);

  free(dest);
  audio_history_buffer_free(buffer);
}

TEST(ReadLatestChannelOutOfRange) {
  audio_history_buffer_t* buffer = audio_history_buffer_create();
  audio_history_buffer_reset(buffer, 2);
  float* dest = (float*)calloc(1024, sizeof(float));
  bool enough_data = false;

  const size_t ch2 = 2;
  audio_history_buffer_status_t status =
      audio_history_buffer_read_latest(buffer, dest, 1024, &ch2, &enough_data);
  ASSERT_EQ(AUDIO_HISTORY_BUFFER_ERROR_OUT_OF_RANGE, status);

  free(dest);
  audio_history_buffer_free(buffer);
}

TEST(AppendMismatchedChannels) {
  audio_history_buffer_t* buffer = audio_history_buffer_create();
  audio_history_buffer_reset(buffer, 2);
  ASSERT_EQ(2, audio_history_buffer_get_channels(buffer));

  audio_chunk_t* chunk = audio_chunk_create(1024, 1);
  audio_chunk_set_valid_frames(chunk, 1024);
  audio_history_buffer_append(buffer, chunk);
  ASSERT_EQ(1, audio_history_buffer_get_channels(buffer));

  audio_chunk_free(chunk);
  audio_history_buffer_free(buffer);
}

TEST(ReadLatestAverageChannelsLargeCount) {
  audio_history_buffer_t* buffer = audio_history_buffer_create();
  audio_history_buffer_reset(buffer, 2);

  audio_chunk_t* chunk = audio_chunk_create(4096, 2);
  for (size_t t = 0; t < 4096; t++) {
    audio_chunk_get_channel(chunk, 0)[t] = 2.0;
    audio_chunk_get_channel(chunk, 1)[t] = 4.0;
  }
  audio_chunk_set_valid_frames(chunk, 4096);

  audio_history_buffer_append(buffer, chunk);

  float* dest = (float*)calloc(4096, sizeof(float));
  bool enough_data = false;

  audio_history_buffer_status_t status =
      audio_history_buffer_read_latest(buffer, dest, 4096, NULL, &enough_data);
  ASSERT_EQ(AUDIO_HISTORY_BUFFER_OK, status);
  ASSERT_TRUE(enough_data);
  ASSERT_NEAR(3.0f, dest[0], 1e-5);
  ASSERT_NEAR(3.0f, dest[2047], 1e-5);
  ASSERT_NEAR(3.0f, dest[2048], 1e-5);
  ASSERT_NEAR(3.0f, dest[4095], 1e-5);

  free(dest);
  audio_chunk_free(chunk);
  audio_history_buffer_free(buffer);
}

typedef struct {
  audio_history_buffer_t* buffer;
  int total_to_write;
  int chunk_size;
  _Atomic bool producer_done;
} history_concurrent_arg_t;

static void* history_producer_thread(void* arg) {
  history_concurrent_arg_t* a = (history_concurrent_arg_t*)arg;
  audio_chunk_t* chunk = audio_chunk_create(a->chunk_size, 2);
  double counter = 0.0;
  int written = 0;
  while (written < a->total_to_write) {
    for (int i = 0; i < a->chunk_size; i++) {
      audio_chunk_get_channel(chunk, 0)[i] = counter;
      audio_chunk_get_channel(chunk, 1)[i] = counter;
      counter += 1.0;
    }
    audio_chunk_set_valid_frames(chunk, a->chunk_size);
    audio_history_buffer_append(a->buffer, chunk);
    written += a->chunk_size;
    sched_yield();
  }
  audio_chunk_free(chunk);
  atomic_store_explicit(&a->producer_done, true, memory_order_release);
  return NULL;
}

TEST(AudioHistoryBuffer_ConcurrentProducerConsumer) {
  audio_history_buffer_t* buffer = audio_history_buffer_create();
  audio_history_buffer_reset(buffer, 2);

  int total_to_write = 100000;
  int chunk_size = 256;
  int read_size = 64;

  history_concurrent_arg_t arg = {buffer, total_to_write, chunk_size, false};
  pthread_t th;
  pthread_create(&th, NULL, history_producer_thread, &arg);

  float* dest = (float*)calloc(read_size, sizeof(float));
  int snapshots_taken = 0;
  const size_t ch0 = 0;

  while (!atomic_load_explicit(&arg.producer_done, memory_order_acquire)) {
    bool enough = false;
    audio_history_buffer_status_t st = audio_history_buffer_read_latest(
        buffer, dest, read_size, &ch0, &enough);
    if (st == AUDIO_HISTORY_BUFFER_OK && enough) {
      for (int i = 1; i < read_size; i++) {
        ASSERT_NEAR(dest[i - 1] + 1.0f, dest[i], 1e-3);
      }
      snapshots_taken++;
    }
    sched_yield();
  }

  pthread_join(th, NULL);

  bool enough = false;
  audio_history_buffer_status_t st =
      audio_history_buffer_read_latest(buffer, dest, read_size, &ch0, &enough);
  ASSERT_EQ(AUDIO_HISTORY_BUFFER_OK, st);
  ASSERT_TRUE(enough);
  for (int i = 1; i < read_size; i++) {
    ASSERT_NEAR(dest[i - 1] + 1.0f, dest[i], 1e-3);
  }

  free(dest);
  audio_history_buffer_free(buffer);
  ASSERT_TRUE(snapshots_taken > 0);
}

TEST(ReadLatestCountExceedsCapacity) {
  audio_history_buffer_t* buffer = audio_history_buffer_create();
  audio_history_buffer_reset(buffer, 2);

  audio_chunk_t* chunk = audio_chunk_create(512, 2);
  audio_chunk_set_valid_frames(chunk, 512);
  audio_history_buffer_append(buffer, chunk);

  float* dest = (float*)calloc(1024, sizeof(float));
  bool enough = true;
  const size_t ch0 = 0;
  audio_history_buffer_status_t status = audio_history_buffer_read_latest(
      buffer, dest, AUDIO_HISTORY_BUFFER_CAPACITY + 1, &ch0, &enough);
  ASSERT_EQ(AUDIO_HISTORY_BUFFER_ERROR_OUT_OF_RANGE, status);
  ASSERT_FALSE(enough);

  free(dest);
  audio_chunk_free(chunk);
  audio_history_buffer_free(buffer);
}

TEST(ReadLatestNotEnoughDataYet) {
  audio_history_buffer_t* buffer = audio_history_buffer_create();
  audio_history_buffer_reset(buffer, 2);

  audio_chunk_t* chunk = audio_chunk_create(256, 2);
  audio_chunk_set_valid_frames(chunk, 256);
  audio_history_buffer_append(buffer, chunk);

  float* dest = (float*)calloc(512, sizeof(float));
  bool enough = true;
  const size_t ch0 = 0;
  audio_history_buffer_status_t status =
      audio_history_buffer_read_latest(buffer, dest, 512, &ch0, &enough);
  ASSERT_EQ(AUDIO_HISTORY_BUFFER_OK, status);
  ASSERT_FALSE(enough);

  free(dest);
  audio_chunk_free(chunk);
  audio_history_buffer_free(buffer);
}

TEST_MAIN()
