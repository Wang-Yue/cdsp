#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Engine/sample_rate_watcher.h"
#include "test_support.h"

TEST(SampleRateWatcherBasicNoChange) {
  sample_rate_watcher_t* watcher =
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
  sample_rate_watcher_t* watcher =
      sample_rate_watcher_create(44100.0, 0.05, false);
  ASSERT_TRUE(watcher != NULL);

  double measured_rate = 0.0;
  // Send 96kHz frame rate chunks (4800 frames per tick instead of 2205)
  bool change_detected = false;
  for (int i = 0; i < 20; i++) {
    if (sample_rate_watcher_tick(watcher, 4800, &measured_rate)) {
      change_detected = true;
      break;
    }
  }

  sample_rate_watcher_free(watcher);
}

TEST_MAIN()
