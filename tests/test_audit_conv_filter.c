// Regression tests for audit report 03: convolution filter behaviour.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "config/config_error.h"
#include "config/config_gen.h"
#include "filters/convolution.h"
#include "filters/filter.h"
#include "test_support.h"

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

// MARK: - F3: non-finite binary float coefficients must be rejected

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

// MARK: - F2: cache must key on the coefficient parameters and full name

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
  // No build pass is opened: both live at the same cache generation.
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

// MARK: - F6: cache hit must not touch the file system again

TEST(AuditConvCacheHitDoesNotRereadFile) {
  char path[256];
  audit_convf_tmp_path(path, sizeof(path), "f6.raw");
  double vals[3] = {0.25, 0.5, 0.25};
  ASSERT_TRUE(audit_convf_write(path, vals, sizeof(vals)));
  filter_config_t cfg = audit_convf_raw_cfg(path, "F64_LE");
  convolution_coeff_cache_begin_build_pass();
  void *f1 = g_convolution_vtable.create("audit_f6", &cfg, 0, 8, NULL, NULL);
  remove(path);
  // Channel 2 of the same step: the file is gone, the cached spectrum is not.
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
  // A new build pass must read the file again (and fail now it is gone).
  convolution_coeff_cache_begin_build_pass();
  config_error_t err;
  config_error_init(&err);
  void *f3 = g_convolution_vtable.create("audit_f6", &cfg, 0, 8, NULL, &err);
  ASSERT_TRUE(f3 == NULL);
  ASSERT_TRUE(err.type != CONFIG_ERR_NONE);
}

TEST_MAIN()
