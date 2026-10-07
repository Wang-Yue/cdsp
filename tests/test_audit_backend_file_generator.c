/**
 * @file test_audit_backend_file_generator.c
 * @brief Regression tests for the signal generator fixes from the backend
 * audit (audit_reports/backends/06_file_generator.md).
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "audio/audio_chunk.h"
#include "backend/audio_backend.h"
#include "backend/backend_error.h"
#include "test_support.h"

static capture_backend_t *audit_gen_create(signal_type_t type, double freq,
                                           double level_db, size_t channels,
                                           int rate) {
  capture_device_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.type = AUDIO_BACKEND_TYPE_GENERATOR;
  cfg.cfg.generator.channels = channels;
  cfg.cfg.generator.signal.type = type;
  cfg.cfg.generator.signal.freq = freq;
  cfg.cfg.generator.signal.level = level_db;
  backend_error_t err;
  backend_error_init(&err, BACKEND_ERROR_NONE, "");
  capture_backend_t *b =
      create_capture_backend(&cfg, rate, 1024, false, NULL, &err);
  if (b && !capture_backend_open(b, &err)) {
    capture_backend_free(b);
    return NULL;
  }
  return b;
}

static int audit_gen_cmp_double(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}

// F-18: white noise is uniform in [-amp, amp], has fine (not 15-bit)
// resolution, and the channels are uncorrelated.
TEST(AuditGeneratorWhiteNoiseQuality) {
  const size_t frames = 4096;
  const int chunks = 16;
  const double amp = 0.5; // -6.02 dB
  capture_backend_t *b = audit_gen_create(SIGNAL_TYPE_WHITE_NOISE, 0.0,
                                          20.0 * log10(amp), 2, 48000);
  ASSERT_TRUE(b != NULL);
  audio_chunk_t *chunk = audio_chunk_create(frames, 2);
  size_t n = frames * (size_t)chunks;
  double *all = (double *)malloc(n * sizeof(double));
  ASSERT_TRUE(all != NULL);
  double sum = 0.0, sum_xy = 0.0, sum_xx = 0.0, sum_yy = 0.0;
  backend_error_t err;
  for (int k = 0; k < chunks; k++) {
    ASSERT_TRUE(capture_backend_read(b, frames, chunk, &err));
    const double *x = audio_chunk_get_channel(chunk, 0);
    const double *y = audio_chunk_get_channel(chunk, 1);
    for (size_t f = 0; f < frames; f++) {
      ASSERT_TRUE(x[f] >= -amp - 1e-12 && x[f] <= amp + 1e-12);
      all[(size_t)k * frames + f] = x[f];
      sum += x[f];
      sum_xy += x[f] * y[f];
      sum_xx += x[f] * x[f];
      sum_yy += y[f] * y[f];
    }
  }
  double mean = sum / (double)n;
  ASSERT_TRUE(fabs(mean) < 0.02);
  // Uniform on [-a, a] has variance a^2/3.
  ASSERT_NEAR(amp * amp / 3.0, sum_xx / (double)n, 0.01);
  double corr = sum_xy / sqrt(sum_xx * sum_yy);
  ASSERT_TRUE(fabs(corr) < 0.05);

  // Resolution: with 65536 samples a 15-bit generator yields at most 32768
  // distinct values; a 53-bit one essentially never repeats.
  qsort(all, n, sizeof(double), audit_gen_cmp_double);
  size_t distinct = 1;
  for (size_t i = 1; i < n; i++) {
    if (all[i] != all[i - 1])
      distinct++;
  }
  ASSERT_TRUE(distinct > n - 16);

  free(all);
  audio_chunk_free(chunk);
  capture_backend_close(b);
  capture_backend_free(b);
}

// F-21: square duty cycle is exactly 50 % even when phase lands on 0.5.
TEST(AuditGeneratorSquareExactDutyCycle) {
  capture_backend_t *b = audit_gen_create(SIGNAL_TYPE_SQUARE, 12000.0, 0.0, 1,
                                          48000); // fs/4: + + - -
  ASSERT_TRUE(b != NULL);
  audio_chunk_t *chunk = audio_chunk_create(64, 1);
  backend_error_t err;
  ASSERT_TRUE(capture_backend_read(b, 64, chunk, &err));
  const double *x = audio_chunk_get_channel(chunk, 0);
  double sum = 0.0;
  for (size_t f = 0; f < 64; f++) {
    ASSERT_NEAR((f % 4) < 2 ? 1.0 : -1.0, x[f], 0.0);
    sum += x[f];
  }
  ASSERT_NEAR(0.0, sum, 0.0); // no DC
  audio_chunk_free(chunk);
  capture_backend_close(b);
  capture_backend_free(b);

  b = audit_gen_create(SIGNAL_TYPE_SQUARE, 24000.0, 0.0, 1, 48000); // fs/2
  ASSERT_TRUE(b != NULL);
  chunk = audio_chunk_create(8, 1);
  ASSERT_TRUE(capture_backend_read(b, 8, chunk, &err));
  x = audio_chunk_get_channel(chunk, 0);
  for (size_t f = 0; f < 8; f++) {
    ASSERT_NEAR((f % 2) == 0 ? 1.0 : -1.0, x[f], 0.0);
  }
  audio_chunk_free(chunk);
  capture_backend_close(b);
  capture_backend_free(b);
}

// F-22/F-23: create() validates its own inputs (no VLA of size 0), reports
// a reason, and periodic signals are identical on every channel.
TEST(AuditGeneratorCreateValidatesAndCopiesChannels) {
  capture_device_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.type = AUDIO_BACKEND_TYPE_GENERATOR;
  cfg.cfg.generator.signal.type = SIGNAL_TYPE_SINE;
  cfg.cfg.generator.signal.freq = 1000.0;
  cfg.cfg.generator.signal.level = 0.0;
  cfg.cfg.generator.channels = 0;
  backend_error_t err;
  backend_error_init(&err, BACKEND_ERROR_NONE, "");
  ASSERT_TRUE(create_capture_backend(&cfg, 48000, 64, false, NULL, &err) ==
              NULL);
  ASSERT_EQ(BACKEND_ERROR_INITIALIZATION_FAILED, err.type);

  cfg.cfg.generator.channels = 2;
  cfg.cfg.generator.signal.freq = NAN;
  backend_error_init(&err, BACKEND_ERROR_NONE, "");
  ASSERT_TRUE(create_capture_backend(&cfg, 48000, 64, false, NULL, &err) ==
              NULL);
  ASSERT_EQ(BACKEND_ERROR_INITIALIZATION_FAILED, err.type);

  capture_backend_t *b =
      audit_gen_create(SIGNAL_TYPE_SINE, 997.0, -3.0, 8, 48000);
  ASSERT_TRUE(b != NULL);
  audio_chunk_t *chunk = audio_chunk_create(256, 8);
  ASSERT_TRUE(capture_backend_read(b, 256, chunk, &err));
  ASSERT_EQ(256, (int)audio_chunk_get_valid_frames(chunk));
  const double *c0 = audio_chunk_get_channel(chunk, 0);
  for (size_t c = 1; c < 8; c++) {
    ASSERT_TRUE(memcmp(c0, audio_chunk_get_channel(chunk, c),
                       256 * sizeof(double)) == 0);
  }
  audio_chunk_free(chunk);
  capture_backend_close(b);
  capture_backend_free(b);
}

TEST_MAIN()
