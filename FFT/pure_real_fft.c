#include "FFT/pure_real_fft.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "Utils/cdsp_memory.h"
#include "Utils/float_helpers.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC push_options
#pragma GCC optimize("O3,fast-math,finite-math-only")
#elif defined(__clang__)
#pragma float_control(precise, off, push)
#endif

// ============================================================================
// Double-Precision (f64) Implementation
// ============================================================================

struct pure_real_fft {
  real_fft_backend_t base;
  size_t length;      // 2N
  size_t half_n;      // N
  int stage_count;
  int* factors;
  size_t* permutation;
  double** twiddle_re;
  double** twiddle_im;
  double* untw_re;    // length N/2 + 1
  double* untw_im;    // length N/2 + 1
  double* scratch_re; // length N
  double* scratch_im; // length N
  double* work_re;    // length N
  double* work_im;    // length N
};

static inline void radix2_stage_d(size_t n, double* work_re, double* work_im,
                                  size_t m, const double* tw_re,
                                  const double* tw_im) {
  size_t block_size = m * 2;
  if (m == 1) {
    PRAGMA_VECTORIZE_LOOP
    for (size_t b = 0; b < n; b += 2) {
      double v0r = work_re[b];
      double v0i = work_im[b];
      double v1r = work_re[b + 1];
      double v1i = work_im[b + 1];
      work_re[b] = v0r + v1r;
      work_im[b] = v0i + v1i;
      work_re[b + 1] = v0r - v1r;
      work_im[b + 1] = v0i - v1i;
    }
    return;
  }
  const double* tw1R = tw_re + m;
  const double* tw1I = tw_im + m;
  for (size_t b = 0; b < n; b += block_size) {
    double* w0r = work_re + b;
    double* w0i = work_im + b;
    double* w1r = w0r + m;
    double* w1i = w0i + m;
    PRAGMA_VECTORIZE_LOOP
    for (size_t k = 0; k < m; k++) {
      double twR = tw1R[k];
      double twI = tw1I[k];
      double v0r = w0r[k], v0i = w0i[k];
      double v1r = w1r[k], v1i = w1i[k];
      double t1r = v1r * twR - v1i * twI;
      double t1i = v1r * twI + v1i * twR;
      w0r[k] = v0r + t1r;
      w0i[k] = v0i + t1i;
      w1r[k] = v0r - t1r;
      w1i[k] = v0i - t1i;
    }
  }
}

static inline void radix4_stage_d(size_t n, double* work_re, double* work_im,
                                  size_t m, const double* tw_re,
                                  const double* tw_im) {
  size_t block_size = m * 4;
  if (m == 1) {
    PRAGMA_VECTORIZE_LOOP
    for (size_t b = 0; b < n; b += 4) {
      double v0r = work_re[b],     v0i = work_im[b];
      double v1r = work_re[b + 1], v1i = work_im[b + 1];
      double v2r = work_re[b + 2], v2i = work_im[b + 2];
      double v3r = work_re[b + 3], v3i = work_im[b + 3];

      double a0r = v0r + v2r, a0i = v0i + v2i;
      double a1r = v0r - v2r, a1i = v0i - v2i;
      double a2r = v1r + v3r, a2i = v1i + v3i;
      double a3r = v1r - v3r, a3i = v1i - v3i;

      work_re[b]     = a0r + a2r;
      work_im[b]     = a0i + a2i;
      work_re[b + 1] = a1r + a3i;
      work_im[b + 1] = a1i - a3r;
      work_re[b + 2] = a0r - a2r;
      work_im[b + 2] = a0i - a2i;
      work_re[b + 3] = a1r - a3i;
      work_im[b + 3] = a1i + a3r;
    }
    return;
  }
  const double* tw1R = tw_re + m;
  const double* tw1I = tw_im + m;
  const double* tw2R = tw_re + 2 * m;
  const double* tw2I = tw_im + 2 * m;
  const double* tw3R = tw_re + 3 * m;
  const double* tw3I = tw_im + 3 * m;

  for (size_t b = 0; b < n; b += block_size) {
    double* w0r = work_re + b;
    double* w0i = work_im + b;
    double* w1r = w0r + m;
    double* w1i = w0i + m;
    double* w2r = w1r + m;
    double* w2i = w1i + m;
    double* w3r = w2r + m;
    double* w3i = w2i + m;
    PRAGMA_VECTORIZE_LOOP
    for (size_t k = 0; k < m; k++) {
      double t1R = tw1R[k], t1I = tw1I[k];
      double t2R = tw2R[k], t2I = tw2I[k];
      double t3R = tw3R[k], t3I = tw3I[k];

      double v0r = w0r[k], v0i = w0i[k];
      double v1r = w1r[k], v1i = w1i[k];
      double v2r = w2r[k], v2i = w2i[k];
      double v3r = w3r[k], v3i = w3i[k];

      double u1r = v1r * t1R - v1i * t1I;
      double u1i = v1r * t1I + v1i * t1R;
      double u2r = v2r * t2R - v2i * t2I;
      double u2i = v2r * t2I + v2i * t2R;
      double u3r = v3r * t3R - v3i * t3I;
      double u3i = v3r * t3I + v3i * t3R;

      double a0r = v0r + u2r, a0i = v0i + u2i;
      double a1r = v0r - u2r, a1i = v0i - u2i;
      double a2r = u1r + u3r, a2i = u1i + u3i;
      double a3r = u1r - u3r, a3i = u1i - u3i;

      w0r[k] = a0r + a2r;
      w0i[k] = a0i + a2i;
      w1r[k] = a1r + a3i;
      w1i[k] = a1i - a3r;
      w2r[k] = a0r - a2r;
      w2i[k] = a0i - a2i;
      w3r[k] = a1r - a3i;
      w3i[k] = a1i + a3r;
    }
  }
}

static inline void radix8_stage_d(size_t n, double* work_re, double* work_im,
                                  size_t m, const double* tw_re,
                                  const double* tw_im) {
  size_t block_size = m * 8;
  const double s2 = 0.70710678118654752440;  // sqrt(2)/2

  if (m == 1) {
    PRAGMA_VECTORIZE_LOOP
    for (size_t b = 0; b < n; b += 8) {
      double v0r = work_re[b],     v0i = work_im[b];
      double v1r = work_re[b + 1], v1i = work_im[b + 1];
      double v2r = work_re[b + 2], v2i = work_im[b + 2];
      double v3r = work_re[b + 3], v3i = work_im[b + 3];
      double v4r = work_re[b + 4], v4i = work_im[b + 4];
      double v5r = work_re[b + 5], v5i = work_im[b + 5];
      double v6r = work_re[b + 6], v6i = work_im[b + 6];
      double v7r = work_re[b + 7], v7i = work_im[b + 7];

      // Step 1: 4-point sub-transforms on evens (0,2,4,6) and odds (1,3,5,7)
      double a0r = v0r + v4r, a0i = v0i + v4i;
      double a1r = v0r - v4r, a1i = v0i - v4i;
      double a2r = v2r + v6r, a2i = v2i + v6i;
      double a3r = v2r - v6r, a3i = v2i - v6i;

      double e0r = a0r + a2r, e0i = a0i + a2i;
      double e1r = a1r + a3i, e1i = a1i - a3r;
      double e2r = a0r - a2r, e2i = a0i - a2i;
      double e3r = a1r - a3i, e3i = a1i + a3r;

      double b0r = v1r + v5r, b0i = v1i + v5i;
      double b1r = v1r - v5r, b1i = v1i - v5i;
      double b2r = v3r + v7r, b2i = v3i + v7i;
      double b3r = v3r - v7r, b3i = v3i - v7i;

      double o0r = b0r + b2r, o0i = b0i + b2i;
      double o1r = b1r + b3i, o1i = b1i - b3r;
      double o2r = b0r - b2r, o2i = b0i - b2i;
      double o3r = b1r - b3i, o3i = b1i + b3r;

      // Step 2: Multiply odd outputs by W_8^k = exp(-2pi*i*k/8)
      // W_8^0 = 1
      // W_8^1 = s2 - i*s2
      double t1r = s2 * (o1r + o1i);
      double t1i = s2 * (o1i - o1r);
      // W_8^2 = -i
      double t2r = o2i;
      double t2i = -o2r;
      // W_8^3 = -s2 - i*s2
      double t3r = s2 * (o3i - o3r);
      double t3i = s2 * (-o3r - o3i);

      // Step 3: Combine even and twiddled odd outputs
      work_re[b]     = e0r + o0r; work_im[b]     = e0i + o0i;
      work_re[b + 1] = e1r + t1r; work_im[b + 1] = e1i + t1i;
      work_re[b + 2] = e2r + t2r; work_im[b + 2] = e2i + t2i;
      work_re[b + 3] = e3r + t3r; work_im[b + 3] = e3i + t3i;
      work_re[b + 4] = e0r - o0r; work_im[b + 4] = e0i - o0i;
      work_re[b + 5] = e1r - t1r; work_im[b + 5] = e1i - t1i;
      work_re[b + 6] = e2r - t2r; work_im[b + 6] = e2i - t2i;
      work_re[b + 7] = e3r - t3r; work_im[b + 7] = e3i - t3i;
    }
    return;
  }

  const double* tw1R = tw_re + m;     const double* tw1I = tw_im + m;
  const double* tw2R = tw_re + 2 * m; const double* tw2I = tw_im + 2 * m;
  const double* tw3R = tw_re + 3 * m; const double* tw3I = tw_im + 3 * m;
  const double* tw4R = tw_re + 4 * m; const double* tw4I = tw_im + 4 * m;
  const double* tw5R = tw_re + 5 * m; const double* tw5I = tw_im + 5 * m;
  const double* tw6R = tw_re + 6 * m; const double* tw6I = tw_im + 6 * m;
  const double* tw7R = tw_re + 7 * m; const double* tw7I = tw_im + 7 * m;

  for (size_t b = 0; b < n; b += block_size) {
    double* w0r = work_re + b;         double* w0i = work_im + b;
    double* w1r = w0r + m;             double* w1i = w0i + m;
    double* w2r = w1r + m;             double* w2i = w1i + m;
    double* w3r = w2r + m;             double* w3i = w2i + m;
    double* w4r = w3r + m;             double* w4i = w3i + m;
    double* w5r = w4r + m;             double* w5i = w4i + m;
    double* w6r = w5r + m;             double* w6i = w5i + m;
    double* w7r = w6r + m;             double* w7i = w6i + m;

    PRAGMA_VECTORIZE_LOOP
    for (size_t k = 0; k < m; k++) {
      double v0r = w0r[k], v0i = w0i[k];
      double v1r = w1r[k], v1i = w1i[k];
      double v2r = w2r[k], v2i = w2i[k];
      double v3r = w3r[k], v3i = w3i[k];
      double v4r = w4r[k], v4i = w4i[k];
      double v5r = w5r[k], v5i = w5i[k];
      double v6r = w6r[k], v6i = w6i[k];
      double v7r = w7r[k], v7i = w7i[k];

      double t1R = tw1R[k], t1I = tw1I[k];
      double t2R = tw2R[k], t2I = tw2I[k];
      double t3R = tw3R[k], t3I = tw3I[k];
      double t4R = tw4R[k], t4I = tw4I[k];
      double t5R = tw5R[k], t5I = tw5I[k];
      double t6R = tw6R[k], t6I = tw6I[k];
      double t7R = tw7R[k], t7I = tw7I[k];

      double u1r = v1r * t1R - v1i * t1I; double u1i = v1r * t1I + v1i * t1R;
      double u2r = v2r * t2R - v2i * t2I; double u2i = v2r * t2I + v2i * t2R;
      double u3r = v3r * t3R - v3i * t3I; double u3i = v3r * t3I + v3i * t3R;
      double u4r = v4r * t4R - v4i * t4I; double u4i = v4r * t4I + v4i * t4R;
      double u5r = v5r * t5R - v5i * t5I; double u5i = v5r * t5I + v5i * t5R;
      double u6r = v6r * t6R - v6i * t6I; double u6i = v6r * t6I + v6i * t6R;
      double u7r = v7r * t7R - v7i * t7I; double u7i = v7r * t7I + v7i * t7R;

      double a0r = v0r + u4r, a0i = v0i + u4i;
      double a1r = v0r - u4r, a1i = v0i - u4i;
      double a2r = u2r + u6r, a2i = u2i + u6i;
      double a3r = u2r - u6r, a3i = u2i - u6i;

      double e0r = a0r + a2r, e0i = a0i + a2i;
      double e1r = a1r + a3i, e1i = a1i - a3r;
      double e2r = a0r - a2r, e2i = a0i - a2i;
      double e3r = a1r - a3i, e3i = a1i + a3r;

      double b0r = u1r + u5r, b0i = u1i + u5i;
      double b1r = u1r - u5r, b1i = u1i - u5i;
      double b2r = u3r + u7r, b2i = u3i + u7i;
      double b3r = u3r - u7r, b3i = u3i - u7i;

      double o0r = b0r + b2r, o0i = b0i + b2i;
      double o1r = b1r + b3i, o1i = b1i - b3r;
      double o2r = b0r - b2r, o2i = b0i - b2i;
      double o3r = b1r - b3i, o3i = b1i + b3r;

      double r1r = s2 * (o1r + o1i); double r1i = s2 * (o1i - o1r);
      double r2r = o2i;              double r2i = -o2r;
      double r3r = s2 * (o3i - o3r); double r3i = s2 * (-o3r - o3i);

      w0r[k] = e0r + o0r; w0i[k] = e0i + o0i;
      w1r[k] = e1r + r1r; w1i[k] = e1i + r1i;
      w2r[k] = e2r + r2r; w2i[k] = e2i + r2i;
      w3r[k] = e3r + r3r; w3i[k] = e3i + r3i;
      w4r[k] = e0r - o0r; w4i[k] = e0i - o0i;
      w5r[k] = e1r - r1r; w5i[k] = e1i - r1i;
      w6r[k] = e2r - r2r; w6i[k] = e2i - r2i;
      w7r[k] = e3r - r3r; w7i[k] = e3i - r3i;
    }
  }
}

static inline void inner_power2_fft_d(pure_real_fft_t* fft, double* work_re,
                                      double* work_im, bool inverse) {
  size_t n = fft->half_n;
  if (inverse) {
    PRAGMA_VECTORIZE_LOOP
    for (size_t i = 0; i < n; i++) {
      work_im[i] = -work_im[i];
    }
  }

  size_t m = 1;
  for (int s = 0; s < fft->stage_count; s++) {
    int r = fft->factors[s];
    const double* twRe = fft->twiddle_re[s];
    const double* twIm = fft->twiddle_im[s];
    if (r == 8) {
      radix8_stage_d(n, work_re, work_im, m, twRe, twIm);
    } else if (r == 4) {
      radix4_stage_d(n, work_re, work_im, m, twRe, twIm);
    } else {
      radix2_stage_d(n, work_re, work_im, m, twRe, twIm);
    }
    m *= (size_t)r;
  }

  if (inverse) {
    PRAGMA_VECTORIZE_LOOP
    for (size_t i = 0; i < n; i++) {
      work_im[i] = -work_im[i];
    }
  }
}

pure_real_fft_t* pure_real_fft_create(size_t length) {
  if (length < 8 || (length & (length - 1)) != 0) return NULL;
  size_t n = length / 2;

  int fs[32];
  int stage_count = 0;
  size_t rem = n;
  int two_pow = 0;
  while (rem % 2 == 0) {
    two_pow++;
    rem /= 2;
  }
  while (two_pow >= 3) {
    fs[stage_count++] = 8;
    two_pow -= 3;
  }
  if (two_pow == 2) {
    fs[stage_count++] = 4;
  } else if (two_pow == 1) {
    fs[stage_count++] = 2;
  }

  pure_real_fft_t* fft = (pure_real_fft_t*)calloc(1, sizeof(pure_real_fft_t));
  if (!fft) return NULL;

  fft->length = length;
  fft->half_n = n;
  fft->stage_count = stage_count;
  fft->factors = (int*)calloc(stage_count, sizeof(int));
  fft->twiddle_re = (double**)calloc(stage_count, sizeof(double*));
  fft->twiddle_im = (double**)calloc(stage_count, sizeof(double*));
  fft->permutation = (size_t*)calloc(n, sizeof(size_t));
  fft->untw_re = (double*)cdsp_aligned_alloc(64, (n / 2 + 1) * sizeof(double));
  fft->untw_im = (double*)cdsp_aligned_alloc(64, (n / 2 + 1) * sizeof(double));
  fft->scratch_re = (double*)cdsp_aligned_alloc(64, n * sizeof(double));
  fft->scratch_im = (double*)cdsp_aligned_alloc(64, n * sizeof(double));
  fft->work_re = (double*)cdsp_aligned_alloc(64, n * sizeof(double));
  fft->work_im = (double*)cdsp_aligned_alloc(64, n * sizeof(double));

  if (!fft->factors || !fft->twiddle_re || !fft->twiddle_im ||
      !fft->permutation || !fft->untw_re || !fft->untw_im ||
      !fft->scratch_re || !fft->scratch_im ||
      !fft->work_re || !fft->work_im) {
    pure_real_fft_free(fft);
    return NULL;
  }

  size_t m = 1;
  for (int s = 0; s < stage_count; s++) {
    int r = fs[s];
    fft->factors[s] = r;
    size_t len = m * (size_t)r;
    fft->twiddle_re[s] = (double*)cdsp_aligned_alloc(64, len * sizeof(double));
    fft->twiddle_im[s] = (double*)cdsp_aligned_alloc(64, len * sizeof(double));
    if (!fft->twiddle_re[s] || !fft->twiddle_im[s]) {
      pure_real_fft_free(fft);
      return NULL;
    }
    double invMR = 1.0 / (double)(m * (size_t)r);
    for (int j = 0; j < r; j++) {
      for (size_t k = 0; k < m; k++) {
        double theta = -2.0 * M_PI * (double)((size_t)j * k) * invMR;
        fft->twiddle_re[s][(size_t)j * m + k] = cos(theta);
        fft->twiddle_im[s][(size_t)j * m + k] = sin(theta);
      }
    }
    m *= (size_t)r;
  }

  // Precompute digit-reversal permutation for length N
  for (size_t i = 0; i < n; i++) {
    size_t idx = i;
    size_t rev = 0;
    size_t m_left = n;
    for (int s = stage_count - 1; s >= 0; s--) {
      int r = fs[s];
      m_left /= (size_t)r;
      size_t d = idx % (size_t)r;
      idx /= (size_t)r;
      rev += d * m_left;
    }
    fft->permutation[i] = rev;
  }

  // Precompute real untwiddle table W_{2N}^k = exp(-i * pi * k / N)
  double invN = 1.0 / (double)n;
  for (size_t k = 0; k <= n / 2; k++) {
    double theta = -M_PI * (double)k * invN;
    fft->untw_re[k] = cos(theta);
    fft->untw_im[k] = sin(theta);
  }

  return fft;
}

void pure_real_fft_forward(pure_real_fft_t* fft, waveform_t real_in,
                           mutable_waveform_t spec_re,
                           mutable_waveform_t spec_im) {
  if (!fft) return;
  size_t n = fft->half_n;
  double* wr = fft->scratch_re;
  double* wi = fft->scratch_im;
  const size_t* perm = fft->permutation;

  // Step 1: De-interleave real input into bit-reversed complex array
  PRAGMA_VECTORIZE_LOOP
  for (size_t k = 0; k < n; k++) {
    size_t p = perm[k];
    wr[p] = real_in[2 * k];
    wi[p] = real_in[2 * k + 1];
  }

  // Step 2: Inner complex power-of-2 FFT
  inner_power2_fft_d(fft, wr, wi, false);

  // Step 3: Simultaneous 2-way real untwiddle
  // DC & Nyquist
  spec_re[0] = wr[0] + wi[0];
  spec_im[0] = 0.0;
  spec_re[n] = wr[0] - wi[0];
  spec_im[n] = 0.0;

  const double* untwR = fft->untw_re;
  const double* untwI = fft->untw_im;
  size_t half_len = n / 2;

  PRAGMA_VECTORIZE_LOOP
  for (size_t k = 1; k <= half_len; k++) {
    double zkR = wr[k];
    double zkI = wi[k];
    double zmkR = wr[n - k];
    double zmkI = wi[n - k];

    double eRe = 0.5 * (zkR + zmkR);
    double eIm = 0.5 * (zkI - zmkI);
    double diffRe = zkR - zmkR;
    double diffIm = zkI + zmkI;

    double oRe = 0.5 * diffIm;
    double oIm = -0.5 * diffRe;

    double twR = untwR[k];
    double twI = untwI[k];

    double woRe = twR * oRe - twI * oIm;
    double woIm = twR * oIm + twI * oRe;

    spec_re[k] = eRe + woRe;
    spec_im[k] = eIm + woIm;
    spec_re[n - k] = eRe - woRe;
    spec_im[n - k] = -(eIm - woIm);
  }
}

void pure_real_fft_inverse(pure_real_fft_t* fft, waveform_t spec_re,
                           waveform_t spec_im, mutable_waveform_t real_out) {
  if (!fft) return;
  size_t n = fft->half_n;
  double* wr = fft->scratch_re;
  double* wi = fft->scratch_im;
  double* pr = fft->work_re;
  double* pi = fft->work_im;
  const size_t* perm = fft->permutation;

  // Step 1: Reconstruct complex spectrum Z[k]
  wr[0] = 0.5 * (spec_re[0] + spec_re[n]);
  wi[0] = 0.5 * (spec_re[0] - spec_re[n]);

  const double* untwR = fft->untw_re;
  const double* untwI = fft->untw_im;
  size_t half_len = n / 2;

  PRAGMA_VECTORIZE_LOOP
  for (size_t k = 1; k <= half_len; k++) {
    double xkR = spec_re[k];
    double xkI = spec_im[k];
    double xmkR = spec_re[n - k];
    double xmkI = spec_im[n - k];

    double eRe = 0.5 * (xkR + xmkR);
    double eIm = 0.5 * (xkI - xmkI);
    double halfDiffRe = 0.5 * (xkR - xmkR);
    double halfDiffIm = 0.5 * (xkI + xmkI);

    double twR = untwR[k];
    double twI = untwI[k];

    double oRe = halfDiffRe * twR + halfDiffIm * twI;
    double oIm = halfDiffIm * twR - halfDiffRe * twI;

    wr[k] = eRe - oIm;
    wi[k] = eIm + oRe;
    wr[n - k] = eRe + oIm;
    wi[n - k] = -(eIm - oRe);
  }

  // Bit-reversal permutation into preallocated work array for inner IFFT
  PRAGMA_VECTORIZE_LOOP
  for (size_t k = 0; k < n; k++) {
    size_t p = perm[k];
    pr[p] = wr[k];
    pi[p] = wi[k];
  }

  // Step 2: Inner complex power-of-2 IFFT
  inner_power2_fft_d(fft, pr, pi, true);

  // Step 3: Fused unpack into real output (with factor of 2)
  PRAGMA_VECTORIZE_LOOP
  for (size_t k = 0; k < n; k++) {
    real_out[2 * k] = 2.0 * pr[k];
    real_out[2 * k + 1] = 2.0 * pi[k];
  }
}

void pure_real_fft_free(pure_real_fft_t* fft) {
  if (!fft) return;
  if (fft->twiddle_re) {
    for (int s = 0; s < fft->stage_count; s++) {
      if (fft->twiddle_re[s]) cdsp_aligned_free(fft->twiddle_re[s]);
    }
    free(fft->twiddle_re);
  }
  if (fft->twiddle_im) {
    for (int s = 0; s < fft->stage_count; s++) {
      if (fft->twiddle_im[s]) cdsp_aligned_free(fft->twiddle_im[s]);
    }
    free(fft->twiddle_im);
  }
  if (fft->factors) free(fft->factors);
  if (fft->permutation) free(fft->permutation);
  if (fft->untw_re) cdsp_aligned_free(fft->untw_re);
  if (fft->untw_im) cdsp_aligned_free(fft->untw_im);
  if (fft->scratch_re) cdsp_aligned_free(fft->scratch_re);
  if (fft->scratch_im) cdsp_aligned_free(fft->scratch_im);
  if (fft->work_re) cdsp_aligned_free(fft->work_re);
  if (fft->work_im) cdsp_aligned_free(fft->work_im);
  free(fft);
}

static void pure_real_fft_forward_wrapper(void* ctx, waveform_t real_in,
                                         mutable_waveform_t spec_re,
                                         mutable_waveform_t spec_im) {
  pure_real_fft_forward((pure_real_fft_t*)ctx, real_in, spec_re, spec_im);
}

static void pure_real_fft_inverse_wrapper(void* ctx, waveform_t spec_re,
                                         waveform_t spec_im,
                                         mutable_waveform_t real_out) {
  pure_real_fft_inverse((pure_real_fft_t*)ctx, spec_re, spec_im, real_out);
}

static void pure_real_fft_free_wrapper(void* ctx) {
  pure_real_fft_free((pure_real_fft_t*)ctx);
}

real_fft_backend_t* pure_real_fft_as_backend(pure_real_fft_t* fft) {
  if (!fft) return NULL;
  fft->base.ctx = fft;
  fft->base.forward = pure_real_fft_forward_wrapper;
  fft->base.inverse = pure_real_fft_inverse_wrapper;
  fft->base.free = pure_real_fft_free_wrapper;
  return &fft->base;
}

// ============================================================================
// Single-Precision (f32) Implementation
// ============================================================================

struct pure_real_fftf {
  real_fftf_backend_t base;
  size_t length;
  size_t half_n;
  int stage_count;
  int* factors;
  size_t* permutation;
  float** twiddle_re;
  float** twiddle_im;
  float* untw_re;
  float* untw_im;
  float* scratch_re;
  float* scratch_im;
  float* work_re;
  float* work_im;
};

static inline void radix2_stage_f(size_t n, float* work_re, float* work_im,
                                  size_t m, const float* tw_re,
                                  const float* tw_im) {
  size_t block_size = m * 2;
  if (m == 1) {
    PRAGMA_VECTORIZE_LOOP
    for (size_t b = 0; b < n; b += 2) {
      float v0r = work_re[b],     v0i = work_im[b];
      float v1r = work_re[b + 1], v1i = work_im[b + 1];
      work_re[b] = v0r + v1r;
      work_im[b] = v0i + v1i;
      work_re[b + 1] = v0r - v1r;
      work_im[b + 1] = v0i - v1i;
    }
    return;
  }
  const float* tw1R = tw_re + m;
  const float* tw1I = tw_im + m;
  for (size_t b = 0; b < n; b += block_size) {
    float* w0r = work_re + b; float* w0i = work_im + b;
    float* w1r = w0r + m;     float* w1i = w0i + m;
    PRAGMA_VECTORIZE_LOOP
    for (size_t k = 0; k < m; k++) {
      float twR = tw1R[k], twI = tw1I[k];
      float v0r = w0r[k], v0i = w0i[k];
      float v1r = w1r[k], v1i = w1i[k];
      float t1r = v1r * twR - v1i * twI;
      float t1i = v1r * twI + v1i * twR;
      w0r[k] = v0r + t1r;
      w0i[k] = v0i + t1i;
      w1r[k] = v0r - t1r;
      w1i[k] = v0i - t1i;
    }
  }
}

static inline void radix4_stage_f(size_t n, float* work_re, float* work_im,
                                  size_t m, const float* tw_re,
                                  const float* tw_im) {
  size_t block_size = m * 4;
  if (m == 1) {
    PRAGMA_VECTORIZE_LOOP
    for (size_t b = 0; b < n; b += 4) {
      float v0r = work_re[b],     v0i = work_im[b];
      float v1r = work_re[b + 1], v1i = work_im[b + 1];
      float v2r = work_re[b + 2], v2i = work_im[b + 2];
      float v3r = work_re[b + 3], v3i = work_im[b + 3];

      float a0r = v0r + v2r, a0i = v0i + v2i;
      float a1r = v0r - v2r, a1i = v0i - v2i;
      float a2r = v1r + v3r, a2i = v1i + v3i;
      float a3r = v1r - v3r, a3i = v1i - v3i;

      work_re[b]     = a0r + a2r;
      work_im[b]     = a0i + a2i;
      work_re[b + 1] = a1r + a3i;
      work_im[b + 1] = a1i - a3r;
      work_re[b + 2] = a0r - a2r;
      work_im[b + 2] = a0i - a2i;
      work_re[b + 3] = a1r - a3i;
      work_im[b + 3] = a1i + a3r;
    }
    return;
  }
  const float* tw1R = tw_re + m;     const float* tw1I = tw_im + m;
  const float* tw2R = tw_re + 2 * m; const float* tw2I = tw_im + 2 * m;
  const float* tw3R = tw_re + 3 * m; const float* tw3I = tw_im + 3 * m;

  for (size_t b = 0; b < n; b += block_size) {
    float* w0r = work_re + b; float* w0i = work_im + b;
    float* w1r = w0r + m;     float* w1i = w0i + m;
    float* w2r = w1r + m;     float* w2i = w1i + m;
    float* w3r = w2r + m;     float* w3i = w2i + m;
    PRAGMA_VECTORIZE_LOOP
    for (size_t k = 0; k < m; k++) {
      float t1R = tw1R[k], t1I = tw1I[k];
      float t2R = tw2R[k], t2I = tw2I[k];
      float t3R = tw3R[k], t3I = tw3I[k];

      float v0r = w0r[k], v0i = w0i[k];
      float v1r = w1r[k], v1i = w1i[k];
      float v2r = w2r[k], v2i = w2i[k];
      float v3r = w3r[k], v3i = w3i[k];

      float u1r = v1r * t1R - v1i * t1I; float u1i = v1r * t1I + v1i * t1R;
      float u2r = v2r * t2R - v2i * t2I; float u2i = v2r * t2I + v2i * t2R;
      float u3r = v3r * t3R - v3i * t3I; float u3i = v3r * t3I + v3i * t3R;

      float a0r = v0r + u2r, a0i = v0i + u2i;
      float a1r = v0r - u2r, a1i = v0i - u2i;
      float a2r = u1r + u3r, a2i = u1i + u3i;
      float a3r = u1r - u3r, a3i = u1i - u3i;

      w0r[k] = a0r + a2r;
      w0i[k] = a0i + a2i;
      w1r[k] = a1r + a3i;
      w1i[k] = a1i - a3r;
      w2r[k] = a0r - a2r;
      w2i[k] = a0i - a2i;
      w3r[k] = a1r - a3i;
      w3i[k] = a1i + a3r;
    }
  }
}

static inline void radix8_stage_f(size_t n, float* work_re, float* work_im,
                                  size_t m, const float* tw_re,
                                  const float* tw_im) {
  size_t block_size = m * 8;
  const float s2 = 0.70710678118654752440f;

  if (m == 1) {
    PRAGMA_VECTORIZE_LOOP
    for (size_t b = 0; b < n; b += 8) {
      float v0r = work_re[b],     v0i = work_im[b];
      float v1r = work_re[b + 1], v1i = work_im[b + 1];
      float v2r = work_re[b + 2], v2i = work_im[b + 2];
      float v3r = work_re[b + 3], v3i = work_im[b + 3];
      float v4r = work_re[b + 4], v4i = work_im[b + 4];
      float v5r = work_re[b + 5], v5i = work_im[b + 5];
      float v6r = work_re[b + 6], v6i = work_im[b + 6];
      float v7r = work_re[b + 7], v7i = work_im[b + 7];

      float a0r = v0r + v4r, a0i = v0i + v4i;
      float a1r = v0r - v4r, a1i = v0i - v4i;
      float a2r = v2r + v6r, a2i = v2i + v6i;
      float a3r = v2r - v6r, a3i = v2i - v6i;

      float e0r = a0r + a2r, e0i = a0i + a2i;
      float e1r = a1r + a3i, e1i = a1i - a3r;
      float e2r = a0r - a2r, e2i = a0i - a2i;
      float e3r = a1r - a3i, e3i = a1i + a3r;

      float b0r = v1r + v5r, b0i = v1i + v5i;
      float b1r = v1r - v5r, b1i = v1i - v5i;
      float b2r = v3r + v7r, b2i = v3i + v7i;
      float b3r = v3r - v7r, b3i = v3i - v7i;

      float o0r = b0r + b2r, o0i = b0i + b2i;
      float o1r = b1r + b3i, o1i = b1i - b3r;
      float o2r = b0r - b2r, o2i = b0i - b2i;
      float o3r = b1r - b3i, o3i = b1i + b3r;

      float t1r = s2 * (o1r + o1i); float t1i = s2 * (o1i - o1r);
      float t2r = o2i;              float t2i = -o2r;
      float t3r = s2 * (o3i - o3r); float t3i = s2 * (-o3r - o3i);

      work_re[b]     = e0r + o0r; work_im[b]     = e0i + o0i;
      work_re[b + 1] = e1r + t1r; work_im[b + 1] = e1i + t1i;
      work_re[b + 2] = e2r + t2r; work_im[b + 2] = e2i + t2i;
      work_re[b + 3] = e3r + t3r; work_im[b + 3] = e3i + t3i;
      work_re[b + 4] = e0r - o0r; work_im[b + 4] = e0i - o0i;
      work_re[b + 5] = e1r - t1r; work_im[b + 5] = e1i - t1i;
      work_re[b + 6] = e2r - t2r; work_im[b + 6] = e2i - t2i;
      work_re[b + 7] = e3r - t3r; work_im[b + 7] = e3i - t3i;
    }
    return;
  }

  const float* tw1R = tw_re + m;     const float* tw1I = tw_im + m;
  const float* tw2R = tw_re + 2 * m; const float* tw2I = tw_im + 2 * m;
  const float* tw3R = tw_re + 3 * m; const float* tw3I = tw_im + 3 * m;
  const float* tw4R = tw_re + 4 * m; const float* tw4I = tw_im + 4 * m;
  const float* tw5R = tw_re + 5 * m; const float* tw5I = tw_im + 5 * m;
  const float* tw6R = tw_re + 6 * m; const float* tw6I = tw_im + 6 * m;
  const float* tw7R = tw_re + 7 * m; const float* tw7I = tw_im + 7 * m;

  for (size_t b = 0; b < n; b += block_size) {
    float* w0r = work_re + b;         float* w0i = work_im + b;
    float* w1r = w0r + m;             float* w1i = w0i + m;
    float* w2r = w1r + m;             float* w2i = w1i + m;
    float* w3r = w2r + m;             float* w3i = w2i + m;
    float* w4r = w3r + m;             float* w4i = w3i + m;
    float* w5r = w4r + m;             float* w5i = w4i + m;
    float* w6r = w5r + m;             float* w6i = w5i + m;
    float* w7r = w6r + m;             float* w7i = w6i + m;

    PRAGMA_VECTORIZE_LOOP
    for (size_t k = 0; k < m; k++) {
      float v0r = w0r[k], v0i = w0i[k];
      float v1r = w1r[k], v1i = w1i[k];
      float v2r = w2r[k], v2i = w2i[k];
      float v3r = w3r[k], v3i = w3i[k];
      float v4r = w4r[k], v4i = w4i[k];
      float v5r = w5r[k], v5i = w5i[k];
      float v6r = w6r[k], v6i = w6i[k];
      float v7r = w7r[k], v7i = w7i[k];

      float t1R = tw1R[k], t1I = tw1I[k];
      float t2R = tw2R[k], t2I = tw2I[k];
      float t3R = tw3R[k], t3I = tw3I[k];
      float t4R = tw4R[k], t4I = tw4I[k];
      float t5R = tw5R[k], t5I = tw5I[k];
      float t6R = tw6R[k], t6I = tw6I[k];
      float t7R = tw7R[k], t7I = tw7I[k];

      float u1r = v1r * t1R - v1i * t1I; float u1i = v1r * t1I + v1i * t1R;
      float u2r = v2r * t2R - v2i * t2I; float u2i = v2r * t2I + v2i * t2R;
      float u3r = v3r * t3R - v3i * t3I; float u3i = v3r * t3I + v3i * t3R;
      float u4r = v4r * t4R - v4i * t4I; float u4i = v4r * t4I + v4i * t4R;
      float u5r = v5r * t5R - v5i * t5I; float u5i = v5r * t5I + v5i * t5R;
      float u6r = v6r * t6R - v6i * t6I; float u6i = v6r * t6I + v6i * t6R;
      float u7r = v7r * t7R - v7i * t7I; float u7i = v7r * t7I + v7i * t7R;

      float a0r = v0r + u4r, a0i = v0i + u4i;
      float a1r = v0r - u4r, a1i = v0i - u4i;
      float a2r = u2r + u6r, a2i = u2i + u6i;
      float a3r = u2r - u6r, a3i = u2i - u6i;

      float e0r = a0r + a2r, e0i = a0i + a2i;
      float e1r = a1r + a3i, e1i = a1i - a3r;
      float e2r = a0r - a2r, e2i = a0i - a2i;
      float e3r = a1r - a3i, e3i = a1i + a3r;

      float b0r = u1r + u5r, b0i = u1i + u5i;
      float b1r = u1r - u5r, b1i = u1i - u5i;
      float b2r = u3r + u7r, b2i = u3i + u7i;
      float b3r = u3r - u7r, b3i = u3i - u7i;

      float o0r = b0r + b2r, o0i = b0i + b2i;
      float o1r = b1r + b3i, o1i = b1i - b3r;
      float o2r = b0r - b2r, o2i = b0i - b2i;
      float o3r = b1r - b3i, o3i = b1i + b3r;

      float r1r = s2 * (o1r + o1i); float r1i = s2 * (o1i - o1r);
      float r2r = o2i;              float r2i = -o2r;
      float r3r = s2 * (o3i - o3r); float r3i = s2 * (-o3r - o3i);

      w0r[k] = e0r + o0r; w0i[k] = e0i + o0i;
      w1r[k] = e1r + r1r; w1i[k] = e1i + r1i;
      w2r[k] = e2r + r2r; w2i[k] = e2i + r2i;
      w3r[k] = e3r + r3r; w3i[k] = e3i + r3i;
      w4r[k] = e0r - o0r; w4i[k] = e0i - o0i;
      w5r[k] = e1r - r1r; w5i[k] = e1i - r1i;
      w6r[k] = e2r - r2r; w6i[k] = e2i - r2i;
      w7r[k] = e3r - r3r; w7i[k] = e3i - r3i;
    }
  }
}

static inline void inner_power2_fft_f(pure_real_fftf_t* fft, float* work_re,
                                      float* work_im, bool inverse) {
  size_t n = fft->half_n;
  if (inverse) {
    PRAGMA_VECTORIZE_LOOP
    for (size_t i = 0; i < n; i++) {
      work_im[i] = -work_im[i];
    }
  }

  size_t m = 1;
  for (int s = 0; s < fft->stage_count; s++) {
    int r = fft->factors[s];
    const float* twRe = fft->twiddle_re[s];
    const float* twIm = fft->twiddle_im[s];
    if (r == 8) {
      radix8_stage_f(n, work_re, work_im, m, twRe, twIm);
    } else if (r == 4) {
      radix4_stage_f(n, work_re, work_im, m, twRe, twIm);
    } else {
      radix2_stage_f(n, work_re, work_im, m, twRe, twIm);
    }
    m *= (size_t)r;
  }

  if (inverse) {
    PRAGMA_VECTORIZE_LOOP
    for (size_t i = 0; i < n; i++) {
      work_im[i] = -work_im[i];
    }
  }
}

pure_real_fftf_t* pure_real_fftf_create(size_t length) {
  if (length < 8 || (length & (length - 1)) != 0) return NULL;
  size_t n = length / 2;

  int fs[32];
  int stage_count = 0;
  size_t rem = n;
  int two_pow = 0;
  while (rem % 2 == 0) {
    two_pow++;
    rem /= 2;
  }
  while (two_pow >= 3) {
    fs[stage_count++] = 8;
    two_pow -= 3;
  }
  if (two_pow == 2) {
    fs[stage_count++] = 4;
  } else if (two_pow == 1) {
    fs[stage_count++] = 2;
  }

  pure_real_fftf_t* fft = (pure_real_fftf_t*)calloc(1, sizeof(pure_real_fftf_t));
  if (!fft) return NULL;

  fft->length = length;
  fft->half_n = n;
  fft->stage_count = stage_count;
  fft->factors = (int*)calloc(stage_count, sizeof(int));
  fft->twiddle_re = (float**)calloc(stage_count, sizeof(float*));
  fft->twiddle_im = (float**)calloc(stage_count, sizeof(float*));
  fft->permutation = (size_t*)calloc(n, sizeof(size_t));
  fft->untw_re = (float*)cdsp_aligned_alloc(64, (n / 2 + 1) * sizeof(float));
  fft->untw_im = (float*)cdsp_aligned_alloc(64, (n / 2 + 1) * sizeof(float));
  fft->scratch_re = (float*)cdsp_aligned_alloc(64, n * sizeof(float));
  fft->scratch_im = (float*)cdsp_aligned_alloc(64, n * sizeof(float));
  fft->work_re = (float*)cdsp_aligned_alloc(64, n * sizeof(float));
  fft->work_im = (float*)cdsp_aligned_alloc(64, n * sizeof(float));

  if (!fft->factors || !fft->twiddle_re || !fft->twiddle_im ||
      !fft->permutation || !fft->untw_re || !fft->untw_im ||
      !fft->scratch_re || !fft->scratch_im ||
      !fft->work_re || !fft->work_im) {
    pure_real_fftf_free(fft);
    return NULL;
  }

  size_t m = 1;
  for (int s = 0; s < stage_count; s++) {
    int r = fs[s];
    fft->factors[s] = r;
    size_t len = m * (size_t)r;
    fft->twiddle_re[s] = (float*)cdsp_aligned_alloc(64, len * sizeof(float));
    fft->twiddle_im[s] = (float*)cdsp_aligned_alloc(64, len * sizeof(float));
    if (!fft->twiddle_re[s] || !fft->twiddle_im[s]) {
      pure_real_fftf_free(fft);
      return NULL;
    }
    float invMR = 1.0f / (float)(m * (size_t)r);
    for (int j = 0; j < r; j++) {
      for (size_t k = 0; k < m; k++) {
        float theta = -2.0f * (float)M_PI * (float)((size_t)j * k) * invMR;
        fft->twiddle_re[s][(size_t)j * m + k] = cosf(theta);
        fft->twiddle_im[s][(size_t)j * m + k] = sinf(theta);
      }
    }
    m *= (size_t)r;
  }

  for (size_t i = 0; i < n; i++) {
    size_t idx = i;
    size_t rev = 0;
    size_t m_left = n;
    for (int s = stage_count - 1; s >= 0; s--) {
      int r = fs[s];
      m_left /= (size_t)r;
      size_t d = idx % (size_t)r;
      idx /= (size_t)r;
      rev += d * m_left;
    }
    fft->permutation[i] = rev;
  }

  float invN = 1.0f / (float)n;
  for (size_t k = 0; k <= n / 2; k++) {
    float theta = -(float)M_PI * (float)k * invN;
    fft->untw_re[k] = cosf(theta);
    fft->untw_im[k] = sinf(theta);
  }

  return fft;
}

void pure_real_fftf_forward(pure_real_fftf_t* fft, const float* real_in,
                            float* spec_re, float* spec_im) {
  if (!fft) return;
  size_t n = fft->half_n;
  float* wr = fft->scratch_re;
  float* wi = fft->scratch_im;
  const size_t* perm = fft->permutation;

  PRAGMA_VECTORIZE_LOOP
  for (size_t k = 0; k < n; k++) {
    size_t p = perm[k];
    wr[p] = real_in[2 * k];
    wi[p] = real_in[2 * k + 1];
  }

  inner_power2_fft_f(fft, wr, wi, false);

  spec_re[0] = wr[0] + wi[0];
  spec_im[0] = 0.0f;
  spec_re[n] = wr[0] - wi[0];
  spec_im[n] = 0.0f;

  const float* untwR = fft->untw_re;
  const float* untwI = fft->untw_im;
  size_t half_len = n / 2;

  PRAGMA_VECTORIZE_LOOP
  for (size_t k = 1; k <= half_len; k++) {
    float zkR = wr[k];
    float zkI = wi[k];
    float zmkR = wr[n - k];
    float zmkI = wi[n - k];

    float eRe = 0.5f * (zkR + zmkR);
    float eIm = 0.5f * (zkI - zmkI);
    float diffRe = zkR - zmkR;
    float diffIm = zkI + zmkI;

    float oRe = 0.5f * diffIm;
    float oIm = -0.5f * diffRe;

    float twR = untwR[k];
    float twI = untwI[k];

    float woRe = twR * oRe - twI * oIm;
    float woIm = twR * oIm + twI * oRe;

    spec_re[k] = eRe + woRe;
    spec_im[k] = eIm + woIm;
    spec_re[n - k] = eRe - woRe;
    spec_im[n - k] = -(eIm - woIm);
  }
}

void pure_real_fftf_inverse(pure_real_fftf_t* fft, const float* spec_re,
                            const float* spec_im, float* real_out) {
  if (!fft) return;
  size_t n = fft->half_n;
  float* wr = fft->scratch_re;
  float* wi = fft->scratch_im;
  float* pr = fft->work_re;
  float* pi = fft->work_im;
  const size_t* perm = fft->permutation;

  wr[0] = 0.5f * (spec_re[0] + spec_re[n]);
  wi[0] = 0.5f * (spec_re[0] - spec_re[n]);

  const float* untwR = fft->untw_re;
  const float* untwI = fft->untw_im;
  size_t half_len = n / 2;

  PRAGMA_VECTORIZE_LOOP
  for (size_t k = 1; k <= half_len; k++) {
    float xkR = spec_re[k];
    float xkI = spec_im[k];
    float xmkR = spec_re[n - k];
    float xmkI = spec_im[n - k];

    float eRe = 0.5f * (xkR + xmkR);
    float eIm = 0.5f * (xkI - xmkI);
    float halfDiffRe = 0.5f * (xkR - xmkR);
    float halfDiffIm = 0.5f * (xkI + xmkI);

    float twR = untwR[k];
    float twI = untwI[k];

    float oRe = halfDiffRe * twR + halfDiffIm * twI;
    float oIm = halfDiffIm * twR - halfDiffRe * twI;

    wr[k] = eRe - oIm;
    wi[k] = eIm + oRe;
    wr[n - k] = eRe + oIm;
    wi[n - k] = -(eIm - oRe);
  }

  PRAGMA_VECTORIZE_LOOP
  for (size_t k = 0; k < n; k++) {
    size_t p = perm[k];
    pr[p] = wr[k];
    pi[p] = wi[k];
  }

  inner_power2_fft_f(fft, pr, pi, true);

  PRAGMA_VECTORIZE_LOOP
  for (size_t k = 0; k < n; k++) {
    real_out[2 * k] = 2.0f * pr[k];
    real_out[2 * k + 1] = 2.0f * pi[k];
  }
}

void pure_real_fftf_free(pure_real_fftf_t* fft) {
  if (!fft) return;
  if (fft->twiddle_re) {
    for (int s = 0; s < fft->stage_count; s++) {
      if (fft->twiddle_re[s]) cdsp_aligned_free(fft->twiddle_re[s]);
    }
    free(fft->twiddle_re);
  }
  if (fft->twiddle_im) {
    for (int s = 0; s < fft->stage_count; s++) {
      if (fft->twiddle_im[s]) cdsp_aligned_free(fft->twiddle_im[s]);
    }
    free(fft->twiddle_im);
  }
  if (fft->factors) free(fft->factors);
  if (fft->permutation) free(fft->permutation);
  if (fft->untw_re) cdsp_aligned_free(fft->untw_re);
  if (fft->untw_im) cdsp_aligned_free(fft->untw_im);
  if (fft->scratch_re) cdsp_aligned_free(fft->scratch_re);
  if (fft->scratch_im) cdsp_aligned_free(fft->scratch_im);
  if (fft->work_re) cdsp_aligned_free(fft->work_re);
  if (fft->work_im) cdsp_aligned_free(fft->work_im);
  free(fft);
}

static void pure_real_fftf_forward_wrapper(void* ctx, const float* real_in,
                                          float* spec_re, float* spec_im) {
  pure_real_fftf_forward((pure_real_fftf_t*)ctx, real_in, spec_re, spec_im);
}

static void pure_real_fftf_inverse_wrapper(void* ctx, const float* spec_re,
                                          const float* spec_im,
                                          float* real_out) {
  pure_real_fftf_inverse((pure_real_fftf_t*)ctx, spec_re, spec_im, real_out);
}

static void pure_real_fftf_free_wrapper(void* ctx) {
  pure_real_fftf_free((pure_real_fftf_t*)ctx);
}

real_fftf_backend_t* pure_real_fftf_as_backend(pure_real_fftf_t* fft) {
  if (!fft) return NULL;
  fft->base.ctx = fft;
  fft->base.forward = pure_real_fftf_forward_wrapper;
  fft->base.inverse = pure_real_fftf_inverse_wrapper;
  fft->base.free = pure_real_fftf_free_wrapper;
  return &fft->base;
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC pop_options
#elif defined(__clang__)
#pragma float_control(pop)
#endif
