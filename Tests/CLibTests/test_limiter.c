#include <math.h>

#include "Filters/filter.h"
#include "Filters/limiter.h"
#include "test_support.h"

TEST(test_hard_clip) {
  double waveform[] = {-2.0, -1.0, 0.0, 0.5, 1.5, 2.0};
  limiter_config_t params = {
      .clip_limit = -6.020599913279624,  // -6.02 dB = 0.5 linear limit
      .soft_clip = false};
  filter_config_t cfg = {.type = FILTER_TYPE_LIMITER,
                         .parameters.limiter = params};
  void* filter = g_limiter_vtable.create("limiter", &cfg, 0, 0, NULL, NULL);
  ASSERT_TRUE(filter != NULL);

  g_limiter_vtable.process(filter, waveform, 6);

  double expected[] = {-0.5, -0.5, 0.0, 0.5, 0.5, 0.5};
  for (size_t i = 0; i < 6; i++) {
    ASSERT_NEAR(expected[i], waveform[i], 1e-5);
  }
  g_limiter_vtable.free(filter);
}

TEST(test_soft_clip) {
  double waveform[] = {-2.0, -0.5, 0.0, 0.5, 2.0};
  limiter_config_t params = {.clip_limit = 0.0,  // 0 dB = 1.0 linear limit
                             .soft_clip = true};
  filter_config_t cfg = {.type = FILTER_TYPE_LIMITER,
                         .parameters.limiter = params};
  void* filter = g_limiter_vtable.create("limiter", &cfg, 0, 0, NULL, NULL);
  ASSERT_TRUE(filter != NULL);

  g_limiter_vtable.process(filter, waveform, 5);

  double expected[] = {-1.0, -0.481481, 0.0, 0.481481, 1.0};
  for (size_t i = 0; i < 5; i++) {
    ASSERT_NEAR(expected[i], waveform[i], 1e-5);
  }
  g_limiter_vtable.free(filter);
}

TEST_MAIN()
