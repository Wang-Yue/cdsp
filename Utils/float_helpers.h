/**
 * @file float_helpers.h
 * @brief DSP helper functions and vectorized operations using float precision.
 */

#ifndef CLIB_UTILS_FLOAT_HELPERS_H
#define CLIB_UTILS_FLOAT_HELPERS_H

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#include "Audio/audio_chunk.h"

#if defined(ENABLE_ACCELERATE)
#include <Accelerate/Accelerate.h>
#elif defined(ENABLE_BLAS)
#include <cblas.h>
#include <string.h>
#else
#include <string.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/**
 * @brief Convert linear gain to dB (float).
 *
 * @param linear Linear gain value.
 * @return Value in decibels. Returns -1000.0f for zero/negative input.
 */
static inline float float_to_db(float linear) {
  if (linear <= 0.0f) return -1000.0f;
  return 20.0f * log10f(linear);
}

/**
 * @brief Find peak absolute value across the first `count` samples of the
 * buffer.
 *
 * @param buffer Input vector.
 * @param count Number of elements to process.
 * @return The peak absolute value as float, or 0.0f if count is 0.
 */
static inline float dsp_ops_peak_absolute(waveform_t buffer, size_t count) {
  if (count == 0) return 0.0f;
#if defined(ENABLE_ACCELERATE)
  double res = 0.0;
  vDSP_maxmgvD(buffer, 1, &res, count);
  return (float)res;
#elif defined(ENABLE_BLAS)
  int idx = cblas_idamax((int)count, buffer, 1);
  return (float)fabs(buffer[idx]);
#else
  float res = 0.0f;
  for (size_t i = 0; i < count; i++) {
    float val = (float)fabs(buffer[i]);
    if (val > res) res = val;
  }
  return res;
#endif
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
static inline float dsp_ops_rms(waveform_t buffer, size_t count) {
  if (count == 0) return 0.0f;
  float sum = 0.0f;
#if defined(__clang__)
#pragma clang loop vectorize(enable) interleave(enable)
#elif defined(__GNUC__)
#pragma GCC ivdep
#endif
  for (size_t i = 0; i < count; i++) {
    float f = (float)buffer[i];
    sum += f * f;
  }
  return sqrtf(sum / (float)count);
}

/**
 * @brief Multiply two float vectors element-wise.
 */
static inline void dsp_ops_float_multiply(const float* a, const float* b,
                                          float* result, size_t count) {
#if defined(ENABLE_ACCELERATE)
  vDSP_vmul(a, 1, b, 1, result, 1, count);
#else
  for (size_t i = 0; i < count; i++) {
    result[i] = a[i] * b[i];
  }
#endif
}

/**
 * @brief Multiply float vector by scalar.
 */
static inline void dsp_ops_float_scalar_multiply(const float* vector,
                                                 float scalar, float* result,
                                                 size_t count) {
#if defined(ENABLE_ACCELERATE)
  vDSP_vsmul(vector, 1, &scalar, result, 1, count);
#elif defined(ENABLE_BLAS)
  if (result == vector) {
    cblas_sscal((int)count, scalar, (float*)vector, 1);
  } else {
    memcpy(result, vector, count * sizeof(float));
    cblas_sscal((int)count, scalar, result, 1);
  }
#else
  for (size_t i = 0; i < count; i++) {
    result[i] = vector[i] * scalar;
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
  double denom = (count > 1) ? (double)(count - 1) : 1.0;
  for (size_t i = 0; i < count; i++) {
    buffer[i] = (float)(0.5 * (1.0 - cos(2.0 * M_PI * (double)i / denom)));
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
  for (size_t i = 1; i < count; i++) {
    if (buffer[i] > res) res = buffer[i];
  }
  return res;
#endif
}

/**
 * @brief Compute the absolute magnitude of separate real and imaginary arrays.
 */
static inline void dsp_ops_float_zvabs(const float* real, const float* imag,
                                       float* magnitudes, size_t count) {
#if defined(ENABLE_ACCELERATE)
  DSPSplitComplex split = {(float*)real, (float*)imag};
  vDSP_zvabs(&split, 1, magnitudes, 1, count);
#else
  for (size_t i = 0; i < count; i++) {
    float re = real[i];
    float im = imag[i];
    magnitudes[i] = sqrtf(re * re + im * im);
  }
#endif
}

/**
 * @brief Threshold a float vector in-place or into another vector.
 */
static inline void dsp_ops_float_vthr(const float* vector, float threshold,
                                      float* result, size_t count) {
#if defined(ENABLE_ACCELERATE)
  vDSP_vthr(vector, 1, &threshold, result, 1, count);
#else
  for (size_t i = 0; i < count; i++) {
    float val = vector[i];
    result[i] = val < threshold ? threshold : val;
  }
#endif
}

/**
 * @brief Convert linear amplitude values to decibels (dBFS).
 */
static inline void dsp_ops_float_vdbcon(const float* vector, float reference,
                                        float* result, size_t count) {
  float ref = reference > 0.0f ? reference : 1.0f;
#if defined(ENABLE_ACCELERATE)
  vDSP_vdbcon(vector, 1, &ref, result, 1, count, 1);
#else
  for (size_t i = 0; i < count; i++) {
    result[i] = float_to_db(vector[i] / ref);
  }
#endif
}

#endif  // CLIB_UTILS_FLOAT_HELPERS_H
