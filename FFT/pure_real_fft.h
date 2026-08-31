#ifndef CLIB_FFT_PUREREALFFT_H
#define CLIB_FFT_PUREREALFFT_H

/**
 * @file pure_real_fft.h
 * @brief High-performance Pure-C Power-of-2 Real FFT implementation.
 *
 * Provides an autovectorized, cache-friendly Real FFT for power-of-2 lengths (>= 8)
 * using unrolled Radix-8, Radix-4, and Radix-2 Cooley-Tukey kernels and
 * simultaneous conjugate-symmetric real-spectrum untwiddling.
 *
 * Matches the semantics of vDSP_fft_zripD and real_fft exactly:
 * - Forward: unscaled DFT (DC at index 0, Nyquist at index length/2).
 * - Inverse: unnormalized IDFT (scales signal by length).
 */

#include <stdbool.h>
#include <stddef.h>

#include "FFT/real_fft.h"
#include "FFT/real_fft_backend.h"
#include "Utils/double_helpers.h"

#ifdef __cplusplus
extern "C" {
#endif

// MARK: - Double-Precision (f64)

typedef struct pure_real_fft pure_real_fft_t;

/**
 * @brief Create a Pure-C Real FFT context for power-of-2 lengths >= 8.
 *
 * @param length FFT length (must be a power of 2 >= 8).
 * @return Context pointer, or NULL on error.
 */
pure_real_fft_t* pure_real_fft_create(size_t length);

/**
 * @brief Forward Real FFT: length real samples -> (length/2 + 1) complex bins.
 */
void pure_real_fft_forward(pure_real_fft_t* fft, waveform_t real_in,
                           mutable_waveform_t spec_re,
                           mutable_waveform_t spec_im);

/**
 * @brief Inverse Real FFT: (length/2 + 1) complex bins -> length real samples.
 */
void pure_real_fft_inverse(pure_real_fft_t* fft, waveform_t spec_re,
                           waveform_t spec_im, mutable_waveform_t real_out);

/**
 * @brief Free context memory.
 */
void pure_real_fft_free(pure_real_fft_t* fft);

/**
 * @brief Return dispatch backend pointer.
 */
real_fft_backend_t* pure_real_fft_as_backend(pure_real_fft_t* fft);

// MARK: - Single-Precision (f32)

typedef struct pure_real_fftf pure_real_fftf_t;

/**
 * @brief Create a single-precision Pure-C Real FFT context for power-of-2 lengths >= 8.
 */
pure_real_fftf_t* pure_real_fftf_create(size_t length);

/**
 * @brief Single-precision Forward Real FFT.
 */
void pure_real_fftf_forward(pure_real_fftf_t* fft, const float* real_in,
                            float* spec_re, float* spec_im);

/**
 * @brief Single-precision Inverse Real FFT.
 */
void pure_real_fftf_inverse(pure_real_fftf_t* fft, const float* spec_re,
                            const float* spec_im, float* real_out);

/**
 * @brief Free single-precision context memory.
 */
void pure_real_fftf_free(pure_real_fftf_t* fft);

/**
 * @brief Return single-precision dispatch backend pointer.
 */
real_fftf_backend_t* pure_real_fftf_as_backend(pure_real_fftf_t* fft);

#ifdef __cplusplus
}
#endif

#endif  // CLIB_FFT_PUREREALFFT_H
