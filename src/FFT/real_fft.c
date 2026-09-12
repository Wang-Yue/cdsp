#include "FFT/real_fft.h"

#include <complex.h>  // IWYU pragma: keep
#include <fftw3.h>
#include <stdlib.h>
#include <string.h>

#include "Config/config_error.h"
#include "Logging/app_logger.h"
#include "Utils/double_helpers.h"
#include "Utils/msan_compat.h"

__attribute__((unused)) static const logger_t g_logger = {"dsp.fft"};

// MARK: - Core RealFFT Context Structures

struct real_fft {
  size_t length;
  size_t spectrum_length;
  double* in_real;
  fftw_complex* out_complex;
  fftw_plan plan_forward;
  fftw_plan plan_inverse;
};

struct real_fftf {
  size_t length;
  size_t spectrum_length;
  float* in_real;
  fftwf_complex* out_complex;
  fftwf_plan plan_forward;
  fftwf_plan plan_inverse;
};

// MARK: - Double-Precision Real FFT

size_t real_fft_get_length(const real_fft_t* fft) {
  return fft ? fft->length : 0;
}

size_t real_fft_get_spectrum_length(const real_fft_t* fft) {
  return fft ? fft->spectrum_length : 0;
}

real_fft_t* real_fft_create(size_t length, config_error_t* err) {
  if (length == 0) {
    config_error_set(err, CONFIG_ERR_PARSE, "RealFFT: length must be positive");
    return NULL;
  }
  if (length % 2 != 0) {
    config_error_set(err, CONFIG_ERR_PARSE,
                     "RealFFT: length must be even, got %zu", length);
    return NULL;
  }
  real_fft_t* fft = (real_fft_t*)calloc(1, sizeof(real_fft_t));
  if (!fft) {
    config_error_set(err, CONFIG_ERR_PARSE, "Failed to allocate RealFFT");
    return NULL;
  }
  fft->length = length;
  fft->spectrum_length = length / 2 + 1;

  fft->in_real = (double*)fftw_malloc(length * sizeof(double));
  fft->out_complex =
      (fftw_complex*)fftw_malloc(fft->spectrum_length * sizeof(fftw_complex));
  if (!fft->in_real || !fft->out_complex) {
    config_error_set(err, CONFIG_ERR_PARSE, "Failed to allocate FFTW buffers");
    real_fft_free(fft);
    return NULL;
  }
  fft->plan_forward = fftw_plan_dft_r2c_1d((int)length, fft->in_real,
                                           fft->out_complex, FFTW_PATIENT);
  fft->plan_inverse = fftw_plan_dft_c2r_1d((int)length, fft->out_complex,
                                           fft->in_real, FFTW_PATIENT);
  if (!fft->plan_forward || !fft->plan_inverse) {
    config_error_set(err, CONFIG_ERR_PARSE, "Failed to create FFTW plan");
    real_fft_free(fft);
    return NULL;
  }
  return fft;
}

void real_fft_forward(real_fft_t* fft, waveform_t real_in,
                      mutable_complex_waveform_t spec_out) {
  if (!fft || !real_in || !spec_out) return;

  fftw_execute_dft_r2c(fft->plan_forward, (double*)real_in,
                       (fftw_complex*)spec_out);
  CDSP_MSAN_UNPOISON(spec_out, fft->spectrum_length * sizeof(complex_t));
}

void real_fft_inverse(real_fft_t* fft, mutable_complex_waveform_t spec_in,
                      mutable_waveform_t real_out) {
  if (!fft || !spec_in || !real_out) return;

  fftw_execute_dft_c2r(fft->plan_inverse, (fftw_complex*)spec_in, real_out);
  CDSP_MSAN_UNPOISON(real_out, fft->length * sizeof(double));
}

void real_fft_free(real_fft_t* fft) {
  if (fft) {
    if (fft->plan_forward) fftw_destroy_plan(fft->plan_forward);
    if (fft->plan_inverse) fftw_destroy_plan(fft->plan_inverse);
    if (fft->in_real) fftw_free(fft->in_real);
    if (fft->out_complex) fftw_free(fft->out_complex);
    free(fft);
  }
}

// MARK: - Single-Precision Real FFT

size_t real_fftf_get_length(const real_fftf_t* fft) {
  return fft ? fft->length : 0;
}

size_t real_fftf_get_spectrum_length(const real_fftf_t* fft) {
  return fft ? fft->spectrum_length : 0;
}

real_fftf_t* real_fftf_create(size_t length) {
  if (length == 0 || length % 2 != 0) return NULL;
  real_fftf_t* fft = (real_fftf_t*)calloc(1, sizeof(real_fftf_t));
  if (!fft) return NULL;
  fft->length = length;
  fft->spectrum_length = length / 2 + 1;

  fft->in_real = (float*)fftwf_malloc(length * sizeof(float));
  fft->out_complex = (fftwf_complex*)fftwf_malloc(fft->spectrum_length *
                                                  sizeof(fftwf_complex));
  if (!fft->in_real || !fft->out_complex) {
    real_fftf_free(fft);
    return NULL;
  }
  fft->plan_forward = fftwf_plan_dft_r2c_1d((int)length, fft->in_real,
                                            fft->out_complex, FFTW_PATIENT);
  fft->plan_inverse = fftwf_plan_dft_c2r_1d((int)length, fft->out_complex,
                                            fft->in_real, FFTW_PATIENT);
  if (!fft->plan_forward || !fft->plan_inverse) {
    real_fftf_free(fft);
    return NULL;
  }
  return fft;
}

void real_fftf_forward(real_fftf_t* fft, const float* real_in,
                       mutable_complex_waveformf_t spec_out) {
  if (!fft || !real_in || !spec_out) return;
  fftwf_execute_dft_r2c(fft->plan_forward, (float*)real_in,
                        (fftwf_complex*)spec_out);
  CDSP_MSAN_UNPOISON(spec_out, fft->spectrum_length * sizeof(complexf_t));
}

void real_fftf_inverse(real_fftf_t* fft, mutable_complex_waveformf_t spec_in,
                       float* real_out) {
  if (!fft || !spec_in || !real_out) return;
  fftwf_execute_dft_c2r(fft->plan_inverse, (fftwf_complex*)spec_in, real_out);
  CDSP_MSAN_UNPOISON(real_out, fft->length * sizeof(float));
}

void real_fftf_free(real_fftf_t* fft) {
  if (fft) {
    if (fft->plan_forward) fftwf_destroy_plan(fft->plan_forward);
    if (fft->plan_inverse) fftwf_destroy_plan(fft->plan_inverse);
    if (fft->in_real) fftwf_free(fft->in_real);
    if (fft->out_complex) fftwf_free(fft->out_complex);
    free(fft);
  }
}
