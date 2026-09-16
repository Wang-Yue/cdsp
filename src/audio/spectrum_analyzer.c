#include "audio/spectrum_analyzer.h"

#include <math.h>
#include <stdbool.h>

#include "audio/audio_history_buffer.h"
#include "fft/real_fft.h"
#include "utils/cdsp_memory.h"
#include "utils/float_helpers.h"

typedef struct {
  int low_k;
  int high_k;
  int nearest_k;
} bin_range_t;

typedef struct {
  double min_freq;
  double max_freq;
  size_t n_bins;
  size_t samplerate;
  float *frequencies;
  bin_range_t *ranges;
  size_t capacity;
} binning_plan_t;

struct spectrum_analyzer {
  size_t fft_n;
  real_fftf_t *fft_setup;
  float *window;
  float window_sum;
  // Preallocated reusable scratch buffers to eliminate frame-by-frame
  // allocations
  float *data;
  complexf_t *spec;
  float *magnitudes;
  float *db_magnitudes;

  // Cached plan for geometric binning to eliminate transcendental operations
  binning_plan_t plan;
  float *out_magnitudes;
  size_t out_capacity;
};
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "logging/app_logger.h"

static const logger_t g_logger = {"dsp.spectrum_analyzer"};

static size_t next_power_of_two(size_t v) {
  if (v <= 1)
    return 1;
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

static size_t spectrum_fft_length_for(double min_freq, size_t samplerate) {
  double min_len = ceil((double)samplerate / min_freq);
  size_t len = (min_len > 1.0) ? (size_t)min_len : 1;
  size_t p2 = next_power_of_two(len);
  if (p2 > AUDIO_HISTORY_BUFFER_CAPACITY) {
    p2 = AUDIO_HISTORY_BUFFER_CAPACITY;
  }
  return p2;
}

static bool spectrum_analyzer_reconfigure_fft(spectrum_analyzer_t *analyzer,
                                              size_t new_n) {
  if (!analyzer || new_n == 0)
    return false;
  real_fftf_t *new_setup = real_fftf_create(new_n);
  if (!new_setup)
    return false;

  float *new_window = (float *)cdsp_aligned_alloc(64, new_n * sizeof(float));
  float *new_data = (float *)cdsp_aligned_alloc(64, new_n * sizeof(float));
  complexf_t *new_spec = (complexf_t *)cdsp_aligned_alloc(
      64, (new_n / 2 + 1) * sizeof(complexf_t));
  float *new_mags =
      (float *)cdsp_aligned_alloc(64, (new_n / 2 + 1) * sizeof(float));
  float *new_db_mags =
      (float *)cdsp_aligned_alloc(64, (new_n / 2 + 1) * sizeof(float));

  if (!new_window || !new_data || !new_spec || !new_mags || !new_db_mags) {
    real_fftf_free(new_setup);
    if (new_window)
      cdsp_aligned_free(new_window);
    if (new_data)
      cdsp_aligned_free(new_data);
    if (new_spec)
      cdsp_aligned_free(new_spec);
    if (new_mags)
      cdsp_aligned_free(new_mags);
    if (new_db_mags)
      cdsp_aligned_free(new_db_mags);
    return false;
  }
  memset(new_window, 0, new_n * sizeof(float));
  memset(new_data, 0, new_n * sizeof(float));
  memset(new_spec, 0, (new_n / 2 + 1) * sizeof(complexf_t));
  memset(new_mags, 0, (new_n / 2 + 1) * sizeof(float));
  memset(new_db_mags, 0, (new_n / 2 + 1) * sizeof(float));

  // Compute symmetric Hann window (accumulated in float for performance)
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

  if (analyzer->fft_setup)
    real_fftf_free(analyzer->fft_setup);
  if (analyzer->window)
    cdsp_aligned_free(analyzer->window);
  if (analyzer->data)
    cdsp_aligned_free(analyzer->data);
  if (analyzer->spec)
    cdsp_aligned_free(analyzer->spec);
  if (analyzer->magnitudes)
    cdsp_aligned_free(analyzer->magnitudes);
  if (analyzer->db_magnitudes)
    cdsp_aligned_free(analyzer->db_magnitudes);

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

size_t spectrum_analyzer_get_fft_n(const spectrum_analyzer_t *analyzer) {
  return analyzer ? analyzer->fft_n : 0;
}

spectrum_analyzer_t *spectrum_analyzer_create(void) {
  spectrum_analyzer_t *analyzer =
      (spectrum_analyzer_t *)calloc(1, sizeof(spectrum_analyzer_t));
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
      (float *)calloc(analyzer->out_capacity, sizeof(float));
  analyzer->plan.ranges =
      (bin_range_t *)calloc(analyzer->out_capacity, sizeof(bin_range_t));
  analyzer->plan.capacity = analyzer->out_capacity;
  analyzer->out_magnitudes =
      (float *)calloc(analyzer->out_capacity, sizeof(float));

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

void spectrum_analyzer_free(spectrum_analyzer_t *analyzer) {
  if (!analyzer)
    return;
  if (analyzer->fft_setup)
    real_fftf_free(analyzer->fft_setup);
  if (analyzer->window)
    cdsp_aligned_free(analyzer->window);
  if (analyzer->data)
    cdsp_aligned_free(analyzer->data);
  if (analyzer->spec)
    cdsp_aligned_free(analyzer->spec);
  if (analyzer->magnitudes)
    cdsp_aligned_free(analyzer->magnitudes);
  if (analyzer->db_magnitudes)
    cdsp_aligned_free(analyzer->db_magnitudes);
  if (analyzer->plan.frequencies)
    free(analyzer->plan.frequencies);
  if (analyzer->plan.ranges)
    free(analyzer->plan.ranges);
  if (analyzer->out_magnitudes)
    free(analyzer->out_magnitudes);
  free(analyzer);
}

spectrum_status_t spectrum_analyzer_compute(spectrum_analyzer_t *analyzer,
                                            audio_history_buffer_t *buffer,
                                            const size_t *channel,
                                            double min_freq, double max_freq,
                                            size_t n_bins, size_t samplerate,
                                            spectrum_result_t *out_result) {
  if (!analyzer || !buffer || !out_result)
    return SPECTRUM_ERROR_INVALID_PARAM;
  if (samplerate == 0 || n_bins < 2 || min_freq <= 0.0 ||
      max_freq <= min_freq || min_freq >= (double)samplerate) {
    return SPECTRUM_ERROR_INVALID_PARAM;
  }

  // Grow output and plan buffers dynamically if requested n_bins exceeds
  // capacity
  if (n_bins > analyzer->out_capacity) {
    size_t new_cap = n_bins;
    float *new_freqs =
        (float *)realloc(analyzer->plan.frequencies, new_cap * sizeof(float));
    bin_range_t *new_ranges = (bin_range_t *)realloc(
        analyzer->plan.ranges, new_cap * sizeof(bin_range_t));
    float *new_mags =
        (float *)realloc(analyzer->out_magnitudes, new_cap * sizeof(float));
    if (!new_freqs || !new_ranges || !new_mags) {
      if (new_freqs)
        analyzer->plan.frequencies = new_freqs;
      if (new_ranges)
        analyzer->plan.ranges = new_ranges;
      if (new_mags)
        analyzer->out_magnitudes = new_mags;
      return SPECTRUM_ERROR_INVALID_PARAM;
    }
    analyzer->plan.frequencies = new_freqs;
    analyzer->plan.ranges = new_ranges;
    analyzer->out_magnitudes = new_mags;
    analyzer->plan.capacity = new_cap;
    analyzer->out_capacity = new_cap;
  }

  if (analyzer->out_capacity == 0 || !analyzer->plan.frequencies ||
      !analyzer->plan.ranges || !analyzer->out_magnitudes) {
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
  // This maps output frequency bins to ranges of FFT bins using double (f64)
  // arithmetic matching upstream CamillaDSP spectrum.rs.
  if (analyzer->plan.min_freq != min_freq ||
      analyzer->plan.max_freq != max_freq || analyzer->plan.n_bins != n_bins ||
      analyzer->plan.samplerate != samplerate) {
    double min_f = (double)min_freq;
    double max_f = (double)max_freq;
    double log_ratio = pow(max_f / min_f, 1.0 / (double)(n_bins - 1));
    double sqrt_log_ratio = sqrt(log_ratio);
    double freq_res = (double)samplerate / (double)analyzer->fft_n;

    for (size_t i = 0; i < n_bins; i++) {
      double center_f = min_f * pow(log_ratio, (double)i);
      analyzer->plan.frequencies[i] = (float)center_f;

      // Define frequency boundaries for this bin (matching upstream
      // spectrum.rs)
      double low_f = (i == 0) ? min_f : (center_f / sqrt_log_ratio);
      double high_f = (i == n_bins - 1) ? max_f : (center_f * sqrt_log_ratio);

      // Convert frequency boundaries to FFT bin indices with safe bounds
      // checking
      double low_bin = floor(low_f / freq_res);
      double high_bin = ceil(high_f / freq_res);
      double nearest_bin = round(center_f / freq_res);

      int low_k = low_bin < 0.0
                      ? 0
                      : (low_bin > (double)half_n ? (int)half_n : (int)low_bin);
      int high_k = high_bin < 0.0 ? 0
                                  : (high_bin > (double)half_n ? (int)half_n
                                                               : (int)high_bin);
      int nearest_k =
          nearest_bin < 0.0
              ? 0
              : (nearest_bin > (double)half_n ? (int)half_n : (int)nearest_bin);

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
    if (start < 0)
      start = 0;
    if (end > (int)half_n)
      end = (int)half_n;
    int len = end - start + 1;

    if (start <= end && len > 0) {
      analyzer->out_magnitudes[i] =
          dsp_ops_float_max(analyzer->db_magnitudes + start, len);
    } else {
      // If the range doesn't cover any FFT bin (e.g. low frequencies with small
      // FFT), fallback to the nearest FFT bin.
      int k = range.nearest_k;
      if (k < 0)
        k = 0;
      if (k > (int)half_n)
        k = (int)half_n;
      analyzer->out_magnitudes[i] = analyzer->db_magnitudes[k];
    }
  }

  out_result->frequencies = analyzer->plan.frequencies;
  out_result->magnitudes = analyzer->out_magnitudes;
  out_result->count = n_bins;
  return SPECTRUM_OK;
}
