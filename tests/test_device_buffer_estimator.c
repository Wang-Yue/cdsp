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
  device_buffer_estimator_add(&est, 240000);

  cdsp_sleep_ms(200);

  // Buffer should have partially drained. Wide bounds, but enough to
  // prove the value is actually being extrapolated rather than held constant.
  size_t level = device_buffer_estimator_estimate(&est);
  ASSERT_TRUE(level < 235000);
  ASSERT_TRUE(level > 10000);
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

TEST(DeviceBufferEstimator_Invalidate) {
  device_buffer_estimator_t est;
  device_buffer_estimator_init(&est, 1000.0);

  size_t level = 0;
  device_buffer_estimator_add(&est, 500);
  ASSERT_EQ(DEVICE_BUFFER_ESTIMATE_OK,
            device_buffer_estimator_checked_estimate(&est, &level));

  device_buffer_estimator_invalidate(&est);
  ASSERT_EQ(DEVICE_BUFFER_ESTIMATE_INVALID,
            device_buffer_estimator_checked_estimate(&est, &level));
  ASSERT_EQ((size_t)0, level);
  ASSERT_EQ((size_t)0, device_buffer_estimator_estimate(&est));

  device_buffer_estimator_add(&est, 300);
  ASSERT_EQ(DEVICE_BUFFER_ESTIMATE_OK,
            device_buffer_estimator_checked_estimate(&est, &level));
}

TEST(DeviceBufferEstimator_StaleAfterTwoChunks) {
  device_buffer_estimator_t est;
  device_buffer_estimator_init(&est, 1000.0);
  // 100 frames at 1000 Hz = 100 ms per chunk, stale limit = 200 ms.
  device_buffer_estimator_set_chunk_size(&est, 100);
  device_buffer_estimator_add(&est, 50000);

  size_t level = 0;
  ASSERT_EQ(DEVICE_BUFFER_ESTIMATE_OK,
            device_buffer_estimator_checked_estimate(&est, &level));

  cdsp_sleep_ms(300);
  ASSERT_EQ(DEVICE_BUFFER_ESTIMATE_STALE,
            device_buffer_estimator_checked_estimate(&est, &level));
  // Extrapolated level is still returned and exposed via unchecked estimate().
  ASSERT_TRUE(level > 0 && level <= 50000);

  // A fresh add clears the stale state.
  device_buffer_estimator_add(&est, 50000);
  ASSERT_EQ(DEVICE_BUFFER_ESTIMATE_OK,
            device_buffer_estimator_checked_estimate(&est, &level));
}

TEST(DeviceBufferEstimator_StaleFollowsUpdateInterval) {
  device_buffer_estimator_t est;
  device_buffer_estimator_init(&est, 1000.0);
  // Chunk is 10 frames (10 ms -> 20 ms chunk limit), but updates arrive
  // every >=250 ms -> averaged interval is >=250 ms -> stale limit is >=500 ms.
  device_buffer_estimator_set_chunk_size(&est, 10);
  uint64_t loop_start_ns = cdsp_time_now_ns();
  device_buffer_estimator_add(&est, 50000);
  for (size_t i = 0; i < DEVICE_BUFFER_ESTIMATOR_INTERVALS; i++) {
    cdsp_sleep_ms(250);
    device_buffer_estimator_add(&est, 50000);
  }
  uint64_t total_loop_ms =
      (cdsp_time_now_ns() - loop_start_ns) / 1000000ULL;

  // Wait 60 ms: past 2 chunks (20 ms), still within 2 update intervals (>=500 ms).
  cdsp_sleep_ms(60);
  size_t level = 0;
  ASSERT_EQ(DEVICE_BUFFER_ESTIMATE_OK,
            device_buffer_estimator_checked_estimate(&est, &level));

  // Wait past 2 update intervals. Since the sum of the 8 intervals is
  // total_loop_ms, 2 * median_interval <= total_loop_ms / 2 < total_loop_ms + 500,
  // even if the OS scheduler delayed iterations during the loop.
  cdsp_sleep_ms((uint32_t)(total_loop_ms + 500));
  ASSERT_EQ(DEVICE_BUFFER_ESTIMATE_STALE,
            device_buffer_estimator_checked_estimate(&est, &level));
}

TEST_MAIN()
