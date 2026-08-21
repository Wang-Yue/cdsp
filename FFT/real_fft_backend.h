#ifndef CLIB_FFT_REALFFT_BACKEND_H
#define CLIB_FFT_REALFFT_BACKEND_H

/**
 * @file real_fft_backend.h
 * @brief Internal backend interface for real-input FFT implementations.
 *
 * This header defines the polymorphic dispatch interface used by RealFFT
 * implementations (vDSP, FFTW, ComplexInner, Software Fallback).
 * It is intended for internal FFT subsystem use and not exposed to public consumers.
 */

#include <stddef.h>

#include "Utils/double_helpers.h"

// MARK: - Double-Precision Backend Interface

/**
 * @brief Function pointer type for double-precision forward real FFT execution.
 */
typedef void (*real_fft_backend_forward_fn)(void* ctx, waveform_t real_in,
                                            mutable_waveform_t spec_re,
                                            mutable_waveform_t spec_im);

/**
 * @brief Function pointer type for double-precision inverse real FFT execution.
 */
typedef void (*real_fft_backend_inverse_fn)(void* ctx, waveform_t spec_re,
                                            waveform_t spec_im,
                                            mutable_waveform_t real_out);

/**
 * @brief Function pointer type for freeing double-precision backend context.
 */
typedef void (*real_fft_backend_free_fn)(void* ctx);

/**
 * @struct real_fft_backend
 * @brief Structure defining the dispatch table for a double-precision real FFT backend.
 */
typedef struct {
  void* ctx;                            /**< Opaque backend context pointer. */
  real_fft_backend_forward_fn forward; /**< Forward FFT function. */
  real_fft_backend_inverse_fn inverse; /**< Inverse FFT function. */
  real_fft_backend_free_fn free;       /**< Cleanup function. */
} real_fft_backend_t;

// MARK: - Single-Precision (Float) Backend Interface

/**
 * @brief Function pointer type for single-precision forward real FFT execution.
 */
typedef void (*real_fftf_backend_forward_fn)(void* ctx, const float* real_in,
                                             float* spec_re, float* spec_im);

/**
 * @brief Function pointer type for single-precision inverse real FFT execution.
 */
typedef void (*real_fftf_backend_inverse_fn)(void* ctx, const float* spec_re,
                                             const float* spec_im,
                                             float* real_out);

/**
 * @brief Function pointer type for freeing single-precision backend context.
 */
typedef void (*real_fftf_backend_free_fn)(void* ctx);

/**
 * @struct real_fftf_backend
 * @brief Structure defining the dispatch table for a single-precision real FFT backend.
 */
typedef struct {
  void* ctx;                             /**< Opaque backend context pointer. */
  real_fftf_backend_forward_fn forward; /**< Forward FFT function. */
  real_fftf_backend_inverse_fn inverse; /**< Inverse FFT function. */
  real_fftf_backend_free_fn free;       /**< Cleanup function. */
} real_fftf_backend_t;

#endif  // CLIB_FFT_REALFFT_BACKEND_H
