#include <complex.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "fft/real_fft.h"
#include "test_support.h"
#include "utils/cdsp_memory.h"
#include "utils/double_helpers.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

TEST(RealFFTFallbackForPrimeFactors) {
  size_t length = 22;
  real_fft_t *real_fft = real_fft_create(length, NULL);
  ASSERT_TRUE(real_fft != NULL);
  ASSERT_EQ(length / 2 + 1, real_fft_get_spectrum_length(real_fft));

  double *input = (double *)calloc(length, sizeof(double));
  input[0] = 1.0;
  complex_t *spec = (complex_t *)calloc(real_fft_get_spectrum_length(real_fft),
                                        sizeof(complex_t));

  real_fft_forward(real_fft, input, spec);
  for (size_t k = 0; k < real_fft_get_spectrum_length(real_fft); k++) {
    double mag = cabs(spec[k]);
    ASSERT_NEAR(1.0, mag, 1e-12);
  }

  double *recovered = (double *)calloc(length, sizeof(double));
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
    real_fft_t *real_fft = real_fft_create(length, NULL);
    ASSERT_TRUE(real_fft != NULL);
    ASSERT_EQ(length / 2 + 1, real_fft_get_spectrum_length(real_fft));

    double *input = (double *)calloc(length, sizeof(double));
    input[0] = 1.0;
    complex_t *spec = (complex_t *)calloc(
        real_fft_get_spectrum_length(real_fft), sizeof(complex_t));

    real_fft_forward(real_fft, input, spec);
    for (size_t k = 0; k < real_fft_get_spectrum_length(real_fft); k++) {
      double mag = cabs(spec[k]);
      ASSERT_NEAR(1.0, mag, 1e-12);
    }

    double *recovered = (double *)calloc(length, sizeof(double));
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
    real_fft_t *real_fft = real_fft_create(length, NULL);
    ASSERT_TRUE(real_fft != NULL);
    ASSERT_EQ(length / 2 + 1, real_fft_get_spectrum_length(real_fft));

    double *input = (double *)calloc(length, sizeof(double));
    input[0] = 1.0;
    complex_t *spec = (complex_t *)calloc(
        real_fft_get_spectrum_length(real_fft), sizeof(complex_t));

    real_fft_forward(real_fft, input, spec);
    for (size_t k = 0; k < real_fft_get_spectrum_length(real_fft); k++) {
      double mag = cabs(spec[k]);
      ASSERT_NEAR(1.0, mag, 1e-12);
    }
    ASSERT_DOUBLE_EQ(0.0, cimag(spec[0]));
    ASSERT_DOUBLE_EQ(0.0,
                     cimag(spec[real_fft_get_spectrum_length(real_fft) - 1]));

    double *recovered = (double *)calloc(length, sizeof(double));
    real_fft_inverse(real_fft, spec, recovered);
    ASSERT_NEAR((double)length, recovered[0], 1e-10);
    for (size_t k = 1; k < length; k++) {
      ASSERT_NEAR(0.0, recovered[k], 1e-10);
    }

    size_t k_bin = length / 4;
    if (k_bin < 1)
      k_bin = 1;
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
    real_fftf_t *fft = real_fftf_create(length);
    ASSERT_TRUE(fft != NULL);
    size_t spec_len = real_fftf_get_spectrum_length(fft);
    ASSERT_EQ(length / 2 + 1, spec_len);

    float *input = (float *)calloc(length, sizeof(float));
    input[0] = 1.0f;
    complexf_t *spec = (complexf_t *)calloc(spec_len, sizeof(complexf_t));

    real_fftf_forward(fft, input, spec);
    for (size_t k = 0; k < spec_len; k++) {
      float mag = cabsf(spec[k]);
      ASSERT_NEAR(1.0f, mag, 1e-5f);
    }

    float *recovered = (float *)calloc(length, sizeof(float));
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

// MARK: - Thread Safety & Complex Kernel Regression Tests

typedef struct {
  int ok;
  size_t base;
} audit_fft_thread_arg_t;

static void *audit_fft_thread(void *p) {
  audit_fft_thread_arg_t *arg = (audit_fft_thread_arg_t *)p;
  arg->ok = 1;
  for (int iter = 0; iter < 40; iter++) {
    size_t n = arg->base << (iter % 4);
    real_fft_t *fft = real_fft_create(n, NULL);
    real_fftf_t *fftf = real_fftf_create(n);
    if (!fft || !fftf) {
      arg->ok = 0;
      real_fft_free(fft);
      real_fftf_free(fftf);
      return NULL;
    }
    double *in = (double *)cdsp_aligned_alloc(64, n * sizeof(double));
    double *out = (double *)cdsp_aligned_alloc(64, n * sizeof(double));
    complex_t *spec =
        (complex_t *)cdsp_aligned_alloc(64, (n / 2 + 1) * sizeof(complex_t));
    for (size_t i = 0; i < n; i++)
      in[i] = sin(0.37 * (double)i) + 0.25 * (double)(i % 3);
    real_fft_forward(fft, in, spec);
    real_fft_inverse(fft, spec, out);
    for (size_t i = 0; i < n; i++) {
      if (fabs(out[i] / (double)n - in[i]) > 1e-12)
        arg->ok = 0;
    }
    cdsp_aligned_free(in);
    cdsp_aligned_free(out);
    cdsp_aligned_free(spec);
    real_fft_free(fft);
    real_fftf_free(fftf);
  }
  return NULL;
}

TEST(AuditConvFftConcurrentPlanCreateDestroy) {
  enum { kThreads = 4 };
  pthread_t threads[kThreads];
  audit_fft_thread_arg_t args[kThreads];
  for (int t = 0; t < kThreads; t++) {
    args[t].ok = 0;
    args[t].base = (size_t)(32 + 16 * t);
    ASSERT_EQ(0, pthread_create(&threads[t], NULL, audit_fft_thread, &args[t]));
  }
  for (int t = 0; t < kThreads; t++)
    pthread_join(threads[t], NULL);
  for (int t = 0; t < kThreads; t++)
    ASSERT_EQ(1, args[t].ok);
}

TEST(AuditConvComplexKernelsMatchReferenceAllTails) {
  for (size_t count = 1; count <= 37; count++) {
    double complex a[37], b[37], out[37], acc[37], ref_acc[37];
    for (size_t i = 0; i < count; i++) {
      a[i] = (0.3 + 0.01 * (double)i) + I * (-0.7 + 0.02 * (double)i);
      b[i] = (1.1 - 0.03 * (double)i) + I * (0.4 + 0.05 * (double)i);
      acc[i] = ref_acc[i] = 0.5 * (double)i - I * 0.25;
    }
    dsp_ops_complex_multiply_interleaved(a, b, out, count);
    dsp_ops_complex_fma_interleaved(acc, a, b, count);
    for (size_t i = 0; i < count; i++) {
      double complex p = a[i] * b[i];
      ref_acc[i] += p;
      ASSERT_NEAR(creal(p), creal(out[i]), 1e-14);
      ASSERT_NEAR(cimag(p), cimag(out[i]), 1e-14);
      ASSERT_NEAR(creal(ref_acc[i]), creal(acc[i]), 1e-14);
      ASSERT_NEAR(cimag(ref_acc[i]), cimag(acc[i]), 1e-14);
    }
  }
}

TEST_MAIN()
