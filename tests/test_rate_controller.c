#if defined(__linux__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif
#include <stddef.h>

#include "engine/rate_controller.h"
#include "test_support.h"
#include "utils/cdsp_time.h"

TEST(ReturnsUnitySpeedAtTarget) {
  pi_rate_controller_t *pi =
      pi_rate_controller_create_default(48000, 1.0, 1024);
  ASSERT_TRUE(pi != NULL);
  double speed = pi_rate_controller_next(pi, 1024.0);
  ASSERT_NEAR(1.0, speed, 1e-12);
  pi_rate_controller_free(pi);
}

TEST(HighBufferTriggersSlowdown) {
  pi_rate_controller_t *pi =
      pi_rate_controller_create_default(48000, 1.0, 1024);
  ASSERT_TRUE(pi != NULL);
  double last_speed = 1.0;
  for (int i = 0; i < 25; i++) {
    last_speed = pi_rate_controller_next(pi, 4096.0);
  }
  ASSERT_TRUE(last_speed < 1.0);
  ASSERT_TRUE(last_speed >= 1.0 - 0.005);
  pi_rate_controller_free(pi);
}

TEST(LowBufferTriggersSpeedup) {
  pi_rate_controller_t *pi =
      pi_rate_controller_create_default(48000, 1.0, 1024);
  ASSERT_TRUE(pi != NULL);
  double last_speed = 1.0;
  for (int i = 0; i < 25; i++) {
    last_speed = pi_rate_controller_next(pi, 256.0);
  }
  ASSERT_TRUE(last_speed > 1.0);
  ASSERT_TRUE(last_speed <= 1.0 + 0.005);
  pi_rate_controller_free(pi);
}

TEST(OutputAlwaysClampedWithin5PerMille) {
  pi_rate_controller_t *pi =
      pi_rate_controller_create_default(48000, 1.0, 1024);
  ASSERT_TRUE(pi != NULL);
  for (int i = 0; i < 200; i++) {
    pi_rate_controller_next(pi, 1000000.0);
  }
  double speed = pi_rate_controller_next(pi, 1000000.0);
  ASSERT_TRUE(speed >= 1.0 - 0.005);
  ASSERT_TRUE(speed <= 1.0 + 0.005);
  pi_rate_controller_free(pi);
}

static bool at_limit(double speed) {
  return fabs(fabs(1.0 - speed) - 0.005) < 1e-12;
}

// Default gains, no ramp, so the error is just level - target.
static pi_rate_controller_t *make_test_controller(void) {
  return pi_rate_controller_create(48000, 10.0, 10000, 0.2, 0.004, 0, 0.33);
}

TEST(SmallErrorsArePlainPI) {
  pi_rate_controller_t *pi = make_test_controller();
  ASSERT_TRUE(pi != NULL);
  double levels[] = {10100.0, 10200.0, 9900.0, 10050.0, 9800.0};
  double accumulated = 0.0;
  for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
    double rel_err = (levels[i] - 10000.0) / 480000.0;
    accumulated += rel_err * 10.0;
    double expected = 1.0 - (0.2 * rel_err + 0.004 * accumulated);
    ASSERT_NEAR(expected, pi_rate_controller_next(pi, levels[i]), 1e-15);
  }
  pi_rate_controller_free(pi);
}

TEST(LeavesTheLimitAsSoonAsTheErrorTurns) {
  // The defaults: chunksize 1024 as the target, a 10 s adjust period. An error
  // can not be larger than the target on the empty side.
  pi_rate_controller_t *pi =
      pi_rate_controller_create(48000, 10.0, 1024, 0.2, 0.004, 0, 0.33);
  ASSERT_TRUE(pi != NULL);
  // Too full for a long time: held at the limit.
  for (int i = 0; i < 1000; i++) {
    pi_rate_controller_next(pi, 1624.0);
  }
  ASSERT_TRUE(at_limit(pi_rate_controller_next(pi, 1624.0)));
  // Now empty: the first reading comes off the limit, and the controller asks
  // for the other side once the integral has come down, about 51 periods at
  // these gains. An integral wound up over the 1000 periods would take about
  // 580, and one wound up for a day several thousand.
  double first = pi_rate_controller_next(pi, 0.0);
  ASSERT_FALSE(at_limit(first));
  int periods = 1;
  double speed = first;
  while (speed < 1.0) {
    speed = pi_rate_controller_next(pi, 0.0);
    periods += 1;
    ASSERT_TRUE(periods <= 60);
  }
  pi_rate_controller_free(pi);
}

TEST(IntegralUnwindsFromSaturation) {
  pi_rate_controller_t *pi = make_test_controller();
  ASSERT_TRUE(pi != NULL);
  for (int i = 0; i < 1000; i++) {
    pi_rate_controller_next(pi, 13000.0);
  }
  // Integrating into saturation is stopped, so what was built up is no more
  // than the limit's worth (measured at target where proportional error is 0).
  double speed_at_target = pi_rate_controller_next(pi, 10000.0);
  ASSERT_TRUE(fabs(1.0 - speed_at_target) <= 0.005 + 1e-12);
  pi_rate_controller_free(pi);
}

TEST(AveragerReturnsNilWhenEmpty) {
  averager_t avg;
  averager_init(&avg);
  double val = 0.0;
  ASSERT_FALSE(averager_get_average(&avg, &val));
}

TEST(AveragerComputesMean) {
  averager_t avg;
  averager_init(&avg);
  averager_add(&avg, 10.0);
  averager_add(&avg, 20.0);
  averager_add(&avg, 30.0);
  double val = 0.0;
  ASSERT_TRUE(averager_get_average(&avg, &val));
  ASSERT_NEAR(20.0, val, 1e-12);
}

TEST(AveragerRestartClearsState) {
  averager_t avg;
  averager_init(&avg);
  averager_add(&avg, 100.0);
  averager_restart(&avg);
  double val = 0.0;
  ASSERT_FALSE(averager_get_average(&avg, &val));
  averager_add(&avg, 5.0);
  ASSERT_TRUE(averager_get_average(&avg, &val));
  ASSERT_DOUBLE_EQ(5.0, val);
}

TEST(StopwatchElapsesMonotonically) {
  stopwatch_t sw;
  stopwatch_init(&sw);
  double t0 = stopwatch_elapsed_seconds(&sw);
  cdsp_sleep_ms(10);
  double t1 = stopwatch_elapsed_seconds(&sw);
  ASSERT_TRUE(t1 > t0);
  stopwatch_restart(&sw);
  double t2 = stopwatch_elapsed_seconds(&sw);
  ASSERT_TRUE(t2 < t1);
}

TEST_MAIN()
