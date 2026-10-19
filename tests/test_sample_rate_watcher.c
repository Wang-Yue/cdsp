#include <stdbool.h>
#include <stdio.h>

#include "engine/sample_rate_watcher.h"
#include "test_support.h"
#include "utils/cdsp_time.h"

TEST(SampleRateWatcherBasicNoChange) {
  sample_rate_watcher_t *watcher =
      sample_rate_watcher_create(44100.0, 0.1, true);
  ASSERT_TRUE(watcher != NULL);

  bool stop_opt = sample_rate_watcher_get_stop_on_rate_change(watcher);
  ASSERT_TRUE(stop_opt);

  double measured_rate = 0.0;
  // Send expected frames matching 44.1kHz rate over 100ms intervals
  for (int i = 0; i < 10; i++) {
    bool changed = sample_rate_watcher_tick(watcher, 4410, &measured_rate);
    ASSERT_FALSE(changed);
  }

  sample_rate_watcher_reset(watcher);
  sample_rate_watcher_free(watcher);
}

TEST(SampleRateWatcherRateChangeDetection) {
  sample_rate_watcher_t *watcher =
      sample_rate_watcher_create(44100.0, 0.15, false);
  ASSERT_TRUE(watcher != NULL);

  double measured_rate = 0.0;
  bool change_detected = false;
  for (int i = 0; i < 20; i++) {
    cdsp_sleep_ms(200);
    if (sample_rate_watcher_tick(watcher, 20000, &measured_rate)) {
      change_detected = true;
      break;
    }
  }
  ASSERT_TRUE(change_detected);

  sample_rate_watcher_free(watcher);
}

TEST(SampleRateWatcherIgnoresAlternatingValues) {
  sample_rate_watcher_t *watcher =
      sample_rate_watcher_create(44100.0, 0.1, false);
  ASSERT_TRUE(watcher != NULL);

  double measured_rate = 0.0;
  // First tick arms the watcher
  ASSERT_FALSE(sample_rate_watcher_tick(watcher, 4410, &measured_rate));

  // Alternate high and low. Because direction flips every tick, consecutive
  // deviations in the same direction never reach 3.
  for (int i = 0; i < 6; i++) {
    cdsp_sleep_ms(250);
    size_t frames = (i % 2 == 0) ? 500000 : 10;
    ASSERT_FALSE(sample_rate_watcher_tick(watcher, frames, &measured_rate));
  }

  sample_rate_watcher_free(watcher);
}

TEST(SampleRateWatcherRestartsCountOnDirectionChange) {
  sample_rate_watcher_t *watcher =
      sample_rate_watcher_create(44100.0, 0.1, false);
  ASSERT_TRUE(watcher != NULL);

  double measured_rate = 0.0;
  // Arm the watcher
  ASSERT_FALSE(sample_rate_watcher_tick(watcher, 0, &measured_rate));

  // Two high ticks (count = 1, 2)
  cdsp_sleep_ms(250);
  ASSERT_FALSE(sample_rate_watcher_tick(watcher, 500000, &measured_rate));
  cdsp_sleep_ms(250);
  ASSERT_FALSE(sample_rate_watcher_tick(watcher, 500000, &measured_rate));

  // Flip to low: restarts count to 1 (does not trigger at 3)
  cdsp_sleep_ms(250);
  ASSERT_FALSE(sample_rate_watcher_tick(watcher, 10, &measured_rate));
  // Second low tick (count = 2)
  cdsp_sleep_ms(250);
  ASSERT_FALSE(sample_rate_watcher_tick(watcher, 10, &measured_rate));
  // Third consecutive low tick triggers (count = 3)
  cdsp_sleep_ms(250);
  ASSERT_TRUE(sample_rate_watcher_tick(watcher, 10, &measured_rate));

  sample_rate_watcher_free(watcher);
}

TEST(SampleRateWatcherResetClearsCount) {
  sample_rate_watcher_t *watcher =
      sample_rate_watcher_create(44100.0, 0.1, false);
  ASSERT_TRUE(watcher != NULL);

  double measured_rate = 0.0;
  ASSERT_FALSE(sample_rate_watcher_tick(watcher, 0, &measured_rate));
  cdsp_sleep_ms(250);
  ASSERT_FALSE(sample_rate_watcher_tick(watcher, 500000, &measured_rate));
  cdsp_sleep_ms(250);
  ASSERT_FALSE(sample_rate_watcher_tick(watcher, 500000, &measured_rate));

  sample_rate_watcher_reset(watcher);

  // Two high ticks after reset should not trigger yet
  cdsp_sleep_ms(250);
  ASSERT_FALSE(sample_rate_watcher_tick(watcher, 500000, &measured_rate));
  cdsp_sleep_ms(250);
  ASSERT_FALSE(sample_rate_watcher_tick(watcher, 500000, &measured_rate));
  // Third high tick after reset triggers
  cdsp_sleep_ms(250);
  ASSERT_TRUE(sample_rate_watcher_tick(watcher, 500000, &measured_rate));

  sample_rate_watcher_free(watcher);
}

TEST_MAIN()
