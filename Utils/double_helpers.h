/**
 * @file double_helpers.h
 * @brief DSP helper functions and vectorized operations using double precision.
 *
 * Default is Double (f64). Change to Float for 32-bit processing.
 */

#ifndef CLIB_UTILS_DOUBLE_HELPERS_H
#define CLIB_UTILS_DOUBLE_HELPERS_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#if defined(ENABLE_ACCELERATE)
#ifndef ACCELERATE_NEW_LAPACK
#define ACCELERATE_NEW_LAPACK
#endif
#include <Accelerate/Accelerate.h>
#endif
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
#define ALWAYS_INLINE __attribute__((always_inline)) static inline
#else
#define ALWAYS_INLINE static inline
#endif

#if defined(__clang__)
#define PRAGMA_VECTORIZE_LOOP \
  _Pragma("clang loop vectorize(assume_safety) interleave(enable)")
#elif defined(__GNUC__)
#define PRAGMA_VECTORIZE_LOOP _Pragma("GCC ivdep")
#elif defined(_MSC_VER)
#define PRAGMA_VECTORIZE_LOOP __pragma(loop(ivdep))
#else
#define PRAGMA_VECTORIZE_LOOP
#endif

/**
 * @typedef mutable_waveform_t
 * @brief A high-performance descriptive view of a single channel's mutable
 * buffer pointer.
 */
typedef double* mutable_waveform_t;

/**
 * @typedef waveform_t
 * @brief A high-performance descriptive view of a single channel's buffer
 * pointer.
 */
typedef const double* waveform_t;

/**
 * @brief Convert dB to linear gain.
 *
 * @param db Value in decibels.
 * @return Linear gain.
 */
static inline double double_from_db(double db) { return pow(10.0, db / 20.0); }

/**
 * @brief Convert linear gain to dB.
 *
 * @param linear Linear gain value.
 * @return Value in decibels.
 */
static inline double double_to_db(double linear) {
  return 20.0 * log10(linear);
}

/**
 * @brief Apply attack/release envelope smoothing to an input signal.
 *
 * @param input The current input value.
 * @param prev The smoothed value from the previous step.
 * @param attack_coeff The attack time constant coefficient.
 * @param release_coeff The release time constant coefficient.
 * @return The smoothed envelope value.
 */
static inline double double_smooth_envelope(double input, double prev,
                                            double attack_coeff,
                                            double release_coeff) {
  if (input >= prev) {
    return attack_coeff * prev + (1.0 - attack_coeff) * input;
  } else {
    return release_coeff * prev + (1.0 - release_coeff) * input;
  }
}

/**
 * @brief Computes modified Bessel function I0(x) using power series.
 *
 * Used for Kaiser window calculation.
 *
 * @param x Input value.
 * @return Value of I0(x).
 */
static inline double double_bessel_i0(double x) {
  double sum = 1.0;
  double denominator = 1.0;
  double i = 1.0;
  while (i < 25.0) {
    denominator *= i;
    double term = pow(x / 2.0, i) / denominator;
    sum += term * term;
    i += 1.0;
  }
  return sum;
}

// Vectorized DSP operations using Apple Accelerate (vDSP) or fallback C loops.
//
// The partial-count ops (add, multiply, multiply_add) need to
// operate on the first count elements of buffers that may be longer
// (chunks have a valid_frames <= frames).
//
// In C, these accept pointers directly so callers holding stable pointers
// (e.g. an audio_buffers_t channel view) avoid any copy overhead or
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC push_options
#pragma GCC optimize("O3,fast-math,finite-math-only")
#elif defined(__clang__)
#pragma float_control(precise, off, push)
#endif

/**
 * @brief Multiply vector by scalar in-place.
 *
 * Computes: `buffer[i] *= scalar` for `i < count`.
 *
 * @param buffer The buffer to multiply (in-place).
 * @param scalar The scalar multiplier.
 * @param count Number of elements to process.
 */
ALWAYS_INLINE void dsp_ops_scalar_multiply(double* buffer, double scalar,
                                           size_t count) {
#if defined(ENABLE_ACCELERATE)
  cblas_dscal((int)count, scalar, buffer, 1);
#else
  PRAGMA_VECTORIZE_LOOP
  for (size_t i = 0; i < count; i++) {
    buffer[i] *= scalar;
  }
#endif
}

/**
 * @brief Zero `count` samples in-place.
 *
 * @param buffer The buffer to clear.
 * @param count Number of elements to clear.
 */
ALWAYS_INLINE void dsp_ops_clear(double* buffer, size_t count) {
  memset(buffer, 0, count * sizeof(double));
}

/**
 * @brief Add vector `a` to vector `b` and write to `out`.
 *
 * Computes: `out[i] = a[i] + b[i]` for `i < count`.
 *
 * @param a First input vector.
 * @param b Second input vector.
 * @param out Destination vector.
 * @param count Number of elements to process.
 */
ALWAYS_INLINE void dsp_ops_vector_add(const double* a, const double* b,
                                      double* out, size_t count) {
#if defined(ENABLE_ACCELERATE)
  vDSP_vaddD(a, 1, b, 1, out, 1, count);
#else
  PRAGMA_VECTORIZE_LOOP
  for (size_t i = 0; i < count; i++) {
    out[i] = a[i] + b[i];
  }
#endif
}

/**
 * @brief Add vector `a` to vector `b` in-place (on `b`).
 *
 * Computes: `b[i] += a[i]` for `i < count`.
 * Must satisfy `count <= capacity(a)` and `count <= capacity(b)`.
 *
 * @param a Input vector.
 * @param b Destination vector (modified in-place).
 * @param count Number of elements to process.
 */
ALWAYS_INLINE void dsp_ops_add(const double* a, double* b, size_t count) {
#if defined(ENABLE_ACCELERATE)
  cblas_daxpy((int)count, 1.0, a, 1, b, 1);
#else
  dsp_ops_vector_add(a, b, b, count);
#endif
}

/**
 * @brief Multiply two vectors element-wise.
 *
 * Computes: `b[i] *= a[i]` for `i < count` (in-place on `b`).
 *
 * @param a Input vector.
 * @param b Destination vector (modified in-place).
 * @param count Number of elements to process.
 */
ALWAYS_INLINE void dsp_ops_multiply(const double* a, double* b, size_t count) {
  PRAGMA_VECTORIZE_LOOP
  for (size_t i = 0; i < count; i++) {
    b[i] *= a[i];
  }
}

/**
 * @brief Multiply-accumulate: `accumulator[i] += a[i] * scalar` for `i <
 * count`.
 *
 * @param a Input vector.
 * @param scalar The scalar multiplier.
 * @param accumulator The accumulator vector (modified in-place).
 * @param count Number of elements to process.
 */
ALWAYS_INLINE void dsp_ops_multiply_add(const double* a, double scalar,
                                        double* accumulator, size_t count) {
#if defined(ENABLE_ACCELERATE)
  cblas_daxpy((int)count, scalar, a, 1, accumulator, 1);
#else
  PRAGMA_VECTORIZE_LOOP
  for (size_t i = 0; i < count; i++) {
    accumulator[i] += a[i] * scalar;
  }
#endif
}

/**
 * @brief Computes the dot product of two vectors (e.g. wave buffer and sinc
 * kernel).
 *
 * @param a First input vector.
 * @param b Second input vector.
 * @param count Number of elements to process.
 * @return The dot product (accumulated sum).
 */
ALWAYS_INLINE double sinc_dot_product(const double* a, const double* b,
                                      size_t count) {
  // WARNING: Do not use vDSP functions in this function as sinc_dot_product is
  // called with tiny count. calling function makes it perform slower.
#if defined(__clang__)
  double sum = 0.0;
  PRAGMA_VECTORIZE_LOOP
  for (size_t i = 0; i < count; i++) {
    sum += a[i] * b[i];
  }
  return sum;
#else
  double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0;
  double s4 = 0.0, s5 = 0.0, s6 = 0.0, s7 = 0.0;
  double s8 = 0.0, s9 = 0.0, s10 = 0.0, s11 = 0.0;
  double s12 = 0.0, s13 = 0.0, s14 = 0.0, s15 = 0.0;
  size_t i = 0;
  PRAGMA_VECTORIZE_LOOP
  for (; i + 15 < count; i += 16) {
    s0 += a[i] * b[i];
    s1 += a[i + 1] * b[i + 1];
    s2 += a[i + 2] * b[i + 2];
    s3 += a[i + 3] * b[i + 3];
    s4 += a[i + 4] * b[i + 4];
    s5 += a[i + 5] * b[i + 5];
    s6 += a[i + 6] * b[i + 6];
    s7 += a[i + 7] * b[i + 7];
    s8 += a[i + 8] * b[i + 8];
    s9 += a[i + 9] * b[i + 9];
    s10 += a[i + 10] * b[i + 10];
    s11 += a[i + 11] * b[i + 11];
    s12 += a[i + 12] * b[i + 12];
    s13 += a[i + 13] * b[i + 13];
    s14 += a[i + 14] * b[i + 14];
    s15 += a[i + 15] * b[i + 15];
  }

  double sum = ((s0 + s1) + (s2 + s3)) + ((s4 + s5) + (s6 + s7)) +
               ((s8 + s9) + (s10 + s11)) + ((s12 + s13) + (s14 + s15));

  for (; i < count; i++) {
    sum += a[i] * b[i];
  }
  return sum;
#endif
}

/**
 * @brief Clip values in vector to [low, high] in-place.
 *
 * @param buffer Input/output vector.
 * @param low Lower clipping limit.
 * @param high Upper clipping limit.
 * @param count Number of elements to process.
 */
ALWAYS_INLINE void dsp_ops_clip(double* buffer, double low, double high,
                                size_t count) {
  PRAGMA_VECTORIZE_LOOP
  for (size_t i = 0; i < count; i++) {
    double val = buffer[i];
    val = val < low ? low : val;
    val = val > high ? high : val;
    buffer[i] = val;
  }
}

/**
 * @brief Pointwise complex multiplication of split-complex vectors.
 *
 * Computes:
 *   out_re[i] = a_re[i] * b_re[i] - a_im[i] * b_im[i]
 *   out_im[i] = a_re[i] * b_im[i] + a_im[i] * b_re[i]
 *
 * @param a_re Real part of first vector.
 * @param a_im Imaginary part of first vector.
 * @param b_re Real part of second vector.
 * @param b_im Imaginary part of second vector.
 * @param out_re Output real vector (can alias a_re or b_re).
 * @param out_im Output imaginary vector (can alias a_im or b_im).
 * @param count Number of complex elements to process.
 */
ALWAYS_INLINE void dsp_ops_complex_multiply(const double* a_re,
                                            const double* a_im,
                                            const double* b_re,
                                            const double* b_im, double* out_re,
                                            double* out_im, size_t count) {
  PRAGMA_VECTORIZE_LOOP
  for (size_t i = 0; i < count; i++) {
    double re = a_re[i];
    double im = a_im[i];
    double fre = b_re[i];
    double fim = b_im[i];
    out_re[i] = re * fre - im * fim;
    out_im[i] = re * fim + im * fre;
  }
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC pop_options
#elif defined(__clang__)
#pragma float_control(pop)
#endif

#endif  // CLIB_UTILS_DOUBLE_HELPERS_H
