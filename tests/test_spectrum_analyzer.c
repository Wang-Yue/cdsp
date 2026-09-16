#if defined(__linux__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>

#include "audio/audio_chunk.h"
#include "audio/audio_history_buffer.h"
#include "audio/spectrum_analyzer.h"
#include "test_support.h"
#include "utils/double_helpers.h"

static audio_chunk_t *sine_chunk(double freq, int samplerate, size_t frames,
                                 size_t start_frame, size_t channels) {
  audio_chunk_t *chunk = audio_chunk_create(frames, channels);
  audio_chunk_set_valid_frames(chunk, frames);
  double dt = 2.0 * M_PI * freq / (double)samplerate;
  for (size_t ch = 0; ch < channels; ch++) {
    mutable_waveform_t buf = audio_chunk_get_channel(chunk, ch);
    for (size_t t = 0; t < frames; t++) {
      buf[t] = sin(dt * (double)(start_frame + t));
    }
  }
  return chunk;
}

TEST(SineProducesPeakAtCarrier) {
  spectrum_analyzer_t *analyzer = spectrum_analyzer_create();
  audio_history_buffer_t *buffer = audio_history_buffer_create();
  audio_history_buffer_reset(buffer, 2);
  int samplerate = 48000;

  for (size_t i = 0; i < 16; i++) {
    audio_chunk_t *chunk = sine_chunk(1000.0, samplerate, 1024, i * 1024, 2);
    audio_history_buffer_append(buffer, chunk);
    audio_chunk_free(chunk);
  }

  spectrum_result_t result = {0};
  const size_t ch0 = 0;
  spectrum_status_t status = spectrum_analyzer_compute(
      analyzer, buffer, &ch0, 20.0f, 20000.0f, 64, samplerate, &result);

  ASSERT_EQ(SPECTRUM_OK, status);
  ASSERT_EQ(64, result.count);

  size_t nearest_1k = 0;
  double min_diff = 1e9;
  for (size_t i = 0; i < result.count; i++) {
    double diff = fabs((double)result.frequencies[i] - 1000.0);
    if (diff < min_diff) {
      min_diff = diff;
      nearest_1k = i;
    }
  }

  size_t peak_index = 0;
  float max_mag = -200.0f;
  for (size_t i = 0; i < result.count; i++) {
    if (result.magnitudes[i] > max_mag) {
      max_mag = result.magnitudes[i];
      peak_index = i;
    }
  }

  ASSERT_EQ(nearest_1k, peak_index);
  ASSERT_TRUE(result.magnitudes[peak_index] < 1.0f);
  ASSERT_TRUE(result.magnitudes[peak_index] > -10.0f);

  audio_history_buffer_free(buffer);
  spectrum_analyzer_free(analyzer);
}

TEST(EmptyBufferThrows) {
  spectrum_analyzer_t *analyzer = spectrum_analyzer_create();
  audio_history_buffer_t *buffer = audio_history_buffer_create();
  spectrum_result_t result = {0};

  spectrum_status_t status = spectrum_analyzer_compute(
      analyzer, buffer, NULL, 20.0f, 20000.0f, 32, 48000, &result);
  ASSERT_EQ(SPECTRUM_ERROR_EMPTY, status);

  audio_history_buffer_free(buffer);
  spectrum_analyzer_free(analyzer);
}

TEST(ChannelOutOfRangeThrows) {
  spectrum_analyzer_t *analyzer = spectrum_analyzer_create();
  audio_history_buffer_t *buffer = audio_history_buffer_create();
  audio_history_buffer_reset(buffer, 2);

  audio_chunk_t *chunk = sine_chunk(440.0, 48000, 1024, 0, 2);
  audio_history_buffer_append(buffer, chunk);
  audio_chunk_free(chunk);

  spectrum_result_t result = {0};
  const size_t ch4 = 4;
  spectrum_status_t status = spectrum_analyzer_compute(
      analyzer, buffer, &ch4, 20.0f, 20000.0f, 32, 48000, &result);
  ASSERT_EQ(SPECTRUM_ERROR_OUT_OF_RANGE, status);

  audio_history_buffer_free(buffer);
  spectrum_analyzer_free(analyzer);
}

TEST(LogBinFrequenciesAreGeometric) {
  spectrum_analyzer_t *analyzer = spectrum_analyzer_create();
  audio_history_buffer_t *buffer = audio_history_buffer_create();
  audio_history_buffer_reset(buffer, 1);

  audio_chunk_t *chunk = audio_chunk_create(4096, 1);
  audio_chunk_set_valid_frames(chunk, 4096);
  audio_history_buffer_append(buffer, chunk);
  audio_chunk_free(chunk);

  spectrum_result_t result = {0};
  const size_t ch0 = 0;
  spectrum_status_t status = spectrum_analyzer_compute(
      analyzer, buffer, &ch0, 20.0f, 20000.0f, 5, 48000, &result);
  ASSERT_EQ(SPECTRUM_OK, status);
  ASSERT_EQ(5, result.count);
  ASSERT_NEAR(20.0f, result.frequencies[0], 1e-3);
  ASSERT_NEAR(20000.0f, result.frequencies[4], 1.0);

  double ratio01 = (double)(result.frequencies[1] / result.frequencies[0]);
  double ratio34 = (double)(result.frequencies[4] / result.frequencies[3]);
  ASSERT_NEAR(ratio01, ratio34, 1e-3);

  audio_history_buffer_free(buffer);
  spectrum_analyzer_free(analyzer);
}

TEST(HistoryBufferAppendLargeChunkExceedingCapacity) {
  audio_history_buffer_t *buffer = audio_history_buffer_create();
  audio_history_buffer_reset(buffer, 2);

  size_t cap = AUDIO_HISTORY_BUFFER_CAPACITY;
  size_t large_frames = cap + 10000;
  audio_chunk_t *chunk = audio_chunk_create(large_frames, 2);
  audio_chunk_set_valid_frames(chunk, large_frames);
  for (size_t ch = 0; ch < 2; ch++) {
    mutable_waveform_t b = audio_chunk_get_channel(chunk, ch);
    for (size_t i = 0; i < large_frames; i++) {
      b[i] = (double)i;
    }
  }

  // Appending chunk exceeding capacity
  audio_history_buffer_append(buffer, chunk);
  audio_chunk_free(chunk);

  // Read latest capacity frames
  float *dest = (float *)malloc(cap * sizeof(float));
  ASSERT_TRUE(dest != NULL);
  bool enough = false;
  const size_t ch0 = 0;
  audio_history_buffer_status_t status =
      audio_history_buffer_read_latest(buffer, dest, cap, &ch0, &enough);
  ASSERT_EQ(AUDIO_HISTORY_BUFFER_OK, status);
  ASSERT_TRUE(enough);

  // The latest `cap` frames must be the tail of the chunk: (large_frames - cap)
  // .. (large_frames - 1)
  for (size_t i = 0; i < cap; i++) {
    float expected = (float)(large_frames - cap + i);
    ASSERT_EQ(expected, dest[i]);
  }

  free(dest);
  audio_history_buffer_free(buffer);
}

TEST(MaxFreqAboveNyquistClamped) {
  spectrum_analyzer_t *analyzer = spectrum_analyzer_create();
  audio_history_buffer_t *buffer = audio_history_buffer_create();
  audio_history_buffer_reset(buffer, 1);

  // 32 kHz sample rate; Nyquist is 16 kHz, max_freq is 20 kHz
  int samplerate = 32000;
  for (size_t i = 0; i < 8; i++) {
    audio_chunk_t *chunk = sine_chunk(1000.0, samplerate, 1024, i * 1024, 1);
    audio_history_buffer_append(buffer, chunk);
    audio_chunk_free(chunk);
  }

  spectrum_result_t result = {0};
  const size_t ch0 = 0;
  spectrum_status_t status = spectrum_analyzer_compute(
      analyzer, buffer, &ch0, 20.0f, 20000.0f, 32, samplerate, &result);
  ASSERT_EQ(SPECTRUM_OK, status);
  ASSERT_EQ(32, result.count);

  audio_history_buffer_free(buffer);
  spectrum_analyzer_free(analyzer);
}

TEST(DynamicFftLength) {
  spectrum_analyzer_t *analyzer = spectrum_analyzer_create();
  ASSERT_TRUE(analyzer != NULL);
  ASSERT_EQ(4096, spectrum_analyzer_get_fft_n(analyzer));

  audio_history_buffer_t *buffer = audio_history_buffer_create();
  audio_history_buffer_reset(buffer, 1);

  // Fill buffer with 32768 samples
  for (size_t i = 0; i < 32; i++) {
    audio_chunk_t *chunk = sine_chunk(1000.0, 96000, 1024, i * 1024, 1);
    audio_history_buffer_append(buffer, chunk);
    audio_chunk_free(chunk);
  }

  spectrum_result_t result = {0};
  const size_t ch0 = 0;

  // 1. 96 kHz with min_freq = 20 -> ceil(96000/20) = 4800 -> 8192
  spectrum_status_t status = spectrum_analyzer_compute(
      analyzer, buffer, &ch0, 20.0f, 20000.0f, 32, 96000, &result);
  ASSERT_EQ(SPECTRUM_OK, status);
  ASSERT_EQ(8192, spectrum_analyzer_get_fft_n(analyzer));

  // 2. 192 kHz with min_freq = 20 -> ceil(192000/20) = 9600 -> 16384
  status = spectrum_analyzer_compute(analyzer, buffer, &ch0, 20.0f, 20000.0f,
                                     32, 192000, &result);
  ASSERT_EQ(SPECTRUM_OK, status);
  ASSERT_EQ(16384, spectrum_analyzer_get_fft_n(analyzer));

  // 3. 48 kHz with min_freq = 10 -> ceil(48000/10) = 4800 -> 8192
  status = spectrum_analyzer_compute(analyzer, buffer, &ch0, 10.0f, 20000.0f,
                                     32, 48000, &result);
  ASSERT_EQ(SPECTRUM_OK, status);
  ASSERT_EQ(8192, spectrum_analyzer_get_fft_n(analyzer));

  // 4. 48 kHz with min_freq = 20 -> ceil(48000/20) = 2400 -> 4096
  status = spectrum_analyzer_compute(analyzer, buffer, &ch0, 20.0f, 20000.0f,
                                     32, 48000, &result);
  ASSERT_EQ(SPECTRUM_OK, status);
  ASSERT_EQ(4096, spectrum_analyzer_get_fft_n(analyzer));

  audio_history_buffer_free(buffer);
}

TEST(SpectrumAnalyzer_NBinsLessThanTwo_Rejected) {
  spectrum_analyzer_t *analyzer = spectrum_analyzer_create();
  audio_history_buffer_t *buffer = audio_history_buffer_create();
  audio_history_buffer_reset(buffer, 1);
  audio_chunk_t *chunk = sine_chunk(1000.0, 48000, 1024, 0, 1);
  audio_history_buffer_append(buffer, chunk);

  const size_t ch0 = 0;
  spectrum_result_t result;
  memset(&result, 0, sizeof(result));

  // n_bins == 1 must fail with SPECTRUM_ERROR_INVALID_PARAM (upstream requires
  // n_bins >= 2)
  spectrum_status_t status = spectrum_analyzer_compute(
      analyzer, buffer, &ch0, 20.0f, 20000.0f, 1, 48000, &result);
  ASSERT_EQ(SPECTRUM_ERROR_INVALID_PARAM, status);

  // n_bins == 0 must also fail
  status = spectrum_analyzer_compute(analyzer, buffer, &ch0, 20.0f, 20000.0f, 0,
                                     48000, &result);
  ASSERT_EQ(SPECTRUM_ERROR_INVALID_PARAM, status);

  audio_chunk_free(chunk);
  audio_history_buffer_free(buffer);
  spectrum_analyzer_free(analyzer);
}

TEST_MAIN()
