// Regression tests for audit report 02 (basic & dynamics filters):
// parameter validation of the C API, which has no FiniteF64 / enum guards
// like upstream's serde deserialization.
#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "config/config_error.h"
#include "config/config_gen.h"
#include "filters/delay.h"
#include "filters/dither.h"
#include "filters/filter.h"
#include "filters/gain.h"
#include "filters/lookahead_limiter.h"
#include "test_support.h"

// MARK: - §3.1 / §3.2 LookaheadLimiter

static filter_config_t make_limiter(double limit, double attack,
                                    time_unit_t attack_unit, double release,
                                    time_unit_t release_unit) {
  filter_config_t cfg = {.type = FILTER_TYPE_LOOKAHEAD_LIMITER};
  cfg.parameters.lookahead_limiter.limit = limit;
  cfg.parameters.lookahead_limiter.attack = attack;
  cfg.parameters.lookahead_limiter.attack_unit = attack_unit;
  cfg.parameters.lookahead_limiter.release = release;
  cfg.parameters.lookahead_limiter.release_unit = release_unit;
  return cfg;
}

TEST(test_audit_limiter_accepts_valid) {
  filter_config_t cfg =
      make_limiter(-1.0, 5.0, TIME_UNIT_MS, 50.0, TIME_UNIT_MS);
  ASSERT_EQ(0, filter_config_validate(&cfg, 48000, NULL));
  // Zero release (instant) is allowed upstream too.
  cfg = make_limiter(-1.0, 0.0, TIME_UNIT_MS, 0.0, TIME_UNIT_MS);
  ASSERT_EQ(0, filter_config_validate(&cfg, 48000, NULL));
}

TEST(test_audit_limiter_rejects_non_finite) {
  const double bad[] = {NAN, INFINITY, -INFINITY};
  for (size_t i = 0; i < 3; i++) {
    filter_config_t cfg =
        make_limiter(bad[i], 5.0, TIME_UNIT_MS, 50.0, TIME_UNIT_MS);
    ASSERT_EQ(-1, filter_config_validate(&cfg, 48000, NULL));
    cfg = make_limiter(-1.0, bad[i], TIME_UNIT_MS, 50.0, TIME_UNIT_MS);
    ASSERT_EQ(-1, filter_config_validate(&cfg, 48000, NULL));
    cfg = make_limiter(-1.0, 5.0, TIME_UNIT_MS, bad[i], TIME_UNIT_MS);
    ASSERT_EQ(-1, filter_config_validate(&cfg, 48000, NULL));
    config_error_t err = {0};
    ASSERT_TRUE(g_lookahead_limiter_vtable.create("lim", &cfg, 48000, 1024,
                                                  NULL, &err) == NULL);
  }
}

TEST(test_audit_limiter_rejects_invalid_units) {
  filter_config_t cfg =
      make_limiter(-1.0, 5.0, TIME_UNIT_INVALID, 50.0, TIME_UNIT_MS);
  ASSERT_EQ(-1, filter_config_validate(&cfg, 48000, NULL));
  cfg = make_limiter(-1.0, 5.0, TIME_UNIT_MS, 50.0, TIME_UNIT_INVALID);
  ASSERT_EQ(-1, filter_config_validate(&cfg, 48000, NULL));
  cfg = make_limiter(-1.0, 5.0, (time_unit_t)42, 50.0, TIME_UNIT_MS);
  ASSERT_EQ(-1, filter_config_validate(&cfg, 48000, NULL));
}

// MARK: - §4.1 Delay

static filter_config_t make_delay(double delay, delay_unit_t unit) {
  filter_config_t cfg = {.type = FILTER_TYPE_DELAY};
  cfg.parameters.delay.delay = delay;
  cfg.parameters.delay.delay_unit = unit;
  cfg.parameters.delay.subsample = false;
  return cfg;
}

TEST(test_audit_delay_samples_cap_without_rate) {
  filter_config_t cfg = make_delay(1e12, DELAY_UNIT_SAMPLES);
  ASSERT_EQ(-1, filter_config_validate(&cfg, 0, NULL));
  ASSERT_EQ(-1, filter_config_validate(&cfg, 48000, NULL));
  ASSERT_TRUE(g_delay_vtable.create("d", &cfg, 0, 0, NULL, NULL) == NULL);
  // Rate-dependent units still pass rate-independent validation.
  cfg = make_delay(10.0, DELAY_UNIT_MS);
  ASSERT_EQ(0, filter_config_validate(&cfg, 0, NULL));
}

TEST(test_audit_delay_rejects_invalid_unit_and_rate) {
  filter_config_t cfg = make_delay(10.0, DELAY_UNIT_INVALID);
  ASSERT_EQ(-1, filter_config_validate(&cfg, 48000, NULL));
  cfg = make_delay(10.0, (delay_unit_t)99);
  ASSERT_EQ(-1, filter_config_validate(&cfg, 48000, NULL));
  cfg = make_delay(10.0, DELAY_UNIT_SAMPLES);
  ASSERT_TRUE(g_delay_vtable.create("d", &cfg, 0, 0, NULL, NULL) == NULL);
  void *ok = g_delay_vtable.create("d", &cfg, 48000, 0, NULL, NULL);
  ASSERT_TRUE(ok != NULL);
  g_delay_vtable.free(ok);
}

// MARK: - §5.1 Dither

static filter_config_t make_dither(dither_type_t type, int bits,
                                   double amplitude) {
  filter_config_t cfg = {.type = FILTER_TYPE_DITHER};
  cfg.parameters.dither.type = type;
  cfg.parameters.dither.bits = bits;
  cfg.parameters.dither.has_amplitude = true;
  cfg.parameters.dither.amplitude = amplitude;
  return cfg;
}

TEST(test_audit_dither_validation) {
  filter_config_t cfg = make_dither(DITHER_TYPE_FLAT, 16, 2.0);
  ASSERT_EQ(0, filter_config_validate(&cfg, 48000, NULL));
  cfg = make_dither(DITHER_TYPE_FLAT, 16, NAN);
  ASSERT_EQ(-1, filter_config_validate(&cfg, 48000, NULL));
  ASSERT_TRUE(g_dither_vtable.create("d", &cfg, 48000, 64, NULL, NULL) == NULL);
  cfg = make_dither(DITHER_TYPE_FLAT, 16, INFINITY);
  ASSERT_EQ(-1, filter_config_validate(&cfg, 48000, NULL));
  cfg = make_dither((dither_type_t)1000, 16, 2.0);
  ASSERT_EQ(-1, filter_config_validate(&cfg, 48000, NULL));
  cfg = make_dither(DITHER_TYPE_SHIBATA_LOW_192, 24, 0.0);
  ASSERT_EQ(0, filter_config_validate(&cfg, 48000, NULL));
  cfg = make_dither(DITHER_TYPE_HIGHPASS, 2000, 0.0);
  ASSERT_EQ(-1, filter_config_validate(&cfg, 48000, NULL));
}

// MARK: - §6.1 Gain

TEST(test_audit_gain_rejects_invalid_scale) {
  filter_config_t cfg = {.type = FILTER_TYPE_GAIN};
  cfg.parameters.gain.gain = 6.0;
  cfg.parameters.gain.scale = GAIN_SCALE_LINEAR;
  ASSERT_EQ(0, filter_config_validate(&cfg, 48000, NULL));
  cfg.parameters.gain.scale = (gain_scale_t)7;
  ASSERT_EQ(-1, filter_config_validate(&cfg, 48000, NULL));
  ASSERT_TRUE(g_gain_vtable.create("g", &cfg, 48000, 64, NULL, NULL) == NULL);
}

// MARK: - §6.3 filter.c type dispatch

TEST(test_audit_filter_rejects_invalid_type) {
  filter_config_t cfg = {.type = FILTER_TYPE_INVALID};
  config_error_t err;
  config_error_init(&err);
  ASSERT_EQ(-1, filter_config_validate(&cfg, 48000, &err));
  ASSERT_TRUE(filter_create("x", &cfg, 48000, 64, NULL, NULL) == NULL);
  cfg.type = (filter_type_t)1234;
  ASSERT_EQ(-1, filter_config_validate(&cfg, 48000, NULL));
  ASSERT_TRUE(filter_create("x", &cfg, 48000, 64, NULL, NULL) == NULL);
}

// MARK: - Clipper linear-limit overflow

TEST(test_audit_clipper_rejects_overflowing_limit) {
  filter_config_t cfg = {.type = FILTER_TYPE_CLIPPER};
  cfg.parameters.clipper.clip_limit = -1.0;
  ASSERT_EQ(0, filter_config_validate(&cfg, 48000, NULL));
  cfg.parameters.clipper.clip_limit = 10000.0;
  ASSERT_EQ(-1, filter_config_validate(&cfg, 48000, NULL));
}

TEST_MAIN()
