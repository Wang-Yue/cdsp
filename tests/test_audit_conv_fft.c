// Regression tests for audit report 03 F4 (FFTW planner thread safety and
// planning-buffer lifetime) and F9 (complex multiply kernels).
#include <complex.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "fft/real_fft.h"
#include "test_support.h"
#include "utils/cdsp_memory.h"
#include "utils/double_helpers.h"

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

// F9: kernels must agree with plain complex arithmetic for every tail length
// (spec_len = chunk_size + 1 is not a multiple of the SIMD width).
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
