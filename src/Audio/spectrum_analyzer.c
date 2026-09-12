#include "Audio/spectrum_analyzer.h"

#include <math.h>
#include <stdbool.h>

#include "Audio/audio_history_buffer.h"
#include "FFT/real_fft.h"
#include "Utils/float_helpers.h"

typedef struct {
  int low_k;
  int high_k;
  int nearest_k;
} bin_range_t;

typedef struct {
  float min_freq;
  float max_freq;
  size_t n_bins;
  size_t samplerate;
  float* frequencies;
  bin_range_t* ranges;
  size_t capacity;
} binning_plan_t;

struct spectrum_analyzer {
  size_t fft_n;
  real_fftf_t* fft_setup;
  float* window;
  float window_sum;
  // Preallocated reusable scratch buffers to eliminate frame-by-frame
  // allocations
  float* data;
  complexf_t* spec;
  float* magnitudes;
  float* db_magnitudes;

  // Cached plan for geometric binning to eliminate transcendental operations
  binning_plan_t plan;
  float* out_magnitudes;
  size_t out_capacity;
};
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "Logging/app_logger.h"

static const logger_t g_logger = {"dsp.spectrum_analyzer"};

static size_t next_power_of_two(size_t v) {
  if (v <= 1) return 1;
  v--;
  v |= v >> 1;
  v |= v >> 2;
  v |= v >> 4;
  v |= v >> 8;
  v |= v >> 16;
#if SIZE_MAX > 0xFFFFFFFF
  v |= v >> 32;
#endif
  v++;
  return v;
}

static size_t spectrum_fft_length_for(float min_freq, size_t samplerate) {
  double min_len = ceil((double)samplerate / (double)min_freq);
  size_t len = (min_len > 1.0) ? (size_t)min_len : 1;
  size_t p2 = next_power_of_two(len);
  if (p2 > AUDIO_HISTORY_BUFFER_CAPACITY) {
    p2 = AUDIO_HISTORY_BUFFER_CAPACITY;
  }
  return p2;
}

static bool spectrum_analyzer_reconfigure_fft(spectrum_analyzer_t* analyzer,
                                              size_t new_n) {
  if (!analyzer || new_n == 0) return false;
  real_fftf_t* new_setup = real_fftf_create(new_n);
  if (!new_setup) return false;

  float* new_window = (float*)calloc(new_n, sizeof(float));
  float* new_data = (float*)calloc(new_n, sizeof(float));
  complexf_t* new_spec = (complexf_t*)calloc(new_n / 2 + 1, sizeof(complexf_t));
  float* new_mags = (float*)calloc(new_n / 2 + 1, sizeof(float));
  float* new_db_mags = (float*)calloc(new_n / 2 + 1, sizeof(float));

  if (!new_window || !new_data || !new_spec || !new_mags || !new_db_mags) {
    real_fftf_free(new_setup);
    if (new_window) free(new_window);
    if (new_data) free(new_data);
    if (new_spec) free(new_spec);
    if (new_mags) free(new_mags);
    if (new_db_mags) free(new_db_mags);
    return false;
  }

  // Compute symmetric Hann window matching upstream CamillaDSP
  // (src/spectrum.rs:152-156)
  float sum = 0.0f;
  if (new_n > 1) {
    double denom = (double)(new_n - 1);
    for (size_t i = 0; i < new_n; i++) {
      new_window[i] =
          (float)(0.5 * (1.0 - cos(2.0 * M_PI * (double)i / denom)));
      sum += new_window[i];
    }
  } else if (new_n == 1) {
    new_window[0] = 1.0f;
    sum = 1.0f;
  }

  if (analyzer->fft_setup) real_fftf_free(analyzer->fft_setup);
  if (analyzer->window) free(analyzer->window);
  if (analyzer->data) free(analyzer->data);
  if (analyzer->spec) free(analyzer->spec);
  if (analyzer->magnitudes) free(analyzer->magnitudes);
  if (analyzer->db_magnitudes) free(analyzer->db_magnitudes);

  analyzer->fft_n = new_n;
  analyzer->fft_setup = new_setup;
  analyzer->window = new_window;
  analyzer->window_sum = sum;
  analyzer->data = new_data;
  analyzer->spec = new_spec;
  analyzer->magnitudes = new_mags;
  analyzer->db_magnitudes = new_db_mags;

  // Invalidate cached plan
  analyzer->plan.samplerate = 0;

  return true;
}

size_t spectrum_analyzer_get_fft_n(const spectrum_analyzer_t* analyzer) {
  return analyzer ? analyzer->fft_n : 0;
}

spectrum_analyzer_t* spectrum_analyzer_create(void) {
  spectrum_analyzer_t* analyzer =
      (spectrum_analyzer_t*)calloc(1, sizeof(spectrum_analyzer_t));
  if (!analyzer) {
    logger_error(&g_logger, "Memory allocation failed for spectrum_analyzer_t");
    return NULL;
  }

  if (!spectrum_analyzer_reconfigure_fft(analyzer, 4096)) {
    logger_error(&g_logger,
                 "Failed to configure initial FFT for spectrum analyzer");
    spectrum_analyzer_free(analyzer);
    return NULL;
  }

  analyzer->out_capacity = 4096;
  analyzer->plan.frequencies =
      (float*)calloc(analyzer->out_capacity, sizeof(float));
  analyzer->plan.ranges =
      (bin_range_t*)calloc(analyzer->out_capacity, sizeof(bin_range_t));
  analyzer->plan.capacity = analyzer->out_capacity;
  analyzer->out_magnitudes =
      (float*)calloc(analyzer->out_capacity, sizeof(float));

  if (!analyzer->plan.frequencies || !analyzer->plan.ranges ||
      !analyzer->out_magnitudes) {
    logger_error(&g_logger,
                 "Failed to allocate memory buffers for spectrum analyzer");
    spectrum_analyzer_free(analyzer);
    return NULL;
  }

  logger_debug(&g_logger,
               "Spectrum analyzer created (fft_n=%zu, out_capacity=%zu)",
               analyzer->fft_n, analyzer->out_capacity);
  return analyzer;
}

void spectrum_analyzer_free(spectrum_analyzer_t* analyzer) {
  if (!analyzer) return;
  if (analyzer->fft_setup) real_fftf_free(analyzer->fft_setup);
  if (analyzer->window) free(analyzer->window);
  if (analyzer->data) free(analyzer->data);
  if (analyzer->spec) free(analyzer->spec);
  if (analyzer->magnitudes) free(analyzer->magnitudes);
  if (analyzer->db_magnitudes) free(analyzer->db_magnitudes);
  if (analyzer->plan.frequencies) free(analyzer->plan.frequencies);
  if (analyzer->plan.ranges) free(analyzer->plan.ranges);
  if (analyzer->out_magnitudes) free(analyzer->out_magnitudes);
  free(analyzer);
}

spectrum_status_t spectrum_analyzer_compute(spectrum_analyzer_t* analyzer,
                                            audio_history_buffer_t* buffer,
                                            const size_t* channel,
                                            float min_freq, float max_freq,
                                            size_t n_bins, size_t samplerate,
                                            spectrum_result_t* out_result) {
  if (!analyzer || !buffer || !out_result) return SPECTRUM_ERROR_INVALID_PARAM;
  if (analyzer->out_capacity == 0 || !analyzer->plan.frequencies ||
      !analyzer->plan.ranges || !analyzer->out_magnitudes) {
    return SPECTRUM_ERROR_INVALID_PARAM;
  }
  if (samplerate == 0 || n_bins < 2 || n_bins > analyzer->out_capacity ||
      min_freq <= 0.0f || max_freq <= min_freq) {
    return SPECTRUM_ERROR_INVALID_PARAM;
  }

  size_t needed_fft_n = spectrum_fft_length_for(min_freq, samplerate);
  if (needed_fft_n != analyzer->fft_n) {
    if (!spectrum_analyzer_reconfigure_fft(analyzer, needed_fft_n)) {
      return SPECTRUM_ERROR_INVALID_PARAM;
    }
  }

  // Read data from history buffer directly into preallocated instance buffer
  bool enough_data = false;
  audio_history_buffer_status_t status = audio_history_buffer_read_latest(
      buffer, analyzer->data, analyzer->fft_n, channel, &enough_data);
  if (status == AUDIO_HISTORY_BUFFER_ERROR_EMPTY) {
    return SPECTRUM_ERROR_EMPTY;
  }
  if (status == AUDIO_HISTORY_BUFFER_ERROR_OUT_OF_RANGE) {
    return SPECTRUM_ERROR_OUT_OF_RANGE;
  }
  if (!enough_data) {
    return SPECTRUM_ERROR_EMPTY;
  }

  // 1. Apply Hann window in-place to reduce spectral leakage
  dsp_ops_float_multiply(analyzer->data, analyzer->window, analyzer->data,
                         analyzer->fft_n);

  // 2. Perform FFT using unified real_fftf
  size_t half_n = analyzer->fft_n / 2;
  real_fftf_forward(analyzer->fft_setup, analyzer->data, analyzer->spec);

  // 3. Compute magnitudes in dBFS directly into preallocated arrays
  float scale = 2.0f / analyzer->window_sum;

  dsp_ops_float_complex_abs(analyzer->spec, analyzer->magnitudes, half_n + 1);
  dsp_ops_float_scalar_multiply(analyzer->magnitudes, scale, half_n + 1);

  // Correct scaling for DC and Nyquist (they are not doubled, so scale by 1.0/N
  // instead of 2.0/N)
  analyzer->magnitudes[0] *= 0.5f;
  analyzer->magnitudes[half_n] *= 0.5f;

  // Convert the entire magnitudes array to decibels (dBFS)
  float ref = 1.0f;
  dsp_ops_float_vdbcon(analyzer->magnitudes, ref, analyzer->db_magnitudes,
                       half_n + 1);

  // 4. Geometric Binning via Cached Plan

  // Recompute the logarithmic binning plan if parameters changed.
  // This maps output frequency bins to ranges of FFT bins.
  if (analyzer->plan.min_freq != min_freq ||
      analyzer->plan.max_freq != max_freq || analyzer->plan.n_bins != n_bins ||
      analyzer->plan.samplerate != samplerate) {
    float log_min = log10f(min_freq);
    float log_max = log10f(max_freq);
    float step = n_bins > 1 ? (log_max - log_min) / (float)(n_bins - 1) : 0.0f;

    for (size_t i = 0; i < n_bins; i++) {
      float center_log = log_min + step * (float)i;
      float center_f = powf(10.0f, center_log);
      analyzer->plan.frequencies[i] = center_f;

      // Define frequency boundaries for this bin
      float low_log = i > 0 ? center_log - step / 2.0f : log_min;
      float high_log = i < n_bins - 1 ? center_log + step / 2.0f : log_max;

      float low_f = powf(10.0f, low_log);
      float high_f = powf(10.0f, high_log);

      // Convert frequency boundaries to FFT bin indices
      int low_k =
          (int)floorf(low_f * (float)analyzer->fft_n / (float)samplerate);
      int high_k =
          (int)ceilf(high_f * (float)analyzer->fft_n / (float)samplerate);
      int nearest_k =
          (int)roundf(center_f * (float)analyzer->fft_n / (float)samplerate);

      analyzer->plan.ranges[i].low_k = low_k;
      analyzer->plan.ranges[i].high_k = high_k;
      analyzer->plan.ranges[i].nearest_k = nearest_k;
    }
    analyzer->plan.min_freq = min_freq;
    analyzer->plan.max_freq = max_freq;
    analyzer->plan.n_bins = n_bins;
    analyzer->plan.samplerate = samplerate;
  }

  // Map FFT magnitudes to the output bins.
  // For each output bin, we take the maximum magnitude within its mapped FFT
  // bin range.
  for (size_t i = 0; i < n_bins; i++) {
    bin_range_t range = analyzer->plan.ranges[i];
    int start = range.low_k;
    int end = range.high_k;
    if (start < 0) start = 0;
    if (end > (int)half_n) end = (int)half_n;
    int len = end - start + 1;

    if (start <= end && len > 0) {
      analyzer->out_magnitudes[i] =
          dsp_ops_float_max(analyzer->db_magnitudes + start, len);
    } else {
      // If the range doesn't cover any FFT bin (e.g. low frequencies with small
      // FFT), fallback to the nearest FFT bin.
      int k = range.nearest_k;
      if (k < 0) k = 0;
      if (k > (int)half_n) k = (int)half_n;
      analyzer->out_magnitudes[i] = analyzer->db_magnitudes[k];
    }
  }

  out_result->frequencies = analyzer->plan.frequencies;
  out_result->magnitudes = analyzer->out_magnitudes;
  out_result->count = n_bins;
  return SPECTRUM_OK;
}
