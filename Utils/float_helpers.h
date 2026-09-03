/**
 * @file float_helpers.h
 * @brief DSP helper functions and vectorized operations using float precision.
 */

#ifndef CLIB_UTILS_FLOAT_HELPERS_H
#define CLIB_UTILS_FLOAT_HELPERS_H

#include <complex.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
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

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/**
 * @brief Convert linear gain to dB (float).
 *
 * @param linear Linear gain value.
 * @return Value in decibels.
 */
static inline float float_to_db(float linear) { return 20.0f * log10f(linear); }

/**
 * @brief Convert dB to linear gain (float).
 *
 * @param db Value in decibels.
 * @return Linear gain.
 */
static inline float float_from_db(float db) { return powf(10.0f, db / 20.0f); }

/**
 * @brief Find peak absolute value across the first `count` samples of the
 * buffer.
 *
 * @param buffer Input vector.
 * @param count Number of elements to process.
 * @return The peak absolute value as float, or 0.0f if count is 0.
 */

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC push_options
#pragma GCC optimize("finite-math-only")
#elif defined(__clang__)
#pragma float_control(precise, off, push)
#endif

ALWAYS_INLINE float dsp_ops_peak_absolute(const double* buffer, size_t count) {
  if (count == 0) return 0.0f;
  float res = 0.0f;
  PRAGMA_VECTORIZE_LOOP
  for (size_t i = 0; i < count; i++) {
    float val = (float)fabs(buffer[i]);
    res = fmaxf(val, res);
  }
  return res;
}

/**
 * @brief Compute root-mean-square over the first `count` samples of the buffer.
 *
 * Uses vectorization pragmas to achieve fast single-pass float accumulation
 * without intermediate buffer allocation.
 *
 * @param buffer Input vector.
 * @param count Number of elements to process.
 * @return The RMS value as float, or 0.0f if count is 0.
 */
ALWAYS_INLINE float dsp_ops_rms(const double* buffer, size_t count) {
  if (count == 0) return 0.0f;
  float sum = 0.0f;
  PRAGMA_VECTORIZE_LOOP
  for (size_t i = 0; i < count; i++) {
    float f = (float)buffer[i];
    sum += f * f;
  }
  return sqrtf(sum / (float)count);
}

/**
 * @brief Convert double-precision array to single-precision float array.
 *
 * @param src Input double array.
 * @param dst Output float array.
 * @param count Number of elements to convert.
 */
ALWAYS_INLINE void dsp_ops_double_to_float(const double* src, float* dst,
                                           size_t count) {
  PRAGMA_VECTORIZE_LOOP
  for (size_t i = 0; i < count; i++) {
    dst[i] = (float)src[i];
  }
}

/**
 * @brief Add float vector `src` to `dst` in-place.
 *
 * Computes: `dst[i] += src[i]` for `i < count`.
 *
 * @param src Input float vector.
 * @param dst Destination float vector (modified in-place).
 * @param count Number of elements to process.
 */
ALWAYS_INLINE void dsp_ops_float_add(const float* src, float* dst,
                                     size_t count) {
#if defined(ENABLE_ACCELERATE)
  vDSP_vadd(dst, 1, src, 1, dst, 1, count);
#else
  PRAGMA_VECTORIZE_LOOP
  for (size_t i = 0; i < count; i++) {
    dst[i] += src[i];
  }
#endif
}

/**
 * @brief Multiply two float vectors element-wise.
 */
ALWAYS_INLINE void dsp_ops_float_multiply(const float* a, const float* b,
                                          float* result, size_t count) {
#if defined(ENABLE_ACCELERATE)
  vDSP_vmul(a, 1, b, 1, result, 1, count);
#else
  PRAGMA_VECTORIZE_LOOP
  for (size_t i = 0; i < count; i++) {
    result[i] = a[i] * b[i];
  }
#endif
}

/**
 * @brief Multiply float vector by scalar (in-place).
 *
 * Computes: `buffer[i] *= scalar` for `i < count`.
 *
 * @param buffer The buffer to multiply (in-place).
 * @param scalar The scalar multiplier.
 * @param count Number of elements to process.
 */
ALWAYS_INLINE void dsp_ops_float_scalar_multiply(float* buffer, float scalar,
                                                 size_t count) {
#if defined(ENABLE_ACCELERATE)
  cblas_sscal((int)count, scalar, buffer, 1);
#else
  PRAGMA_VECTORIZE_LOOP
  for (size_t i = 0; i < count; i++) {
    buffer[i] *= scalar;
  }
#endif
}

/**
 * @brief Generate a Hann window.
 */
static inline void dsp_ops_float_hann_window(float* buffer, size_t count) {
#if defined(ENABLE_ACCELERATE)
  vDSP_hann_window(buffer, (vDSP_Length)count, 0);
#else
  if (count == 0) return;
  if (count == 1) {
    buffer[0] = 0.0f;
    return;
  }
  float delta = (float)(M_PI / (double)count);
  float c[4], s[4];
  for (int k = 0; k < 4; k++) {
    c[k] = cosf((float)k * delta);
    s[k] = sinf((float)k * delta);
  }
  float delta4 = 4.0f * delta;
  float s_half4 = sinf(0.5f * delta4);
  float alpha = 2.0f * s_half4 * s_half4;
  float beta = sinf(delta4);

  size_t half = count / 2;
  size_t i = 0;
  for (; i + 3 <= half; i += 4) {
    float v0 = s[0] * s[0];
    float v1 = s[1] * s[1];
    float v2 = s[2] * s[2];
    float v3 = s[3] * s[3];
    buffer[i + 0] = v0;
    if (i + 0 > 0) buffer[count - (i + 0)] = v0;
    buffer[i + 1] = v1;
    buffer[count - (i + 1)] = v1;
    buffer[i + 2] = v2;
    buffer[count - (i + 2)] = v2;
    buffer[i + 3] = v3;
    buffer[count - (i + 3)] = v3;

    for (int k = 0; k < 4; k++) {
      float c_next = c[k] - (alpha * c[k] + beta * s[k]);
      float s_next = s[k] - (alpha * s[k] - beta * c[k]);
      c[k] = c_next;
      s[k] = s_next;
    }
  }
  for (; i <= half; i++) {
    float val = s[0] * s[0];
    buffer[i] = val;
    if (i > 0 && count > i) buffer[count - i] = val;
    float c_next = c[0] - (alpha * c[0] + beta * s[0]);
    float s_next = s[0] - (alpha * s[0] - beta * c[0]);
    c[0] = c_next;
    s[0] = s_next;
  }
#endif
}

/**
 * @brief Find the maximum value in a float vector.
 */
static inline float dsp_ops_float_max(const float* buffer, size_t count) {
  if (count == 0) return -200.0f;
#if defined(ENABLE_ACCELERATE)
  float res = 0.0f;
  vDSP_maxv(buffer, 1, &res, count);
  return res;
#else
  float res = buffer[0];
  PRAGMA_VECTORIZE_LOOP
  for (size_t i = 1; i < count; i++) {
    res = fmaxf(buffer[i], res);
  }
  return res;
#endif
}

/**
 * @brief Compute the absolute magnitude of separate real and imaginary arrays.
 */
static inline void dsp_ops_float_zvabs(const float* real, const float* imag,
                                       float* magnitudes, size_t count) {
  PRAGMA_VECTORIZE_LOOP
  for (size_t i = 0; i < count; i++) {
    float re = real[i];
    float im = imag[i];
    magnitudes[i] = sqrtf(re * re + im * im);
  }
}

/**
 * @brief Compute the absolute magnitude of an interleaved float complex array.
 *
 * @param spec Interleaved complex input vector (length count).
 * @param magnitudes Output float array (length count).
 * @param count Number of complex elements.
 */
static inline void dsp_ops_float_complex_abs(const float complex* spec,
                                             float* magnitudes, size_t count) {
  const float* ptr = (const float*)spec;
  PRAGMA_VECTORIZE_LOOP
  for (size_t i = 0; i < count; i++) {
    float re = ptr[2 * i];
    float im = ptr[2 * i + 1];
    magnitudes[i] = sqrtf(re * re + im * im);
  }
}

/**
 * @brief Convert linear amplitude values to decibels (dBFS).
 */
static inline void dsp_ops_float_vdbcon(const float* vector, float reference,
                                        float* result, size_t count) {
#if defined(ENABLE_ACCELERATE)
  vDSP_vdbcon(vector, 1, &reference, result, 1, count, 1);
#else
  const float inv_ref = 1.0f / reference;
  PRAGMA_VECTORIZE_LOOP
  for (size_t i = 0; i < count; i++) {
    result[i] = float_to_db(vector[i] * inv_ref);
  }
#endif
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC pop_options
#elif defined(__clang__)
#pragma float_control(pop)
#endif

#endif  // CLIB_UTILS_FLOAT_HELPERS_H
