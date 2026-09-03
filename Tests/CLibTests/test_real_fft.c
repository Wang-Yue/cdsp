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
  complex_t* spec = (complex_t*)calloc(real_fft_get_spectrum_length(real_fft),
                                       sizeof(complex_t));

  real_fft_forward(real_fft, input, spec);
  for (size_t k = 0; k < real_fft_get_spectrum_length(real_fft); k++) {
    double mag = cabs(spec[k]);
    ASSERT_NEAR(1.0, mag, 1e-12);
  }

  double* recovered = (double*)calloc(length, sizeof(double));
  real_fft_inverse(real_fft, spec, recovered);
  ASSERT_NEAR((double)length, recovered[0], 1e-10);
  for (size_t k = 1; k < length; k++) {
    ASSERT_NEAR(0.0, recovered[k], 1e-10);
  }

  real_fft_free(real_fft);
  free(input);
  free(spec);
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
    complex_t* spec = (complex_t*)calloc(real_fft_get_spectrum_length(real_fft),
                                         sizeof(complex_t));

    real_fft_forward(real_fft, input, spec);
    for (size_t k = 0; k < real_fft_get_spectrum_length(real_fft); k++) {
      double mag = cabs(spec[k]);
      ASSERT_NEAR(1.0, mag, 1e-12);
    }

    double* recovered = (double*)calloc(length, sizeof(double));
    real_fft_inverse(real_fft, spec, recovered);
    ASSERT_NEAR((double)length, recovered[0], 1e-9);
    for (size_t k = 1; k < length; k++) {
      ASSERT_NEAR(0.0, recovered[k], 1e-9);
    }

    real_fft_free(real_fft);
    free(input);
    free(spec);
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
    complex_t* spec = (complex_t*)calloc(real_fft_get_spectrum_length(real_fft),
                                         sizeof(complex_t));

    real_fft_forward(real_fft, input, spec);
    for (size_t k = 0; k < real_fft_get_spectrum_length(real_fft); k++) {
      double mag = cabs(spec[k]);
      ASSERT_NEAR(1.0, mag, 1e-12);
    }
    ASSERT_DOUBLE_EQ(0.0, cimag(spec[0]));
    ASSERT_DOUBLE_EQ(0.0,
                     cimag(spec[real_fft_get_spectrum_length(real_fft) - 1]));

    double* recovered = (double*)calloc(length, sizeof(double));
    real_fft_inverse(real_fft, spec, recovered);
    ASSERT_NEAR((double)length, recovered[0], 1e-10);
    for (size_t k = 1; k < length; k++) {
      ASSERT_NEAR(0.0, recovered[k], 1e-10);
    }

    size_t k_bin = length / 4;
    if (k_bin < 1) k_bin = 1;
    for (size_t n = 0; n < length; n++) {
      input[n] = cos(2.0 * M_PI * (double)k_bin * (double)n / (double)length);
    }
    real_fft_forward(real_fft, input, spec);
    double expected_re = (double)length / 2.0;
    ASSERT_NEAR(expected_re, creal(spec[k_bin]), 1e-9);
    ASSERT_NEAR(0.0, cimag(spec[k_bin]), 1e-9);

    real_fft_free(real_fft);
    free(input);
    free(spec);
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
    complexf_t* spec = (complexf_t*)calloc(spec_len, sizeof(complexf_t));

    real_fftf_forward(fft, input, spec);
    for (size_t k = 0; k < spec_len; k++) {
      float mag = cabsf(spec[k]);
      ASSERT_NEAR(1.0f, mag, 1e-5f);
    }

    float* recovered = (float*)calloc(length, sizeof(float));
    real_fftf_inverse(fft, spec, recovered);
    ASSERT_NEAR((float)length, recovered[0], 1e-4f);
    for (size_t k = 1; k < length; k++) {
      ASSERT_NEAR(0.0f, recovered[k], 1e-4f);
    }

    real_fftf_free(fft);
    free(input);
    free(spec);
    free(recovered);
  }
}

TEST_MAIN()
