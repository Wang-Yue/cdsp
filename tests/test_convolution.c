#if defined(__linux__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "config/config_error.h"
#include "config/config_gen.h"
#include "filters/convolution.h"
#include "filters/filter.h"
#include "test_support.h"

TEST(MovingAverage) {
  double coeffs[] = {0.5, 0.5};
  conv_config_t params = {
      .type = CONV_TYPE_VALUES, .values = coeffs, .values_count = 2};
  filter_config_t cfg = {.type = FILTER_TYPE_CONV, .parameters.conv = params};
  void *filter = g_convolution_vtable.create("conv", &cfg, 0, 8, NULL, NULL);
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
  for (int i = 0; i < 32; i++)
    ir[i] = (double)i;
  conv_config_t params = {
      .type = CONV_TYPE_VALUES, .values = ir, .values_count = 32};
  filter_config_t cfg = {.type = FILTER_TYPE_CONV, .parameters.conv = params};
  void *filter = g_convolution_vtable.create("conv", &cfg, 0, 8, NULL, NULL);
  ASSERT_TRUE(filter != NULL);

  double impulse[8] = {1.0, 0, 0, 0, 0, 0, 0, 0};
  g_convolution_vtable.process(filter, impulse, 8);
  for (int i = 0; i < 8; i++)
    ASSERT_NEAR((double)i, impulse[i], 1e-5);

  double zeros[8] = {0};
  g_convolution_vtable.process(filter, zeros, 8);
  for (int i = 0; i < 8; i++)
    ASSERT_NEAR((double)(i + 8), zeros[i], 1e-5);

  memset(zeros, 0, sizeof(zeros));
  g_convolution_vtable.process(filter, zeros, 8);
  for (int i = 0; i < 8; i++)
    ASSERT_NEAR((double)(i + 16), zeros[i], 1e-5);

  memset(zeros, 0, sizeof(zeros));
  g_convolution_vtable.process(filter, zeros, 8);
  for (int i = 0; i < 8; i++)
    ASSERT_NEAR((double)(i + 24), zeros[i], 1e-5);

  memset(zeros, 0, sizeof(zeros));
  g_convolution_vtable.process(filter, zeros, 8);
  for (int i = 0; i < 8; i++)
    ASSERT_NEAR(0.0, zeros[i], 1e-5);

  g_convolution_vtable.free(filter);
}

TEST(IdentityConvolution) {
  double coeffs[] = {1.0};
  conv_config_t params = {
      .type = CONV_TYPE_VALUES, .values = coeffs, .values_count = 1};
  filter_config_t cfg = {.type = FILTER_TYPE_CONV, .parameters.conv = params};
  void *filter = g_convolution_vtable.create("conv", &cfg, 0, 8, NULL, NULL);
  ASSERT_TRUE(filter != NULL);

  double wave[] = {1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  g_convolution_vtable.process(filter, wave, 8);
  ASSERT_NEAR(1.0, wave[0], 1e-7);
  for (int i = 1; i < 8; i++)
    ASSERT_NEAR(0.0, wave[i], 1e-7);
  g_convolution_vtable.free(filter);
}

TEST(DelayConvolution) {
  double coeffs[] = {0.0, 0.0, 0.0, 1.0};
  conv_config_t params = {
      .type = CONV_TYPE_VALUES, .values = coeffs, .values_count = 4};
  filter_config_t cfg = {.type = FILTER_TYPE_CONV, .parameters.conv = params};
  void *filter = g_convolution_vtable.create("conv", &cfg, 0, 8, NULL, NULL);
  ASSERT_TRUE(filter != NULL);

  double wave[] = {1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  g_convolution_vtable.process(filter, wave, 8);
  ASSERT_NEAR(0.0, wave[0], 1e-7);
  ASSERT_NEAR(0.0, wave[1], 1e-7);
  ASSERT_NEAR(0.0, wave[2], 1e-7);
  ASSERT_NEAR(1.0, wave[3], 1e-7);
  for (int i = 4; i < 8; i++)
    ASSERT_NEAR(0.0, wave[i], 1e-7);
  g_convolution_vtable.free(filter);
}

TEST(ConvolutionWithSineWave) {
  double coeffs[] = {0.5, 0.5};
  conv_config_t params = {
      .type = CONV_TYPE_VALUES, .values = coeffs, .values_count = 2};
  filter_config_t cfg = {.type = FILTER_TYPE_CONV, .parameters.conv = params};
  void *filter = g_convolution_vtable.create("conv", &cfg, 0, 64, NULL, NULL);
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
    if (fabs(wave[i]) > peak)
      peak = fabs(wave[i]);
  }
  ASSERT_TRUE(fabs(peak - expected_gain) < expected_gain * 0.10);
  g_convolution_vtable.free(filter);
}

TEST(EmptyIRThrows) {
  conv_config_t params = {
      .type = CONV_TYPE_VALUES, .values = NULL, .values_count = 0};
  filter_config_t cfg = {.type = FILTER_TYPE_CONV, .parameters.conv = params};
  void *filter = g_convolution_vtable.create("conv", &cfg, 0, 8, NULL, NULL);
  ASSERT_TRUE(filter == NULL);
}

TEST(DummyIsIdentity) {
  conv_config_t params = {.type = CONV_TYPE_DUMMY, .length = 4};
  filter_config_t cfg = {.type = FILTER_TYPE_CONV, .parameters.conv = params};
  void *filter = g_convolution_vtable.create("conv", &cfg, 0, 8, NULL, NULL);
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
  conv_config_t params = {
      .type = CONV_TYPE_VALUES, .values = ir, .values_count = 4};
  filter_config_t cfg = {.type = FILTER_TYPE_CONV, .parameters.conv = params};

  void *f1 = g_convolution_vtable.create("shared_conv", &cfg, 0, 8, NULL, NULL);
  void *f2 = g_convolution_vtable.create("shared_conv", &cfg, 0, 8, NULL, NULL);
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
  conv_config_t params = {
      .type = CONV_TYPE_VALUES, .values = ir, .values_count = 4};
  filter_config_t cfg = {.type = FILTER_TYPE_CONV, .parameters.conv = params};

  void *short_f =
      g_convolution_vtable.create("size_conv", &cfg, 0, 8, NULL, NULL);
  void *long_f =
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
static bool write_temp_file(const char *path, const void *contents,
                            size_t len) {
  FILE *f = fopen(path, "wb");
  if (!f)
    return false;
  bool ok = len == 0 || fwrite(contents, 1, len, f) == len;
  fclose(f);
  return ok;
}

TEST(UnparsableTextCoeffFileIsRejected) {
  char path[512];
  snprintf(path, sizeof(path), "/tmp/cdsp_test_conv_unparsable.txt");
  const char *body = "# only a comment, no coefficients\n";
  ASSERT_TRUE(write_temp_file(path, body, strlen(body)));

  conv_config_t params = {.type = CONV_TYPE_RAW, .format = "TEXT"};
  snprintf(params.filename, sizeof(params.filename), "%s", path);
  filter_config_t cfg = {.type = FILTER_TYPE_CONV, .parameters.conv = params};

  config_error_t err;
  config_error_init(&err);
  void *filter =
      g_convolution_vtable.create("conv_bad_text", &cfg, 0, 8, NULL, &err);
  ASSERT_TRUE(filter == NULL);
  ASSERT_TRUE(err.type != CONFIG_ERR_NONE);
  remove(path);
}

TEST(CommentInsideTextCoeffFileIsRejected) {
  char path[512];
  snprintf(path, sizeof(path), "/tmp/cdsp_test_conv_mid_comment.txt");
  const char *body = "1.0\n# comment\n2.0\n";
  ASSERT_TRUE(write_temp_file(path, body, strlen(body)));

  conv_config_t params = {.type = CONV_TYPE_RAW, .format = "TEXT"};
  snprintf(params.filename, sizeof(params.filename), "%s", path);
  filter_config_t cfg = {.type = FILTER_TYPE_CONV, .parameters.conv = params};

  config_error_t err;
  config_error_init(&err);
  void *filter =
      g_convolution_vtable.create("conv_mid_comment", &cfg, 0, 8, NULL, &err);
  ASSERT_TRUE(filter == NULL);
  ASSERT_TRUE(err.type != CONFIG_ERR_NONE);
  remove(path);
}

TEST(EmptyLineInsideTextCoeffFileIsRejected) {
  char path[512];
  snprintf(path, sizeof(path), "/tmp/cdsp_test_conv_empty_line.txt");
  const char *body = "1.0\n\n2.0\n";
  ASSERT_TRUE(write_temp_file(path, body, strlen(body)));

  conv_config_t params = {.type = CONV_TYPE_RAW, .format = "TEXT"};
  snprintf(params.filename, sizeof(params.filename), "%s", path);
  filter_config_t cfg = {.type = FILTER_TYPE_CONV, .parameters.conv = params};

  config_error_t err;
  config_error_init(&err);
  void *filter =
      g_convolution_vtable.create("conv_empty_line", &cfg, 0, 8, NULL, &err);
  ASSERT_TRUE(filter == NULL);
  ASSERT_TRUE(err.type != CONFIG_ERR_NONE);
  remove(path);
}

TEST(MultipleValuesOnLineIsRejected) {
  char path[512];
  snprintf(path, sizeof(path), "/tmp/cdsp_test_conv_multi_val.txt");
  const char *body = "1.0 2.0\n";
  ASSERT_TRUE(write_temp_file(path, body, strlen(body)));

  conv_config_t params = {.type = CONV_TYPE_RAW, .format = "TEXT"};
  snprintf(params.filename, sizeof(params.filename), "%s", path);
  filter_config_t cfg = {.type = FILTER_TYPE_CONV, .parameters.conv = params};

  config_error_t err;
  config_error_init(&err);
  void *filter =
      g_convolution_vtable.create("conv_multi_val", &cfg, 0, 8, NULL, &err);
  ASSERT_TRUE(filter == NULL);
  ASSERT_TRUE(err.type != CONFIG_ERR_NONE);
  remove(path);
}

TEST(HexFloatIsRejected) {
  char path[512];
  snprintf(path, sizeof(path), "/tmp/cdsp_test_conv_hex_float.txt");
  const char *body = "0x1.0p0\n";
  ASSERT_TRUE(write_temp_file(path, body, strlen(body)));

  conv_config_t params = {.type = CONV_TYPE_RAW, .format = "TEXT"};
  snprintf(params.filename, sizeof(params.filename), "%s", path);
  filter_config_t cfg = {.type = FILTER_TYPE_CONV, .parameters.conv = params};

  config_error_t err;
  config_error_init(&err);
  void *filter =
      g_convolution_vtable.create("conv_hex_float", &cfg, 0, 8, NULL, &err);
  ASSERT_TRUE(filter == NULL);
  ASSERT_TRUE(err.type != CONFIG_ERR_NONE);
  remove(path);
}

TEST(TextReadBytesLinesLimitsLines) {
  char path[512];
  snprintf(path, sizeof(path), "/tmp/cdsp_test_conv_line_limit.txt");
  const char *body = "1.0\n2.0\n3.0\n4.0\n";
  ASSERT_TRUE(write_temp_file(path, body, strlen(body)));

  conv_config_t params = {
      .type = CONV_TYPE_RAW, .format = "TEXT", .read_bytes_lines = 2};
  snprintf(params.filename, sizeof(params.filename), "%s", path);
  filter_config_t cfg = {.type = FILTER_TYPE_CONV, .parameters.conv = params};

  config_error_t err;
  config_error_init(&err);
  void *filter =
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
  char path[512];
  snprintf(path, sizeof(path), "/tmp/cdsp_test_conv_long_line.txt");
  char buf[300];
  memset(buf, ' ', 200);
  snprintf(buf + 200, sizeof(buf) - 200, "1.5\n");
  ASSERT_TRUE(write_temp_file(path, buf, strlen(buf)));

  conv_config_t params = {.type = CONV_TYPE_RAW, .format = "TEXT"};
  snprintf(params.filename, sizeof(params.filename), "%s", path);
  filter_config_t cfg = {.type = FILTER_TYPE_CONV, .parameters.conv = params};

  config_error_t err;
  config_error_init(&err);
  void *filter =
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
  char path[512];
  snprintf(path, sizeof(path), "/tmp/cdsp_test_conv_truncated.raw");
  const unsigned char body[1] = {0x7f};
  ASSERT_TRUE(write_temp_file(path, body, sizeof(body)));

  conv_config_t params = {.type = CONV_TYPE_RAW, .format = "S16LE"};
  snprintf(params.filename, sizeof(params.filename), "%s", path);
  filter_config_t cfg = {.type = FILTER_TYPE_CONV, .parameters.conv = params};

  config_error_t err;
  config_error_init(&err);
  void *filter =
      g_convolution_vtable.create("conv_truncated", &cfg, 0, 8, NULL, &err);
  ASSERT_TRUE(filter == NULL);
  ASSERT_TRUE(err.type != CONFIG_ERR_NONE);
  remove(path);
}

TEST(PartialChunkProcessesImmediatelyWithoutStaleSamples) {
  double ir[] = {1.0, 0.5};
  conv_config_t params = {
      .type = CONV_TYPE_VALUES, .values = ir, .values_count = 2};
  filter_config_t cfg = {.type = FILTER_TYPE_CONV, .parameters.conv = params};
  void *filter =
      g_convolution_vtable.create("conv_partial", &cfg, 0, 8, NULL, NULL);
  ASSERT_TRUE(filter != NULL);

  // Send a partial chunk of 4 samples (chunk_size is 8)
  double wave[4] = {1.0, 2.0, 3.0, 4.0};
  g_convolution_vtable.process(filter, wave, 4);

  // Output must be computed immediately without waiting for a full chunk or
  // emitting stale zeros
  ASSERT_NEAR(1.0, wave[0], 1e-7); // 1.0 * 1.0
  ASSERT_NEAR(2.5, wave[1], 1e-7); // 2.0 * 1.0 + 1.0 * 0.5
  ASSERT_NEAR(4.0, wave[2], 1e-7); // 3.0 * 1.0 + 2.0 * 0.5
  ASSERT_NEAR(5.5, wave[3], 1e-7); // 4.0 * 1.0 + 3.0 * 0.5

  g_convolution_vtable.free(filter);
}

// MARK: - Validation & Cache Regression Tests

static void audit_convf_tmp_path(char *buf, size_t len, const char *tag) {
  snprintf(buf, len, "/tmp/cdsp_audit_convf_%d_%s", (int)getpid(), tag);
}

static bool audit_convf_write(const char *path, const void *data, size_t len) {
  FILE *f = fopen(path, "wb");
  if (!f)
    return false;
  bool ok = len == 0 || fwrite(data, 1, len, f) == len;
  fclose(f);
  return ok;
}

static filter_config_t audit_convf_raw_cfg(const char *path,
                                           const char *format) {
  filter_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.type = FILTER_TYPE_CONV;
  cfg.parameters.conv.type = CONV_TYPE_RAW;
  snprintf(cfg.parameters.conv.filename, sizeof(cfg.parameters.conv.filename),
           "%s", path);
  snprintf(cfg.parameters.conv.format, sizeof(cfg.parameters.conv.format), "%s",
           format);
  return cfg;
}

TEST(AuditConvRawF32NaNCoefficientRejected) {
  char path[256];
  audit_convf_tmp_path(path, sizeof(path), "f3_f32.raw");
  float vals[3] = {0.5f, NAN, 0.25f};
  ASSERT_TRUE(audit_convf_write(path, vals, sizeof(vals)));
  filter_config_t cfg = audit_convf_raw_cfg(path, "F32_LE");
  config_error_t err;
  config_error_init(&err);
  int rc = g_convolution_vtable.validate(&cfg, 48000, &err);
  config_error_init(&err);
  void *f =
      g_convolution_vtable.create("audit_f3_f32", &cfg, 48000, 8, NULL, &err);
  remove(path);
  ASSERT_EQ(-1, rc);
  ASSERT_TRUE(f == NULL);
  ASSERT_TRUE(err.type != CONFIG_ERR_NONE);
}

TEST(AuditConvRawF64InfCoefficientRejected) {
  char path[256];
  audit_convf_tmp_path(path, sizeof(path), "f3_f64.raw");
  double vals[2] = {1.0, -INFINITY};
  ASSERT_TRUE(audit_convf_write(path, vals, sizeof(vals)));
  filter_config_t cfg = audit_convf_raw_cfg(path, "F64_LE");
  config_error_t err;
  config_error_init(&err);
  int rc = g_convolution_vtable.validate(&cfg, 48000, &err);
  remove(path);
  ASSERT_EQ(-1, rc);
}

TEST(AuditConvRawFiniteFloatCoefficientsAccepted) {
  char path[256];
  audit_convf_tmp_path(path, sizeof(path), "f3_ok.raw");
  float vals[2] = {0.5f, -0.25f};
  ASSERT_TRUE(audit_convf_write(path, vals, sizeof(vals)));
  filter_config_t cfg = audit_convf_raw_cfg(path, "F32_LE");
  config_error_t err;
  config_error_init(&err);
  int rc = g_convolution_vtable.validate(&cfg, 48000, &err);
  remove(path);
  ASSERT_EQ(0, rc);
}

static filter_config_t audit_convf_values_cfg(double *values, size_t n) {
  filter_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.type = FILTER_TYPE_CONV;
  cfg.parameters.conv.type = CONV_TYPE_VALUES;
  cfg.parameters.conv.values = values;
  cfg.parameters.conv.values_count = n;
  return cfg;
}

TEST(AuditConvCacheSameNameDifferentValuesNotShared) {
  double ir_a[] = {1.0};
  double ir_b[] = {0.0, 0.5};
  filter_config_t cfg_a = audit_convf_values_cfg(ir_a, 1);
  filter_config_t cfg_b = audit_convf_values_cfg(ir_b, 2);
  void *fa = g_convolution_vtable.create("audit_dup", &cfg_a, 0, 8, NULL, NULL);
  void *fb = g_convolution_vtable.create("audit_dup", &cfg_b, 0, 8, NULL, NULL);
  ASSERT_TRUE(fa != NULL);
  ASSERT_TRUE(fb != NULL);
  double wa[8] = {1.0, 0, 0, 0, 0, 0, 0, 0};
  double wb[8] = {1.0, 0, 0, 0, 0, 0, 0, 0};
  g_convolution_vtable.process(fa, wa, 8);
  g_convolution_vtable.process(fb, wb, 8);
  g_convolution_vtable.free(fa);
  g_convolution_vtable.free(fb);
  ASSERT_NEAR(1.0, wa[0], 1e-12);
  ASSERT_NEAR(0.0, wa[1], 1e-12);
  ASSERT_NEAR(0.0, wb[0], 1e-12);
  ASSERT_NEAR(0.5, wb[1], 1e-12);
}

TEST(AuditConvCacheLongNamesSharingPrefixNotShared) {
  char name_a[128];
  char name_b[128];
  memset(name_a, 'x', 100);
  memset(name_b, 'x', 100);
  name_a[100] = 'A';
  name_b[100] = 'B';
  name_a[101] = name_b[101] = '\0';
  double ir_a[] = {1.0};
  double ir_b[] = {-1.0};
  filter_config_t cfg_a = audit_convf_values_cfg(ir_a, 1);
  filter_config_t cfg_b = audit_convf_values_cfg(ir_b, 1);
  convolution_coeff_cache_begin_build_pass();
  void *fa = g_convolution_vtable.create(name_a, &cfg_a, 0, 8, NULL, NULL);
  void *fb = g_convolution_vtable.create(name_b, &cfg_b, 0, 8, NULL, NULL);
  ASSERT_TRUE(fa != NULL);
  ASSERT_TRUE(fb != NULL);
  double wa[8] = {1.0, 0, 0, 0, 0, 0, 0, 0};
  double wb[8] = {1.0, 0, 0, 0, 0, 0, 0, 0};
  g_convolution_vtable.process(fa, wa, 8);
  g_convolution_vtable.process(fb, wb, 8);
  g_convolution_vtable.free(fa);
  g_convolution_vtable.free(fb);
  ASSERT_NEAR(1.0, wa[0], 1e-12);
  ASSERT_NEAR(-1.0, wb[0], 1e-12);
}

TEST(AuditConvCacheHitDoesNotRereadFile) {
  char path[256];
  audit_convf_tmp_path(path, sizeof(path), "f6.raw");
  double vals[3] = {0.25, 0.5, 0.25};
  ASSERT_TRUE(audit_convf_write(path, vals, sizeof(vals)));
  filter_config_t cfg = audit_convf_raw_cfg(path, "F64_LE");
  convolution_coeff_cache_begin_build_pass();
  void *f1 = g_convolution_vtable.create("audit_f6", &cfg, 0, 8, NULL, NULL);
  remove(path);
  void *f2 = g_convolution_vtable.create("audit_f6", &cfg, 0, 8, NULL, NULL);
  ASSERT_TRUE(f1 != NULL);
  ASSERT_TRUE(f2 != NULL);
  double w[8] = {1.0, 0, 0, 0, 0, 0, 0, 0};
  g_convolution_vtable.process(f2, w, 8);
  g_convolution_vtable.free(f1);
  g_convolution_vtable.free(f2);
  ASSERT_NEAR(0.25, w[0], 1e-12);
  ASSERT_NEAR(0.5, w[1], 1e-12);
  ASSERT_NEAR(0.25, w[2], 1e-12);
  convolution_coeff_cache_begin_build_pass();
  config_error_t err;
  config_error_init(&err);
  void *f3 = g_convolution_vtable.create("audit_f6", &cfg, 0, 8, NULL, &err);
  ASSERT_TRUE(f3 == NULL);
  ASSERT_TRUE(err.type != CONFIG_ERR_NONE);
}

TEST_MAIN()
