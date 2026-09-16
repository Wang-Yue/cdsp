#include <stddef.h>

#include "audio/audio_chunk.h"
#include "audio/silence_counter.h"
#include "config/engine_config_types.h"
#include "test_support.h"

TEST(DisabledWhenTimeoutZero) {
  silence_counter_t *counter = silence_counter_create(-40.0, 0.0, 48000, 1024);
  for (int i = 0; i < 10; i++) {
    ASSERT_EQ(PROCESSING_STATE_RUNNING,
              silence_counter_update(counter, 0.0001f));
  }
  silence_counter_free(counter);
}

TEST(StaysRunningUntilLimitReached) {
  silence_counter_t *counter = silence_counter_create(-40.0, 1.0, 48000, 1024);
  size_t limit = 47;
  ASSERT_EQ(limit, silence_counter_get_limit_chunks(counter));
  for (size_t i = 0; i < limit; i++) {
    ASSERT_EQ(PROCESSING_STATE_RUNNING,
              silence_counter_update(counter, 0.0001f));
  }
  ASSERT_EQ(PROCESSING_STATE_PAUSED, silence_counter_update(counter, 0.0001f));
  silence_counter_free(counter);
}

TEST(RecoversWhenSignalReturns) {
  silence_counter_t *counter = silence_counter_create(-40.0, 0.5, 48000, 1024);
  for (int i = 0; i < 60; i++) {
    silence_counter_update(counter, 0.0001f);
  }
  ASSERT_EQ(PROCESSING_STATE_PAUSED, silence_counter_update(counter, 0.0001f));
  ASSERT_EQ(PROCESSING_STATE_RUNNING, silence_counter_update(counter, 0.5f));
  ASSERT_EQ(PROCESSING_STATE_RUNNING, silence_counter_update(counter, 0.8f));
  silence_counter_free(counter);
}

TEST(ThresholdIsExclusive) {
  // -40 dB -> 0.01 linear threshold
  silence_counter_t *counter = silence_counter_create(-40.0, 1.0, 48000, 1024);
  for (int i = 0; i < 10; i++) {
    silence_counter_update(counter, 0.01f);
  }
  ASSERT_EQ(10, silence_counter_get_silent_chunks(counter));
  silence_counter_update(counter, 0.0101f);
  ASSERT_EQ(0, silence_counter_get_silent_chunks(counter));
  silence_counter_free(counter);
}

TEST(ChunkValueRangeSymmetricSignal) {
  audio_chunk_t *chunk = audio_chunk_create(128, 2);
  mutable_waveform_t ch0 = audio_chunk_get_channel(chunk, 0);
  mutable_waveform_t ch1 = audio_chunk_get_channel(chunk, 1);
  for (size_t i = 0; i < 128; i++) {
    ch0[i] = 0.0;
    ch1[i] = 0.0;
  }
  ch0[10] = 0.05;
  ch0[20] = -0.05;
  double range = audio_chunk_get_value_range(chunk);
  // Symmetric signal has value range: max(0.05) - min(-0.05) = 0.10 (~2 * peak)
  ASSERT_NEAR(0.10, range, 1e-6);

  audio_chunk_free(chunk);
}

TEST_MAIN()
