#include <stdbool.h>
#include <stddef.h>

#include "config/filter_config_types.h"
#include "filters/clipper.h"
#include "filters/filter.h"
#include "test_support.h"

TEST(test_hard_clip) {
  double waveform[] = {-2.0, -1.0, 0.0, 0.5, 1.5, 2.0};
  clipper_config_t params = {
      .clip_limit = -6.020599913279624, // -6.02 dB = 0.5 linear limit
      .soft_clip = false};
  filter_config_t cfg = {.type = FILTER_TYPE_CLIPPER,
                         .parameters.clipper = params};
  void *filter = g_clipper_vtable.create("clipper", &cfg, 0, 0, NULL, NULL);
  ASSERT_TRUE(filter != NULL);

  g_clipper_vtable.process(filter, waveform, 6);

  double expected[] = {-0.5, -0.5, 0.0, 0.5, 0.5, 0.5};
  for (size_t i = 0; i < 6; i++) {
    ASSERT_NEAR(expected[i], waveform[i], 1e-5);
  }
  g_clipper_vtable.free(filter);
}

TEST(test_soft_clip) {
  double waveform[] = {-2.0, -0.5, 0.0, 0.5, 2.0};
  clipper_config_t params = {.clip_limit = 0.0, // 0 dB = 1.0 linear limit
                             .soft_clip = true};
  filter_config_t cfg = {.type = FILTER_TYPE_CLIPPER,
                         .parameters.clipper = params};
  void *filter = g_clipper_vtable.create("clipper", &cfg, 0, 0, NULL, NULL);
  ASSERT_TRUE(filter != NULL);

  g_clipper_vtable.process(filter, waveform, 5);

  double expected[] = {-1.0, -0.481481, 0.0, 0.481481, 1.0};
  for (size_t i = 0; i < 5; i++) {
    ASSERT_NEAR(expected[i], waveform[i], 1e-5);
  }
  g_clipper_vtable.free(filter);
}

TEST(test_clipper_validation_and_extended_range) {
  config_error_t err;
  memset(&err, 0, sizeof(err));

  // clip_limit = +30 dB (outside old [-120, +20] range) must be accepted
  clipper_config_t params_high = {.clip_limit = 30.0, .soft_clip = false};
  filter_config_t cfg_high = {.type = FILTER_TYPE_CLIPPER,
                              .parameters.clipper = params_high};
  ASSERT_EQ(0, g_clipper_vtable.validate(&cfg_high, 44100, &err));

  // clip_limit = -150 dB (outside old [-120, +20] range) must be accepted
  clipper_config_t params_low = {.clip_limit = -150.0, .soft_clip = false};
  filter_config_t cfg_low = {.type = FILTER_TYPE_CLIPPER,
                             .parameters.clipper = params_low};
  ASSERT_EQ(0, g_clipper_vtable.validate(&cfg_low, 44100, &err));

  // NaN or Inf should be rejected
  clipper_config_t params_nan = {.clip_limit = 0.0 / 0.0, .soft_clip = false};
  filter_config_t cfg_nan = {.type = FILTER_TYPE_CLIPPER,
                             .parameters.clipper = params_nan};
  ASSERT_EQ(-1, g_clipper_vtable.validate(&cfg_nan, 44100, &err));
}

TEST_MAIN()
