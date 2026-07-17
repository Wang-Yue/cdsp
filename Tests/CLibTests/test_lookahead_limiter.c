#include <math.h>

#include "Filters/clipper.h"
#include "Filters/filter.h"
#include "Filters/lookahead_limiter.h"
#include "test_support.h"

static bool is_close(double left, double right, double maxdiff) {
  return fabs(left - right) < maxdiff;
}

static bool compare_waveforms(const double* left, const double* right,
                              size_t count, double maxdiff) {
  for (size_t i = 0; i < count; i++) {
    if (!is_close(left[i], right[i], maxdiff)) return false;
  }
  return true;
}

TEST(test_lookahead_limiter_basic) {
  lookahead_limiter_config_t params = {.limit = 0.0,
                                       .attack = 4.0,
                                       .attack_unit = TIME_UNIT_SAMPLES,
                                       .release = 1.0 / log(2.0),
                                       .release_unit = TIME_UNIT_SAMPLES};
  filter_config_t cfg = {.type = FILTER_TYPE_LOOKAHEAD_LIMITER,
                         .parameters.lookahead_limiter = params};
  void* filter = g_lookahead_limiter_vtable.create("lookahead_limiter", &cfg,
                                                   48000, 1024, NULL, NULL);
  ASSERT_TRUE(filter != NULL);

  double input[] = {1.0, 1.0, 1.0, 1.0, 1.0, 2.0, -2.0, 1.0, 1.0, 2.0,
                    1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0,  1.0, 1.0};
  double expected[] = {0.0,
                       0.0,
                       0.0,
                       0.0,
                       1.0,
                       1.0,
                       0.875,
                       0.75,
                       0.625,
                       1.0,
                       -1.0,
                       pow(0.5, 1.0 / 2.0),
                       0.625,
                       1.0,
                       pow(0.5, 1.0 / 2.0),
                       pow(0.5, 1.0 / 4.0),
                       pow(0.5, 1.0 / 8.0),
                       pow(0.5, 1.0 / 16.0),
                       pow(0.5, 1.0 / 32.0)};

  g_lookahead_limiter_vtable.process(filter, input, 19);
  ASSERT_TRUE(compare_waveforms(input, expected, 19, 1e-6));
  g_lookahead_limiter_vtable.free(filter);
}

TEST(test_lookahead_limiter_same_as_limiter) {
  lookahead_limiter_config_t params_lookahead = {
      .limit = 0.0,
      .attack = 0.0,
      .attack_unit = TIME_UNIT_SAMPLES,
      .release = 0.0,
      .release_unit = TIME_UNIT_SAMPLES};
  filter_config_t cfg_lookahead = {
      .type = FILTER_TYPE_LOOKAHEAD_LIMITER,
      .parameters.lookahead_limiter = params_lookahead};
  void* filter_lookahead = g_lookahead_limiter_vtable.create(
      "lookahead", &cfg_lookahead, 48000, 1024, NULL, NULL);
  ASSERT_TRUE(filter_lookahead != NULL);

  clipper_config_t params_clipper = {.clip_limit = 0.0, .soft_clip = false};
  filter_config_t cfg_clipper = {.type = FILTER_TYPE_CLIPPER,
                                 .parameters.clipper = params_clipper};
  void* filter_clipper =
      g_clipper_vtable.create("clipper", &cfg_clipper, 0, 0, NULL, NULL);
  ASSERT_TRUE(filter_clipper != NULL);

  double lookahead_input[] = {0.5, 1.0, 2.0, -2.0, -1.0, -0.5, 1.5, -1.5, 0.0};
  double limiter_input[] = {0.5, 1.0, 2.0, -2.0, -1.0, -0.5, 1.5, -1.5, 0.0};

  g_lookahead_limiter_vtable.process(filter_lookahead, lookahead_input, 9);
  g_clipper_vtable.process(filter_clipper, limiter_input, 9);

  for (size_t i = 0; i < 9; i++) {
    ASSERT_DOUBLE_EQ(limiter_input[i], lookahead_input[i]);
  }

  g_lookahead_limiter_vtable.free(filter_lookahead);
  g_clipper_vtable.free(filter_clipper);
}

TEST(test_lookahead_limiter_zero_release) {
  lookahead_limiter_config_t params = {.limit = 0.0,
                                       .attack = 2.0,
                                       .attack_unit = TIME_UNIT_SAMPLES,
                                       .release = 0.0,
                                       .release_unit = TIME_UNIT_SAMPLES};
  filter_config_t cfg = {.type = FILTER_TYPE_LOOKAHEAD_LIMITER,
                         .parameters.lookahead_limiter = params};
  void* filter = g_lookahead_limiter_vtable.create("lookahead", &cfg, 48000,
                                                   1024, NULL, NULL);
  ASSERT_TRUE(filter != NULL);
  double input[] = {1.0, 1.0, 1.0, 2.0, 2.0, 2.0, 2.0, 2.0, 1.0, 1.0, 1.0};
  g_lookahead_limiter_vtable.process(filter, input, 11);
  for (size_t i = 0; i < 11; i++) {
    ASSERT_TRUE(fabs(input[i]) <= 1.0);
  }
  g_lookahead_limiter_vtable.free(filter);
}

TEST(test_lookahead_limiter_state_persistence) {
  lookahead_limiter_config_t params = {.limit = 0.0,
                                       .attack = 5.0,
                                       .attack_unit = TIME_UNIT_SAMPLES,
                                       .release = 1.0 / log(2.0),
                                       .release_unit = TIME_UNIT_SAMPLES};
  filter_config_t cfg = {.type = FILTER_TYPE_LOOKAHEAD_LIMITER,
                         .parameters.lookahead_limiter = params};
  void* filter = g_lookahead_limiter_vtable.create("lookahead", &cfg, 48000,
                                                   1024, NULL, NULL);
  ASSERT_TRUE(filter != NULL);

  double buf1[] = {1.0, 1.0, 1.0, 1.0, 1.0, 2.0, 1.0, 1.0, 1.0, 1.0, 1.0};
  double expected1[] = {0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.9, 0.8, 0.7, 0.6, 1.0};
  g_lookahead_limiter_vtable.process(filter, buf1, 11);
  ASSERT_TRUE(compare_waveforms(buf1, expected1, 11, 1e-6));

  double buf2[] = {1.0, 1.0, 1.0, 1.0};
  double expected2[] = {pow(0.5, 1.0 / 2.0), pow(0.5, 1.0 / 4.0),
                        pow(0.5, 1.0 / 8.0), pow(0.5, 1.0 / 16.0)};
  g_lookahead_limiter_vtable.process(filter, buf2, 4);
  ASSERT_TRUE(compare_waveforms(buf2, expected2, 4, 1e-6));

  g_lookahead_limiter_vtable.free(filter);
}

TEST(test_lookahead_limiter_attack_over_one_second_rejected) {
  lookahead_limiter_config_t params = {.limit = 0.0,
                                       .attack = 48001.0,
                                       .attack_unit = TIME_UNIT_SAMPLES,
                                       .release = 4.0,
                                       .release_unit = TIME_UNIT_SAMPLES};
  filter_config_t cfg = {.type = FILTER_TYPE_LOOKAHEAD_LIMITER,
                         .parameters.lookahead_limiter = params};
  ASSERT_NE(0, g_lookahead_limiter_vtable.validate(&cfg, 48000, NULL));
}

TEST(test_lookahead_limiter_chunksize_larger_than_samplerate) {
  lookahead_limiter_config_t params = {.limit = 0.0,
                                       .attack = 4.0,
                                       .attack_unit = TIME_UNIT_SAMPLES,
                                       .release = 1.0,
                                       .release_unit = TIME_UNIT_SAMPLES};
  filter_config_t cfg = {.type = FILTER_TYPE_LOOKAHEAD_LIMITER,
                         .parameters.lookahead_limiter = params};
  void* filter =
      g_lookahead_limiter_vtable.create("lookahead", &cfg, 4, 8, NULL, NULL);
  ASSERT_TRUE(filter != NULL);
  double input[] = {1.0, 1.0, 2.0, 1.0, 1.0, -2.0, 1.0, 1.0};
  g_lookahead_limiter_vtable.process(filter, input, 8);
  g_lookahead_limiter_vtable.free(filter);
}

TEST_MAIN()
