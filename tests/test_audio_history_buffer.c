#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>

#include "audio/audio_chunk.h"
#include "audio/audio_history_buffer.h"
#include "audio/processing_parameters.h"
#include "test_support.h"
#include "utils/cdsp_time.h"
#include "utils/float_helpers.h"

TEST(Reset) {
  audio_history_buffer_t *buffer = audio_history_buffer_create();
  ASSERT_TRUE(buffer != NULL);
  ASSERT_EQ(0, audio_history_buffer_get_channels(buffer));

  audio_history_buffer_reset(buffer, 2);
  ASSERT_EQ(2, audio_history_buffer_get_channels(buffer));
  audio_history_buffer_free(buffer);
}

TEST(AppendAndRead) {
  audio_history_buffer_t *buffer = audio_history_buffer_create();
  audio_history_buffer_reset(buffer, 2);

  audio_chunk_t *chunk = audio_chunk_create(1024, 2);
  for (size_t t = 0; t < 1024; t++) {
    audio_chunk_get_channel(chunk, 0)[t] = (double)t;
    audio_chunk_get_channel(chunk, 1)[t] = (double)(t * 2);
  }
  audio_chunk_set_valid_frames(chunk, 1024);

  audio_history_buffer_append(buffer, chunk);

  float *dest = (float *)calloc(1024, sizeof(float));
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
  audio_history_buffer_t *buffer = audio_history_buffer_create();
  audio_history_buffer_reset(buffer, 2);

  audio_chunk_t *chunk = audio_chunk_create(1024, 2);
  for (size_t t = 0; t < 1024; t++) {
    audio_chunk_get_channel(chunk, 0)[t] = 1.0;
    audio_chunk_get_channel(chunk, 1)[t] = 3.0;
  }
  audio_chunk_set_valid_frames(chunk, 1024);

  audio_history_buffer_append(buffer, chunk);

  float *dest = (float *)calloc(1024, sizeof(float));
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
  audio_history_buffer_t *buffer = audio_history_buffer_create();
  float *dest = (float *)calloc(1024, sizeof(float));
  bool enough_data = false;

  audio_history_buffer_status_t status =
      audio_history_buffer_read_latest(buffer, dest, 1024, NULL, &enough_data);
  ASSERT_EQ(AUDIO_HISTORY_BUFFER_ERROR_EMPTY, status);

  free(dest);
  audio_history_buffer_free(buffer);
}

TEST(ReadLatestChannelOutOfRange) {
  audio_history_buffer_t *buffer = audio_history_buffer_create();
  audio_history_buffer_reset(buffer, 2);
  float *dest = (float *)calloc(1024, sizeof(float));
  bool enough_data = false;

  const size_t ch2 = 2;
  audio_history_buffer_status_t status =
      audio_history_buffer_read_latest(buffer, dest, 1024, &ch2, &enough_data);
  ASSERT_EQ(AUDIO_HISTORY_BUFFER_ERROR_OUT_OF_RANGE, status);

  free(dest);
  audio_history_buffer_free(buffer);
}

TEST(AppendMismatchedChannels) {
  audio_history_buffer_t *buffer = audio_history_buffer_create();
  audio_history_buffer_reset(buffer, 2);
  ASSERT_EQ(2, audio_history_buffer_get_channels(buffer));

  audio_chunk_t *chunk = audio_chunk_create(1024, 1);
  audio_chunk_set_valid_frames(chunk, 1024);
  // Report 08 §1.4: append runs on the audio thread and must not reallocate;
  // a mismatched chunk is dropped and the layout is kept.
  audio_history_buffer_append(buffer, chunk);
  ASSERT_EQ(2, audio_history_buffer_get_channels(buffer));
  float dest[16];
  bool enough = true;
  const size_t ch0 = 0;
  ASSERT_EQ(AUDIO_HISTORY_BUFFER_OK,
            audio_history_buffer_read_latest(buffer, dest, 16, &ch0, &enough));
  ASSERT_FALSE(enough);

  audio_chunk_free(chunk);
  audio_history_buffer_free(buffer);
}

TEST(ReadLatestAverageChannelsLargeCount) {
  audio_history_buffer_t *buffer = audio_history_buffer_create();
  audio_history_buffer_reset(buffer, 2);

  audio_chunk_t *chunk = audio_chunk_create(4096, 2);
  for (size_t t = 0; t < 4096; t++) {
    audio_chunk_get_channel(chunk, 0)[t] = 2.0;
    audio_chunk_get_channel(chunk, 1)[t] = 4.0;
  }
  audio_chunk_set_valid_frames(chunk, 4096);

  audio_history_buffer_append(buffer, chunk);

  float *dest = (float *)calloc(4096, sizeof(float));
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
  audio_history_buffer_t *buffer;
  int total_to_write;
  int chunk_size;
  _Atomic bool producer_done;
} history_concurrent_arg_t;

static void *history_producer_thread(void *arg) {
  history_concurrent_arg_t *a = (history_concurrent_arg_t *)arg;
  audio_chunk_t *chunk = audio_chunk_create(a->chunk_size, 2);
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
    if ((written % (a->chunk_size * 8)) == 0) {
      cdsp_sleep_ms(1);
    } else {
      sched_yield();
    }
  }
  audio_chunk_free(chunk);
  atomic_store_explicit(&a->producer_done, true, memory_order_release);
  return NULL;
}

TEST(AudioHistoryBuffer_ConcurrentProducerConsumer) {
  audio_history_buffer_t *buffer = audio_history_buffer_create();
  audio_history_buffer_reset(buffer, 2);

  int total_to_write = 100000;
  int chunk_size = 256;
  int read_size = 64;

  history_concurrent_arg_t arg = {buffer, total_to_write, chunk_size, false};
  pthread_t th;
  pthread_create(&th, NULL, history_producer_thread, &arg);

  float *dest = (float *)calloc(read_size, sizeof(float));
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
  audio_history_buffer_t *buffer = audio_history_buffer_create();
  audio_history_buffer_reset(buffer, 2);

  audio_chunk_t *chunk = audio_chunk_create(512, 2);
  audio_chunk_set_valid_frames(chunk, 512);
  audio_history_buffer_append(buffer, chunk);

  float *dest = (float *)calloc(1024, sizeof(float));
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
  audio_history_buffer_t *buffer = audio_history_buffer_create();
  audio_history_buffer_reset(buffer, 2);

  audio_chunk_t *chunk = audio_chunk_create(256, 2);
  audio_chunk_set_valid_frames(chunk, 256);
  audio_history_buffer_append(buffer, chunk);

  float *dest = (float *)calloc(512, sizeof(float));
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

TEST(HistoryBufferEnabledLifecycle) {
  audio_history_buffer_t *buffer = audio_history_buffer_create();
  ASSERT_TRUE(buffer != NULL);
  // Default standalone buffer is enabled
  ASSERT_TRUE(audio_history_buffer_is_enabled(buffer));

  // Explicitly disable
  audio_history_buffer_set_enabled(buffer, false);
  ASSERT_FALSE(audio_history_buffer_is_enabled(buffer));

  audio_history_buffer_reset(buffer, 2);
  // reset must not affect enabled flag
  ASSERT_FALSE(audio_history_buffer_is_enabled(buffer));

  audio_chunk_t *chunk = audio_chunk_create(256, 2);
  for (size_t t = 0; t < 256; t++) {
    audio_chunk_get_channel(chunk, 0)[t] = (double)(t + 1);
    audio_chunk_get_channel(chunk, 1)[t] = (double)(t + 1);
  }
  audio_chunk_set_valid_frames(chunk, 256);

  // Appending while disabled must be a no-op
  audio_history_buffer_append(buffer, chunk);

  float *dest = (float *)calloc(256, sizeof(float));
  bool enough = false;
  const size_t ch0 = 0;

  // read_latest while disabled still works if called, but since nothing was
  // appended, enough_data is false
  audio_history_buffer_status_t status =
      audio_history_buffer_read_latest(buffer, dest, 256, &ch0, &enough);
  ASSERT_EQ(AUDIO_HISTORY_BUFFER_OK, status);
  ASSERT_FALSE(enough);
  ASSERT_FALSE(audio_history_buffer_is_enabled(buffer));

  // Now explicitly enable it
  audio_history_buffer_set_enabled(buffer, true);
  ASSERT_TRUE(audio_history_buffer_is_enabled(buffer));

  // Now that it is enabled, append succeeds and records samples
  audio_history_buffer_append(buffer, chunk);
  status = audio_history_buffer_read_latest(buffer, dest, 256, &ch0, &enough);
  ASSERT_EQ(AUDIO_HISTORY_BUFFER_OK, status);
  ASSERT_TRUE(enough);
  ASSERT_FLOAT_EQ(1.0f, dest[0]);
  ASSERT_FLOAT_EQ(256.0f, dest[255]);

  free(dest);
  audio_chunk_free(chunk);
  audio_history_buffer_free(buffer);
}

TEST(UnusedChannelIsStoredAsSilence) {
  audio_history_buffer_t *buffer = audio_history_buffer_create();
  audio_history_buffer_reset(buffer, 2);

  audio_chunk_t *chunk = audio_chunk_create(64, 2);
  for (size_t t = 0; t < 64; t++) {
    audio_chunk_get_channel(chunk, 0)[t] = 1.0;
    audio_chunk_get_channel(chunk, 1)[t] = 2.0;
  }
  audio_chunk_set_valid_frames(chunk, 64);
  bool used[2] = {true, false};
  audio_chunk_set_used_channels(chunk, used);

  audio_history_buffer_append(buffer, chunk);

  float dest[64];
  bool enough = false;
  const size_t ch0 = 0;
  const size_t ch1 = 1;

  ASSERT_EQ(AUDIO_HISTORY_BUFFER_OK,
            audio_history_buffer_read_latest(buffer, dest, 64, &ch0, &enough));
  ASSERT_TRUE(enough);
  for (size_t t = 0; t < 64; t++) {
    ASSERT_FLOAT_EQ(1.0f, dest[t]);
  }

  ASSERT_EQ(AUDIO_HISTORY_BUFFER_OK,
            audio_history_buffer_read_latest(buffer, dest, 64, &ch1, &enough));
  ASSERT_TRUE(enough);
  for (size_t t = 0; t < 64; t++) {
    ASSERT_FLOAT_EQ(0.0f, dest[t]);
  }

  audio_chunk_free(chunk);
  audio_history_buffer_free(buffer);
}

TEST(AverageLeavesOutUnusedChannels) {
  audio_history_buffer_t *buffer = audio_history_buffer_create();
  audio_history_buffer_reset(buffer, 3);

  audio_chunk_t *chunk = audio_chunk_create(64, 3);
  for (size_t t = 0; t < 64; t++) {
    audio_chunk_get_channel(chunk, 0)[t] = 1.0;
    audio_chunk_get_channel(chunk, 1)[t] = 99.0;
    audio_chunk_get_channel(chunk, 2)[t] = 3.0;
  }
  audio_chunk_set_valid_frames(chunk, 64);
  bool used[3] = {true, false, true};
  audio_chunk_set_used_channels(chunk, used);

  audio_history_buffer_append(buffer, chunk);

  float dest[64];
  bool enough = false;
  ASSERT_EQ(AUDIO_HISTORY_BUFFER_OK,
            audio_history_buffer_read_latest(buffer, dest, 64, NULL, &enough));
  ASSERT_TRUE(enough);
  for (size_t t = 0; t < 64; t++) {
    ASSERT_NEAR(2.0f, dest[t], 1e-5);
  }

  audio_chunk_free(chunk);
  audio_history_buffer_free(buffer);
}

TEST(AverageOfOnlyUnusedChannelsIsSilence) {
  audio_history_buffer_t *buffer = audio_history_buffer_create();
  audio_history_buffer_reset(buffer, 2);

  audio_chunk_t *chunk = audio_chunk_create(64, 2);
  for (size_t t = 0; t < 64; t++) {
    audio_chunk_get_channel(chunk, 0)[t] = 5.0;
    audio_chunk_get_channel(chunk, 1)[t] = 5.0;
  }
  audio_chunk_set_valid_frames(chunk, 64);
  bool used[2] = {false, false};
  audio_chunk_set_used_channels(chunk, used);

  audio_history_buffer_append(buffer, chunk);

  float dest[64];
  for (size_t t = 0; t < 64; t++) {
    dest[t] = 42.0f;
  }
  bool enough = false;
  ASSERT_EQ(AUDIO_HISTORY_BUFFER_OK,
            audio_history_buffer_read_latest(buffer, dest, 64, NULL, &enough));
  ASSERT_TRUE(enough);
  for (size_t t = 0; t < 64; t++) {
    ASSERT_FLOAT_EQ(0.0f, dest[t]);
  }

  audio_chunk_free(chunk);
  audio_history_buffer_free(buffer);
}

TEST(UnusedChannelBecomingUsed) {
  audio_history_buffer_t *buffer = audio_history_buffer_create();
  audio_history_buffer_reset(buffer, 2);

  audio_chunk_t *chunk1 = audio_chunk_create(32, 2);
  for (size_t t = 0; t < 32; t++) {
    audio_chunk_get_channel(chunk1, 0)[t] = 1.0;
    audio_chunk_get_channel(chunk1, 1)[t] = 9.0;
  }
  audio_chunk_set_valid_frames(chunk1, 32);
  bool used1[2] = {true, false};
  audio_chunk_set_used_channels(chunk1, used1);
  audio_history_buffer_append(buffer, chunk1);

  audio_chunk_t *chunk2 = audio_chunk_create(32, 2);
  for (size_t t = 0; t < 32; t++) {
    audio_chunk_get_channel(chunk2, 0)[t] = 1.0;
    audio_chunk_get_channel(chunk2, 1)[t] = 3.0;
  }
  audio_chunk_set_valid_frames(chunk2, 32);
  bool used2[2] = {true, true};
  audio_chunk_set_used_channels(chunk2, used2);
  audio_history_buffer_append(buffer, chunk2);

  float dest[64];
  bool enough = false;
  const size_t ch1 = 1;
  ASSERT_EQ(AUDIO_HISTORY_BUFFER_OK,
            audio_history_buffer_read_latest(buffer, dest, 64, &ch1, &enough));
  ASSERT_TRUE(enough);
  for (size_t t = 0; t < 32; t++) {
    ASSERT_FLOAT_EQ(0.0f, dest[t]);
  }
  for (size_t t = 32; t < 64; t++) {
    ASSERT_FLOAT_EQ(3.0f, dest[t]);
  }

  ASSERT_EQ(AUDIO_HISTORY_BUFFER_OK,
            audio_history_buffer_read_latest(buffer, dest, 32, NULL, &enough));
  ASSERT_TRUE(enough);
  for (size_t t = 0; t < 32; t++) {
    ASSERT_NEAR(2.0f, dest[t], 1e-5);
  }

  audio_chunk_free(chunk1);
  audio_chunk_free(chunk2);
  audio_history_buffer_free(buffer);
}

// MARK: - Level History & Telemetry Tests

TEST(AuditAudio_RateAdjustNullFallbackMatchesDefault) {
  ASSERT_DOUBLE_EQ(0.0, processing_parameters_get_rate_adjust(NULL));
  processing_parameters_t *params = processing_parameters_create(2, 2);
  ASSERT_TRUE(params != NULL);
  ASSERT_DOUBLE_EQ(processing_parameters_get_rate_adjust(NULL),
                   processing_parameters_get_rate_adjust(params));
  processing_parameters_free(params);
}

static void fill_chunk_const(audio_chunk_t *chunk, size_t channels,
                             size_t frames, double value) {
  for (size_t c = 0; c < channels; c++) {
    double *ch = audio_chunk_get_channel(chunk, c);
    for (size_t i = 0; i < frames; i++)
      ch[i] = (i & 1) ? -value : value;
  }
  audio_chunk_set_valid_frames(chunk, frames);
}

TEST(AuditAudio_RmsHistoryAveragesPower) {
  processing_parameters_t *params = processing_parameters_create(2, 2);
  ASSERT_TRUE(params != NULL);
  audio_chunk_t *chunk = audio_chunk_create(256, 2);
  ASSERT_TRUE(chunk != NULL);

  fill_chunk_const(chunk, 2, 256, 0.5);
  processing_parameters_update_capture_levels(params, chunk);
  fill_chunk_const(chunk, 2, 256, 0.125);
  processing_parameters_update_capture_levels(params, chunk);

  float rms[2] = {0};
  ASSERT_TRUE(
      processing_parameters_get_capture_signal_rms_since(params, 0, rms, 2));
  double expected_db = 10.0 * log10((0.25 + 0.015625) / 2.0);
  ASSERT_NEAR(expected_db, rms[0], 1e-4);
  ASSERT_NEAR(expected_db, rms[1], 1e-4);

  float peak[2] = {0};
  ASSERT_TRUE(
      processing_parameters_get_capture_signal_peak_since(params, 0, peak, 2));
  ASSERT_NEAR(20.0 * log10(0.5), peak[0], 1e-4);

  // Instantaneous RMS reports the last chunk.
  float inst[2] = {0};
  processing_parameters_get_capture_signal_rms(params, inst, 2);
  ASSERT_NEAR(20.0 * log10(0.125), inst[0], 1e-4);

  audio_chunk_free(chunk);
  processing_parameters_free(params);
}

typedef struct {
  processing_parameters_t *params;
  audio_chunk_t *chunk;
  atomic_bool stop;
} level_writer_ctx_t;

static void *level_writer_thread(void *arg) {
  level_writer_ctx_t *ctx = (level_writer_ctx_t *)arg;
  while (!atomic_load(&ctx->stop)) {
    processing_parameters_update_capture_levels(ctx->params, ctx->chunk);
    cdsp_sleep_us(1000);
  }
  return NULL;
}

TEST(AuditAudio_LevelHistoryReadersSucceedUnderLoad) {
  enum { CH = 32, FRAMES = 2048 };
  processing_parameters_t *params = processing_parameters_create(CH, CH);
  ASSERT_TRUE(params != NULL);
  audio_chunk_t *chunk = audio_chunk_create(FRAMES, CH);
  ASSERT_TRUE(chunk != NULL);
  fill_chunk_const(chunk, CH, FRAMES, 0.25);
  processing_parameters_update_capture_levels(params, chunk);

  level_writer_ctx_t ctx = {.params = params, .chunk = chunk};
  atomic_init(&ctx.stop, false);
  pthread_t th;
  ASSERT_EQ(0, pthread_create(&th, NULL, level_writer_thread, &ctx));

  float levels[CH];
  int ok = 0, total = 0;
  for (int i = 0; i < 500; i++) {
    bool r1 = processing_parameters_get_capture_signal_rms_since(params, 0,
                                                                 levels, CH);
    if (r1)
      ASSERT_NEAR(20.0 * log10(0.25), levels[CH - 1], 1e-3);
    bool r2 = processing_parameters_get_capture_signal_peak_since(params, 0,
                                                                   levels, CH);
    if (r2)
      ASSERT_NEAR(20.0 * log10(0.25), levels[0], 1e-3);
    ok += (int)r1 + (int)r2;
    total += 2;
  }
  atomic_store(&ctx.stop, true);
  pthread_join(th, NULL);
  ASSERT_TRUE(ok * 100 >= total * 90);

  audio_chunk_free(chunk);
  processing_parameters_free(params);
}

TEST(AuditAudio_TransferTelemetryCopiesInstantLevels) {
  processing_parameters_t *src = processing_parameters_create(2, 2);
  processing_parameters_t *dst = processing_parameters_create(2, 2);
  ASSERT_TRUE(src != NULL && dst != NULL);
  audio_chunk_t *chunk = audio_chunk_create(128, 2);
  fill_chunk_const(chunk, 2, 128, 0.5);
  processing_parameters_update_capture_levels(src, chunk);
  processing_parameters_update_playback_levels(src, chunk);

  processing_parameters_transfer_telemetry(dst, src);

  float v[2] = {0};
  processing_parameters_get_capture_signal_peak(dst, v, 2);
  ASSERT_NEAR(20.0 * log10(0.5), v[0], 1e-4);
  processing_parameters_get_playback_signal_rms(dst, v, 2);
  ASSERT_NEAR(20.0 * log10(0.5), v[1], 1e-4);
  ASSERT_TRUE(processing_parameters_get_capture_signal_rms_since(dst, 0, v, 2));
  ASSERT_NEAR(20.0 * log10(0.5), v[0], 1e-4);

  uint64_t gen = processing_parameters_get_chunk_generation(dst, true);
  processing_parameters_update_capture_levels(dst, chunk);
  ASSERT_EQ(gen + 1, processing_parameters_get_chunk_generation(dst, true));

  audio_chunk_free(chunk);
  processing_parameters_free(src);
  processing_parameters_free(dst);
}

TEST_MAIN()
