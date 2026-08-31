// Arbitrary-N complex DFT via Bluestein's chirp-z transform.
//
// References:
//   * L. I. Bluestein, "A linear filtering approach to the computation of
//     the discrete Fourier transform," NEREM Record 10, 1968.
//   * L. R. Rabiner, R. W. Schafer, C. M. Rader, "The Chirp z-Transform
//     Algorithm," IEEE Trans. Audio Electroacoust. AU-17(2):86–92, 1969.
//   * Oppenheim & Schafer, *Discrete-Time Signal Processing*, 3rd ed.,
//     §9.6 "Computation of the DFT Using the Chirp Transform Algorithm".
//
// The identity 2nk = n² + k² − (k − n)² rewrites the DFT
//   X[k] = Σₙ x[n]·exp(−2πi·nk/N)
// as the convolution
//   X[k] = exp(−iπk²/N) · Σₙ (x[n]·exp(−iπn²/N)) · exp(+iπ(k−n)²/N).
// The inner sum is the convolution of the chirp-modulated input with the
// length-(2N−1) chirp kernel b[n] = exp(+iπn²/N). We zero-pad both to the
// smallest optimal mixed-radix composite size M ≥ 2N − 1 and evaluate the
// convolution via the standard FFT-multiply-IFFT pipeline using
// `mixed_radix_fft`; the outer chirp is applied as a pointwise post-multiply.
//
// Inner FFT sizes use smooth composites (factors in {2, 3, 5, 7, 11, 13})
// rather than strict powers of two, eliminating up to 49% of unnecessary
// padding.
//
// Storage uses heap-allocated double buffers (allocated in create, freed
// in free) so the hot path runs directly on raw pointers with 0 dynamic
// memory allocations during execution.

#include "FFT/bluestein_fft.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "Config/config_error.h"
#include "FFT/mixed_radix_fft.h"
#include "Utils/double_helpers.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct bluestein_fft {
  arbitrary_complex_fft_t base;
  /// Logical DFT length.
  size_t n;
  /// Inner mixed-radix smooth FFT length, >= 2n - 1.
  size_t m;
  // Forward chirp α[k] = exp(-iπk²/N), length n. Stored as
  // (cos(πk²/N), -sin(πk²/N)).
  double* alpha_re;
  double* alpha_im;
  // Same chirp pre-scaled by 1/m, used in the post-multiply step. Folding
  // the IFFT's missing 1/m scale into α here lets us skip a separate
  // vector scaling pass per execute() call.
  double* alpha_post_re;
  double* alpha_post_im;
  // Pre-transformed b sequence (length m), used in the convolution step.
  double* b_freq_re;
  double* b_freq_im;
  // Mixed-radix FFT engine for length m.
  mixed_radix_fft_t* conv_fft;
  // Hot-path scratch (length m).
  double* a_re;
  double* a_im;
  double* a_re_f;
  double* a_im_f;
  double* p_re;
  double* p_im;
  double* c_re;
  double* c_im;
};

/**
 * @brief Static wrapper to execute the Bluestein FFT, matching the
 * arbitrary_complex_fft interface.
 *
 * @param ctx The generic context pointer (pointing to a bluestein_fft_t
 * instance).
 * @param real_in Input real component array.
 * @param imag_in Input imaginary component array.
 * @param real_out Output real component array.
 * @param imag_out Output imaginary component array.
 * @param inverse True for inverse DFT, false for forward DFT.
 */
static void bluestein_fft_execute_wrapper(void* ctx, waveform_t real_in,
                                          waveform_t imag_in,
                                          mutable_waveform_t real_out,
                                          mutable_waveform_t imag_out,
                                          bool inverse) {
  bluestein_fft_execute((bluestein_fft_t*)ctx, real_in, imag_in, real_out,
                        imag_out, inverse);
}

/**
 * @brief Static wrapper to free the Bluestein FFT context, matching the
 * arbitrary_complex_fft interface.
 *
 * @param ctx The generic context pointer (pointing to a bluestein_fft_t
 * instance).
 */
static void bluestein_fft_free_wrapper(void* ctx) {
  bluestein_fft_free((bluestein_fft_t*)ctx);
}

/**
 * @brief Checks if n is a smooth composite number supported by fast unrolled
 * mixed-radix stages (factors in {2, 3, 5, 7, 11, 13}).
 */
static inline bool is_fast_mixed_radix_smooth(size_t n) {
  while (n % 2 == 0) n /= 2;
  while (n % 3 == 0) n /= 3;
  while (n % 5 == 0) n /= 5;
  while (n % 7 == 0) n /= 7;
  while (n % 11 == 0) n /= 11;
  while (n % 13 == 0) n /= 13;
  return n == 1;
}

/**
 * @brief Finds the smallest optimal mixed-radix smooth convolution size m >=
 * min_l.
 */
static inline size_t find_optimal_bluestein_m(size_t min_l) {
  if (min_l < 16) return 16;
  size_t m = min_l;
  while (!is_fast_mixed_radix_smooth(m)) {
    m++;
  }
  return m;
}

bluestein_fft_t* bluestein_fft_create(size_t n, config_error_t* err) {
  if (n == 0) {
    config_error_set(err, CONFIG_ERR_PARSE, "BluesteinFFT: n must be positive");
    return NULL;
  }

  // Find the smallest optimal size `m` for the inner FFT.
  // The size must be at least 2n - 1 to prevent time-domain aliasing during
  // the linear convolution.
  size_t min_l = 2 * n - 1;
  size_t m = find_optimal_bluestein_m(min_l);

  mixed_radix_fft_t* conv_fft = mixed_radix_fft_create(m);
  if (!conv_fft) {
    config_error_set(
        err, CONFIG_ERR_PARSE,
        "BluesteinFFT: failed to create convolution FFT plan for size %zu", m);
    return NULL;
  }

  bluestein_fft_t* fft = (bluestein_fft_t*)calloc(1, sizeof(bluestein_fft_t));
  if (!fft) {
    config_error_set(err, CONFIG_ERR_PARSE, "Failed to allocate BluesteinFFT");
    mixed_radix_fft_free(conv_fft);
    return NULL;
  }
  fft->base.ctx = fft;
  fft->base.execute = bluestein_fft_execute_wrapper;
  fft->base.free = bluestein_fft_free_wrapper;
  fft->n = n;
  fft->m = m;
  fft->conv_fft = conv_fft;

  fft->alpha_re = (double*)calloc(n, sizeof(double));
  fft->alpha_im = (double*)calloc(n, sizeof(double));
  fft->alpha_post_re = (double*)calloc(n, sizeof(double));
  fft->alpha_post_im = (double*)calloc(n, sizeof(double));
  fft->b_freq_re = (double*)calloc(m, sizeof(double));
  fft->b_freq_im = (double*)calloc(m, sizeof(double));
  fft->a_re = (double*)calloc(m, sizeof(double));
  fft->a_im = (double*)calloc(m, sizeof(double));
  fft->a_re_f = (double*)calloc(m, sizeof(double));
  fft->a_im_f = (double*)calloc(m, sizeof(double));
  fft->p_re = (double*)calloc(m, sizeof(double));
  fft->p_im = (double*)calloc(m, sizeof(double));
  fft->c_re = (double*)calloc(m, sizeof(double));
  fft->c_im = (double*)calloc(m, sizeof(double));

  if (!fft->alpha_re || !fft->alpha_im || !fft->alpha_post_re ||
      !fft->alpha_post_im || !fft->b_freq_re || !fft->b_freq_im || !fft->a_re ||
      !fft->a_im || !fft->a_re_f || !fft->a_im_f || !fft->p_re || !fft->p_im ||
      !fft->c_re || !fft->c_im) {
    config_error_set(err, CONFIG_ERR_PARSE,
                     "Failed to allocate BluesteinFFT scratch buffers");
    bluestein_fft_free(fft);
    return NULL;
  }

  double inv_md = 1.0 / (double)m;
  // Initialise the outer chirp α[k] = exp(-iπk²/N) (Rabiner-Schafer-Rader
  // 1969, eq. 8). The `(k*k) % (2*n)` reduction keeps the trig argument
  // bounded so cos/sin retain full precision for large N. `alphaPost`
  // stores the same chirp scaled by 1/M to absorb the IFFT normalisation
  // into the post-multiply step.
  for (size_t k = 0; k < n; k++) {
    double theta = M_PI * (double)((k * k) % (2 * n)) / (double)n;
    double c = cos(theta);
    double s = -sin(theta);
    fft->alpha_re[k] = c;
    fft->alpha_im[k] = s;
    fft->alpha_post_re[k] = c * inv_md;
    fft->alpha_post_im[k] = s * inv_md;
  }

  double* b_re = (double*)calloc(m, sizeof(double));
  double* b_im = (double*)calloc(m, sizeof(double));
  if (!b_re || !b_im) {
    config_error_set(err, CONFIG_ERR_PARSE,
                     "Failed to allocate BluesteinFFT kernel buffers");
    free(b_re);
    free(b_im);
    bluestein_fft_free(fft);
    return NULL;
  }
  // Build the chirp kernel b[k] = exp(+iπk²/N), zero-padded and
  // periodically extended to length M so that the M-point cyclic
  // convolution computes the desired linear convolution over the
  // valid range k ∈ [0, n). Per Oppenheim & Schafer §9.6:
  //   b[0]   = 1
  //   b[k]   = exp(+iπk²/N)            for k = 1..n-1
  //   b[m-k] = b[k]                    (mirrored copy at the wrap)
  //   b[k]   = 0                       elsewhere
  // We FFT this kernel once at setup; the hot path multiplies the
  // input's spectrum by it pointwise.
  b_re[0] = 1.0;
  for (size_t k = 1; k < n; k++) {
    double theta = M_PI * (double)((k * k) % (2 * n)) / (double)n;
    double c = cos(theta);
    double s = sin(theta);
    b_re[k] = c;
    b_im[k] = s;
    b_re[m - k] = c;
    b_im[m - k] = s;
  }
  mixed_radix_fft_execute(fft->conv_fft, b_re, b_im, fft->b_freq_re,
                          fft->b_freq_im, false);
  free(b_re);
  free(b_im);

  return fft;
}

void bluestein_fft_execute(bluestein_fft_t* fft, waveform_t real_in,
                           waveform_t imag_in, mutable_waveform_t real_out,
                           mutable_waveform_t imag_out, bool inverse) {
  if (!fft) return;
  size_t n = fft->n;
  size_t m = fft->m;

  // Step 1: Pre-multiply with chirp alpha: a[k] = alpha[k] * x[k] (or conj(x)
  // for inverse)
  double conj_sign = inverse ? -1.0 : 1.0;
  for (size_t k = 0; k < n; k++) {
    double xr = real_in[k];
    double xi = imag_in[k] * conj_sign;
    double ar = fft->alpha_re[k];
    double ai = fft->alpha_im[k];
    fft->a_re[k] = xr * ar - xi * ai;
    fft->a_im[k] = xr * ai + xi * ar;
  }
  if (m > n) {
    memset(fft->a_re + n, 0, (m - n) * sizeof(double));
    memset(fft->a_im + n, 0, (m - n) * sizeof(double));
  }

  // Step 2: Cyclic convolution via mixed_radix_fft:
  // Forward FFT
  mixed_radix_fft_execute(fft->conv_fft, fft->a_re, fft->a_im, fft->a_re_f,
                          fft->a_im_f, false);

  // Pointwise complex multiply: P = A * B
  for (size_t i = 0; i < m; i++) {
    double ar = fft->a_re_f[i], ai = fft->a_im_f[i];
    double br = fft->b_freq_re[i], bi = fft->b_freq_im[i];
    fft->p_re[i] = ar * br - ai * bi;
    fft->p_im[i] = ar * bi + ai * br;
  }

  // Inverse FFT
  mixed_radix_fft_execute(fft->conv_fft, fft->p_re, fft->p_im, fft->c_re,
                          fft->c_im, true);

  // Step 3: Post-multiply by scaled chirp alpha_post: out = alpha_post * c
  for (size_t k = 0; k < n; k++) {
    double cr = fft->c_re[k];
    double ci = fft->c_im[k];
    double apr = fft->alpha_post_re[k];
    double api = fft->alpha_post_im[k];
    real_out[k] = cr * apr - ci * api;
    double im_val = cr * api + ci * apr;
    imag_out[k] = inverse ? -im_val : im_val;
  }
}

void bluestein_fft_free(bluestein_fft_t* fft) {
  if (!fft) return;
  if (fft->conv_fft) mixed_radix_fft_free(fft->conv_fft);
  free(fft->alpha_re);
  free(fft->alpha_im);
  free(fft->alpha_post_re);
  free(fft->alpha_post_im);
  free(fft->b_freq_re);
  free(fft->b_freq_im);
  free(fft->a_re);
  free(fft->a_im);
  free(fft->a_re_f);
  free(fft->a_im_f);
  free(fft->p_re);
  free(fft->p_im);
  free(fft->c_re);
  free(fft->c_im);
  free(fft);
}
