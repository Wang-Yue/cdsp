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
                                           fft->out_complex, FFTW_ESTIMATE);
  fft->plan_inverse = fftw_plan_dft_c2r_1d((int)length, fft->out_complex,
                                           fft->in_real, FFTW_ESTIMATE);
  if (!fft->plan_forward || !fft->plan_inverse) {
    config_error_set(err, CONFIG_ERR_PARSE, "Failed to create FFTW plan");
    real_fft_free(fft);
    return NULL;
  }
  return fft;
}

void real_fft_forward(real_fft_t* fft, waveform_t real_in,
                      mutable_waveform_t spec_re, mutable_waveform_t spec_im) {
  if (!fft || !real_in || !spec_re || !spec_im) return;
  memcpy(fft->in_real, real_in, fft->length * sizeof(double));
  fftw_execute(fft->plan_forward);
  CDSP_MSAN_UNPOISON(fft->out_complex,
                     fft->spectrum_length * sizeof(fftw_complex));
  for (size_t i = 0; i < fft->spectrum_length; i++) {
    spec_re[i] = __real__(fft->out_complex[i]);
    spec_im[i] = __imag__(fft->out_complex[i]);
  }
}

void real_fft_inverse(real_fft_t* fft, waveform_t spec_re, waveform_t spec_im,
                      mutable_waveform_t real_out) {
  if (!fft || !spec_re || !spec_im || !real_out) return;
  for (size_t i = 0; i < fft->spectrum_length; i++) {
    __real__(fft->out_complex[i]) = spec_re[i];
    __imag__(fft->out_complex[i]) = spec_im[i];
  }
  fftw_execute(fft->plan_inverse);
  CDSP_MSAN_UNPOISON(fft->in_real, fft->length * sizeof(double));
  memcpy(real_out, fft->in_real, fft->length * sizeof(double));
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
                                            fft->out_complex, FFTW_ESTIMATE);
  fft->plan_inverse = fftwf_plan_dft_c2r_1d((int)length, fft->out_complex,
                                            fft->in_real, FFTW_ESTIMATE);
  if (!fft->plan_forward || !fft->plan_inverse) {
    real_fftf_free(fft);
    return NULL;
  }
  return fft;
}

void real_fftf_forward(real_fftf_t* fft, const float* real_in, float* spec_re,
                       float* spec_im) {
  if (!fft || !real_in || !spec_re || !spec_im) return;
  memcpy(fft->in_real, real_in, fft->length * sizeof(float));
  fftwf_execute(fft->plan_forward);
  CDSP_MSAN_UNPOISON(fft->out_complex,
                     fft->spectrum_length * sizeof(fftwf_complex));
  for (size_t i = 0; i < fft->spectrum_length; i++) {
    spec_re[i] = __real__(fft->out_complex[i]);
    spec_im[i] = __imag__(fft->out_complex[i]);
  }
}

void real_fftf_inverse(real_fftf_t* fft, const float* spec_re,
                       const float* spec_im, float* real_out) {
  if (!fft || !spec_re || !spec_im || !real_out) return;
  for (size_t i = 0; i < fft->spectrum_length; i++) {
    __real__(fft->out_complex[i]) = spec_re[i];
    __imag__(fft->out_complex[i]) = spec_im[i];
  }
  fftwf_execute(fft->plan_inverse);
  CDSP_MSAN_UNPOISON(fft->in_real, fft->length * sizeof(float));
  memcpy(real_out, fft->in_real, fft->length * sizeof(float));
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
