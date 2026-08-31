// Arbitrary-N complex DFT via iterative DIT Cooley-Tukey where all prime
// factors are all ≤ 7. Targets `N = 1029 = 3 · 7³` and `N = 1120 = 2⁵ · 5 · 7`
// — the inner FFT sizes that `real_fft` needs for 44.1↔48 kHz
// resampling. Compared with Bluestein-on-vDSP, this trades the inner
// power-of-2 transforms (M = 4096) for a direct decomposition into
// `O(N · Σ pᵢ)` ops — about 6× fewer arithmetic operations at N = 1029.
//
// Note on the radix-2/4/8 stages: they're not redundant with
// `real_fft`'s outer `vdsp_real_fft` fast path. That fast path
// fires only when the *whole* real-FFT length is a power of two; the
// radix-2/4/8 stages here handle the *power-of-two portion* of a mixed
// factorisation (e.g. `1120 = 2⁵·5·7` collapses into `[8, 4, 5, 7]`).
// Without them this module could only support odd-prime-only sizes like
// `105 = 3·5·7`, and most of our resampler's mixed-rate FFTs would fall
// through to Bluestein.
//
// Architecture: classic iterative DIT (decimation-in-time) Cooley-Tukey.
//   1. Permute input via mixed-radix digit reversal.
//   2. For each factor `r` (in order), apply length-`r` butterflies on
//      stride-`m` groups, where `m` grows by `r` after each stage. Twiddle
//      factors W_{m·r}^(j·k) are pre-computed once at init.
//   3. Copy out (with conjugation for the inverse direction).
//
// Inverse FFT uses the identity `IDFT(x) = conj(DFT(conj(x)))`, so we only
// pre-compute the forward twiddles. Both transforms are unnormalised.
//
// All buffers (twiddles, permutation LUT, scratch) are heap-allocated at
// init and freed in deinit. The hot path runs purely on raw pointers — no
// allocations, no closures.

#include "FFT/mixed_radix_fft.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "Utils/double_helpers.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct mixed_radix_fft {
  arbitrary_complex_fft_t base;
  size_t n;
  int stage_count;
  /// Prime factorisation of `n`, smallest first. The DIT stages walk this
  /// list left-to-right.
  int* factors;
  /// Per-stage forward twiddles, length `m_s · r_s` (for stage `s` with
  /// pre-stage subblock size `m_s` and radix `r_s`). The `j = 0` row is
  /// trivial (W^0 = 1) but we keep it for uniform indexing.
  double** twiddle_re;
  double** twiddle_im;
  /// Mixed-radix digit-reversal permutation. `permutation[i]` is where
  /// input element `i` ends up in the post-permutation buffer.
  size_t* permutation;
  /// Active read/write buffers for the butterfly stages. Re-pointed at
  /// the caller's `realOut`/`imagOut` at the start of each `execute`
  /// call — the permutation step writes the post-permute samples
  /// directly into the output buffer, every stage runs in-place on
  /// the output, and we skip the final memcpy that the older "internal
  /// scratch + copy out" pattern needed.
  ///
  double* work_re;
  double* work_im;
  double* scratch_re;
  double* scratch_im;
};

/**
 * @brief Wrapper for the mixed-radix FFT execution.
 *
 * Conforms to the arbitrary_complex_fft_t interface.
 */
static void mixed_radix_fft_execute_wrapper(void* ctx, waveform_t real_in,
                                            waveform_t imag_in,
                                            mutable_waveform_t real_out,
                                            mutable_waveform_t imag_out,
                                            bool inverse) {
  mixed_radix_fft_execute((mixed_radix_fft_t*)ctx, real_in, imag_in, real_out,
                          imag_out, inverse);
}

/**
 * @brief Wrapper for the mixed-radix FFT free function.
 *
 * Conforms to the arbitrary_complex_fft_t interface.
 */
static void mixed_radix_fft_free_wrapper(void* ctx) {
  mixed_radix_fft_free((mixed_radix_fft_t*)ctx);
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC push_options
#pragma GCC optimize("O3,fast-math,finite-math-only")
#elif defined(__clang__)
#pragma float_control(precise, off, push)
#endif

/**
 * @brief Apply radix-2 butterflies across `n / (m·2)` blocks of size `m·2`.
 *
 * Twiddle table layout: twRe[j·m + k] for j ∈ {0, 1}, k ∈ [0, m).
 *
 * Computes:
 *   v0 = x0 + x1 * tw
 *   v1 = x0 - x1 * tw
 *
 * @param fft Pointer to the FFT context.
 * @param m Current subblock size.
 * @param tw_re Real part of twiddle factors.
 * @param tw_im Imaginary part of twiddle factors.
 */
static inline void stage_radix2(mixed_radix_fft_t* fft, double* work_re,
                                double* work_im, size_t m, const double* tw_re,
                                const double* tw_im) {
  size_t block_size = m * 2;
  if (m == 1) {
#if defined(__clang__)
#pragma clang loop vectorize(assume_safety) interleave(enable)
#elif defined(__GNUC__)
#pragma GCC ivdep
#endif
    for (size_t b = 0; b < fft->n; b += 2) {
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
  for (size_t b = 0; b < fft->n; b += block_size) {
    double* w0r = work_re + b;
    double* w0i = work_im + b;
    double* w1r = w0r + m;
    double* w1i = w0i + m;
#if defined(__clang__)
#pragma clang loop vectorize(assume_safety) interleave(enable)
#elif defined(__GNUC__)
#pragma GCC ivdep
#endif
    for (size_t k = 0; k < m; k++) {
      double twR = tw1R[k];
      double twI = tw1I[k];
      double v1r = w1r[k] * twR - w1i[k] * twI;
      double v1i = w1r[k] * twI + w1i[k] * twR;
      double v0r = w0r[k];
      double v0i = w0i[k];
      w0r[k] = v0r + v1r;
      w0i[k] = v0i + v1i;
      w1r[k] = v0r - v1r;
      w1i[k] = v0i - v1i;
    }
  }
}

/**
 * @brief Apply radix-3 butterflies. Same layout as radix-2.
 *
 * Uses the standard radix-3 DFT formulation:
 *   s = v1 + v2
 *   d = v1 - v2
 *   a = v0 - 0.5 * s
 *   b = i * sin(2π/3) * d
 *   O[0] = v0 + s
 *   O[1] = a + b
 *   O[2] = a - b
 *
 * @param fft Pointer to the FFT context.
 * @param m Current subblock size.
 * @param tw_re Real part of twiddle factors.
 * @param tw_im Imaginary part of twiddle factors.
 */
static inline void stage_radix3(mixed_radix_fft_t* fft, double* work_re,
                                double* work_im, size_t m, const double* tw_re,
                                const double* tw_im) {
  size_t block_size = m * 3;
  // W3 = exp(-2π i / 3) = (-1/2, -√3/2). The constant `√3/2` recurs below.
  double s32 = sin(2.0 * M_PI / 3.0);
  if (m == 1) {
    size_t b = 0;
    for (; b + 6 <= fft->n; b += 6) {
      double v0r_0 = work_re[b];
      double v0i_0 = work_im[b];
      double v1r_0 = work_re[b + 1];
      double v1i_0 = work_im[b + 1];
      double v2r_0 = work_re[b + 2];
      double v2i_0 = work_im[b + 2];

      double v0r_1 = work_re[b + 3];
      double v0i_1 = work_im[b + 3];
      double v1r_1 = work_re[b + 4];
      double v1i_1 = work_im[b + 4];
      double v2r_1 = work_re[b + 5];
      double v2i_1 = work_im[b + 5];

      double sR_0 = v1r_0 + v2r_0;
      double sI_0 = v1i_0 + v2i_0;
      double dR_0 = v1r_0 - v2r_0;
      double dI_0 = v1i_0 - v2i_0;
      double aR_0 = v0r_0 - 0.5 * sR_0;
      double aI_0 = v0i_0 - 0.5 * sI_0;
      double bR_0 = s32 * dR_0;
      double bI_0 = s32 * dI_0;

      double sR_1 = v1r_1 + v2r_1;
      double sI_1 = v1i_1 + v2i_1;
      double dR_1 = v1r_1 - v2r_1;
      double dI_1 = v1i_1 - v2i_1;
      double aR_1 = v0r_1 - 0.5 * sR_1;
      double aI_1 = v0i_1 - 0.5 * sI_1;
      double bR_1 = s32 * dR_1;
      double bI_1 = s32 * dI_1;

      work_re[b] = v0r_0 + sR_0;
      work_im[b] = v0i_0 + sI_0;
      work_re[b + 1] = aR_0 + bI_0;
      work_im[b + 1] = aI_0 - bR_0;
      work_re[b + 2] = aR_0 - bI_0;
      work_im[b + 2] = aI_0 + bR_0;

      work_re[b + 3] = v0r_1 + sR_1;
      work_im[b + 3] = v0i_1 + sI_1;
      work_re[b + 4] = aR_1 + bI_1;
      work_im[b + 4] = aI_1 - bR_1;
      work_re[b + 5] = aR_1 - bI_1;
      work_im[b + 5] = aI_1 + bR_1;
    }
    for (; b < fft->n; b += 3) {
      double v0r = work_re[b];
      double v0i = work_im[b];
      double v1r = work_re[b + 1];
      double v1i = work_im[b + 1];
      double v2r = work_re[b + 2];
      double v2i = work_im[b + 2];
      double sR = v1r + v2r;
      double sI = v1i + v2i;
      double dR = v1r - v2r;
      double dI = v1i - v2i;
      double aR = v0r - 0.5 * sR;
      double aI = v0i - 0.5 * sI;
      double bR = s32 * dR;
      double bI = s32 * dI;
      work_re[b] = v0r + sR;
      work_im[b] = v0i + sI;
      work_re[b + 1] = aR + bI;
      work_im[b + 1] = aI - bR;
      work_re[b + 2] = aR - bI;
      work_im[b + 2] = aI + bR;
    }
    return;
  }
  const double* tw1R = tw_re + m;
  const double* tw1I = tw_im + m;
  const double* tw2R = tw_re + 2 * m;
  const double* tw2I = tw_im + 2 * m;
  for (size_t b = 0; b < fft->n; b += block_size) {
    double* w0r = work_re + b;
    double* w0i = work_im + b;
    double* w1r = w0r + m;
    double* w1i = w0i + m;
    double* w2r = w1r + m;
    double* w2i = w1i + m;
#if defined(__clang__)
#pragma clang loop vectorize(assume_safety) interleave(enable)
#elif defined(__GNUC__)
#pragma GCC ivdep
#endif
    for (size_t k = 0; k < m; k++) {
      double tw1R_k = tw1R[k];
      double tw1I_k = tw1I[k];
      double tw2R_k = tw2R[k];
      double tw2I_k = tw2I[k];
      // Twiddle.
      double v1r = w1r[k] * tw1R_k - w1i[k] * tw1I_k;
      double v1i = w1r[k] * tw1I_k + w1i[k] * tw1R_k;
      double v2r = w2r[k] * tw2R_k - w2i[k] * tw2I_k;
      double v2i = w2r[k] * tw2I_k + w2i[k] * tw2R_k;
      double v0r = w0r[k];
      double v0i = w0i[k];
      // Radix-3 DFT.
      double sR = v1r + v2r;
      double sI = v1i + v2i;
      double dR = v1r - v2r;
      double dI = v1i - v2i;
      double aR = v0r - 0.5 * sR;
      double aI = v0i - 0.5 * sI;
      double bR = s32 * dR;
      double bI = s32 * dI;
      w0r[k] = v0r + sR;
      w0i[k] = v0i + sI;
      w1r[k] = aR + bI;
      w1i[k] = aI - bR;
      w2r[k] = aR - bI;
      w2i[k] = aI + bR;
    }
  }
}

/**
 * @brief Apply radix-4 butterflies.
 *
 * The inner DFT is multiplication-free — the four 4th-roots of unity are
 * `{1, -i, -1, i}`, so the inner stage is just adds and ±i swaps.
 * Only the 3 outer-stage twiddles (`v[1], v[2], v[3]`) cost real multiplies.
 *
 * @param fft Pointer to the FFT context.
 * @param m Current subblock size.
 * @param tw_re Real part of twiddle factors.
 * @param tw_im Imaginary part of twiddle factors.
 */
static inline void stage_radix4(mixed_radix_fft_t* fft, double* work_re,
                                double* work_im, size_t m, const double* tw_re,
                                const double* tw_im) {
  size_t block_size = m * 4;
  if (m == 1) {
#if defined(__clang__)
#pragma clang loop vectorize(assume_safety) interleave(enable)
#elif defined(__GNUC__)
#pragma GCC ivdep
#endif
    for (size_t b = 0; b < fft->n; b += 4) {
      double v0r = work_re[b];
      double v0i = work_im[b];
      double v1r = work_re[b + 1];
      double v1i = work_im[b + 1];
      double v2r = work_re[b + 2];
      double v2i = work_im[b + 2];
      double v3r = work_re[b + 3];
      double v3i = work_im[b + 3];
      double t0r = v0r + v2r;
      double t0i = v0i + v2i;
      double t1r2 = v0r - v2r;
      double t1i2 = v0i - v2i;
      double t2r2 = v1r + v3r;
      double t2i2 = v1i + v3i;
      double t3r2 = v1r - v3r;
      double t3i2 = v1i - v3i;
      work_re[b] = t0r + t2r2;
      work_im[b] = t0i + t2i2;
      work_re[b + 1] = t1r2 + t3i2;
      work_im[b + 1] = t1i2 - t3r2;
      work_re[b + 2] = t0r - t2r2;
      work_im[b + 2] = t0i - t2i2;
      work_re[b + 3] = t1r2 - t3i2;
      work_im[b + 3] = t1i2 + t3r2;
    }
    return;
  }
  const double* tw1R = tw_re + m;
  const double* tw1I = tw_im + m;
  const double* tw2R = tw_re + 2 * m;
  const double* tw2I = tw_im + 2 * m;
  const double* tw3R = tw_re + 3 * m;
  const double* tw3I = tw_im + 3 * m;
  for (size_t b = 0; b < fft->n; b += block_size) {
    double* w0r = work_re + b;
    double* w0i = work_im + b;
    double* w1r = w0r + m;
    double* w1i = w0i + m;
    double* w2r = w1r + m;
    double* w2i = w1i + m;
    double* w3r = w2r + m;
    double* w3i = w2i + m;
#if defined(__clang__)
#pragma clang loop vectorize(assume_safety) interleave(enable)
#elif defined(__GNUC__)
#pragma GCC ivdep
#endif
    for (size_t k = 0; k < m; k++) {
      double tw1R_k = tw1R[k];
      double tw1I_k = tw1I[k];
      double tw2R_k = tw2R[k];
      double tw2I_k = tw2I[k];
      double tw3R_k = tw3R[k];
      double tw3I_k = tw3I[k];
      double v0r = w0r[k];
      double v0i = w0i[k];
      double v1r = w1r[k] * tw1R_k - w1i[k] * tw1I_k;
      double v1i = w1r[k] * tw1I_k + w1i[k] * tw1R_k;
      double v2r = w2r[k] * tw2R_k - w2i[k] * tw2I_k;
      double v2i = w2r[k] * tw2I_k + w2i[k] * tw2R_k;
      double v3r = w3r[k] * tw3R_k - w3i[k] * tw3I_k;
      double v3i = w3r[k] * tw3I_k + w3i[k] * tw3R_k;
      // Inner radix-4 DFT: T0=v0+v2, T1=v0-v2, T2=v1+v3, T3=v1-v3
      // O[0]=T0+T2, O[1]=T1-i·T3, O[2]=T0-T2, O[3]=T1+i·T3
      // -i·z = (z.im, -z.re); +i·z = (-z.im, z.re).
      double t0r = v0r + v2r;
      double t0i = v0i + v2i;
      double t1r2 = v0r - v2r;
      double t1i2 = v0i - v2i;
      double t2r2 = v1r + v3r;
      double t2i2 = v1i + v3i;
      double t3r2 = v1r - v3r;
      double t3i2 = v1i - v3i;
      w0r[k] = t0r + t2r2;
      w0i[k] = t0i + t2i2;
      w1r[k] = t1r2 + t3i2;
      w1i[k] = t1i2 - t3r2;
      w2r[k] = t0r - t2r2;
      w2i[k] = t0i - t2i2;
      w3r[k] = t1r2 - t3i2;
      w3i[k] = t1i2 + t3r2;
    }
  }
}

/**
 * @brief Apply radix-5 butterflies.
 *
 * Output layout uses the conjugate-pair factoring trick:
 *     O[k] = v0 + Σ_p W^(p·k) · v_p,  W = exp(-2πi/5)
 *
 * Outputs `O[1]` and `O[4]` differ only in the sign of the `W^(p·k)·v_p`
 * imaginary parts (since `W^4 = conj(W)`); same for `O[2]` and `O[3]`
 * (since `W^3 = conj(W²)`). Pre-compute a "common" w_R-weighted sum and
 * a "twist" w_I-weighted sum once per pair, then assemble the four
 * outputs as `r0 ± common ± twist`. Cuts the multiplies per butterfly
 * from ~48 to ~32 (-33 %) without changing the scalar tail's
 * arithmetic identity.
 *
 * @param fft Pointer to the FFT context.
 * @param m Current subblock size.
 * @param tw_re Real part of twiddle factors.
 * @param tw_im Imaginary part of twiddle factors.
 */
static inline void stage_radix5(mixed_radix_fft_t* fft, double* work_re,
                                double* work_im, size_t m, const double* tw_re,
                                const double* tw_im) {
  size_t block_size = m * 5;
  // Radix-5 uses these inner DFT constants. tw_5^k = exp(-2πi·k/5).
  double w1R = cos(2.0 * M_PI / 5.0);
  double w1I = -sin(2.0 * M_PI / 5.0);
  double w2R = cos(4.0 * M_PI / 5.0);
  double w2I = -sin(4.0 * M_PI / 5.0);

  if (m == 1) {
    size_t b = 0;
    for (; b + 10 <= fft->n; b += 10) {
      double v0r_0 = work_re[b];
      double v0i_0 = work_im[b];
      double v1r_0 = work_re[b + 1];
      double v1i_0 = work_im[b + 1];
      double v2r_0 = work_re[b + 2];
      double v2i_0 = work_im[b + 2];
      double v3r_0 = work_re[b + 3];
      double v3i_0 = work_im[b + 3];
      double v4r_0 = work_re[b + 4];
      double v4i_0 = work_im[b + 4];

      double v0r_1 = work_re[b + 5];
      double v0i_1 = work_im[b + 5];
      double v1r_1 = work_re[b + 6];
      double v1i_1 = work_im[b + 6];
      double v2r_1 = work_re[b + 7];
      double v2i_1 = work_im[b + 7];
      double v3r_1 = work_re[b + 8];
      double v3i_1 = work_im[b + 8];
      double v4r_1 = work_re[b + 9];
      double v4i_1 = work_im[b + 9];

      // Butterfly 0
      double sum14R_0 = v1r_0 + v4r_0;
      double sum14I_0 = v1i_0 + v4i_0;
      double diff14R_0 = v1r_0 - v4r_0;
      double diff14I_0 = v1i_0 - v4i_0;
      double sum23R_0 = v2r_0 + v3r_0;
      double sum23I_0 = v2i_0 + v3i_0;
      double diff23R_0 = v2r_0 - v3r_0;
      double diff23I_0 = v2i_0 - v3i_0;
      work_re[b] = v0r_0 + sum14R_0 + sum23R_0;
      work_im[b] = v0i_0 + sum14I_0 + sum23I_0;
      double cR14_0 = w1R * sum14R_0 + w2R * sum23R_0;
      double cI14_0 = w1R * sum14I_0 + w2R * sum23I_0;
      double tR14_0 = w1I * diff14I_0 + w2I * diff23I_0;
      double tI14_0 = w1I * diff14R_0 + w2I * diff23R_0;
      work_re[b + 1] = v0r_0 + cR14_0 - tR14_0;
      work_im[b + 1] = v0i_0 + cI14_0 + tI14_0;
      work_re[b + 4] = v0r_0 + cR14_0 + tR14_0;
      work_im[b + 4] = v0i_0 + cI14_0 - tI14_0;
      double cR23_0 = w2R * sum14R_0 + w1R * sum23R_0;
      double cI23_0 = w2R * sum14I_0 + w1R * sum23I_0;
      double tR23_0 = w2I * diff14I_0 - w1I * diff23I_0;
      double tI23_0 = w2I * diff14R_0 - w1I * diff23R_0;
      work_re[b + 2] = v0r_0 + cR23_0 - tR23_0;
      work_im[b + 2] = v0i_0 + cI23_0 + tI23_0;
      work_re[b + 3] = v0r_0 + cR23_0 + tR23_0;
      work_im[b + 3] = v0i_0 + cI23_0 - tI23_0;

      // Butterfly 1
      double sum14R_1 = v1r_1 + v4r_1;
      double sum14I_1 = v1i_1 + v4i_1;
      double diff14R_1 = v1r_1 - v4r_1;
      double diff14I_1 = v1i_1 - v4i_1;
      double sum23R_1 = v2r_1 + v3r_1;
      double sum23I_1 = v2i_1 + v3i_1;
      double diff23R_1 = v2r_1 - v3r_1;
      double diff23I_1 = v2i_1 - v3i_1;
      work_re[b + 5] = v0r_1 + sum14R_1 + sum23R_1;
      work_im[b + 5] = v0i_1 + sum14I_1 + sum23I_1;
      double cR14_1 = w1R * sum14R_1 + w2R * sum23R_1;
      double cI14_1 = w1R * sum14I_1 + w2R * sum23I_1;
      double tR14_1 = w1I * diff14I_1 + w2I * diff23I_1;
      double tI14_1 = w1I * diff14R_1 + w2I * diff23R_1;
      work_re[b + 6] = v0r_1 + cR14_1 - tR14_1;
      work_im[b + 6] = v0i_1 + cI14_1 + tI14_1;
      work_re[b + 9] = v0r_1 + cR14_1 + tR14_1;
      work_im[b + 9] = v0i_1 + cI14_1 - tI14_1;
      double cR23_1 = w2R * sum14R_1 + w1R * sum23R_1;
      double cI23_1 = w2R * sum14I_1 + w1R * sum23I_1;
      double tR23_1 = w2I * diff14I_1 - w1I * diff23I_1;
      double tI23_1 = w2I * diff14R_1 - w1I * diff23R_1;
      work_re[b + 7] = v0r_1 + cR23_1 - tR23_1;
      work_im[b + 7] = v0i_1 + cI23_1 + tI23_1;
      work_re[b + 8] = v0r_1 + cR23_1 + tR23_1;
      work_im[b + 8] = v0i_1 + cI23_1 - tI23_1;
    }
    for (; b < fft->n; b += 5) {
      double v0r = work_re[b];
      double v0i = work_im[b];
      double v1r = work_re[b + 1];
      double v1i = work_im[b + 1];
      double v2r = work_re[b + 2];
      double v2i = work_im[b + 2];
      double v3r = work_re[b + 3];
      double v3i = work_im[b + 3];
      double v4r = work_re[b + 4];
      double v4i = work_im[b + 4];
      double sum14R = v1r + v4r;
      double sum14I = v1i + v4i;
      double diff14R = v1r - v4r;
      double diff14I = v1i - v4i;
      double sum23R = v2r + v3r;
      double sum23I = v2i + v3i;
      double diff23R = v2r - v3r;
      double diff23I = v2i - v3i;
      work_re[b] = v0r + sum14R + sum23R;
      work_im[b] = v0i + sum14I + sum23I;
      double cR14 = w1R * sum14R + w2R * sum23R;
      double cI14 = w1R * sum14I + w2R * sum23I;
      double tR14 = w1I * diff14I + w2I * diff23I;
      double tI14 = w1I * diff14R + w2I * diff23R;
      work_re[b + 1] = v0r + cR14 - tR14;
      work_im[b + 1] = v0i + cI14 + tI14;
      work_re[b + 4] = v0r + cR14 + tR14;
      work_im[b + 4] = v0i + cI14 - tI14;
      double cR23 = w2R * sum14R + w1R * sum23R;
      double cI23 = w2R * sum14I + w1R * sum23I;
      double tR23 = w2I * diff14I - w1I * diff23I;
      double tI23 = w2I * diff14R - w1I * diff23R;
      work_re[b + 2] = v0r + cR23 - tR23;
      work_im[b + 2] = v0i + cI23 + tI23;
      work_re[b + 3] = v0r + cR23 + tR23;
      work_im[b + 3] = v0i + cI23 - tI23;
    }
    return;
  }
  const double* tw1R = tw_re + m;
  const double* tw1I = tw_im + m;
  const double* tw2R = tw_re + 2 * m;
  const double* tw2I = tw_im + 2 * m;
  const double* tw3R = tw_re + 3 * m;
  const double* tw3I = tw_im + 3 * m;
  const double* tw4R = tw_re + 4 * m;
  const double* tw4I = tw_im + 4 * m;
  for (size_t b = 0; b < fft->n; b += block_size) {
    double* w0r = work_re + b;
    double* w0i = work_im + b;
    double* w1r = w0r + m;
    double* w1i = w0i + m;
    double* w2r = w1r + m;
    double* w2i = w1i + m;
    double* w3r = w2r + m;
    double* w3i = w2i + m;
    double* w4r = w3r + m;
    double* w4i = w3i + m;
#if defined(__clang__)
#pragma clang loop vectorize(assume_safety) interleave(enable)
#elif defined(__GNUC__)
#pragma GCC ivdep
#endif
    for (size_t k = 0; k < m; k++) {
      // Outer-stage twiddle on samples 1..4.
      double t1R = tw1R[k];
      double t1I = tw1I[k];
      double t2R = tw2R[k];
      double t2I = tw2I[k];
      double t3R = tw3R[k];
      double t3I = tw3I[k];
      double t4R = tw4R[k];
      double t4I = tw4I[k];
      double v1r = w1r[k] * t1R - w1i[k] * t1I;
      double v1i = w1r[k] * t1I + w1i[k] * t1R;
      double v2r = w2r[k] * t2R - w2i[k] * t2I;
      double v2i = w2r[k] * t2I + w2i[k] * t2R;
      double v3r = w3r[k] * t3R - w3i[k] * t3I;
      double v3i = w3r[k] * t3I + w3i[k] * t3R;
      double v4r = w4r[k] * t4R - w4i[k] * t4I;
      double v4i = w4r[k] * t4I + w4i[k] * t4R;
      double v0r = w0r[k];
      double v0i = w0i[k];
      // Radix-5 DFT (direct, not Winograd). 4 unique inner products plus
      // the DC term — straightforward and lets the compiler issue plenty
      // of FMAs.
      //
      //   O[0] = v0 + v1 + v2 + v3 + v4
      //   O[k] = v0 + W^k·v1 + W^(2k)·v2 + W^(3k)·v3 + W^(4k)·v4,
      //          W = exp(-2πi/5).
      //
      // Since W^4 = conj(W) and W^3 = conj(W²), the four non-DC outputs
      // come in two conjugate pairs: (O[1], O[4]) and (O[2], O[3]). Each
      // pair shares a "common" (w_R · sum) term and a "twist"
      // (w_I · diff) term — see the SIMD2 body above for the derivation.
      double sum14R = v1r + v4r;
      double sum14I = v1i + v4i;
      double diff14R = v1r - v4r;
      double diff14I = v1i - v4i;
      double sum23R = v2r + v3r;
      double sum23I = v2i + v3i;
      double diff23R = v2r - v3r;
      double diff23I = v2i - v3i;
      // O[0]
      w0r[k] = v0r + sum14R + sum23R;
      w0i[k] = v0i + sum14I + sum23I;
      // Conjugate pair (1, 4).
      double cR14 = w1R * sum14R + w2R * sum23R;
      double cI14 = w1R * sum14I + w2R * sum23I;
      double tR14 = w1I * diff14I + w2I * diff23I;
      double tI14 = w1I * diff14R + w2I * diff23R;
      w1r[k] = v0r + cR14 - tR14;
      w1i[k] = v0i + cI14 + tI14;
      w4r[k] = v0r + cR14 + tR14;
      w4i[k] = v0i + cI14 - tI14;
      // Conjugate pair (2, 3).
      double cR23 = w2R * sum14R + w1R * sum23R;
      double cI23 = w2R * sum14I + w1R * sum23I;
      double tR23 = w2I * diff14I - w1I * diff23I;
      double tI23 = w2I * diff14R - w1I * diff23R;
      w2r[k] = v0r + cR23 - tR23;
      w2i[k] = v0i + cI23 + tI23;
      w3r[k] = v0r + cR23 + tR23;
      w3i[k] = v0i + cI23 - tI23;
    }
  }
}

/**
 * @brief Apply radix-7 butterflies.
 *
 * Direct DFT — 6 unique pairs of conjugate twiddles. Compute each output as
 * `v0 + Σ pair-products`.
 *
 * The six non-DC outputs come in three conjugate pairs:
 * `(O[1], O[6])`, `(O[2], O[5])`, `(O[3], O[4])` — each pair shares a
 * w_R-weighted "common" term and a w_I-weighted "twist" term, with
 * only the twist's sign (and the imag flip) distinguishing the two
 * outputs in a pair. This factoring cuts the multiplies per butterfly
 * from ~96 to ~60 (-38 %) versus computing each output from scratch.
 *
 * @param fft Pointer to the FFT context.
 * @param m Current subblock size.
 * @param tw_re Real part of twiddle factors.
 * @param tw_im Imaginary part of twiddle factors.
 */
static inline void stage_radix7(mixed_radix_fft_t* fft, double* work_re,
                                double* work_im, size_t m, const double* tw_re,
                                const double* tw_im) {
  size_t block_size = m * 7;
  double w1R = cos(2.0 * M_PI / 7.0);
  double w1I = -sin(2.0 * M_PI / 7.0);
  double w2R = cos(4.0 * M_PI / 7.0);
  double w2I = -sin(4.0 * M_PI / 7.0);
  double w3R = cos(6.0 * M_PI / 7.0);
  double w3I = -sin(6.0 * M_PI / 7.0);

  if (m == 1) {
    size_t b = 0;
    for (; b + 14 <= fft->n; b += 14) {
      double v0r_0 = work_re[b];
      double v0i_0 = work_im[b];
      double v1r_0 = work_re[b + 1];
      double v1i_0 = work_im[b + 1];
      double v2r_0 = work_re[b + 2];
      double v2i_0 = work_im[b + 2];
      double v3r_0 = work_re[b + 3];
      double v3i_0 = work_im[b + 3];
      double v4r_0 = work_re[b + 4];
      double v4i_0 = work_im[b + 4];
      double v5r_0 = work_re[b + 5];
      double v5i_0 = work_im[b + 5];
      double v6r_0 = work_re[b + 6];
      double v6i_0 = work_im[b + 6];

      double v0r_1 = work_re[b + 7];
      double v0i_1 = work_im[b + 7];
      double v1r_1 = work_re[b + 8];
      double v1i_1 = work_im[b + 8];
      double v2r_1 = work_re[b + 9];
      double v2i_1 = work_im[b + 9];
      double v3r_1 = work_re[b + 10];
      double v3i_1 = work_im[b + 10];
      double v4r_1 = work_re[b + 11];
      double v4i_1 = work_im[b + 11];
      double v5r_1 = work_re[b + 12];
      double v5i_1 = work_im[b + 12];
      double v6r_1 = work_re[b + 13];
      double v6i_1 = work_im[b + 13];

      // Butterfly 0
      double s16R_0 = v1r_0 + v6r_0;
      double s16I_0 = v1i_0 + v6i_0;
      double d16R_0 = v1r_0 - v6r_0;
      double d16I_0 = v1i_0 - v6i_0;
      double s25R_0 = v2r_0 + v5r_0;
      double s25I_0 = v2i_0 + v5i_0;
      double d25R_0 = v2r_0 - v5r_0;
      double d25I_0 = v2i_0 - v5i_0;
      double s34R_0 = v3r_0 + v4r_0;
      double s34I_0 = v3i_0 + v4i_0;
      double d34R_0 = v3r_0 - v4r_0;
      double d34I_0 = v3i_0 - v4i_0;
      work_re[b] = v0r_0 + s16R_0 + s25R_0 + s34R_0;
      work_im[b] = v0i_0 + s16I_0 + s25I_0 + s34I_0;
      double cR16_0 = w1R * s16R_0 + w2R * s25R_0 + w3R * s34R_0;
      double cI16_0 = w1R * s16I_0 + w2R * s25I_0 + w3R * s34I_0;
      double tR16_0 = w1I * d16I_0 + w2I * d25I_0 + w3I * d34I_0;
      double tI16_0 = w1I * d16R_0 + w2I * d25R_0 + w3I * d34R_0;
      work_re[b + 1] = v0r_0 + cR16_0 - tR16_0;
      work_im[b + 1] = v0i_0 + cI16_0 + tI16_0;
      work_re[b + 6] = v0r_0 + cR16_0 + tR16_0;
      work_im[b + 6] = v0i_0 + cI16_0 - tI16_0;
      double cR25_0 = w2R * s16R_0 + w3R * s25R_0 + w1R * s34R_0;
      double cI25_0 = w2R * s16I_0 + w3R * s25I_0 + w1R * s34I_0;
      double tR25_0 = w2I * d16I_0 - w3I * d25I_0 - w1I * d34I_0;
      double tI25_0 = w2I * d16R_0 - w3I * d25R_0 - w1I * d34R_0;
      work_re[b + 2] = v0r_0 + cR25_0 - tR25_0;
      work_im[b + 2] = v0i_0 + cI25_0 + tI25_0;
      work_re[b + 5] = v0r_0 + cR25_0 + tR25_0;
      work_im[b + 5] = v0i_0 + cI25_0 - tI25_0;
      double cR34_0 = w3R * s16R_0 + w1R * s25R_0 + w2R * s34R_0;
      double cI34_0 = w3R * s16I_0 + w1R * s25I_0 + w2R * s34I_0;
      double tR34_0 = w3I * d16I_0 - w1I * d25I_0 + w2I * d34I_0;
      double tI34_0 = w3I * d16R_0 - w1I * d25R_0 + w2I * d34R_0;
      work_re[b + 3] = v0r_0 + cR34_0 - tR34_0;
      work_im[b + 3] = v0i_0 + cI34_0 + tI34_0;
      work_re[b + 4] = v0r_0 + cR34_0 + tR34_0;
      work_im[b + 4] = v0i_0 + cI34_0 - tI34_0;

      // Butterfly 1
      double s16R_1 = v1r_1 + v6r_1;
      double s16I_1 = v1i_1 + v6i_1;
      double d16R_1 = v1r_1 - v6r_1;
      double d16I_1 = v1i_1 - v6i_1;
      double s25R_1 = v2r_1 + v5r_1;
      double s25I_1 = v2i_1 + v5i_1;
      double d25R_1 = v2r_1 - v5r_1;
      double d25I_1 = v2i_1 - v5i_1;
      double s34R_1 = v3r_1 + v4r_1;
      double s34I_1 = v3i_1 + v4i_1;
      double d34R_1 = v3r_1 - v4r_1;
      double d34I_1 = v3i_1 - v4i_1;
      work_re[b + 7] = v0r_1 + s16R_1 + s25R_1 + s34R_1;
      work_im[b + 7] = v0i_1 + s16I_1 + s25I_1 + s34I_1;
      double cR16_1 = w1R * s16R_1 + w2R * s25R_1 + w3R * s34R_1;
      double cI16_1 = w1R * s16I_1 + w2R * s25I_1 + w3R * s34I_1;
      double tR16_1 = w1I * d16I_1 + w2I * d25I_1 + w3I * d34I_1;
      double tI16_1 = w1I * d16R_1 + w2I * d25R_1 + w3I * d34R_1;
      work_re[b + 8] = v0r_1 + cR16_1 - tR16_1;
      work_im[b + 8] = v0i_1 + cI16_1 + tI16_1;
      work_re[b + 13] = v0r_1 + cR16_1 + tR16_1;
      work_im[b + 13] = v0i_1 + cI16_1 - tI16_1;
      double cR25_1 = w2R * s16R_1 + w3R * s25R_1 + w1R * s34R_1;
      double cI25_1 = w2R * s16I_1 + w3R * s25I_1 + w1R * s34I_1;
      double tR25_1 = w2I * d16I_1 - w3I * d25I_1 - w1I * d34I_1;
      double tI25_1 = w2I * d16R_1 - w3I * d25R_1 - w1I * d34R_1;
      work_re[b + 9] = v0r_1 + cR25_1 - tR25_1;
      work_im[b + 9] = v0i_1 + cI25_1 + tI25_1;
      work_re[b + 12] = v0r_1 + cR25_1 + tR25_1;
      work_im[b + 12] = v0i_1 + cI25_1 - tI25_1;
      double cR34_1 = w3R * s16R_1 + w1R * s25R_1 + w2R * s34R_1;
      double cI34_1 = w3R * s16I_1 + w1R * s25I_1 + w2R * s34I_1;
      double tR34_1 = w3I * d16I_1 - w1I * d25I_1 + w2I * d34I_1;
      double tI34_1 = w3I * d16R_1 - w1I * d25R_1 + w2I * d34R_1;
      work_re[b + 10] = v0r_1 + cR34_1 - tR34_1;
      work_im[b + 10] = v0i_1 + cI34_1 + tI34_1;
      work_re[b + 11] = v0r_1 + cR34_1 + tR34_1;
      work_im[b + 11] = v0i_1 + cI34_1 - tI34_1;
    }
    for (; b < fft->n; b += 7) {
      double v0r = work_re[b];
      double v0i = work_im[b];
      double v1r = work_re[b + 1];
      double v1i = work_im[b + 1];
      double v2r = work_re[b + 2];
      double v2i = work_im[b + 2];
      double v3r = work_re[b + 3];
      double v3i = work_im[b + 3];
      double v4r = work_re[b + 4];
      double v4i = work_im[b + 4];
      double v5r = work_re[b + 5];
      double v5i = work_im[b + 5];
      double v6r = work_re[b + 6];
      double v6i = work_im[b + 6];
      // Build pair sums/diffs with conjugate-symmetric partners.
      // {1,6}: W^1, W^6 = W^-1 → coef pair (w1, conj(w1))
      // {2,5}: W^2, W^5 = W^-2 → (w2, conj(w2))
      // {3,4}: W^3, W^4 = W^-3 → (w3, conj(w3))
      double s16R = v1r + v6r;
      double s16I = v1i + v6i;
      double d16R = v1r - v6r;
      double d16I = v1i - v6i;
      double s25R = v2r + v5r;
      double s25I = v2i + v5i;
      double d25R = v2r - v5r;
      double d25I = v2i - v5i;
      double s34R = v3r + v4r;
      double s34I = v3i + v4i;
      double d34R = v3r - v4r;
      double d34I = v3i - v4i;
      // O[0] = v0 + sum of all sums.
      work_re[b] = v0r + s16R + s25R + s34R;
      work_im[b] = v0i + s16I + s25I + s34I;
      // Conjugate-pair factoring.
      // Pair (1, 6).
      double cR16 = w1R * s16R + w2R * s25R + w3R * s34R;
      double cI16 = w1R * s16I + w2R * s25I + w3R * s34I;
      double tR16 = w1I * d16I + w2I * d25I + w3I * d34I;
      double tI16 = w1I * d16R + w2I * d25R + w3I * d34R;
      work_re[b + 1] = v0r + cR16 - tR16;
      work_im[b + 1] = v0i + cI16 + tI16;
      work_re[b + 6] = v0r + cR16 + tR16;
      work_im[b + 6] = v0i + cI16 - tI16;
      // Pair (2, 5).
      double cR25 = w2R * s16R + w3R * s25R + w1R * s34R;
      double cI25 = w2R * s16I + w3R * s25I + w1R * s34I;
      double tR25 = w2I * d16I - w3I * d25I - w1I * d34I;
      double tI25 = w2I * d16R - w3I * d25R - w1I * d34R;
      work_re[b + 2] = v0r + cR25 - tR25;
      work_im[b + 2] = v0i + cI25 + tI25;
      work_re[b + 5] = v0r + cR25 + tR25;
      work_im[b + 5] = v0i + cI25 - tI25;
      // Pair (3, 4).
      double cR34 = w3R * s16R + w1R * s25R + w2R * s34R;
      double cI34 = w3R * s16I + w1R * s25I + w2R * s34I;
      double tR34 = w3I * d16I - w1I * d25I + w2I * d34I;
      double tI34 = w3I * d16R - w1I * d25R + w2I * d34R;
      work_re[b + 3] = v0r + cR34 - tR34;
      work_im[b + 3] = v0i + cI34 + tI34;
      work_re[b + 4] = v0r + cR34 + tR34;
      work_im[b + 4] = v0i + cI34 - tI34;
    }
    return;
  }

  const double* tw1R = tw_re + m;
  const double* tw1I = tw_im + m;
  const double* tw2R = tw_re + 2 * m;
  const double* tw2I = tw_im + 2 * m;
  const double* tw3R = tw_re + 3 * m;
  const double* tw3I = tw_im + 3 * m;
  const double* tw4R = tw_re + 4 * m;
  const double* tw4I = tw_im + 4 * m;
  const double* tw5R = tw_re + 5 * m;
  const double* tw5I = tw_im + 5 * m;
  const double* tw6R = tw_re + 6 * m;
  const double* tw6I = tw_im + 6 * m;
  for (size_t b = 0; b < fft->n; b += block_size) {
    double* w0r = work_re + b;
    double* w0i = work_im + b;
    double* w1r = w0r + m;
    double* w1i = w0i + m;
    double* w2r = w1r + m;
    double* w2i = w1i + m;
    double* w3r = w2r + m;
    double* w3i = w2i + m;
    double* w4r = w3r + m;
    double* w4i = w3i + m;
    double* w5r = w4r + m;
    double* w5i = w4i + m;
    double* w6r = w5r + m;
    double* w6i = w5i + m;
#if defined(__clang__)
#pragma clang loop vectorize(assume_safety) interleave(enable)
#elif defined(__GNUC__)
#pragma GCC ivdep
#endif
    for (size_t k = 0; k < m; k++) {
      // Outer-stage twiddles on samples 1..6.
      double t1R = tw1R[k];
      double t1I = tw1I[k];
      double t2R = tw2R[k];
      double t2I = tw2I[k];
      double t3R = tw3R[k];
      double t3I = tw3I[k];
      double t4R = tw4R[k];
      double t4I = tw4I[k];
      double t5R = tw5R[k];
      double t5I = tw5I[k];
      double t6R = tw6R[k];
      double t6I = tw6I[k];
      double v1r = w1r[k] * t1R - w1i[k] * t1I;
      double v1i = w1r[k] * t1I + w1i[k] * t1R;
      double v2r = w2r[k] * t2R - w2i[k] * t2I;
      double v2i = w2r[k] * t2I + w2i[k] * t2R;
      double v3r = w3r[k] * t3R - w3i[k] * t3I;
      double v3i = w3r[k] * t3I + w3i[k] * t3R;
      double v4r = w4r[k] * t4R - w4i[k] * t4I;
      double v4i = w4r[k] * t4I + w4i[k] * t4R;
      double v5r = w5r[k] * t5R - w5i[k] * t5I;
      double v5i = w5r[k] * t5I + w5i[k] * t5R;
      double v6r = w6r[k] * t6R - w6i[k] * t6I;
      double v6i = w6r[k] * t6I + w6i[k] * t6R;
      double v0r = w0r[k];
      double v0i = w0i[k];
      // Build pair sums/diffs with conjugate-symmetric partners.
      // {1,6}: W^1, W^6 = W^-1 → coef pair (w1, conj(w1))
      // {2,5}: W^2, W^5 = W^-2 → (w2, conj(w2))
      // {3,4}: W^3, W^4 = W^-3 → (w3, conj(w3))
      double s16R = v1r + v6r;
      double s16I = v1i + v6i;
      double d16R = v1r - v6r;
      double d16I = v1i - v6i;
      double s25R = v2r + v5r;
      double s25I = v2i + v5i;
      double d25R = v2r - v5r;
      double d25I = v2i - v5i;
      double s34R = v3r + v4r;
      double s34I = v3i + v4i;
      double d34R = v3r - v4r;
      double d34I = v3i - v4i;
      // O[0] = v0 + sum of all sums.
      w0r[k] = v0r + s16R + s25R + s34R;
      w0i[k] = v0i + s16I + s25I + s34I;
      // Conjugate-pair factoring.
      // Pair (1, 6).
      double cR16 = w1R * s16R + w2R * s25R + w3R * s34R;
      double cI16 = w1R * s16I + w2R * s25I + w3R * s34I;
      double tR16 = w1I * d16I + w2I * d25I + w3I * d34I;
      double tI16 = w1I * d16R + w2I * d25R + w3I * d34R;
      w1r[k] = v0r + cR16 - tR16;
      w1i[k] = v0i + cI16 + tI16;
      w6r[k] = v0r + cR16 + tR16;
      w6i[k] = v0i + cI16 - tI16;
      // Pair (2, 5).
      double cR25 = w2R * s16R + w3R * s25R + w1R * s34R;
      double cI25 = w2R * s16I + w3R * s25I + w1R * s34I;
      double tR25 = w2I * d16I - w3I * d25I - w1I * d34I;
      double tI25 = w2I * d16R - w3I * d25R - w1I * d34R;
      w2r[k] = v0r + cR25 - tR25;
      w2i[k] = v0i + cI25 + tI25;
      w5r[k] = v0r + cR25 + tR25;
      w5i[k] = v0i + cI25 - tI25;
      // Pair (3, 4).
      double cR34 = w3R * s16R + w1R * s25R + w2R * s34R;
      double cI34 = w3R * s16I + w1R * s25I + w2R * s34I;
      double tR34 = w3I * d16I - w1I * d25I + w2I * d34I;
      double tI34 = w3I * d16R - w1I * d25R + w2I * d34R;
      w3r[k] = v0r + cR34 - tR34;
      w3i[k] = v0i + cI34 + tI34;
      w4r[k] = v0r + cR34 + tR34;
      w4i[k] = v0i + cI34 - tI34;
    }
  }
}

/**
 * @brief Apply radix-8 butterflies.
 *
 * The inner DFT is computed via DIT decomposition into two radix-4s
 * (even-indexed and odd-indexed), then combined with the trivial 8th-root
 * twiddles `W_8^k = exp(-2πi·k/8)`. Multiplications cost only the constant
 * `√2/2` for the k=1 and k=3 inner twiddles — k=0 is free, k=2 is
 * `-i` (free), so no real-coefficient multiplies on the inner DFT
 * beyond the two `√2/2` cross-terms.
 *
 * @param fft Pointer to the FFT context.
 * @param m Current subblock size.
 * @param tw_re Real part of twiddle factors.
 * @param tw_im Imaginary part of twiddle factors.
 */
static inline void stage_radix8(mixed_radix_fft_t* fft, double* work_re,
                                double* work_im, size_t m, const double* tw_re,
                                const double* tw_im) {
  size_t block_size = m * 8;
  double s2 = 0.7071067811865476;  // √2/2

  if (m == 1) {
#if defined(__clang__)
#pragma clang loop vectorize(assume_safety) interleave(enable)
#elif defined(__GNUC__)
#pragma GCC ivdep
#endif
    for (size_t b = 0; b < fft->n; b += 8) {
      double v0r = work_re[b];
      double v0i = work_im[b];
      double v1r = work_re[b + 1];
      double v1i = work_im[b + 1];
      double v2r = work_re[b + 2];
      double v2i = work_im[b + 2];
      double v3r = work_re[b + 3];
      double v3i = work_im[b + 3];
      double v4r = work_re[b + 4];
      double v4i = work_im[b + 4];
      double v5r = work_re[b + 5];
      double v5i = work_im[b + 5];
      double v6r = work_re[b + 6];
      double v6i = work_im[b + 6];
      double v7r = work_re[b + 7];
      double v7i = work_im[b + 7];
      // Even radix-4: DFT of (v0, v2, v4, v6).
      double eA0r = v0r + v4r;
      double eA0i = v0i + v4i;
      double eA1r = v0r - v4r;
      double eA1i = v0i - v4i;
      double eA2r = v2r + v6r;
      double eA2i = v2i + v6i;
      double eA3r = v2r - v6r;
      double eA3i = v2i - v6i;
      double e0r = eA0r + eA2r;
      double e0i = eA0i + eA2i;
      double e1r = eA1r + eA3i;
      double e1i = eA1i - eA3r;
      double e2r = eA0r - eA2r;
      double e2i = eA0i - eA2i;
      double e3r = eA1r - eA3i;
      double e3i = eA1i + eA3r;
      // Odd radix-4: DFT of (v1, v3, v5, v7).
      double oA0r = v1r + v5r;
      double oA0i = v1i + v5i;
      double oA1r = v1r - v5r;
      double oA1i = v1i - v5i;
      double oA2r = v3r + v7r;
      double oA2i = v3i + v7i;
      double oA3r = v3r - v7r;
      double oA3i = v3i - v7i;
      double oo0r = oA0r + oA2r;
      double oo0i = oA0i + oA2i;
      double oo1r = oA1r + oA3i;
      double oo1i = oA1i - oA3r;
      double oo2r = oA0r - oA2r;
      double oo2i = oA0i - oA2i;
      double oo3r = oA1r - oA3i;
      double oo3i = oA1i + oA3r;
      // Apply W_8^k to odd outputs:
      //   W_8^0 = 1; W_8^1 = (s2, -s2); W_8^2 = -i; W_8^3 = (-s2, -s2).
      double w0r = oo0r;
      double w0i = oo0i;
      double w1r = s2 * (oo1r + oo1i);
      double w1i = s2 * (oo1i - oo1r);
      double w2r = oo2i;
      double w2i = -oo2r;
      double w3r = s2 * (oo3i - oo3r);
      double w3i = -s2 * (oo3r + oo3i);
      // O[k] = E[k] + W_8^k·O_odd[k], O[k+4] = E[k] - W_8^k·O_odd[k].
      work_re[b] = e0r + w0r;
      work_im[b] = e0i + w0i;
      work_re[b + 1] = e1r + w1r;
      work_im[b + 1] = e1i + w1i;
      work_re[b + 2] = e2r + w2r;
      work_im[b + 2] = e2i + w2i;
      work_re[b + 3] = e3r + w3r;
      work_im[b + 3] = e3i + w3i;
      work_re[b + 4] = e0r - w0r;
      work_im[b + 4] = e0i - w0i;
      work_re[b + 5] = e1r - w1r;
      work_im[b + 5] = e1i - w1i;
      work_re[b + 6] = e2r - w2r;
      work_im[b + 6] = e2i - w2i;
      work_re[b + 7] = e3r - w3r;
      work_im[b + 7] = e3i - w3i;
    }
    return;
  }

  const double* tw1R = tw_re + m;
  const double* tw1I = tw_im + m;
  const double* tw2R = tw_re + 2 * m;
  const double* tw2I = tw_im + 2 * m;
  const double* tw3R = tw_re + 3 * m;
  const double* tw3I = tw_im + 3 * m;
  const double* tw4R = tw_re + 4 * m;
  const double* tw4I = tw_im + 4 * m;
  const double* tw5R = tw_re + 5 * m;
  const double* tw5I = tw_im + 5 * m;
  const double* tw6R = tw_re + 6 * m;
  const double* tw6I = tw_im + 6 * m;
  const double* tw7R = tw_re + 7 * m;
  const double* tw7I = tw_im + 7 * m;
  for (size_t b = 0; b < fft->n; b += block_size) {
    double* w0r = work_re + b;
    double* w0i = work_im + b;
    double* w1r = w0r + m;
    double* w1i = w0i + m;
    double* w2r = w1r + m;
    double* w2i = w1i + m;
    double* w3r = w2r + m;
    double* w3i = w2i + m;
    double* w4r = w3r + m;
    double* w4i = w3i + m;
    double* w5r = w4r + m;
    double* w5i = w4i + m;
    double* w6r = w5r + m;
    double* w6i = w5i + m;
    double* w7r = w6r + m;
    double* w7i = w6i + m;
#if defined(__clang__)
#pragma clang loop vectorize(assume_safety) interleave(enable)
#elif defined(__GNUC__)
#pragma GCC ivdep
#endif
    for (size_t k = 0; k < m; k++) {
      double t1R = tw1R[k];
      double t1I = tw1I[k];
      double t2R = tw2R[k];
      double t2I = tw2I[k];
      double t3R = tw3R[k];
      double t3I = tw3I[k];
      double t4R = tw4R[k];
      double t4I = tw4I[k];
      double t5R = tw5R[k];
      double t5I = tw5I[k];
      double t6R = tw6R[k];
      double t6I = tw6I[k];
      double t7R = tw7R[k];
      double t7I = tw7I[k];
      double v0r = w0r[k];
      double v0i = w0i[k];
      double v1r = w1r[k] * t1R - w1i[k] * t1I;
      double v1i = w1r[k] * t1I + w1i[k] * t1R;
      double v2r = w2r[k] * t2R - w2i[k] * t2I;
      double v2i = w2r[k] * t2I + w2i[k] * t2R;
      double v3r = w3r[k] * t3R - w3i[k] * t3I;
      double v3i = w3r[k] * t3I + w3i[k] * t3R;
      double v4r = w4r[k] * t4R - w4i[k] * t4I;
      double v4i = w4r[k] * t4I + w4i[k] * t4R;
      double v5r = w5r[k] * t5R - w5i[k] * t5I;
      double v5i = w5r[k] * t5I + w5i[k] * t5R;
      double v6r = w6r[k] * t6R - w6i[k] * t6I;
      double v6i = w6r[k] * t6I + w6i[k] * t6R;
      double v7r = w7r[k] * t7R - w7i[k] * t7I;
      double v7i = w7r[k] * t7I + w7i[k] * t7R;
      // Even radix-4: DFT of (v0, v2, v4, v6).
      double eA0r = v0r + v4r;
      double eA0i = v0i + v4i;
      double eA1r = v0r - v4r;
      double eA1i = v0i - v4i;
      double eA2r = v2r + v6r;
      double eA2i = v2i + v6i;
      double eA3r = v2r - v6r;
      double eA3i = v2i - v6i;
      double e0r = eA0r + eA2r;
      double e0i = eA0i + eA2i;
      double e1r = eA1r + eA3i;
      double e1i = eA1i - eA3r;
      double e2r = eA0r - eA2r;
      double e2i = eA0i - eA2i;
      double e3r = eA1r - eA3i;
      double e3i = eA1i + eA3r;
      // Odd radix-4: DFT of (v1, v3, v5, v7).
      double oA0r = v1r + v5r;
      double oA0i = v1i + v5i;
      double oA1r = v1r - v5r;
      double oA1i = v1i - v5i;
      double oA2r = v3r + v7r;
      double oA2i = v3i + v7i;
      double oA3r = v3r - v7r;
      double oA3i = v3i - v7i;
      double oo0r = oA0r + oA2r;
      double oo0i = oA0i + oA2i;
      double oo1r = oA1r + oA3i;
      double oo1i = oA1i - oA3r;
      double oo2r = oA0r - oA2r;
      double oo2i = oA0i - oA2i;
      double oo3r = oA1r - oA3i;
      double oo3i = oA1i + oA3r;
      // Apply W_8^k to odd outputs:
      //   W_8^0 = 1; W_8^1 = (s2, -s2); W_8^2 = -i; W_8^3 = (-s2, -s2).
      double wo0r = oo0r;
      double wo0i = oo0i;
      double wo1r = s2 * (oo1r + oo1i);
      double wo1i = s2 * (oo1i - oo1r);
      double wo2r = oo2i;
      double wo2i = -oo2r;
      double wo3r = s2 * (oo3i - oo3r);
      double wo3i = -s2 * (oo3r + oo3i);
      // O[k] = E[k] + W_8^k·O_odd[k], O[k+4] = E[k] - W_8^k·O_odd[k].
      w0r[k] = e0r + wo0r;
      w0i[k] = e0i + wo0i;
      w1r[k] = e1r + wo1r;
      w1i[k] = e1i + wo1i;
      w2r[k] = e2r + wo2r;
      w2i[k] = e2i + wo2i;
      w3r[k] = e3r + wo3r;
      w3i[k] = e3i + wo3i;
      w4r[k] = e0r - wo0r;
      w4i[k] = e0i - wo0i;
      w5r[k] = e1r - wo1r;
      w5i[k] = e1i - wo1i;
      w6r[k] = e2r - wo2r;
      w6i[k] = e2i - wo2i;
      w7r[k] = e3r - wo3r;
      w7i[k] = e3i - wo3i;
    }
  }
}

/**
 * @brief Apply radix-9 butterflies (composite 3x3).
 */
static inline void stage_radix9(mixed_radix_fft_t* fft, double* work_re,
                                double* work_im, size_t m, const double* tw_re,
                                const double* tw_im) {
  size_t block_size = m * 9;
  const double s32 = 0.86602540378443864676;  // sin(2pi/3) = sqrt(3)/2
  const double c1 = 0.76604444311897803520;   // cos(2pi/9)
  const double s1 = 0.64278760968653932632;   // sin(2pi/9)
  const double c2 = 0.17364817766693034885;   // cos(4pi/9)
  const double s2 = 0.98480775301220805937;   // sin(4pi/9)
  const double c4 = -0.93969262078590838405;  // cos(8pi/9)
  const double s4 = 0.34202014332566873304;   // sin(8pi/9)

  if (m == 1) {
    for (size_t b = 0; b < fft->n; b += 9) {
      double v0r = work_re[b], v0i = work_im[b];
      double v1r = work_re[b + 1], v1i = work_im[b + 1];
      double v2r = work_re[b + 2], v2i = work_im[b + 2];
      double v3r = work_re[b + 3], v3i = work_im[b + 3];
      double v4r = work_re[b + 4], v4i = work_im[b + 4];
      double v5r = work_re[b + 5], v5i = work_im[b + 5];
      double v6r = work_re[b + 6], v6i = work_im[b + 6];
      double v7r = work_re[b + 7], v7i = work_im[b + 7];
      double v8r = work_re[b + 8], v8i = work_im[b + 8];

      // Sub-DFT 0: (v0, v3, v6)
      double s0r = v3r + v6r, s0i = v3i + v6i;
      double d0r = v3r - v6r, d0i = v3i - v6i;
      double h0r = v0r - 0.5 * s0r, h0i = v0i - 0.5 * s0i;
      double t0r = s32 * d0i, t0i = -s32 * d0r;
      double a0r = v0r + s0r, a0i = v0i + s0i;
      double a1r = h0r + t0r, a1i = h0i + t0i;
      double a2r = h0r - t0r, a2i = h0i - t0i;

      // Sub-DFT 1: (v1, v4, v7)
      double s1r = v4r + v7r, s1i = v4i + v7i;
      double d1r = v4r - v7r, d1i = v4i - v7i;
      double h1r = v1r - 0.5 * s1r, h1i = v1i - 0.5 * s1i;
      double t1r = s32 * d1i, t1i = -s32 * d1r;
      double b0r = v1r + s1r, b0i = v1i + s1i;
      double b1r = h1r + t1r, b1i = h1i + t1i;
      double b2r = h1r - t1r, b2i = h1i - t1i;

      // Sub-DFT 2: (v2, v5, v8)
      double s2r = v5r + v8r, s2i = v5i + v8i;
      double d2r = v5r - v8r, d2i = v5i - v8i;
      double h2r = v2r - 0.5 * s2r, h2i = v2i - 0.5 * s2i;
      double t2r = s32 * d2i, t2i = -s32 * d2r;
      double c0r = v2r + s2r, c0i = v2i + s2i;
      double c1r = h2r + t2r, c1i = h2i + t2i;
      double c2r = h2r - t2r, c2i = h2i - t2i;

      // Internal twiddles
      double b1_tw_r = b1r * c1 + b1i * s1;
      double b1_tw_i = b1i * c1 - b1r * s1;
      double b2_tw_r = b2r * c2 + b2i * s2;
      double b2_tw_i = b2i * c2 - b2r * s2;

      double c1_tw_r = c1r * c2 + c1i * s2;
      double c1_tw_i = c1i * c2 - c1r * s2;
      double c2_tw_r = c2r * c4 + c2i * s4;
      double c2_tw_i = c2i * c4 - c2r * s4;

      // Across (a0, b0', c0')
      double sa0r = b0r + c0r, sa0i = b0i + c0i;
      double da0r = b0r - c0r, da0i = b0i - c0i;
      double ha0r = a0r - 0.5 * sa0r, ha0i = a0i - 0.5 * sa0i;
      double ta0r = s32 * da0i, ta0i = -s32 * da0r;
      work_re[b] = a0r + sa0r;
      work_im[b] = a0i + sa0i;
      work_re[b + 3] = ha0r + ta0r;
      work_im[b + 3] = ha0i + ta0i;
      work_re[b + 6] = ha0r - ta0r;
      work_im[b + 6] = ha0i - ta0i;

      // Across (a1, b1', c1')
      double sa1r = b1_tw_r + c1_tw_r, sa1i = b1_tw_i + c1_tw_i;
      double da1r = b1_tw_r - c1_tw_r, da1i = b1_tw_i - c1_tw_i;
      double ha1r = a1r - 0.5 * sa1r, ha1i = a1i - 0.5 * sa1i;
      double ta1r = s32 * da1i, ta1i = -s32 * da1r;
      work_re[b + 1] = a1r + sa1r;
      work_im[b + 1] = a1i + sa1i;
      work_re[b + 4] = ha1r + ta1r;
      work_im[b + 4] = ha1i + ta1i;
      work_re[b + 7] = ha1r - ta1r;
      work_im[b + 7] = ha1i - ta1i;

      // Across (a2, b2', c2')
      double sa2r = b2_tw_r + c2_tw_r, sa2i = b2_tw_i + c2_tw_i;
      double da2r = b2_tw_r - c2_tw_r, da2i = b2_tw_i - c2_tw_i;
      double ha2r = a2r - 0.5 * sa2r, ha2i = a2i - 0.5 * sa2i;
      double ta2r = s32 * da2i, ta2i = -s32 * da2r;
      work_re[b + 2] = a2r + sa2r;
      work_im[b + 2] = a2i + sa2i;
      work_re[b + 5] = ha2r + ta2r;
      work_im[b + 5] = ha2i + ta2i;
      work_re[b + 8] = ha2r - ta2r;
      work_im[b + 8] = ha2i - ta2i;
    }
    return;
  }

  const double* tw1R = tw_re + m;
  const double* tw1I = tw_im + m;
  const double* tw2R = tw_re + 2 * m;
  const double* tw2I = tw_im + 2 * m;
  const double* tw3R = tw_re + 3 * m;
  const double* tw3I = tw_im + 3 * m;
  const double* tw4R = tw_re + 4 * m;
  const double* tw4I = tw_im + 4 * m;
  const double* tw5R = tw_re + 5 * m;
  const double* tw5I = tw_im + 5 * m;
  const double* tw6R = tw_re + 6 * m;
  const double* tw6I = tw_im + 6 * m;
  const double* tw7R = tw_re + 7 * m;
  const double* tw7I = tw_im + 7 * m;
  const double* tw8R = tw_re + 8 * m;
  const double* tw8I = tw_im + 8 * m;

  for (size_t b = 0; b < fft->n; b += block_size) {
    double* w0r = work_re + b;
    double* w0i = work_im + b;
    double* w1r = w0r + m;
    double* w1i = w0i + m;
    double* w2r = w1r + m;
    double* w2i = w1i + m;
    double* w3r = w2r + m;
    double* w3i = w2i + m;
    double* w4r = w3r + m;
    double* w4i = w3i + m;
    double* w5r = w4r + m;
    double* w5i = w4i + m;
    double* w6r = w5r + m;
    double* w6i = w5i + m;
    double* w7r = w6r + m;
    double* w7i = w6i + m;
    double* w8r = w7r + m;
    double* w8i = w7i + m;

#if defined(__clang__)
#pragma clang loop vectorize(assume_safety) interleave(enable)
#elif defined(__GNUC__)
#pragma GCC ivdep
#endif
    for (size_t k = 0; k < m; k++) {
      double v0r = w0r[k], v0i = w0i[k];
      double t1R = tw1R[k], t1I = tw1I[k];
      double t2R = tw2R[k], t2I = tw2I[k];
      double t3R = tw3R[k], t3I = tw3I[k];
      double t4R = tw4R[k], t4I = tw4I[k];
      double t5R = tw5R[k], t5I = tw5I[k];
      double t6R = tw6R[k], t6I = tw6I[k];
      double t7R = tw7R[k], t7I = tw7I[k];
      double t8R = tw8R[k], t8I = tw8I[k];

      double v1r = w1r[k] * t1R - w1i[k] * t1I;
      double v1i = w1r[k] * t1I + w1i[k] * t1R;
      double v2r = w2r[k] * t2R - w2i[k] * t2I;
      double v2i = w2r[k] * t2I + w2i[k] * t2R;
      double v3r = w3r[k] * t3R - w3i[k] * t3I;
      double v3i = w3r[k] * t3I + w3i[k] * t3R;
      double v4r = w4r[k] * t4R - w4i[k] * t4I;
      double v4i = w4r[k] * t4I + w4i[k] * t4R;
      double v5r = w5r[k] * t5R - w5i[k] * t5I;
      double v5i = w5r[k] * t5I + w5i[k] * t5R;
      double v6r = w6r[k] * t6R - w6i[k] * t6I;
      double v6i = w6r[k] * t6I + w6i[k] * t6R;
      double v7r = w7r[k] * t7R - w7i[k] * t7I;
      double v7i = w7r[k] * t7I + w7i[k] * t7R;
      double v8r = w8r[k] * t8R - w8i[k] * t8I;
      double v8i = w8r[k] * t8I + w8i[k] * t8R;

      // Sub-DFT 0
      double s0r = v3r + v6r, s0i = v3i + v6i;
      double d0r = v3r - v6r, d0i = v3i - v6i;
      double h0r = v0r - 0.5 * s0r, h0i = v0i - 0.5 * s0i;
      double t0r = s32 * d0i, t0i = -s32 * d0r;
      double a0r = v0r + s0r, a0i = v0i + s0i;
      double a1r = h0r + t0r, a1i = h0i + t0i;
      double a2r = h0r - t0r, a2i = h0i - t0i;

      // Sub-DFT 1
      double s1r = v4r + v7r, s1i = v4i + v7i;
      double d1r = v4r - v7r, d1i = v4i - v7i;
      double h1r = v1r - 0.5 * s1r, h1i = v1i - 0.5 * s1i;
      double t1r = s32 * d1i, t1i = -s32 * d1r;
      double b0r = v1r + s1r, b0i = v1i + s1i;
      double b1r = h1r + t1r, b1i = h1i + t1i;
      double b2r = h1r - t1r, b2i = h1i - t1i;

      // Sub-DFT 2
      double s2r = v5r + v8r, s2i = v5i + v8i;
      double d2r = v5r - v8r, d2i = v5i - v8i;
      double h2r = v2r - 0.5 * s2r, h2i = v2i - 0.5 * s2i;
      double t2r = s32 * d2i, t2i = -s32 * d2r;
      double c0r = v2r + s2r, c0i = v2i + s2i;
      double c1r = h2r + t2r, c1i = h2i + t2i;
      double c2r = h2r - t2r, c2i = h2i - t2i;

      // Internal twiddles
      double b1_tw_r = b1r * c1 + b1i * s1;
      double b1_tw_i = b1i * c1 - b1r * s1;
      double b2_tw_r = b2r * c2 + b2i * s2;
      double b2_tw_i = b2i * c2 - b2r * s2;

      double c1_tw_r = c1r * c2 + c1i * s2;
      double c1_tw_i = c1i * c2 - c1r * s2;
      double c2_tw_r = c2r * c4 + c2i * s4;
      double c2_tw_i = c2i * c4 - c2r * s4;

      // Across 0
      double sa0r = b0r + c0r, sa0i = b0i + c0i;
      double da0r = b0r - c0r, da0i = b0i - c0i;
      double ha0r = a0r - 0.5 * sa0r, ha0i = a0i - 0.5 * sa0i;
      double ta0r = s32 * da0i, ta0i = -s32 * da0r;
      w0r[k] = a0r + sa0r;
      w0i[k] = a0i + sa0i;
      w3r[k] = ha0r + ta0r;
      w3i[k] = ha0i + ta0i;
      w6r[k] = ha0r - ta0r;
      w6i[k] = ha0i - ta0i;

      // Across 1
      double sa1r = b1_tw_r + c1_tw_r, sa1i = b1_tw_i + c1_tw_i;
      double da1r = b1_tw_r - c1_tw_r, da1i = b1_tw_i - c1_tw_i;
      double ha1r = a1r - 0.5 * sa1r, ha1i = a1i - 0.5 * sa1i;
      double ta1r = s32 * da1i, ta1i = -s32 * da1r;
      w1r[k] = a1r + sa1r;
      w1i[k] = a1i + sa1i;
      w4r[k] = ha1r + ta1r;
      w4i[k] = ha1i + ta1i;
      w7r[k] = ha1r - ta1r;
      w7i[k] = ha1i - ta1i;

      // Across 2
      double sa2r = b2_tw_r + c2_tw_r, sa2i = b2_tw_i + c2_tw_i;
      double da2r = b2_tw_r - c2_tw_r, da2i = b2_tw_i - c2_tw_i;
      double ha2r = a2r - 0.5 * sa2r, ha2i = a2i - 0.5 * sa2i;
      double ta2r = s32 * da2i, ta2i = -s32 * da2r;
      w2r[k] = a2r + sa2r;
      w2i[k] = a2i + sa2i;
      w5r[k] = ha2r + ta2r;
      w5i[k] = ha2i + ta2i;
      w8r[k] = ha2r - ta2r;
      w8i[k] = ha2i - ta2i;
    }
  }
}

/**
 * @brief Apply radix-11 butterflies.
 */
static inline void stage_radix11(mixed_radix_fft_t* fft, double* work_re,
                                 double* work_im, size_t m, const double* tw_re,
                                 const double* tw_im) {
  size_t block_size = m * 11;
  double w1R = cos(2.0 * M_PI / 11.0);
  double w1I = -sin(2.0 * M_PI / 11.0);
  double w2R = cos(4.0 * M_PI / 11.0);
  double w2I = -sin(4.0 * M_PI / 11.0);
  double w3R = cos(6.0 * M_PI / 11.0);
  double w3I = -sin(6.0 * M_PI / 11.0);
  double w4R = cos(8.0 * M_PI / 11.0);
  double w4I = -sin(8.0 * M_PI / 11.0);
  double w5R = cos(10.0 * M_PI / 11.0);
  double w5I = -sin(10.0 * M_PI / 11.0);

  if (m == 1) {
    size_t b = 0;
    for (; b + 22 <= fft->n; b += 22) {
#define BUTTERFLY11_M1(offset)                                           \
  double v0r_##offset = work_re[b + offset];                             \
  double v0i_##offset = work_im[b + offset];                             \
  double v1r_##offset = work_re[b + offset + 1];                         \
  double v1i_##offset = work_im[b + offset + 1];                         \
  double v2r_##offset = work_re[b + offset + 2];                         \
  double v2i_##offset = work_im[b + offset + 2];                         \
  double v3r_##offset = work_re[b + offset + 3];                         \
  double v3i_##offset = work_im[b + offset + 3];                         \
  double v4r_##offset = work_re[b + offset + 4];                         \
  double v4i_##offset = work_im[b + offset + 4];                         \
  double v5r_##offset = work_re[b + offset + 5];                         \
  double v5i_##offset = work_im[b + offset + 5];                         \
  double v6r_##offset = work_re[b + offset + 6];                         \
  double v6i_##offset = work_im[b + offset + 6];                         \
  double v7r_##offset = work_re[b + offset + 7];                         \
  double v7i_##offset = work_im[b + offset + 7];                         \
  double v8r_##offset = work_re[b + offset + 8];                         \
  double v8i_##offset = work_im[b + offset + 8];                         \
  double v9r_##offset = work_re[b + offset + 9];                         \
  double v9i_##offset = work_im[b + offset + 9];                         \
  double v10r_##offset = work_re[b + offset + 10];                       \
  double v10i_##offset = work_im[b + offset + 10];                       \
                                                                         \
  double s1_##offset = v1r_##offset + v10r_##offset;                     \
  double s1i_##offset = v1i_##offset + v10i_##offset;                    \
  double d1_##offset = v1r_##offset - v10r_##offset;                     \
  double d1i_##offset = v1i_##offset - v10i_##offset;                    \
  double s2_##offset = v2r_##offset + v9r_##offset;                      \
  double s2i_##offset = v2i_##offset + v9i_##offset;                     \
  double d2_##offset = v2r_##offset - v9r_##offset;                      \
  double d2i_##offset = v2i_##offset - v9i_##offset;                     \
  double s3_##offset = v3r_##offset + v8r_##offset;                      \
  double s3i_##offset = v3i_##offset + v8i_##offset;                     \
  double d3_##offset = v3r_##offset - v8r_##offset;                      \
  double d3i_##offset = v3i_##offset - v8i_##offset;                     \
  double s4_##offset = v4r_##offset + v7r_##offset;                      \
  double s4i_##offset = v4i_##offset + v7i_##offset;                     \
  double d4_##offset = v4r_##offset - v7r_##offset;                      \
  double d4i_##offset = v4i_##offset - v7i_##offset;                     \
  double s5_##offset = v5r_##offset + v6r_##offset;                      \
  double s5i_##offset = v5i_##offset + v6i_##offset;                     \
  double d5_##offset = v5r_##offset - v6r_##offset;                      \
  double d5i_##offset = v5i_##offset - v6i_##offset;                     \
                                                                         \
  work_re[b + offset] = v0r_##offset + s1_##offset + s2_##offset +       \
                        s3_##offset + s4_##offset + s5_##offset;         \
  work_im[b + offset] = v0i_##offset + s1i_##offset + s2i_##offset +     \
                        s3i_##offset + s4i_##offset + s5i_##offset;      \
                                                                         \
  double cR1_##offset = w1R * s1_##offset + w2R * s2_##offset +          \
                        w3R * s3_##offset + w4R * s4_##offset +          \
                        w5R * s5_##offset;                               \
  double cI1_##offset = w1R * s1i_##offset + w2R * s2i_##offset +        \
                        w3R * s3i_##offset + w4R * s4i_##offset +        \
                        w5R * s5i_##offset;                              \
  double tR1_##offset = w1I * d1i_##offset + w2I * d2i_##offset +        \
                        w3I * d3i_##offset + w4I * d4i_##offset +        \
                        w5I * d5i_##offset;                              \
  double tI1_##offset = w1I * d1_##offset + w2I * d2_##offset +          \
                        w3I * d3_##offset + w4I * d4_##offset +          \
                        w5I * d5_##offset;                               \
  work_re[b + offset + 1] = v0r_##offset + cR1_##offset - tR1_##offset;  \
  work_im[b + offset + 1] = v0i_##offset + cI1_##offset + tI1_##offset;  \
  work_re[b + offset + 10] = v0r_##offset + cR1_##offset + tR1_##offset; \
  work_im[b + offset + 10] = v0i_##offset + cI1_##offset - tI1_##offset; \
                                                                         \
  double cR2_##offset = w2R * s1_##offset + w4R * s2_##offset +          \
                        w5R * s3_##offset + w3R * s4_##offset +          \
                        w1R * s5_##offset;                               \
  double cI2_##offset = w2R * s1i_##offset + w4R * s2i_##offset +        \
                        w5R * s3i_##offset + w3R * s4i_##offset +        \
                        w1R * s5i_##offset;                              \
  double tR2_##offset = w2I * d1i_##offset + w4I * d2i_##offset -        \
                        w5I * d3i_##offset - w3I * d4i_##offset -        \
                        w1I * d5i_##offset;                              \
  double tI2_##offset = w2I * d1_##offset + w4I * d2_##offset -          \
                        w5I * d3_##offset - w3I * d4_##offset -          \
                        w1I * d5_##offset;                               \
  work_re[b + offset + 2] = v0r_##offset + cR2_##offset - tR2_##offset;  \
  work_im[b + offset + 2] = v0i_##offset + cI2_##offset + tI2_##offset;  \
  work_re[b + offset + 9] = v0r_##offset + cR2_##offset + tR2_##offset;  \
  work_im[b + offset + 9] = v0i_##offset + cI2_##offset - tI2_##offset;  \
                                                                         \
  double cR3_##offset = w3R * s1_##offset + w5R * s2_##offset +          \
                        w2R * s3_##offset + w1R * s4_##offset +          \
                        w4R * s5_##offset;                               \
  double cI3_##offset = w3R * s1i_##offset + w5R * s2i_##offset +        \
                        w2R * s3i_##offset + w1R * s4i_##offset +        \
                        w4R * s5i_##offset;                              \
  double tR3_##offset = w3I * d1i_##offset - w5I * d2i_##offset -        \
                        w2I * d3i_##offset + w1I * d4i_##offset +        \
                        w4I * d5i_##offset;                              \
  double tI3_##offset = w3I * d1_##offset - w5I * d2_##offset -          \
                        w2I * d3_##offset + w1I * d4_##offset +          \
                        w4I * d5_##offset;                               \
  work_re[b + offset + 3] = v0r_##offset + cR3_##offset - tR3_##offset;  \
  work_im[b + offset + 3] = v0i_##offset + cI3_##offset + tI3_##offset;  \
  work_re[b + offset + 8] = v0r_##offset + cR3_##offset + tR3_##offset;  \
  work_im[b + offset + 8] = v0i_##offset + cI3_##offset - tI3_##offset;  \
                                                                         \
  double cR4_##offset = w4R * s1_##offset + w3R * s2_##offset +          \
                        w1R * s3_##offset + w5R * s4_##offset +          \
                        w2R * s5_##offset;                               \
  double cI4_##offset = w4R * s1i_##offset + w3R * s2i_##offset +        \
                        w1R * s3i_##offset + w5R * s4i_##offset +        \
                        w2R * s5i_##offset;                              \
  double tR4_##offset = w4I * d1i_##offset - w3I * d2i_##offset +        \
                        w1I * d3i_##offset + w5I * d4i_##offset -        \
                        w2I * d5i_##offset;                              \
  double tI4_##offset = w4I * d1_##offset - w3I * d2_##offset +          \
                        w1I * d3_##offset + w5I * d4_##offset -          \
                        w2I * d5_##offset;                               \
  work_re[b + offset + 4] = v0r_##offset + cR4_##offset - tR4_##offset;  \
  work_im[b + offset + 4] = v0i_##offset + cI4_##offset + tI4_##offset;  \
  work_re[b + offset + 7] = v0r_##offset + cR4_##offset + tR4_##offset;  \
  work_im[b + offset + 7] = v0i_##offset + cI4_##offset - tI4_##offset;  \
                                                                         \
  double cR5_##offset = w5R * s1_##offset + w1R * s2_##offset +          \
                        w4R * s3_##offset + w2R * s4_##offset +          \
                        w3R * s5_##offset;                               \
  double cI5_##offset = w5R * s1i_##offset + w1R * s2i_##offset +        \
                        w4R * s3i_##offset + w2R * s4i_##offset +        \
                        w3R * s5i_##offset;                              \
  double tR5_##offset = w5I * d1i_##offset - w1I * d2i_##offset +        \
                        w4I * d3i_##offset - w2I * d4i_##offset +        \
                        w3I * d5i_##offset;                              \
  double tI5_##offset = w5I * d1_##offset - w1I * d2_##offset +          \
                        w4I * d3_##offset - w2I * d4_##offset +          \
                        w3I * d5_##offset;                               \
  work_re[b + offset + 5] = v0r_##offset + cR5_##offset - tR5_##offset;  \
  work_im[b + offset + 5] = v0i_##offset + cI5_##offset + tI5_##offset;  \
  work_re[b + offset + 6] = v0r_##offset + cR5_##offset + tR5_##offset;  \
  work_im[b + offset + 6] = v0i_##offset + cI5_##offset - tI5_##offset;

      BUTTERFLY11_M1(0);
      BUTTERFLY11_M1(11);
#undef BUTTERFLY11_M1
    }
    for (; b < fft->n; b += 11) {
      double v0r = work_re[b], v0i = work_im[b];
      double v1r = work_re[b + 1], v1i = work_im[b + 1];
      double v2r = work_re[b + 2], v2i = work_im[b + 2];
      double v3r = work_re[b + 3], v3i = work_im[b + 3];
      double v4r = work_re[b + 4], v4i = work_im[b + 4];
      double v5r = work_re[b + 5], v5i = work_im[b + 5];
      double v6r = work_re[b + 6], v6i = work_im[b + 6];
      double v7r = work_re[b + 7], v7i = work_im[b + 7];
      double v8r = work_re[b + 8], v8i = work_im[b + 8];
      double v9r = work_re[b + 9], v9i = work_im[b + 9];
      double v10r = work_re[b + 10], v10i = work_im[b + 10];

      double s1 = v1r + v10r, s1i = v1i + v10i;
      double d1 = v1r - v10r, d1i = v1i - v10i;
      double s2 = v2r + v9r, s2i = v2i + v9i;
      double d2 = v2r - v9r, d2i = v2i - v9i;
      double s3 = v3r + v8r, s3i = v3i + v8i;
      double d3 = v3r - v8r, d3i = v3i - v8i;
      double s4 = v4r + v7r, s4i = v4i + v7i;
      double d4 = v4r - v7r, d4i = v4i - v7i;
      double s5 = v5r + v6r, s5i = v5i + v6i;
      double d5 = v5r - v6r, d5i = v5i - v6i;

      work_re[b] = v0r + s1 + s2 + s3 + s4 + s5;
      work_im[b] = v0i + s1i + s2i + s3i + s4i + s5i;

      double cR1 = w1R * s1 + w2R * s2 + w3R * s3 + w4R * s4 + w5R * s5;
      double cI1 = w1R * s1i + w2R * s2i + w3R * s3i + w4R * s4i + w5R * s5i;
      double tR1 = w1I * d1i + w2I * d2i + w3I * d3i + w4I * d4i + w5I * d5i;
      double tI1 = w1I * d1 + w2I * d2 + w3I * d3 + w4I * d4 + w5I * d5;
      work_re[b + 1] = v0r + cR1 - tR1;
      work_im[b + 1] = v0i + cI1 + tI1;
      work_re[b + 10] = v0r + cR1 + tR1;
      work_im[b + 10] = v0i + cI1 - tI1;

      double cR2 = w2R * s1 + w4R * s2 + w5R * s3 + w3R * s4 + w1R * s5;
      double cI2 = w2R * s1i + w4R * s2i + w5R * s3i + w3R * s4i + w1R * s5i;
      double tR2 = w2I * d1i + w4I * d2i - w5I * d3i - w3I * d4i - w1I * d5i;
      double tI2 = w2I * d1 + w4I * d2 - w5I * d3 - w3I * d4 - w1I * d5;
      work_re[b + 2] = v0r + cR2 - tR2;
      work_im[b + 2] = v0i + cI2 + tI2;
      work_re[b + 9] = v0r + cR2 + tR2;
      work_im[b + 9] = v0i + cI2 - tI2;

      double cR3 = w3R * s1 + w5R * s2 + w2R * s3 + w1R * s4 + w4R * s5;
      double cI3 = w3R * s1i + w5R * s2i + w2R * s3i + w1R * s4i + w4R * s5i;
      double tR3 = w3I * d1i - w5I * d2i - w2I * d3i + w1I * d4i + w4I * d5i;
      double tI3 = w3I * d1 - w5I * d2 - w2I * d3 + w1I * d4 + w4I * d5;
      work_re[b + 3] = v0r + cR3 - tR3;
      work_im[b + 3] = v0i + cI3 + tI3;
      work_re[b + 8] = v0r + cR3 + tR3;
      work_im[b + 8] = v0i + cI3 - tI3;

      double cR4 = w4R * s1 + w3R * s2 + w1R * s3 + w5R * s4 + w2R * s5;
      double cI4 = w4R * s1i + w3R * s2i + w1R * s3i + w5R * s4i + w2R * s5i;
      double tR4 = w4I * d1i - w3I * d2i + w1I * d3i + w5I * d4i - w2I * d5i;
      double tI4 = w4I * d1 - w3I * d2 + w1I * d3 + w5I * d4 - w2I * d5;
      work_re[b + 4] = v0r + cR4 - tR4;
      work_im[b + 4] = v0i + cI4 + tI4;
      work_re[b + 7] = v0r + cR4 + tR4;
      work_im[b + 7] = v0i + cI4 - tI4;

      double cR5 = w5R * s1 + w1R * s2 + w4R * s3 + w2R * s4 + w3R * s5;
      double cI5 = w5R * s1i + w1R * s2i + w4R * s3i + w2R * s4i + w3R * s5i;
      double tR5 = w5I * d1i - w1I * d2i + w4I * d3i - w2I * d4i + w3I * d5i;
      double tI5 = w5I * d1 - w1I * d2 + w4I * d3 - w2I * d4 + w3I * d5;
      work_re[b + 5] = v0r + cR5 - tR5;
      work_im[b + 5] = v0i + cI5 + tI5;
      work_re[b + 6] = v0r + cR5 + tR5;
      work_im[b + 6] = v0i + cI5 - tI5;
    }
    return;
  }

  const double* tw1R = tw_re + m;
  const double* tw1I = tw_im + m;
  const double* tw2R = tw_re + 2 * m;
  const double* tw2I = tw_im + 2 * m;
  const double* tw3R = tw_re + 3 * m;
  const double* tw3I = tw_im + 3 * m;
  const double* tw4R = tw_re + 4 * m;
  const double* tw4I = tw_im + 4 * m;
  const double* tw5R = tw_re + 5 * m;
  const double* tw5I = tw_im + 5 * m;
  const double* tw6R = tw_re + 6 * m;
  const double* tw6I = tw_im + 6 * m;
  const double* tw7R = tw_re + 7 * m;
  const double* tw7I = tw_im + 7 * m;
  const double* tw8R = tw_re + 8 * m;
  const double* tw8I = tw_im + 8 * m;
  const double* tw9R = tw_re + 9 * m;
  const double* tw9I = tw_im + 9 * m;
  const double* tw10R = tw_re + 10 * m;
  const double* tw10I = tw_im + 10 * m;

  for (size_t b = 0; b < fft->n; b += block_size) {
    double* w0r = work_re + b;
    double* w0i = work_im + b;
    double* w1r = w0r + m;
    double* w1i = w0i + m;
    double* w2r = w1r + m;
    double* w2i = w1i + m;
    double* w3r = w2r + m;
    double* w3i = w2i + m;
    double* w4r = w3r + m;
    double* w4i = w3i + m;
    double* w5r = w4r + m;
    double* w5i = w4i + m;
    double* w6r = w5r + m;
    double* w6i = w5i + m;
    double* w7r = w6r + m;
    double* w7i = w6i + m;
    double* w8r = w7r + m;
    double* w8i = w7i + m;
    double* w9r = w8r + m;
    double* w9i = w8i + m;
    double* w10r = w9r + m;
    double* w10i = w9i + m;

#if defined(__clang__)
#pragma clang loop vectorize(assume_safety) interleave(enable)
#elif defined(__GNUC__)
#pragma GCC ivdep
#endif
    for (size_t k = 0; k < m; k++) {
      double t1R = tw1R[k], t1I = tw1I[k];
      double t2R = tw2R[k], t2I = tw2I[k];
      double t3R = tw3R[k], t3I = tw3I[k];
      double t4R = tw4R[k], t4I = tw4I[k];
      double t5R = tw5R[k], t5I = tw5I[k];
      double t6R = tw6R[k], t6I = tw6I[k];
      double t7R = tw7R[k], t7I = tw7I[k];
      double t8R = tw8R[k], t8I = tw8I[k];
      double t9R = tw9R[k], t9I = tw9I[k];
      double t10R = tw10R[k], t10I = tw10I[k];

      double v0r = w0r[k], v0i = w0i[k];
      double v1r = w1r[k] * t1R - w1i[k] * t1I;
      double v1i = w1r[k] * t1I + w1i[k] * t1R;
      double v2r = w2r[k] * t2R - w2i[k] * t2I;
      double v2i = w2r[k] * t2I + w2i[k] * t2R;
      double v3r = w3r[k] * t3R - w3i[k] * t3I;
      double v3i = w3r[k] * t3I + w3i[k] * t3R;
      double v4r = w4r[k] * t4R - w4i[k] * t4I;
      double v4i = w4r[k] * t4I + w4i[k] * t4R;
      double v5r = w5r[k] * t5R - w5i[k] * t5I;
      double v5i = w5r[k] * t5I + w5i[k] * t5R;
      double v6r = w6r[k] * t6R - w6i[k] * t6I;
      double v6i = w6r[k] * t6I + w6i[k] * t6R;
      double v7r = w7r[k] * t7R - w7i[k] * t7I;
      double v7i = w7r[k] * t7I + w7i[k] * t7R;
      double v8r = w8r[k] * t8R - w8i[k] * t8I;
      double v8i = w8r[k] * t8I + w8i[k] * t8R;
      double v9r = w9r[k] * t9R - w9i[k] * t9I;
      double v9i = w9r[k] * t9I + w9i[k] * t9R;
      double v10r = w10r[k] * t10R - w10i[k] * t10I;
      double v10i = w10r[k] * t10I + w10i[k] * t10R;

      double s1 = v1r + v10r, s1i = v1i + v10i;
      double d1 = v1r - v10r, d1i = v1i - v10i;
      double s2 = v2r + v9r, s2i = v2i + v9i;
      double d2 = v2r - v9r, d2i = v2i - v9i;
      double s3 = v3r + v8r, s3i = v3i + v8i;
      double d3 = v3r - v8r, d3i = v3i - v8i;
      double s4 = v4r + v7r, s4i = v4i + v7i;
      double d4 = v4r - v7r, d4i = v4i - v7i;
      double s5 = v5r + v6r, s5i = v5i + v6i;
      double d5 = v5r - v6r, d5i = v5i - v6i;

      w0r[k] = v0r + s1 + s2 + s3 + s4 + s5;
      w0i[k] = v0i + s1i + s2i + s3i + s4i + s5i;

      double cR1 = w1R * s1 + w2R * s2 + w3R * s3 + w4R * s4 + w5R * s5;
      double cI1 = w1R * s1i + w2R * s2i + w3R * s3i + w4R * s4i + w5R * s5i;
      double tR1 = w1I * d1i + w2I * d2i + w3I * d3i + w4I * d4i + w5I * d5i;
      double tI1 = w1I * d1 + w2I * d2 + w3I * d3 + w4I * d4 + w5I * d5;
      w1r[k] = v0r + cR1 - tR1;
      w1i[k] = v0i + cI1 + tI1;
      w10r[k] = v0r + cR1 + tR1;
      w10i[k] = v0i + cI1 - tI1;

      double cR2 = w2R * s1 + w4R * s2 + w5R * s3 + w3R * s4 + w1R * s5;
      double cI2 = w2R * s1i + w4R * s2i + w5R * s3i + w3R * s4i + w1R * s5i;
      double tR2 = w2I * d1i + w4I * d2i - w5I * d3i - w3I * d4i - w1I * d5i;
      double tI2 = w2I * d1 + w4I * d2 - w5I * d3 - w3I * d4 - w1I * d5;
      w2r[k] = v0r + cR2 - tR2;
      w2i[k] = v0i + cI2 + tI2;
      w9r[k] = v0r + cR2 + tR2;
      w9i[k] = v0i + cI2 - tI2;

      double cR3 = w3R * s1 + w5R * s2 + w2R * s3 + w1R * s4 + w4R * s5;
      double cI3 = w3R * s1i + w5R * s2i + w2R * s3i + w1R * s4i + w4R * s5i;
      double tR3 = w3I * d1i - w5I * d2i - w2I * d3i + w1I * d4i + w4I * d5i;
      double tI3 = w3I * d1 - w5I * d2 - w2I * d3 + w1I * d4 + w4I * d5;
      w3r[k] = v0r + cR3 - tR3;
      w3i[k] = v0i + cI3 + tI3;
      w8r[k] = v0r + cR3 + tR3;
      w8i[k] = v0i + cI3 - tI3;

      double cR4 = w4R * s1 + w3R * s2 + w1R * s3 + w5R * s4 + w2R * s5;
      double cI4 = w4R * s1i + w3R * s2i + w1R * s3i + w5R * s4i + w2R * s5i;
      double tR4 = w4I * d1i - w3I * d2i + w1I * d3i + w5I * d4i - w2I * d5i;
      double tI4 = w4I * d1 - w3I * d2 + w1I * d3 + w5I * d4 - w2I * d5;
      w4r[k] = v0r + cR4 - tR4;
      w4i[k] = v0i + cI4 + tI4;
      w7r[k] = v0r + cR4 + tR4;
      w7i[k] = v0i + cI4 - tI4;

      double cR5 = w5R * s1 + w1R * s2 + w4R * s3 + w2R * s4 + w3R * s5;
      double cI5 = w5R * s1i + w1R * s2i + w4R * s3i + w2R * s4i + w3R * s5i;
      double tR5 = w5I * d1i - w1I * d2i + w4I * d3i - w2I * d4i + w3I * d5i;
      double tI5 = w5I * d1 - w1I * d2 + w4I * d3 - w2I * d4 + w3I * d5;
      w5r[k] = v0r + cR5 - tR5;
      w5i[k] = v0i + cI5 + tI5;
      w6r[k] = v0r + cR5 + tR5;
      w6i[k] = v0i + cI5 - tI5;
    }
  }
}

/**
 * @brief Apply radix-13 butterflies.
 */
static inline void stage_radix13(mixed_radix_fft_t* fft, double* work_re,
                                 double* work_im, size_t m, const double* tw_re,
                                 const double* tw_im) {
  size_t block_size = m * 13;
  double w1R = cos(2.0 * M_PI / 13.0);
  double w1I = -sin(2.0 * M_PI / 13.0);
  double w2R = cos(4.0 * M_PI / 13.0);
  double w2I = -sin(4.0 * M_PI / 13.0);
  double w3R = cos(6.0 * M_PI / 13.0);
  double w3I = -sin(6.0 * M_PI / 13.0);
  double w4R = cos(8.0 * M_PI / 13.0);
  double w4I = -sin(8.0 * M_PI / 13.0);
  double w5R = cos(10.0 * M_PI / 13.0);
  double w5I = -sin(10.0 * M_PI / 13.0);
  double w6R = cos(12.0 * M_PI / 13.0);
  double w6I = -sin(12.0 * M_PI / 13.0);

  if (m == 1) {
    size_t b = 0;
    for (; b + 26 <= fft->n; b += 26) {
#define BUTTERFLY13_M1(offset)                                                 \
  double v0r_##offset = work_re[b + offset];                                   \
  double v0i_##offset = work_im[b + offset];                                   \
  double v1r_##offset = work_re[b + offset + 1];                               \
  double v1i_##offset = work_im[b + offset + 1];                               \
  double v2r_##offset = work_re[b + offset + 2];                               \
  double v2i_##offset = work_im[b + offset + 2];                               \
  double v3r_##offset = work_re[b + offset + 3];                               \
  double v3i_##offset = work_im[b + offset + 3];                               \
  double v4r_##offset = work_re[b + offset + 4];                               \
  double v4i_##offset = work_im[b + offset + 4];                               \
  double v5r_##offset = work_re[b + offset + 5];                               \
  double v5i_##offset = work_im[b + offset + 5];                               \
  double v6r_##offset = work_re[b + offset + 6];                               \
  double v6i_##offset = work_im[b + offset + 6];                               \
  double v7r_##offset = work_re[b + offset + 7];                               \
  double v7i_##offset = work_im[b + offset + 7];                               \
  double v8r_##offset = work_re[b + offset + 8];                               \
  double v8i_##offset = work_im[b + offset + 8];                               \
  double v9r_##offset = work_re[b + offset + 9];                               \
  double v9i_##offset = work_im[b + offset + 9];                               \
  double v10r_##offset = work_re[b + offset + 10];                             \
  double v10i_##offset = work_im[b + offset + 10];                             \
  double v11r_##offset = work_re[b + offset + 11];                             \
  double v11i_##offset = work_im[b + offset + 11];                             \
  double v12r_##offset = work_re[b + offset + 12];                             \
  double v12i_##offset = work_im[b + offset + 12];                             \
                                                                               \
  double s1_##offset = v1r_##offset + v12r_##offset;                           \
  double s1i_##offset = v1i_##offset + v12i_##offset;                          \
  double d1_##offset = v1r_##offset - v12r_##offset;                           \
  double d1i_##offset = v1i_##offset - v12i_##offset;                          \
  double s2_##offset = v2r_##offset + v11r_##offset;                           \
  double s2i_##offset = v2i_##offset + v11i_##offset;                          \
  double d2_##offset = v2r_##offset - v11r_##offset;                           \
  double d2i_##offset = v2i_##offset - v11i_##offset;                          \
  double s3_##offset = v3r_##offset + v10r_##offset;                           \
  double s3i_##offset = v3i_##offset + v10i_##offset;                          \
  double d3_##offset = v3r_##offset - v10r_##offset;                           \
  double d3i_##offset = v3i_##offset - v10i_##offset;                          \
  double s4_##offset = v4r_##offset + v9r_##offset;                            \
  double s4i_##offset = v4i_##offset + v9i_##offset;                           \
  double d4_##offset = v4r_##offset - v9r_##offset;                            \
  double d4i_##offset = v4i_##offset - v9i_##offset;                           \
  double s5_##offset = v5r_##offset + v8r_##offset;                            \
  double s5i_##offset = v5i_##offset + v8i_##offset;                           \
  double d5_##offset = v5r_##offset - v8r_##offset;                            \
  double d5i_##offset = v5i_##offset - v8i_##offset;                           \
  double s6_##offset = v6r_##offset + v7r_##offset;                            \
  double s6i_##offset = v6i_##offset + v7i_##offset;                           \
  double d6_##offset = v6r_##offset - v7r_##offset;                            \
  double d6i_##offset = v6i_##offset - v7i_##offset;                           \
                                                                               \
  work_re[b + offset] = v0r_##offset + s1_##offset + s2_##offset +             \
                        s3_##offset + s4_##offset + s5_##offset + s6_##offset; \
  work_im[b + offset] = v0i_##offset + s1i_##offset + s2i_##offset +           \
                        s3i_##offset + s4i_##offset + s5i_##offset +           \
                        s6i_##offset;                                          \
                                                                               \
  double cR1_##offset = w1R * s1_##offset + w2R * s2_##offset +                \
                        w3R * s3_##offset + w4R * s4_##offset +                \
                        w5R * s5_##offset + w6R * s6_##offset;                 \
  double cI1_##offset = w1R * s1i_##offset + w2R * s2i_##offset +              \
                        w3R * s3i_##offset + w4R * s4i_##offset +              \
                        w5R * s5i_##offset + w6R * s6i_##offset;               \
  double tR1_##offset = w1I * d1i_##offset + w2I * d2i_##offset +              \
                        w3I * d3i_##offset + w4I * d4i_##offset +              \
                        w5I * d5i_##offset + w6I * d6i_##offset;               \
  double tI1_##offset = w1I * d1_##offset + w2I * d2_##offset +                \
                        w3I * d3_##offset + w4I * d4_##offset +                \
                        w5I * d5_##offset + w6I * d6_##offset;                 \
  work_re[b + offset + 1] = v0r_##offset + cR1_##offset - tR1_##offset;        \
  work_im[b + offset + 1] = v0i_##offset + cI1_##offset + tI1_##offset;        \
  work_re[b + offset + 12] = v0r_##offset + cR1_##offset + tR1_##offset;       \
  work_im[b + offset + 12] = v0i_##offset + cI1_##offset - tI1_##offset;       \
                                                                               \
  double cR2_##offset = w2R * s1_##offset + w4R * s2_##offset +                \
                        w6R * s3_##offset + w5R * s4_##offset +                \
                        w3R * s5_##offset + w1R * s6_##offset;                 \
  double cI2_##offset = w2R * s1i_##offset + w4R * s2i_##offset +              \
                        w6R * s3i_##offset + w5R * s4i_##offset +              \
                        w3R * s5i_##offset + w1R * s6i_##offset;               \
  double tR2_##offset = w2I * d1i_##offset + w4I * d2i_##offset +              \
                        w6I * d3i_##offset - w5I * d4i_##offset -              \
                        w3I * d5i_##offset - w1I * d6i_##offset;               \
  double tI2_##offset = w2I * d1_##offset + w4I * d2_##offset +                \
                        w6I * d3_##offset - w5I * d4_##offset -                \
                        w3I * d5_##offset - w1I * d6_##offset;                 \
  work_re[b + offset + 2] = v0r_##offset + cR2_##offset - tR2_##offset;        \
  work_im[b + offset + 2] = v0i_##offset + cI2_##offset + tI2_##offset;        \
  work_re[b + offset + 11] = v0r_##offset + cR2_##offset + tR2_##offset;       \
  work_im[b + offset + 11] = v0i_##offset + cI2_##offset - tI2_##offset;       \
                                                                               \
  double cR3_##offset = w3R * s1_##offset + w6R * s2_##offset +                \
                        w4R * s3_##offset + w1R * s4_##offset +                \
                        w2R * s5_##offset + w5R * s6_##offset;                 \
  double cI3_##offset = w3R * s1i_##offset + w6R * s2i_##offset +              \
                        w4R * s3i_##offset + w1R * s4i_##offset +              \
                        w2R * s5i_##offset + w5R * s6i_##offset;               \
  double tR3_##offset = w3I * d1i_##offset + w6I * d2i_##offset -              \
                        w4I * d3i_##offset - w1I * d4i_##offset +              \
                        w2I * d5i_##offset + w5I * d6i_##offset;               \
  double tI3_##offset = w3I * d1_##offset + w6I * d2_##offset -                \
                        w4I * d3_##offset - w1I * d4_##offset +                \
                        w2I * d5_##offset + w5I * d6_##offset;                 \
  work_re[b + offset + 3] = v0r_##offset + cR3_##offset - tR3_##offset;        \
  work_im[b + offset + 3] = v0i_##offset + cI3_##offset + tI3_##offset;        \
  work_re[b + offset + 10] = v0r_##offset + cR3_##offset + tR3_##offset;       \
  work_im[b + offset + 10] = v0i_##offset + cI3_##offset - tI3_##offset;       \
                                                                               \
  double cR4_##offset = w4R * s1_##offset + w5R * s2_##offset +                \
                        w1R * s3_##offset + w3R * s4_##offset +                \
                        w6R * s5_##offset + w2R * s6_##offset;                 \
  double cI4_##offset = w4R * s1i_##offset + w5R * s2i_##offset +              \
                        w1R * s3i_##offset + w3R * s4i_##offset +              \
                        w6R * s5i_##offset + w2R * s6i_##offset;               \
  double tR4_##offset = w4I * d1i_##offset - w5I * d2i_##offset -              \
                        w1I * d3i_##offset + w3I * d4i_##offset -              \
                        w6I * d5i_##offset - w2I * d6i_##offset;               \
  double tI4_##offset = w4I * d1_##offset - w5I * d2_##offset -                \
                        w1I * d3_##offset + w3I * d4_##offset -                \
                        w6I * d5_##offset - w2I * d6_##offset;                 \
  work_re[b + offset + 4] = v0r_##offset + cR4_##offset - tR4_##offset;        \
  work_im[b + offset + 4] = v0i_##offset + cI4_##offset + tI4_##offset;        \
  work_re[b + offset + 9] = v0r_##offset + cR4_##offset + tR4_##offset;        \
  work_im[b + offset + 9] = v0i_##offset + cI4_##offset - tI4_##offset;        \
                                                                               \
  double cR5_##offset = w5R * s1_##offset + w3R * s2_##offset +                \
                        w2R * s3_##offset + w6R * s4_##offset +                \
                        w1R * s5_##offset + w4R * s6_##offset;                 \
  double cI5_##offset = w5R * s1i_##offset + w3R * s2i_##offset +              \
                        w2R * s3i_##offset + w6R * s4i_##offset +              \
                        w1R * s5i_##offset + w4R * s6i_##offset;               \
  double tR5_##offset = w5I * d1i_##offset - w3I * d2i_##offset +              \
                        w2I * d3i_##offset - w6I * d4i_##offset -              \
                        w1I * d5i_##offset + w4I * d6i_##offset;               \
  double tI5_##offset = w5I * d1_##offset - w3I * d2_##offset +                \
                        w2I * d3_##offset - w6I * d4_##offset -                \
                        w1I * d5_##offset + w4I * d6_##offset;                 \
  work_re[b + offset + 5] = v0r_##offset + cR5_##offset - tR5_##offset;        \
  work_im[b + offset + 5] = v0i_##offset + cI5_##offset + tI5_##offset;        \
  work_re[b + offset + 8] = v0r_##offset + cR5_##offset + tR5_##offset;        \
  work_im[b + offset + 8] = v0i_##offset + cI5_##offset - tI5_##offset;        \
                                                                               \
  double cR6_##offset = w6R * s1_##offset + w1R * s2_##offset +                \
                        w5R * s3_##offset + w2R * s4_##offset +                \
                        w4R * s5_##offset + w3R * s6_##offset;                 \
  double cI6_##offset = w6R * s1i_##offset + w1R * s2i_##offset +              \
                        w5R * s3i_##offset + w2R * s4i_##offset +              \
                        w4R * s5i_##offset + w3R * s6i_##offset;               \
  double tR6_##offset = w6I * d1i_##offset - w1I * d2i_##offset +              \
                        w5I * d3i_##offset - w2I * d4i_##offset +              \
                        w4I * d5i_##offset - w3I * d6i_##offset;               \
  double tI6_##offset = w6I * d1_##offset - w1I * d2_##offset +                \
                        w5I * d3_##offset - w2I * d4_##offset +                \
                        w4I * d5_##offset - w3I * d6_##offset;                 \
  work_re[b + offset + 6] = v0r_##offset + cR6_##offset - tR6_##offset;        \
  work_im[b + offset + 6] = v0i_##offset + cI6_##offset + tI6_##offset;        \
  work_re[b + offset + 7] = v0r_##offset + cR6_##offset + tR6_##offset;        \
  work_im[b + offset + 7] = v0i_##offset + cI6_##offset - tI6_##offset;

      BUTTERFLY13_M1(0);
      BUTTERFLY13_M1(13);
#undef BUTTERFLY13_M1
    }
    for (; b < fft->n; b += 13) {
      double v0r = work_re[b], v0i = work_im[b];
      double v1r = work_re[b + 1], v1i = work_im[b + 1];
      double v2r = work_re[b + 2], v2i = work_im[b + 2];
      double v3r = work_re[b + 3], v3i = work_im[b + 3];
      double v4r = work_re[b + 4], v4i = work_im[b + 4];
      double v5r = work_re[b + 5], v5i = work_im[b + 5];
      double v6r = work_re[b + 6], v6i = work_im[b + 6];
      double v7r = work_re[b + 7], v7i = work_im[b + 7];
      double v8r = work_re[b + 8], v8i = work_im[b + 8];
      double v9r = work_re[b + 9], v9i = work_im[b + 9];
      double v10r = work_re[b + 10], v10i = work_im[b + 10];
      double v11r = work_re[b + 11], v11i = work_im[b + 11];
      double v12r = work_re[b + 12], v12i = work_im[b + 12];

      double s1 = v1r + v12r, s1i = v1i + v12i;
      double d1 = v1r - v12r, d1i = v1i - v12i;
      double s2 = v2r + v11r, s2i = v2i + v11i;
      double d2 = v2r - v11r, d2i = v2i - v11i;
      double s3 = v3r + v10r, s3i = v3i + v10i;
      double d3 = v3r - v10r, d3i = v3i - v10i;
      double s4 = v4r + v9r, s4i = v4i + v9i;
      double d4 = v4r - v9r, d4i = v4i - v9i;
      double s5 = v5r + v8r, s5i = v5i + v8i;
      double d5 = v5r - v8r, d5i = v5i - v8i;
      double s6 = v6r + v7r, s6i = v6i + v7i;
      double d6 = v6r - v7r, d6i = v6i - v7i;

      work_re[b] = v0r + s1 + s2 + s3 + s4 + s5 + s6;
      work_im[b] = v0i + s1i + s2i + s3i + s4i + s5i + s6i;

      double cR1 =
          w1R * s1 + w2R * s2 + w3R * s3 + w4R * s4 + w5R * s5 + w6R * s6;
      double cI1 =
          w1R * s1i + w2R * s2i + w3R * s3i + w4R * s4i + w5R * s5i + w6R * s6i;
      double tR1 =
          w1I * d1i + w2I * d2i + w3I * d3i + w4I * d4i + w5I * d5i + w6I * d6i;
      double tI1 =
          w1I * d1 + w2I * d2 + w3I * d3 + w4I * d4 + w5I * d5 + w6I * d6;
      work_re[b + 1] = v0r + cR1 - tR1;
      work_im[b + 1] = v0i + cI1 + tI1;
      work_re[b + 12] = v0r + cR1 + tR1;
      work_im[b + 12] = v0i + cI1 - tI1;

      double cR2 =
          w2R * s1 + w4R * s2 + w6R * s3 + w5R * s4 + w3R * s5 + w1R * s6;
      double cI2 =
          w2R * s1i + w4R * s2i + w6R * s3i + w5R * s4i + w3R * s5i + w1R * s6i;
      double tR2 =
          w2I * d1i + w4I * d2i + w6I * d3i - w5I * d4i - w3I * d5i - w1I * d6i;
      double tI2 =
          w2I * d1 + w4I * d2 + w6I * d3 - w5I * d4 - w3I * d5 - w1I * d6;
      work_re[b + 2] = v0r + cR2 - tR2;
      work_im[b + 2] = v0i + cI2 + tI2;
      work_re[b + 11] = v0r + cR2 + tR2;
      work_im[b + 11] = v0i + cI2 - tI2;

      double cR3 =
          w3R * s1 + w6R * s2 + w4R * s3 + w1R * s4 + w2R * s5 + w5R * s6;
      double cI3 =
          w3R * s1i + w6R * s2i + w4R * s3i + w1R * s4i + w2R * s5i + w5R * s6i;
      double tR3 =
          w3I * d1i + w6I * d2i - w4I * d3i - w1I * d4i + w2I * d5i + w5I * d6i;
      double tI3 =
          w3I * d1 + w6I * d2 - w4I * d3 - w1I * d4 + w2I * d5 + w5I * d6;
      work_re[b + 3] = v0r + cR3 - tR3;
      work_im[b + 3] = v0i + cI3 + tI3;
      work_re[b + 10] = v0r + cR3 + tR3;
      work_im[b + 10] = v0i + cI3 - tI3;

      double cR4 =
          w4R * s1 + w5R * s2 + w1R * s3 + w3R * s4 + w6R * s5 + w2R * s6;
      double cI4 =
          w4R * s1i + w5R * s2i + w1R * s3i + w3R * s4i + w6R * s5i + w2R * s6i;
      double tR4 =
          w4I * d1i - w5I * d2i - w1I * d3i + w3I * d4i - w6I * d5i - w2I * d6i;
      double tI4 =
          w4I * d1 - w5I * d2 - w1I * d3 + w3I * d4 - w6I * d5 - w2I * d6;
      work_re[b + 4] = v0r + cR4 - tR4;
      work_im[b + 4] = v0i + cI4 + tI4;
      work_re[b + 9] = v0r + cR4 + tR4;
      work_im[b + 9] = v0i + cI4 - tI4;

      double cR5 =
          w5R * s1 + w3R * s2 + w2R * s3 + w6R * s4 + w1R * s5 + w4R * s6;
      double cI5 =
          w5R * s1i + w3R * s2i + w2R * s3i + w6R * s4i + w1R * s5i + w4R * s6i;
      double tR5 =
          w5I * d1i - w3I * d2i + w2I * d3i - w6I * d4i - w1I * d5i + w4I * d6i;
      double tI5 =
          w5I * d1 - w3I * d2 + w2I * d3 - w6I * d4 - w1I * d5 + w4I * d6;
      work_re[b + 5] = v0r + cR5 - tR5;
      work_im[b + 5] = v0i + cI5 + tI5;
      work_re[b + 8] = v0r + cR5 + tR5;
      work_im[b + 8] = v0i + cI5 - tI5;

      double cR6 =
          w6R * s1 + w1R * s2 + w5R * s3 + w2R * s4 + w4R * s5 + w3R * s6;
      double cI6 =
          w6R * s1i + w1R * s2i + w5R * s3i + w2R * s4i + w4R * s5i + w3R * s6i;
      double tR6 =
          w6I * d1i - w1I * d2i + w5I * d3i - w2I * d4i + w4I * d5i - w3I * d6i;
      double tI6 =
          w6I * d1 - w1I * d2 + w5I * d3 - w2I * d4 + w4I * d5 - w3I * d6;
      work_re[b + 6] = v0r + cR6 - tR6;
      work_im[b + 6] = v0i + cI6 + tI6;
      work_re[b + 7] = v0r + cR6 + tR6;
      work_im[b + 7] = v0i + cI6 - tI6;
    }
    return;
  }

  const double* tw1R = tw_re + m;
  const double* tw1I = tw_im + m;
  const double* tw2R = tw_re + 2 * m;
  const double* tw2I = tw_im + 2 * m;
  const double* tw3R = tw_re + 3 * m;
  const double* tw3I = tw_im + 3 * m;
  const double* tw4R = tw_re + 4 * m;
  const double* tw4I = tw_im + 4 * m;
  const double* tw5R = tw_re + 5 * m;
  const double* tw5I = tw_im + 5 * m;
  const double* tw6R = tw_re + 6 * m;
  const double* tw6I = tw_im + 6 * m;
  const double* tw7R = tw_re + 7 * m;
  const double* tw7I = tw_im + 7 * m;
  const double* tw8R = tw_re + 8 * m;
  const double* tw8I = tw_im + 8 * m;
  const double* tw9R = tw_re + 9 * m;
  const double* tw9I = tw_im + 9 * m;
  const double* tw10R = tw_re + 10 * m;
  const double* tw10I = tw_im + 10 * m;
  const double* tw11R = tw_re + 11 * m;
  const double* tw11I = tw_im + 11 * m;
  const double* tw12R = tw_re + 12 * m;
  const double* tw12I = tw_im + 12 * m;

  for (size_t b = 0; b < fft->n; b += block_size) {
    double* w0r = work_re + b;
    double* w0i = work_im + b;
    double* w1r = w0r + m;
    double* w1i = w0i + m;
    double* w2r = w1r + m;
    double* w2i = w1i + m;
    double* w3r = w2r + m;
    double* w3i = w2i + m;
    double* w4r = w3r + m;
    double* w4i = w3i + m;
    double* w5r = w4r + m;
    double* w5i = w4i + m;
    double* w6r = w5r + m;
    double* w6i = w5i + m;
    double* w7r = w6r + m;
    double* w7i = w6i + m;
    double* w8r = w7r + m;
    double* w8i = w7i + m;
    double* w9r = w8r + m;
    double* w9i = w8i + m;
    double* w10r = w9r + m;
    double* w10i = w9i + m;
    double* w11r = w10r + m;
    double* w11i = w10i + m;
    double* w12r = w11r + m;
    double* w12i = w11i + m;

#if defined(__clang__)
#pragma clang loop vectorize(assume_safety) interleave(enable)
#elif defined(__GNUC__)
#pragma GCC ivdep
#endif
    for (size_t k = 0; k < m; k++) {
      double t1R = tw1R[k], t1I = tw1I[k];
      double t2R = tw2R[k], t2I = tw2I[k];
      double t3R = tw3R[k], t3I = tw3I[k];
      double t4R = tw4R[k], t4I = tw4I[k];
      double t5R = tw5R[k], t5I = tw5I[k];
      double t6R = tw6R[k], t6I = tw6I[k];
      double t7R = tw7R[k], t7I = tw7I[k];
      double t8R = tw8R[k], t8I = tw8I[k];
      double t9R = tw9R[k], t9I = tw9I[k];
      double t10R = tw10R[k], t10I = tw10I[k];
      double t11R = tw11R[k], t11I = tw11I[k];
      double t12R = tw12R[k], t12I = tw12I[k];

      double v0r = w0r[k], v0i = w0i[k];
      double v1r = w1r[k] * t1R - w1i[k] * t1I;
      double v1i = w1r[k] * t1I + w1i[k] * t1R;
      double v2r = w2r[k] * t2R - w2i[k] * t2I;
      double v2i = w2r[k] * t2I + w2i[k] * t2R;
      double v3r = w3r[k] * t3R - w3i[k] * t3I;
      double v3i = w3r[k] * t3I + w3i[k] * t3R;
      double v4r = w4r[k] * t4R - w4i[k] * t4I;
      double v4i = w4r[k] * t4I + w4i[k] * t4R;
      double v5r = w5r[k] * t5R - w5i[k] * t5I;
      double v5i = w5r[k] * t5I + w5i[k] * t5R;
      double v6r = w6r[k] * t6R - w6i[k] * t6I;
      double v6i = w6r[k] * t6I + w6i[k] * t6R;
      double v7r = w7r[k] * t7R - w7i[k] * t7I;
      double v7i = w7r[k] * t7I + w7i[k] * t7R;
      double v8r = w8r[k] * t8R - w8i[k] * t8I;
      double v8i = w8r[k] * t8I + w8i[k] * t8R;
      double v9r = w9r[k] * t9R - w9i[k] * t9I;
      double v9i = w9r[k] * t9I + w9i[k] * t9R;
      double v10r = w10r[k] * t10R - w10i[k] * t10I;
      double v10i = w10r[k] * t10I + w10i[k] * t10R;
      double v11r = w11r[k] * t11R - w11i[k] * t11I;
      double v11i = w11r[k] * t11I + w11i[k] * t11R;
      double v12r = w12r[k] * t12R - w12i[k] * t12I;
      double v12i = w12r[k] * t12I + w12i[k] * t12R;

      double s1 = v1r + v12r, s1i = v1i + v12i;
      double d1 = v1r - v12r, d1i = v1i - v12i;
      double s2 = v2r + v11r, s2i = v2i + v11i;
      double d2 = v2r - v11r, d2i = v2i - v11i;
      double s3 = v3r + v10r, s3i = v3i + v10i;
      double d3 = v3r - v10r, d3i = v3i - v10i;
      double s4 = v4r + v9r, s4i = v4i + v9i;
      double d4 = v4r - v9r, d4i = v4i - v9i;
      double s5 = v5r + v8r, s5i = v5i + v8i;
      double d5 = v5r - v8r, d5i = v5i - v8i;
      double s6 = v6r + v7r, s6i = v6i + v7i;
      double d6 = v6r - v7r, d6i = v6i - v7i;

      w0r[k] = v0r + s1 + s2 + s3 + s4 + s5 + s6;
      w0i[k] = v0i + s1i + s2i + s3i + s4i + s5i + s6i;

      double cR1 =
          w1R * s1 + w2R * s2 + w3R * s3 + w4R * s4 + w5R * s5 + w6R * s6;
      double cI1 =
          w1R * s1i + w2R * s2i + w3R * s3i + w4R * s4i + w5R * s5i + w6R * s6i;
      double tR1 =
          w1I * d1i + w2I * d2i + w3I * d3i + w4I * d4i + w5I * d5i + w6I * d6i;
      double tI1 =
          w1I * d1 + w2I * d2 + w3I * d3 + w4I * d4 + w5I * d5 + w6I * d6;
      w1r[k] = v0r + cR1 - tR1;
      w1i[k] = v0i + cI1 + tI1;
      w12r[k] = v0r + cR1 + tR1;
      w12i[k] = v0i + cI1 - tI1;

      double cR2 =
          w2R * s1 + w4R * s2 + w6R * s3 + w5R * s4 + w3R * s5 + w1R * s6;
      double cI2 =
          w2R * s1i + w4R * s2i + w6R * s3i + w5R * s4i + w3R * s5i + w1R * s6i;
      double tR2 =
          w2I * d1i + w4I * d2i + w6I * d3i - w5I * d4i - w3I * d5i - w1I * d6i;
      double tI2 =
          w2I * d1 + w4I * d2 + w6I * d3 - w5I * d4 - w3I * d5 - w1I * d6;
      w2r[k] = v0r + cR2 - tR2;
      w2i[k] = v0i + cI2 + tI2;
      w11r[k] = v0r + cR2 + tR2;
      w11i[k] = v0i + cI2 - tI2;

      double cR3 =
          w3R * s1 + w6R * s2 + w4R * s3 + w1R * s4 + w2R * s5 + w5R * s6;
      double cI3 =
          w3R * s1i + w6R * s2i + w4R * s3i + w1R * s4i + w2R * s5i + w5R * s6i;
      double tR3 =
          w3I * d1i + w6I * d2i - w4I * d3i - w1I * d4i + w2I * d5i + w5I * d6i;
      double tI3 =
          w3I * d1 + w6I * d2 - w4I * d3 - w1I * d4 + w2I * d5 + w5I * d6;
      w3r[k] = v0r + cR3 - tR3;
      w3i[k] = v0i + cI3 + tI3;
      w10r[k] = v0r + cR3 + tR3;
      w10i[k] = v0i + cI3 - tI3;

      double cR4 =
          w4R * s1 + w5R * s2 + w1R * s3 + w3R * s4 + w6R * s5 + w2R * s6;
      double cI4 =
          w4R * s1i + w5R * s2i + w1R * s3i + w3R * s4i + w6R * s5i + w2R * s6i;
      double tR4 =
          w4I * d1i - w5I * d2i - w1I * d3i + w3I * d4i - w6I * d5i - w2I * d6i;
      double tI4 =
          w4I * d1 - w5I * d2 - w1I * d3 + w3I * d4 - w6I * d5 - w2I * d6;
      w4r[k] = v0r + cR4 - tR4;
      w4i[k] = v0i + cI4 + tI4;
      w9r[k] = v0r + cR4 + tR4;
      w9i[k] = v0i + cI4 - tI4;

      double cR5 =
          w5R * s1 + w3R * s2 + w2R * s3 + w6R * s4 + w1R * s5 + w4R * s6;
      double cI5 =
          w5R * s1i + w3R * s2i + w2R * s3i + w6R * s4i + w1R * s5i + w4R * s6i;
      double tR5 =
          w5I * d1i - w3I * d2i + w2I * d3i - w6I * d4i - w1I * d5i + w4I * d6i;
      double tI5 =
          w5I * d1 - w3I * d2 + w2I * d3 - w6I * d4 - w1I * d5 + w4I * d6;
      w5r[k] = v0r + cR5 - tR5;
      w5i[k] = v0i + cI5 + tI5;
      w8r[k] = v0r + cR5 + tR5;
      w8i[k] = v0i + cI5 - tI5;

      double cR6 =
          w6R * s1 + w1R * s2 + w5R * s3 + w2R * s4 + w4R * s5 + w3R * s6;
      double cI6 =
          w6R * s1i + w1R * s2i + w5R * s3i + w2R * s4i + w4R * s5i + w3R * s6i;
      double tR6 =
          w6I * d1i - w1I * d2i + w5I * d3i - w2I * d4i + w4I * d5i - w3I * d6i;
      double tI6 =
          w6I * d1 - w1I * d2 + w5I * d3 - w2I * d4 + w4I * d5 - w3I * d6;
      w6r[k] = v0r + cR6 - tR6;
      w6i[k] = v0i + cI6 + tI6;
      w7r[k] = v0r + cR6 + tR6;
      w7i[k] = v0i + cI6 - tI6;
    }
  }
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC pop_options
#elif defined(__clang__)
#pragma float_control(pop)
#endif

mixed_radix_fft_t* mixed_radix_fft_create(size_t n) {
  if (n == 0) return NULL;
  // Factorise into 2/3/4/5/7/8 and arbitrary prime radices.
  // Power-of-2 portion prefers larger radixes: `2⁵ = 32 → [8, 4]`.
  int fs[64];
  int stage_count = 0;
  size_t rem = n;

  int two_pow = 0;
  while (rem % 2 == 0) {
    two_pow++;
    rem /= 2;
  }
  // Greedy: take 8s while we have >= 3 powers of 2 remaining, then a
  // single 4 if 2 remain, or a 2 if 1 remains.
  while (two_pow >= 3) {
    fs[stage_count++] = 8;
    two_pow -= 3;
  }
  if (two_pow == 2) {
    fs[stage_count++] = 4;
  } else if (two_pow == 1) {
    fs[stage_count++] = 2;
  }

  // 2. Powers of 3: greedy 9 -> 3
  int three_pow = 0;
  while (rem % 3 == 0) {
    three_pow++;
    rem /= 3;
  }
  while (three_pow >= 2) {
    fs[stage_count++] = 9;
    three_pow -= 2;
  }
  if (three_pow == 1) {
    fs[stage_count++] = 3;
  }

  // 3. Small primes with dedicated unrolled kernels: 5, 7, 11, 13
  int small_primes[4] = {5, 7, 11, 13};
  for (int i = 0; i < 4; i++) {
    int p = small_primes[i];
    while (rem % (size_t)p == 0) {
      fs[stage_count++] = p;
      rem /= (size_t)p;
    }
  }

  // If there are unsupported prime factors remaining (p > 13), mixed-radix
  // cannot handle this size; return NULL to allow fallback (e.g. Bluestein).
  if (rem > 1) {
    return NULL;
  }

  mixed_radix_fft_t* fft =
      (mixed_radix_fft_t*)calloc(1, sizeof(mixed_radix_fft_t));
  if (!fft) return NULL;

  fft->base.ctx = fft;
  fft->base.execute = mixed_radix_fft_execute_wrapper;
  fft->base.free = mixed_radix_fft_free_wrapper;
  fft->n = n;
  fft->stage_count = stage_count;
  fft->factors = (int*)calloc(stage_count, sizeof(int));
  fft->twiddle_re = (double**)calloc(stage_count, sizeof(double*));
  fft->twiddle_im = (double**)calloc(stage_count, sizeof(double*));
  fft->permutation = (size_t*)calloc(n, sizeof(size_t));

  if (!fft->factors || !fft->twiddle_re || !fft->twiddle_im ||
      !fft->permutation) {
    mixed_radix_fft_free(fft);
    return NULL;
  }

  for (int s = 0; s < stage_count; s++) {
    fft->factors[s] = fs[s];
    fft->twiddle_re[s] = NULL;
    fft->twiddle_im[s] = NULL;
  }

  // Allocate per-stage twiddle buffers.
  size_t m = 1;
  for (int s = 0; s < stage_count; s++) {
    int r = fs[s];
    size_t len = m * (size_t)r;
    fft->twiddle_re[s] = (double*)calloc(len, sizeof(double));
    fft->twiddle_im[s] = (double*)calloc(len, sizeof(double));
    if (!fft->twiddle_re[s] || !fft->twiddle_im[s]) {
      mixed_radix_fft_free(fft);
      return NULL;
    }
    // twiddle[j*m + k] = W_{m·r}^(j·k) for j in 0..r-1, k in 0..m-1.
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

  // Pre-compute the digit-reversal permutation. We store `factors` in
  // stage-iteration order (`factors[0]` is the radix processed first, with
  // `m = 1`); the corresponding decimation order, used to build the perm,
  // is the reverse. So we iterate `factors.reversed()` here. Failing to
  // reverse leaves stage 0 operating on the wrong input groups — the bug
  // that turned this whole mixed-radix path into garbage on the first
  // attempt.
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
  fft->scratch_re = (double*)malloc(n * sizeof(double));
  fft->scratch_im = (double*)malloc(n * sizeof(double));
  if (!fft->scratch_re || !fft->scratch_im) {
    mixed_radix_fft_free(fft);
    return NULL;
  }

  return fft;
}

void mixed_radix_fft_execute(mixed_radix_fft_t* fft, waveform_t real_in,
                             waveform_t imag_in, mutable_waveform_t real_out,
                             mutable_waveform_t imag_out, bool inverse) {
  if (!fft) return;
  const double* src_re = real_in;
  const double* src_im = imag_in;

  if (real_in == real_out && fft->scratch_re) {
    memcpy(fft->scratch_re, real_in, fft->n * sizeof(double));
    src_re = fft->scratch_re;
  }
  if (imag_in == imag_out && fft->scratch_im) {
    memcpy(fft->scratch_im, imag_in, fft->n * sizeof(double));
    src_im = fft->scratch_im;
  }

  double* work_re = real_out;
  double* work_im = imag_out;

  // Step 1: permute input. For inverse, conjugate as we go
  if (inverse) {
    for (size_t i = 0; i < fft->n; i++) {
      size_t p = fft->permutation[i];
      work_re[p] = src_re[i];
      work_im[p] = -src_im[i];
    }
  } else {
    for (size_t i = 0; i < fft->n; i++) {
      size_t p = fft->permutation[i];
      work_re[p] = src_re[i];
      work_im[p] = src_im[i];
    }
  }

  // Step 2: butterfly stages, all in-place on (work_re, work_im) =
  // (real_out, imag_out).
  size_t m = 1;
  for (int s = 0; s < fft->stage_count; s++) {
    int r = fft->factors[s];
    const double* twRe = fft->twiddle_re[s];
    const double* twIm = fft->twiddle_im[s];
    switch (r) {
      case 2:
        stage_radix2(fft, work_re, work_im, m, twRe, twIm);
        break;
      case 3:
        stage_radix3(fft, work_re, work_im, m, twRe, twIm);
        break;
      case 4:
        stage_radix4(fft, work_re, work_im, m, twRe, twIm);
        break;
      case 5:
        stage_radix5(fft, work_re, work_im, m, twRe, twIm);
        break;
      case 7:
        stage_radix7(fft, work_re, work_im, m, twRe, twIm);
        break;
      case 8:
        stage_radix8(fft, work_re, work_im, m, twRe, twIm);
        break;
      case 9:
        stage_radix9(fft, work_re, work_im, m, twRe, twIm);
        break;
      case 11:
        stage_radix11(fft, work_re, work_im, m, twRe, twIm);
        break;
      case 13:
        stage_radix13(fft, work_re, work_im, m, twRe, twIm);
        break;
      default:
        break;
    }
    m *= (size_t)r;
  }

  // Step 3: re-conjugate the imaginary part for the inverse direction.
  // Forward direction is already done in place — no copy needed.
  if (inverse) {
    for (size_t i = 0; i < fft->n; i++) {
      imag_out[i] = -imag_out[i];
    }
  }
}

void mixed_radix_fft_free(mixed_radix_fft_t* fft) {
  if (!fft) return;
  if (fft->twiddle_re) {
    for (int s = 0; s < fft->stage_count; s++) {
      if (fft->twiddle_re[s]) free(fft->twiddle_re[s]);
    }
    free(fft->twiddle_re);
  }
  if (fft->twiddle_im) {
    for (int s = 0; s < fft->stage_count; s++) {
      if (fft->twiddle_im[s]) free(fft->twiddle_im[s]);
    }
    free(fft->twiddle_im);
  }
  if (fft->factors) free(fft->factors);
  if (fft->permutation) free(fft->permutation);
  if (fft->scratch_re) free(fft->scratch_re);
  if (fft->scratch_im) free(fft->scratch_im);
  free(fft);
}
