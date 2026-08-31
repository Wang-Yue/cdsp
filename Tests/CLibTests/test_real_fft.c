#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "FFT/real_fft.h"
#include "test_support.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

TEST(RealFFTFallbackForPrimeFactors) {
  size_t length = 22;
  real_fft_t* real_fft = real_fft_create(length, NULL);
  ASSERT_TRUE(real_fft != NULL);
  ASSERT_EQ(length / 2 + 1, real_fft_get_spectrum_length(real_fft));

  double* input = (double*)calloc(length, sizeof(double));
  input[0] = 1.0;
  double* spec_re =
      (double*)calloc(real_fft_get_spectrum_length(real_fft), sizeof(double));
  double* spec_im =
      (double*)calloc(real_fft_get_spectrum_length(real_fft), sizeof(double));

  real_fft_forward(real_fft, input, spec_re, spec_im);
  for (size_t k = 0; k < real_fft_get_spectrum_length(real_fft); k++) {
    double mag = sqrt(spec_re[k] * spec_re[k] + spec_im[k] * spec_im[k]);
    ASSERT_NEAR(1.0, mag, 1e-12);
  }

  double* recovered = (double*)calloc(length, sizeof(double));
  real_fft_inverse(real_fft, spec_re, spec_im, recovered);
  ASSERT_NEAR((double)length, recovered[0], 1e-10);
  for (size_t k = 1; k < length; k++) {
    ASSERT_NEAR(0.0, recovered[k], 1e-10);
  }

  real_fft_free(real_fft);
  free(input);
  free(spec_re);
  free(spec_im);
  free(recovered);
}

TEST(RealFFTVDSPDFTInnerRoundtrip) {
  size_t lengths[] = {48, 80, 240, 2560};
  for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
    size_t length = lengths[i];
    real_fft_t* real_fft = real_fft_create(length, NULL);
    ASSERT_TRUE(real_fft != NULL);
    ASSERT_EQ(length / 2 + 1, real_fft_get_spectrum_length(real_fft));

    double* input = (double*)calloc(length, sizeof(double));
    input[0] = 1.0;
    double* spec_re =
        (double*)calloc(real_fft_get_spectrum_length(real_fft), sizeof(double));
    double* spec_im =
        (double*)calloc(real_fft_get_spectrum_length(real_fft), sizeof(double));

    real_fft_forward(real_fft, input, spec_re, spec_im);
    for (size_t k = 0; k < real_fft_get_spectrum_length(real_fft); k++) {
      double mag = sqrt(spec_re[k] * spec_re[k] + spec_im[k] * spec_im[k]);
      ASSERT_NEAR(1.0, mag, 1e-12);
    }

    double* recovered = (double*)calloc(length, sizeof(double));
    real_fft_inverse(real_fft, spec_re, spec_im, recovered);
    ASSERT_NEAR((double)length, recovered[0], 1e-9);
    for (size_t k = 1; k < length; k++) {
      ASSERT_NEAR(0.0, recovered[k], 1e-9);
    }

    real_fft_free(real_fft);
    free(input);
    free(spec_re);
    free(spec_im);
    free(recovered);
  }
}

TEST(RealFFTPow2VDSPRoundtrip) {
  size_t lengths[] = {8, 16, 32, 64, 1024, 2048, 4096};
  for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
    size_t length = lengths[i];
    real_fft_t* real_fft = real_fft_create(length, NULL);
    ASSERT_TRUE(real_fft != NULL);
    ASSERT_EQ(length / 2 + 1, real_fft_get_spectrum_length(real_fft));

    double* input = (double*)calloc(length, sizeof(double));
    input[0] = 1.0;
    double* spec_re =
        (double*)calloc(real_fft_get_spectrum_length(real_fft), sizeof(double));
    double* spec_im =
        (double*)calloc(real_fft_get_spectrum_length(real_fft), sizeof(double));

    real_fft_forward(real_fft, input, spec_re, spec_im);
    for (size_t k = 0; k < real_fft_get_spectrum_length(real_fft); k++) {
      double mag = sqrt(spec_re[k] * spec_re[k] + spec_im[k] * spec_im[k]);
      ASSERT_NEAR(1.0, mag, 1e-12);
    }
    ASSERT_DOUBLE_EQ(0.0, spec_im[0]);
    ASSERT_DOUBLE_EQ(0.0, spec_im[real_fft_get_spectrum_length(real_fft) - 1]);

    size_t spec_len = real_fft_get_spectrum_length(real_fft);
    double* spec_re_copy = (double*)malloc(spec_len * sizeof(double));
    double* spec_im_copy = (double*)malloc(spec_len * sizeof(double));
    memcpy(spec_re_copy, spec_re, spec_len * sizeof(double));
    memcpy(spec_im_copy, spec_im, spec_len * sizeof(double));

    double* recovered = (double*)calloc(length, sizeof(double));
    real_fft_inverse(real_fft, spec_re, spec_im, recovered);
    ASSERT_NEAR((double)length, recovered[0], 1e-10);
    for (size_t k = 1; k < length; k++) {
      ASSERT_NEAR(0.0, recovered[k], 1e-10);
    }
    for (size_t k = 0; k < spec_len; k++) {
      ASSERT_DOUBLE_EQ(spec_re_copy[k], spec_re[k]);
      ASSERT_DOUBLE_EQ(spec_im_copy[k], spec_im[k]);
    }
    free(spec_re_copy);
    free(spec_im_copy);

    size_t k_bin = length / 4;
    if (k_bin < 1) k_bin = 1;
    for (size_t n = 0; n < length; n++) {
      input[n] = cos(2.0 * M_PI * (double)k_bin * (double)n / (double)length);
    }
    real_fft_forward(real_fft, input, spec_re, spec_im);
    double expected_re = (double)length / 2.0;
    ASSERT_NEAR(expected_re, spec_re[k_bin], 1e-9);
    ASSERT_NEAR(0.0, spec_im[k_bin], 1e-9);

    real_fft_free(real_fft);
    free(input);
    free(spec_re);
    free(spec_im);
    free(recovered);
  }
}

TEST(RealFFTSinglePrecisionRoundtrip) {
  size_t lengths[] = {16, 48, 64, 128, 512, 1024};
  for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
    size_t length = lengths[i];
    real_fftf_t* fft = real_fftf_create(length);
    ASSERT_TRUE(fft != NULL);
    size_t spec_len = real_fftf_get_spectrum_length(fft);
    ASSERT_EQ(length / 2 + 1, spec_len);

    float* input = (float*)calloc(length, sizeof(float));
    input[0] = 1.0f;
    float* spec_re = (float*)calloc(spec_len, sizeof(float));
    float* spec_im = (float*)calloc(spec_len, sizeof(float));

    real_fftf_forward(fft, input, spec_re, spec_im);
    for (size_t k = 0; k < spec_len; k++) {
      float mag = sqrtf(spec_re[k] * spec_re[k] + spec_im[k] * spec_im[k]);
      ASSERT_NEAR(1.0f, mag, 1e-5f);
    }

    float* recovered = (float*)calloc(length, sizeof(float));
    real_fftf_inverse(fft, spec_re, spec_im, recovered);
    ASSERT_NEAR((float)length, recovered[0], 1e-4f);
    for (size_t k = 1; k < length; k++) {
      ASSERT_NEAR(0.0f, recovered[k], 1e-4f);
    }

    real_fftf_free(fft);
    free(input);
    free(spec_re);
    free(spec_im);
    free(recovered);
  }
}

TEST_MAIN()
