#ifndef CLIB_FFT_MIXEDRADIXFFT_H
#define CLIB_FFT_MIXEDRADIXFFT_H

/**
 * @file mixed_radix_fft.h
 * @brief Arbitrary-N complex DFT via iterative Cooley-Tukey and Rader's
 * mixed-radix FFT.
 *
 * Architecture:
 *   - Specialized fast unrolled kernels:
 *       * Powers of 2: Radix-8, Radix-4, Radix-2
 *       * Powers of 3: Radix-9, Radix-3
 *       * Small Primes: Radix-5, Radix-7, Radix-11, Radix-13
 *   - Rader's FFT Algorithm for arbitrary primes p > 13: converts length-p
 *     prime DFT stages into length-(p-1) cyclic convolutions computed via
 *     internal power-of-2 FFTs in O(p log p) time.
 *
 * All buffers (twiddles, permutation LUT, scratch) are heap-allocated at
 * init and freed in deinit. The hot path runs purely on raw pointers with
 * 0 dynamic memory allocations during execution.
 */

#include <stdbool.h>
#include <stddef.h>

#include "FFT/arbitrary_complex_fft.h"
#include "Utils/double_helpers.h"

/**
 * @struct mixed_radix_fft
 * @brief Opaque structure representing a mixed-radix complex FFT context.
 */
typedef struct mixed_radix_fft mixed_radix_fft_t;

/**
 * @brief Creates a mixed-radix complex FFT context for length N.
 *
 * Supports arbitrary positive integers N > 0.
 *
 * @param n The transform length.
 * @return A pointer to the created mixed_radix_fft_t context, or NULL on
 * allocation failure.
 */
mixed_radix_fft_t* mixed_radix_fft_create(size_t n);

/**
 * @brief Computes the N-point complex DFT.
 *
 * @param fft The mixed-radix FFT context.
 * @param real_in Input buffer for the real parts of the signal.
 * @param imag_in Input buffer for the imaginary parts of the signal.
 * @param real_out Output buffer for the real parts of the result.
 * @param imag_out Output buffer for the imaginary parts of the result.
 * @param inverse false for unnormalised forward transform, true for
 * unnormalised inverse.
 */
void mixed_radix_fft_execute(mixed_radix_fft_t* fft, waveform_t real_in,
                             waveform_t imag_in, mutable_waveform_t real_out,
                             mutable_waveform_t imag_out, bool inverse);

/**
 * @brief Frees the mixed-radix FFT context.
 *
 * @param fft The context to destroy.
 */
void mixed_radix_fft_free(mixed_radix_fft_t* fft);

/**
 * @brief Casts the mixed-radix FFT context to a generic arbitrary complex FFT
 * context.
 *
 * @param fft The mixed-radix FFT context.
 * @return A pointer to the arbitrary_complex_fft_t context.
 */
static inline arbitrary_complex_fft_t* mixed_radix_fft_as_arbitrary(
    mixed_radix_fft_t* fft) {
  return (arbitrary_complex_fft_t*)fft;
}

#endif  // CLIB_FFT_MIXEDRADIXFFT_H
