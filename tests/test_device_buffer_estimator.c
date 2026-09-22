#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "test_support.h"
#include "utils/cdsp_time.h"
#include "utils/device_buffer_estimator.h"

// These tests express durations through cdsp_sleep_ms / cdsp_time_now_ns
// rather than real wall-clock time. Both are scaled by the same factor in test
// builds, so sleeping for N "milliseconds" advances the clock the estimator
// reads by roughly N milliseconds. Bounds are kept loose because the scaling
// amplifies scheduling jitter by the same factor.

TEST(DeviceBufferEstimator_UnpublishedReadsZero) {
  device_buffer_estimator_t est;
  device_buffer_estimator_init(&est, 48000.0);

  // Nothing published yet, so there is no level to extrapolate from.
  ASSERT_EQ((size_t)0, device_buffer_estimator_estimate(&est));
}

TEST(DeviceBufferEstimator_NullIsSafe) {
  ASSERT_EQ((size_t)0, device_buffer_estimator_estimate(NULL));
  // Must not crash.
  device_buffer_estimator_init(NULL, 48000.0);
  device_buffer_estimator_reset(NULL);
  device_buffer_estimator_set_rate(NULL, 44100.0);
  device_buffer_estimator_add(NULL, 128);
}

TEST(DeviceBufferEstimator_ReportsJustPublishedLevel) {
  device_buffer_estimator_t est;
  device_buffer_estimator_init(&est, 48000.0);

  // One second of audio; almost none of it can have drained yet.
  device_buffer_estimator_add(&est, 48000);
  size_t level = device_buffer_estimator_estimate(&est);
  ASSERT_TRUE(level <= 48000);
  ASSERT_TRUE(level > 47000);
}

TEST(DeviceBufferEstimator_DecaysOverTime) {
  device_buffer_estimator_t est;
  device_buffer_estimator_init(&est, 48000.0);
  device_buffer_estimator_add(&est, 48000);

  cdsp_sleep_ms(500);

  // Roughly half the buffer should have drained. Wide bounds, but enough to
  // prove the value is actually being extrapolated rather than held constant.
  size_t level = device_buffer_estimator_estimate(&est);
  ASSERT_TRUE(level < 40000);
  ASSERT_TRUE(level > 4000);
}

TEST(DeviceBufferEstimator_SaturatesAtZero) {
  device_buffer_estimator_t est;
  device_buffer_estimator_init(&est, 48000.0);

  // 100 ms of audio, left to drain for far longer than that.
  device_buffer_estimator_add(&est, 4800);
  cdsp_sleep_ms(1000);

  // The engine's playback drain loop waits for this to reach exactly zero.
  ASSERT_EQ((size_t)0, device_buffer_estimator_estimate(&est));
}

TEST(DeviceBufferEstimator_ResetClearsLevel) {
  device_buffer_estimator_t est;
  device_buffer_estimator_init(&est, 48000.0);
  device_buffer_estimator_add(&est, 48000);
  ASSERT_TRUE(device_buffer_estimator_estimate(&est) > 0);

  device_buffer_estimator_reset(&est);

  // A stale timestamp surviving a device restart would make the first estimate
  // of the next session collapse to zero, so reset must clear it outright.
  ASSERT_EQ((size_t)0, device_buffer_estimator_estimate(&est));
}

TEST(DeviceBufferEstimator_SetRateClearsLevel) {
  device_buffer_estimator_t est;
  device_buffer_estimator_init(&est, 48000.0);
  device_buffer_estimator_add(&est, 48000);

  // The stored level was measured against the old rate, so it is discarded.
  device_buffer_estimator_set_rate(&est, 96000.0);
  ASSERT_EQ((size_t)0, device_buffer_estimator_estimate(&est));

  // The new rate drains twice as fast: 48000 frames is now half a second.
  device_buffer_estimator_add(&est, 48000);
  cdsp_sleep_ms(1000);
  ASSERT_EQ((size_t)0, device_buffer_estimator_estimate(&est));
}

TEST(DeviceBufferEstimator_UnknownRateHoldsLevel) {
  device_buffer_estimator_t est;
  device_buffer_estimator_init(&est, 0.0);

  // With no usable rate there is nothing to extrapolate from, so the last
  // published value is reported unchanged rather than decaying to zero.
  device_buffer_estimator_add(&est, 1024);
  cdsp_sleep_ms(500);
  ASSERT_EQ((size_t)1024, device_buffer_estimator_estimate(&est));
}

TEST(DeviceBufferEstimator_LaterPublishReplacesEarlier) {
  device_buffer_estimator_t est;
  device_buffer_estimator_init(&est, 48000.0);

  device_buffer_estimator_add(&est, 48000);
  cdsp_sleep_ms(100);
  // `add` replaces rather than accumulates, matching upstream's naming.
  device_buffer_estimator_add(&est, 2400);

  size_t level = device_buffer_estimator_estimate(&est);
  ASSERT_TRUE(level <= 2400);
  ASSERT_TRUE(level > 1500);
}

TEST_MAIN()
