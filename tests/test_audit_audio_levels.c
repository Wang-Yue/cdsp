// Regression tests for audit report 08 (src/audio, src/utils) fixes.
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#include "audio/audio_chunk.h"
#include "audio/processing_parameters.h"
#include "test_support.h"
#include "utils/cdsp_time.h"
#include "utils/float_helpers.h"

// Report 08 §2.1 / report 07 §2.4: NULL fallback must match the create-time
// default (0.0 = inactive, upstream CaptureStatus::default()).
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

// Report 08 §1.3: RMS history is averaged as linear power (upstream
// add_record_squared / average_sqrt_since), not via dB round-trips.
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
    // ~1 kHz publish rate: faster than any real chunk period (1024 frames @
    // 48 kHz is ~21 ms), but not a zero-gap loop that no reader could ever
    // observe a stable sequence against under sanitizer slowdown.
    cdsp_sleep_us(1000);
  }
  return NULL;
}

// Report 08 §1.1: readers must not fail just because the audio thread is
// computing levels (the seqlock window no longer covers the reductions).
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
  // Previously ~1% of reads succeeded. 90% leaves headroom for sanitizer /
  // parallel-ctest scheduling noise while still catching that regression.
  ASSERT_TRUE(ok * 100 >= total * 90);

  audio_chunk_free(chunk);
  processing_parameters_free(params);
}

// Report 08 §2.2: telemetry transfer carries instantaneous levels and leaves
// the destination history readable (even sequence).
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

  // The destination keeps accepting new records after a transfer.
  uint64_t gen = processing_parameters_get_chunk_generation(dst, true);
  processing_parameters_update_capture_levels(dst, chunk);
  ASSERT_EQ(gen + 1, processing_parameters_get_chunk_generation(dst, true));

  audio_chunk_free(chunk);
  processing_parameters_free(src);
  processing_parameters_free(dst);
}

TEST_MAIN()
