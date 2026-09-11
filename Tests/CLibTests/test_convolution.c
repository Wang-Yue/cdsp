#if defined(__linux__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif
#include <math.h>
#include <string.h>

#include "Config/filter_config_types.h"
#include "Filters/convolution.h"
#include "Filters/filter.h"
#include "test_support.h"

TEST(MovingAverage) {
  double coeffs[] = {0.5, 0.5};
  convolution_config_t params = {
      .type = CONV_TYPE_VALUES, .values = coeffs, .values_count = 2};
  filter_config_t cfg = {.type = FILTER_TYPE_CONV, .parameters.conv = params};
  void* filter = g_convolution_vtable.create("conv", &cfg, 0, 8, NULL, NULL);
  ASSERT_TRUE(filter != NULL);

  double wave[] = {1.0, 1.0, 1.0, 0.0, 0.0, -1.0, 0.0, 0.0};
  double expected[] = {0.5, 1.0, 1.0, 0.5, 0.0, -0.5, -0.5, 0.0};

  g_convolution_vtable.process(filter, wave, 8);
  for (size_t i = 0; i < 8; i++) {
    ASSERT_NEAR(expected[i], wave[i], 1e-7);
  }
  g_convolution_vtable.free(filter);
}

TEST(SegmentedConvolution) {
  double ir[32];
  for (int i = 0; i < 32; i++) ir[i] = (double)i;
  convolution_config_t params = {
      .type = CONV_TYPE_VALUES, .values = ir, .values_count = 32};
  filter_config_t cfg = {.type = FILTER_TYPE_CONV, .parameters.conv = params};
  void* filter = g_convolution_vtable.create("conv", &cfg, 0, 8, NULL, NULL);
  ASSERT_TRUE(filter != NULL);

  double impulse[8] = {1.0, 0, 0, 0, 0, 0, 0, 0};
  g_convolution_vtable.process(filter, impulse, 8);
  for (int i = 0; i < 8; i++) ASSERT_NEAR((double)i, impulse[i], 1e-5);

  double zeros[8] = {0};
  g_convolution_vtable.process(filter, zeros, 8);
  for (int i = 0; i < 8; i++) ASSERT_NEAR((double)(i + 8), zeros[i], 1e-5);

  memset(zeros, 0, sizeof(zeros));
  g_convolution_vtable.process(filter, zeros, 8);
  for (int i = 0; i < 8; i++) ASSERT_NEAR((double)(i + 16), zeros[i], 1e-5);

  memset(zeros, 0, sizeof(zeros));
  g_convolution_vtable.process(filter, zeros, 8);
  for (int i = 0; i < 8; i++) ASSERT_NEAR((double)(i + 24), zeros[i], 1e-5);

  memset(zeros, 0, sizeof(zeros));
  g_convolution_vtable.process(filter, zeros, 8);
  for (int i = 0; i < 8; i++) ASSERT_NEAR(0.0, zeros[i], 1e-5);

  g_convolution_vtable.free(filter);
}

TEST(IdentityConvolution) {
  double coeffs[] = {1.0};
  convolution_config_t params = {
      .type = CONV_TYPE_VALUES, .values = coeffs, .values_count = 1};
  filter_config_t cfg = {.type = FILTER_TYPE_CONV, .parameters.conv = params};
  void* filter = g_convolution_vtable.create("conv", &cfg, 0, 8, NULL, NULL);
  ASSERT_TRUE(filter != NULL);

  double wave[] = {1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  g_convolution_vtable.process(filter, wave, 8);
  ASSERT_NEAR(1.0, wave[0], 1e-7);
  for (int i = 1; i < 8; i++) ASSERT_NEAR(0.0, wave[i], 1e-7);
  g_convolution_vtable.free(filter);
}

TEST(DelayConvolution) {
  double coeffs[] = {0.0, 0.0, 0.0, 1.0};
  convolution_config_t params = {
      .type = CONV_TYPE_VALUES, .values = coeffs, .values_count = 4};
  filter_config_t cfg = {.type = FILTER_TYPE_CONV, .parameters.conv = params};
  void* filter = g_convolution_vtable.create("conv", &cfg, 0, 8, NULL, NULL);
  ASSERT_TRUE(filter != NULL);

  double wave[] = {1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  g_convolution_vtable.process(filter, wave, 8);
  ASSERT_NEAR(0.0, wave[0], 1e-7);
  ASSERT_NEAR(0.0, wave[1], 1e-7);
  ASSERT_NEAR(0.0, wave[2], 1e-7);
  ASSERT_NEAR(1.0, wave[3], 1e-7);
  for (int i = 4; i < 8; i++) ASSERT_NEAR(0.0, wave[i], 1e-7);
  g_convolution_vtable.free(filter);
}

TEST(ConvolutionWithSineWave) {
  double coeffs[] = {0.5, 0.5};
  convolution_config_t params = {
      .type = CONV_TYPE_VALUES, .values = coeffs, .values_count = 2};
  filter_config_t cfg = {.type = FILTER_TYPE_CONV, .parameters.conv = params};
  void* filter = g_convolution_vtable.create("conv", &cfg, 0, 64, NULL, NULL);
  ASSERT_TRUE(filter != NULL);

  double sample_rate = 48000.0;
  double freq = 100.0;
  double theta = 2.0 * M_PI * freq / sample_rate;
  double expected_gain = 0.5 * (1.0 + cos(theta));

  double wave[64];
  for (int chunk = 0; chunk < 8; chunk++) {
    int offset = chunk * 64;
    for (int i = 0; i < 64; i++) {
      wave[i] = cos(2.0 * M_PI * freq * (double)(offset + i) / sample_rate);
    }
    g_convolution_vtable.process(filter, wave, 64);
  }

  double peak = 0.0;
  for (int i = 0; i < 64; i++) {
    if (fabs(wave[i]) > peak) peak = fabs(wave[i]);
  }
  ASSERT_TRUE(fabs(peak - expected_gain) < expected_gain * 0.10);
  g_convolution_vtable.free(filter);
}

TEST(EmptyIRThrows) {
  convolution_config_t params = {
      .type = CONV_TYPE_VALUES, .values = NULL, .values_count = 0};
  filter_config_t cfg = {.type = FILTER_TYPE_CONV, .parameters.conv = params};
  void* filter = g_convolution_vtable.create("conv", &cfg, 0, 8, NULL, NULL);
  ASSERT_TRUE(filter == NULL);
}

TEST(DummyIsIdentity) {
  convolution_config_t params = {.type = CONV_TYPE_DUMMY, .length = 4};
  filter_config_t cfg = {.type = FILTER_TYPE_CONV, .parameters.conv = params};
  void* filter = g_convolution_vtable.create("conv", &cfg, 0, 8, NULL, NULL);
  ASSERT_TRUE(filter != NULL);

  double wave[] = {0.3, -0.2, 0.7, -0.1, 0.0, 0.5, -0.4, 0.9};
  double original[] = {0.3, -0.2, 0.7, -0.1, 0.0, 0.5, -0.4, 0.9};
  g_convolution_vtable.process(filter, wave, 8);

  for (int i = 0; i < 8; i++) {
    ASSERT_NEAR(original[i], wave[i], 1e-7);
  }
  g_convolution_vtable.free(filter);
}

TEST(CachedBuildSharesCoeffsButNotState) {
  double ir[] = {0.1, 0.2, 0.3, 0.4};
  convolution_config_t params = {
      .type = CONV_TYPE_VALUES, .values = ir, .values_count = 4};
  filter_config_t cfg = {.type = FILTER_TYPE_CONV, .parameters.conv = params};

  void* f1 = g_convolution_vtable.create("shared_conv", &cfg, 0, 8, NULL, NULL);
  void* f2 = g_convolution_vtable.create("shared_conv", &cfg, 0, 8, NULL, NULL);
  ASSERT_TRUE(f1 != NULL);
  ASSERT_TRUE(f2 != NULL);

  double wave1[8] = {1.0, 0, 0, 0, 0, 0, 0, 0};
  double wave2[8] = {0, 1.0, 0, 0, 0, 0, 0, 0};

  g_convolution_vtable.process(f1, wave1, 8);
  g_convolution_vtable.process(f2, wave2, 8);

  // wave1 should have impulse response at [0..3]
  ASSERT_NEAR(0.1, wave1[0], 1e-7);
  ASSERT_NEAR(0.2, wave1[1], 1e-7);
  ASSERT_NEAR(0.3, wave1[2], 1e-7);
  ASSERT_NEAR(0.4, wave1[3], 1e-7);

  // wave2 should have impulse response at [1..4]
  ASSERT_NEAR(0.0, wave2[0], 1e-7);
  ASSERT_NEAR(0.1, wave2[1], 1e-7);
  ASSERT_NEAR(0.2, wave2[2], 1e-7);
  ASSERT_NEAR(0.3, wave2[3], 1e-7);
  ASSERT_NEAR(0.4, wave2[4], 1e-7);

  g_convolution_vtable.free(f1);
  g_convolution_vtable.free(f2);
}

TEST(CacheDoesNotShareAcrossLengths) {
  double ir[] = {0.1, 0.2, 0.3, 0.4};
  convolution_config_t params = {
      .type = CONV_TYPE_VALUES, .values = ir, .values_count = 4};
  filter_config_t cfg = {.type = FILTER_TYPE_CONV, .parameters.conv = params};

  void* short_f =
      g_convolution_vtable.create("size_conv", &cfg, 0, 8, NULL, NULL);
  void* long_f =
      g_convolution_vtable.create("size_conv", &cfg, 0, 16, NULL, NULL);
  ASSERT_TRUE(short_f != NULL);
  ASSERT_TRUE(long_f != NULL);

  double short_wave[8] = {1.0, 0, 0, 0, 0, 0, 0, 0};
  double long_wave[16] = {1.0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

  g_convolution_vtable.process(short_f, short_wave, 8);
  g_convolution_vtable.process(long_f, long_wave, 16);

  for (size_t i = 0; i < 4; i++) {
    ASSERT_NEAR(ir[i], short_wave[i], 1e-7);
    ASSERT_NEAR(ir[i], long_wave[i], 1e-7);
  }

  g_convolution_vtable.free(short_f);
  g_convolution_vtable.free(long_f);
}

/* --- Coefficient-load failures must be fatal (audit 03-1) --- */

/**
 * @brief Writes @p contents to @p path, returning false on any failure.
 */
static bool write_temp_file(const char* path, const void* contents,
                            size_t len) {
  FILE* f = fopen(path, "wb");
  if (!f) return false;
  bool ok = len == 0 || fwrite(contents, 1, len, f) == len;
  fclose(f);
  return ok;
}

TEST(UnparsableTextCoeffFileIsRejected) {
  const char* path = "/tmp/cdsp_test_conv_unparsable.txt";
  const char* body = "# only a comment, no coefficients\n";
  ASSERT_TRUE(write_temp_file(path, body, strlen(body)));

  convolution_config_t params = {.type = CONV_TYPE_RAW, .format = "TEXT"};
  snprintf(params.filename, sizeof(params.filename), "%s", path);
  filter_config_t cfg = {.type = FILTER_TYPE_CONV, .parameters.conv = params};

  config_error_t err;
  config_error_init(&err);
  void* filter =
      g_convolution_vtable.create("conv_bad_text", &cfg, 0, 8, NULL, &err);
  ASSERT_TRUE(filter == NULL);
  ASSERT_TRUE(err.type != CONFIG_ERR_NONE);
  remove(path);
}

TEST(CommentInsideTextCoeffFileIsRejected) {
  const char* path = "/tmp/cdsp_test_conv_mid_comment.txt";
  const char* body = "1.0\n# comment\n2.0\n";
  ASSERT_TRUE(write_temp_file(path, body, strlen(body)));

  convolution_config_t params = {.type = CONV_TYPE_RAW, .format = "TEXT"};
  snprintf(params.filename, sizeof(params.filename), "%s", path);
  filter_config_t cfg = {.type = FILTER_TYPE_CONV, .parameters.conv = params};

  config_error_t err;
  config_error_init(&err);
  void* filter =
      g_convolution_vtable.create("conv_mid_comment", &cfg, 0, 8, NULL, &err);
  ASSERT_TRUE(filter == NULL);
  ASSERT_TRUE(err.type != CONFIG_ERR_NONE);
  remove(path);
}

TEST(EmptyLineInsideTextCoeffFileIsRejected) {
  const char* path = "/tmp/cdsp_test_conv_empty_line.txt";
  const char* body = "1.0\n\n2.0\n";
  ASSERT_TRUE(write_temp_file(path, body, strlen(body)));

  convolution_config_t params = {.type = CONV_TYPE_RAW, .format = "TEXT"};
  snprintf(params.filename, sizeof(params.filename), "%s", path);
  filter_config_t cfg = {.type = FILTER_TYPE_CONV, .parameters.conv = params};

  config_error_t err;
  config_error_init(&err);
  void* filter =
      g_convolution_vtable.create("conv_empty_line", &cfg, 0, 8, NULL, &err);
  ASSERT_TRUE(filter == NULL);
  ASSERT_TRUE(err.type != CONFIG_ERR_NONE);
  remove(path);
}

TEST(MultipleValuesOnLineIsRejected) {
  const char* path = "/tmp/cdsp_test_conv_multi_val.txt";
  const char* body = "1.0 2.0\n";
  ASSERT_TRUE(write_temp_file(path, body, strlen(body)));

  convolution_config_t params = {.type = CONV_TYPE_RAW, .format = "TEXT"};
  snprintf(params.filename, sizeof(params.filename), "%s", path);
  filter_config_t cfg = {.type = FILTER_TYPE_CONV, .parameters.conv = params};

  config_error_t err;
  config_error_init(&err);
  void* filter =
      g_convolution_vtable.create("conv_multi_val", &cfg, 0, 8, NULL, &err);
  ASSERT_TRUE(filter == NULL);
  ASSERT_TRUE(err.type != CONFIG_ERR_NONE);
  remove(path);
}

TEST(HexFloatIsRejected) {
  const char* path = "/tmp/cdsp_test_conv_hex_float.txt";
  const char* body = "0x1.0p0\n";
  ASSERT_TRUE(write_temp_file(path, body, strlen(body)));

  convolution_config_t params = {.type = CONV_TYPE_RAW, .format = "TEXT"};
  snprintf(params.filename, sizeof(params.filename), "%s", path);
  filter_config_t cfg = {.type = FILTER_TYPE_CONV, .parameters.conv = params};

  config_error_t err;
  config_error_init(&err);
  void* filter =
      g_convolution_vtable.create("conv_hex_float", &cfg, 0, 8, NULL, &err);
  ASSERT_TRUE(filter == NULL);
  ASSERT_TRUE(err.type != CONFIG_ERR_NONE);
  remove(path);
}

TEST(TextReadBytesLinesLimitsLines) {
  const char* path = "/tmp/cdsp_test_conv_line_limit.txt";
  const char* body = "1.0\n2.0\n3.0\n4.0\n";
  ASSERT_TRUE(write_temp_file(path, body, strlen(body)));

  convolution_config_t params = {
      .type = CONV_TYPE_RAW, .format = "TEXT", .read_bytes_lines = 2};
  snprintf(params.filename, sizeof(params.filename), "%s", path);
  filter_config_t cfg = {.type = FILTER_TYPE_CONV, .parameters.conv = params};

  config_error_t err;
  config_error_init(&err);
  void* filter =
      g_convolution_vtable.create("conv_line_limit", &cfg, 0, 8, NULL, &err);
  ASSERT_TRUE(filter != NULL);

  double wave[8] = {1.0, 0, 0, 0, 0, 0, 0, 0};
  g_convolution_vtable.process(filter, wave, 8);
  ASSERT_NEAR(1.0, wave[0], 1e-7);
  ASSERT_NEAR(2.0, wave[1], 1e-7);
  ASSERT_NEAR(0.0, wave[2], 1e-7);

  g_convolution_vtable.free(filter);
  remove(path);
}

TEST(LongLineTextCoeffParsed) {
  const char* path = "/tmp/cdsp_test_conv_long_line.txt";
  char buf[300];
  memset(buf, ' ', 200);
  snprintf(buf + 200, sizeof(buf) - 200, "1.5\n");
  ASSERT_TRUE(write_temp_file(path, buf, strlen(buf)));

  convolution_config_t params = {.type = CONV_TYPE_RAW, .format = "TEXT"};
  snprintf(params.filename, sizeof(params.filename), "%s", path);
  filter_config_t cfg = {.type = FILTER_TYPE_CONV, .parameters.conv = params};

  config_error_t err;
  config_error_init(&err);
  void* filter =
      g_convolution_vtable.create("conv_long_line", &cfg, 0, 8, NULL, &err);
  ASSERT_TRUE(filter != NULL);

  double wave[8] = {1.0, 0, 0, 0, 0, 0, 0, 0};
  g_convolution_vtable.process(filter, wave, 8);
  ASSERT_NEAR(1.5, wave[0], 1e-7);

  g_convolution_vtable.free(filter);
  remove(path);
}

TEST(TruncatedRawCoeffFileIsRejected) {
  // One byte is less than a single S16LE frame, so no coefficient can be read.
  const char* path = "/tmp/cdsp_test_conv_truncated.raw";
  const unsigned char body[1] = {0x7f};
  ASSERT_TRUE(write_temp_file(path, body, sizeof(body)));

  convolution_config_t params = {.type = CONV_TYPE_RAW, .format = "S16LE"};
  snprintf(params.filename, sizeof(params.filename), "%s", path);
  filter_config_t cfg = {.type = FILTER_TYPE_CONV, .parameters.conv = params};

  config_error_t err;
  config_error_init(&err);
  void* filter =
      g_convolution_vtable.create("conv_truncated", &cfg, 0, 8, NULL, &err);
  ASSERT_TRUE(filter == NULL);
  ASSERT_TRUE(err.type != CONFIG_ERR_NONE);
  remove(path);
}

TEST(PartialChunkProcessesImmediatelyWithoutStaleSamples) {
  double ir[] = {1.0, 0.5};
  convolution_config_t params = {
      .type = CONV_TYPE_VALUES, .values = ir, .values_count = 2};
  filter_config_t cfg = {.type = FILTER_TYPE_CONV, .parameters.conv = params};
  void* filter =
      g_convolution_vtable.create("conv_partial", &cfg, 0, 8, NULL, NULL);
  ASSERT_TRUE(filter != NULL);

  // Send a partial chunk of 4 samples (chunk_size is 8)
  double wave[4] = {1.0, 2.0, 3.0, 4.0};
  g_convolution_vtable.process(filter, wave, 4);

  // Output must be computed immediately without waiting for a full chunk or
  // emitting stale zeros
  ASSERT_NEAR(1.0, wave[0], 1e-7);  // 1.0 * 1.0
  ASSERT_NEAR(2.5, wave[1], 1e-7);  // 2.0 * 1.0 + 1.0 * 0.5
  ASSERT_NEAR(4.0, wave[2], 1e-7);  // 3.0 * 1.0 + 2.0 * 0.5
  ASSERT_NEAR(5.5, wave[3], 1e-7);  // 4.0 * 1.0 + 3.0 * 0.5

  g_convolution_vtable.free(filter);
}

TEST_MAIN()
