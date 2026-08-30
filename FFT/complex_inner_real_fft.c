#if defined(ENABLE_ACCELERATE)
// Real-FFT backend that builds a 2N-point real FFT from one N-point
// complex FFT plus an O(N) "untwiddle" pass. Used for any even length
// that doesn't qualify for `vdsp_real_fft` (i.e. non-power-of-two, or
// pow2 < 8).
//
// The inner N-point complex FFT is supplied by the caller —
// `real_fft_create` picks between `mixed_radix_fft` and `bluestein_fft`
// based on `half_n`'s factorisation.
// This module stays purely about the real-FFT structure (packing,
// untwiddle, inverse unpack) and never re-decides the backend.
//
// Algorithm references:
//   - https://www.dsprelated.com/showarticle/4.php (Real FFT from complex FFT)
//   - https://en.wikipedia.org/wiki/Fast_Fourier_transform#Real-input_FFTs

#include "FFT/complex_inner_real_fft.h"

#include <Accelerate/Accelerate.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>

#include "Utils/double_helpers.h"
#include "real_fft_backend.h"

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC push_options
#pragma GCC optimize("O3,fast-math,finite-math-only")
#elif defined(__clang__)
#pragma float_control(precise, off, push)
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct complex_inner_real_fft {
  real_fft_backend_t base;
  size_t half_n;  // = length / 2 = N
  /// The N-point complex FFT picked at construction. Could be any
  /// `arbitrary_complex_fft_t` — `mixed_radix_fft` or `bluestein_fft`
  /// depending on what `real_fft_create` chose.
  arbitrary_complex_fft_t* inner;
  // Unit-modulus twiddle table `W[k] = exp(-iπk/N)` for k = 0..N-1.
  double* twiddle_re;
  double* twiddle_im;
  // Hot-path scratch (length N).
  double* z_re;
  double* z_im;
  double* z_f_re;
  double* z_f_im;
};

/**
 * @brief Wrapper for the forward FFT implementation.
 *
 * This function conforms to the signature required by the real_fft_backend_t
 * interface. It casts the context pointer back to complex_inner_real_fft_t and
 * calls the actual forward function.
 *
 * @param ctx Pointer to the complex_inner_real_fft_t context.
 * @param real_in Input real waveform.
 * @param spec_re Output real part of the spectrum.
 * @param spec_im Output imaginary part of the spectrum.
 */
static void complex_inner_real_fft_forward_wrapper(void* ctx,
                                                   waveform_t real_in,
                                                   mutable_waveform_t spec_re,
                                                   mutable_waveform_t spec_im) {
  complex_inner_real_fft_forward((complex_inner_real_fft_t*)ctx, real_in,
                                 spec_re, spec_im);
}

/**
 * @brief Wrapper for the inverse FFT implementation.
 *
 * This function conforms to the signature required by the real_fft_backend_t
 * interface. It casts the context pointer back to complex_inner_real_fft_t and
 * calls the actual inverse function.
 *
 * @param ctx Pointer to the complex_inner_real_fft_t context.
 * @param spec_re Input real part of the spectrum.
 * @param spec_im Input imaginary part of the spectrum.
 * @param real_out Output real waveform.
 */
static void complex_inner_real_fft_inverse_wrapper(
    void* ctx, waveform_t spec_re, waveform_t spec_im,
    mutable_waveform_t real_out) {
  complex_inner_real_fft_inverse((complex_inner_real_fft_t*)ctx, spec_re,
                                 spec_im, real_out);
}

/**
 * @brief Wrapper for the free function.
 *
 * This function conforms to the signature required by the real_fft_backend_t
 * interface. It casts the context pointer back to complex_inner_real_fft_t and
 * calls the actual free function.
 *
 * @param ctx Pointer to the complex_inner_real_fft_t context.
 */
static void complex_inner_real_fft_free_wrapper(void* ctx) {
  complex_inner_real_fft_free((complex_inner_real_fft_t*)ctx);
}

complex_inner_real_fft_t* complex_inner_real_fft_create(
    size_t length, arbitrary_complex_fft_t* inner) {
  if (length == 0 || length % 2 != 0 || !inner) return NULL;
  size_t half_n = length / 2;
  complex_inner_real_fft_t* fft =
      (complex_inner_real_fft_t*)calloc(1, sizeof(complex_inner_real_fft_t));
  if (!fft) return NULL;

  fft->base.ctx = fft;
  fft->base.forward = complex_inner_real_fft_forward_wrapper;
  fft->base.inverse = complex_inner_real_fft_inverse_wrapper;
  fft->base.free = complex_inner_real_fft_free_wrapper;
  fft->half_n = half_n;

  fft->twiddle_re = (double*)calloc(half_n, sizeof(double));
  fft->twiddle_im = (double*)calloc(half_n, sizeof(double));
  fft->z_re = (double*)calloc(half_n, sizeof(double));
  fft->z_im = (double*)calloc(half_n, sizeof(double));
  fft->z_f_re = (double*)calloc(half_n, sizeof(double));
  fft->z_f_im = (double*)calloc(half_n, sizeof(double));

  if (!fft->twiddle_re || !fft->twiddle_im || !fft->z_re || !fft->z_im ||
      !fft->z_f_re || !fft->z_f_im) {
    complex_inner_real_fft_free(fft);
    return NULL;
  }

  fft->inner = inner;

  for (size_t k = 0; k < half_n; k++) {
    double theta = -M_PI * (double)k / (double)half_n;
    fft->twiddle_re[k] = cos(theta);
    fft->twiddle_im[k] = sin(theta);
  }
  return fft;
}

void complex_inner_real_fft_forward(complex_inner_real_fft_t* fft,
                                    waveform_t real_in,
                                    mutable_waveform_t spec_re,
                                    mutable_waveform_t spec_im) {
  if (!fft) return;
  size_t n = fft->half_n;

  // Pack the 2N real samples into N complex: z[k] = x[2k] + i·x[2k+1].
  // Reinterpret `realIn` as interleaved complex pairs and let `vDSP_ctozD`
  // do the deinterleave in one pass.
  DSPDoubleSplitComplex zSplit = {fft->z_re, fft->z_im};
  vDSP_ctozD((const DSPDoubleComplex*)real_in, 2, &zSplit, 1, (vDSP_Length)n);

  // Z = FFT_N(z). Unnormalised forward.
  arbitrary_complex_fft_execute(fft->inner, fft->z_re, fft->z_im, fft->z_f_re,
                                fft->z_f_im, false);

  // DC and Nyquist bins (both real):
  //   X[0] = Re(Z[0]) + Im(Z[0])
  //   X[N] = Re(Z[0]) - Im(Z[0])
  double z0r = fft->z_f_re[0];
  double z0i = fft->z_f_im[0];
  spec_re[0] = z0r + z0i;
  spec_im[0] = 0.0;
  spec_re[n] = z0r - z0i;
  spec_im[n] = 0.0;

  // Generic untwiddle for k ∈ [1, N):
  //   E[k] = ½ · (Z[k] + conj(Z[N-k]))
  //   O[k] = -½·i · (Z[k] - conj(Z[N-k]))
  //   X[k] = E[k] + W^k · O[k],  W^k = exp(-iπk/N)
  const double* __restrict__ z_f_re = fft->z_f_re;
  const double* __restrict__ z_f_im = fft->z_f_im;
  const double* __restrict__ tw_re = fft->twiddle_re;
  const double* __restrict__ tw_im = fft->twiddle_im;
  double* __restrict__ out_re = spec_re;
  double* __restrict__ out_im = spec_im;
#if defined(__clang__)
#pragma clang loop vectorize(assume_safety) interleave(enable)
#elif defined(__GNUC__)
#pragma GCC ivdep
#endif
  for (size_t k = 1; k < n; k++) {
    double zkR = z_f_re[k];
    double zkI = z_f_im[k];
    double zmR = z_f_re[n - k];
    double zmI = z_f_im[n - k];
    double eRe = 0.5 * (zkR + zmR);
    double eIm = 0.5 * (zkI - zmI);
    double diffRe = zkR - zmR;
    double diffIm = zkI + zmI;
    double oRe = 0.5 * diffIm;
    double oIm = -0.5 * diffRe;
    double twR = tw_re[k];
    double twI = tw_im[k];
    double woRe = twR * oRe - twI * oIm;
    double woIm = twR * oIm + twI * oRe;
    out_re[k] = eRe + woRe;
    out_im[k] = eIm + woIm;
  }
}

void complex_inner_real_fft_inverse(complex_inner_real_fft_t* fft,
                                    waveform_t spec_re, waveform_t spec_im,
                                    mutable_waveform_t real_out) {
  if (!fft) return;
  size_t n = fft->half_n;
  // DC bin packs the special pair (X[0], X[N]):
  //   z[0] = ½·(X[0] + X[N]) + ½·i·(X[0] - X[N])
  double x0 = spec_re[0];
  double xN = spec_re[n];
  fft->z_re[0] = 0.5 * (x0 + xN);
  fft->z_im[0] = 0.5 * (x0 - xN);

  // Generic inverse untwiddle for k ∈ [1, N):
  //   E[k] = ½·(X[k] + conj(X[N-k]))
  //   O[k] = ½·conj(W^k)·(X[k] - conj(X[N-k]))
  //   z[k] = E[k] + i·O[k]
  const double* __restrict__ in_re = spec_re;
  const double* __restrict__ in_im = spec_im;
  const double* __restrict__ tw_re = fft->twiddle_re;
  const double* __restrict__ tw_im = fft->twiddle_im;
  double* __restrict__ z_re = fft->z_re;
  double* __restrict__ z_im = fft->z_im;
#if defined(__clang__)
#pragma clang loop vectorize(assume_safety) interleave(enable)
#elif defined(__GNUC__)
#pragma GCC ivdep
#endif
  for (size_t k = 1; k < n; k++) {
    double xkR = in_re[k];
    double xkI = in_im[k];
    double xmR = in_re[n - k];
    double xmI = in_im[n - k];
    double eRe = 0.5 * (xkR + xmR);
    double eIm = 0.5 * (xkI - xmI);
    double halfDiffRe = 0.5 * (xkR - xmR);
    double halfDiffIm = 0.5 * (xkI + xmI);
    double twR = tw_re[k];
    double twI = tw_im[k];
    double oRe = halfDiffRe * twR + halfDiffIm * twI;
    double oIm = halfDiffIm * twR - halfDiffRe * twI;
    z_re[k] = eRe - oIm;
    z_im[k] = eIm + oRe;
  }

  // Inner inverse FFT. The inner returns the unnormalised N-point IFFT,
  // i.e. `N · z`. The textbook unnormalised 2N-point IFFT equals `2 · N · z`,
  // so the unpack picks up a factor of 2.
  arbitrary_complex_fft_execute(fft->inner, fft->z_re, fft->z_im, fft->z_f_re,
                                fft->z_f_im, true);

  // Fused scale-by-2 and interleave unpack in a single memory pass.
  // Reconstructs the 2N unnormalised real samples:
  // real_out[2k] = 2·Re(z[k]), real_out[2k+1] = 2·Im(z[k]).
  const double* __restrict__ z_f_re = fft->z_f_re;
  const double* __restrict__ z_f_im = fft->z_f_im;
  double* __restrict__ out = real_out;
#if defined(__clang__)
#pragma clang loop vectorize(assume_safety) interleave(enable)
#elif defined(__GNUC__)
#pragma GCC ivdep
#endif
  for (size_t k = 0; k < n; k++) {
    out[2 * k] = 2.0 * z_f_re[k];
    out[2 * k + 1] = 2.0 * z_f_im[k];
  }
}

void complex_inner_real_fft_free(complex_inner_real_fft_t* fft) {
  if (!fft) return;
  if (fft->inner) arbitrary_complex_fft_free(fft->inner);
  if (fft->twiddle_re) free(fft->twiddle_re);
  if (fft->twiddle_im) free(fft->twiddle_im);
  if (fft->z_re) free(fft->z_re);
  if (fft->z_im) free(fft->z_im);
  if (fft->z_f_re) free(fft->z_f_re);
  if (fft->z_f_im) free(fft->z_f_im);
  free(fft);
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC pop_options
#elif defined(__clang__)
#pragma float_control(pop)
#endif

#endif  // ENABLE_ACCELERATE
