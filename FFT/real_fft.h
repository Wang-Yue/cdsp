#ifndef CLIB_FFT_REALFFT_H
#define CLIB_FFT_REALFFT_H

/**
 * @file real_fft.h
 * @brief Real-input FFT of arbitrary even length.
 *
 * `real_fft_create` is the **single dispatch point** for the resampler's FFT
 * subsystem — it inspects the requested length once and picks the fastest
 * available backend, so callers (and the per-backend modules) never repeat that
 * decision.
 *
 * Decision tree (top-to-bottom, first match wins):
 *   1. `length` is a power of two `≥ 8`
 *      → `vdsp_real_fft` (`vdsp_real_fft.c`), wrapping Apple's
 *      `vDSP_fft_zrip` / `vDSP_fft_zripD` (radix-2 split-complex real FFT,
 *      hand-tuned NEON on Apple Silicon).
 *   2. Otherwise (arbitrary even length): a 2N-point real FFT is built
 *      from one N-point complex FFT plus an O(N) untwiddle pass —
 *      `complex_inner_real_fft` (`complex_inner_real_fft.c`). The inner
 *      complex FFT is routed in priority order:
 *      a. `mixed_radix_fft` (`mixed_radix_fft.c`) — native mixed-radix
 *         for prime factorisations in `{2, 3, 5, 7}`. Its radix-2/4/8
 *         stages handle the *power-of-two portion* of a mixed factorisation
 *         (e.g. `1120 = 2⁵·5·7` factored as `[8, 4, 5, 7]`).
 *      b. `bluestein_fft` (`bluestein_fft.c`) — universal fallback
 *         for anything with a prime factor `> 7` (e.g. `11→13k` rate pair,
 *         halfN = 1034 has primes 11 and 47).
 *
 * Every backend exposes the same external semantics — forward =
 * unscaled DFT, inverse = `length · signal` — so the resampler is
 * oblivious to which path runs.
 *
 * Algorithm references:
 *   - https://www.dsprelated.com/showarticle/4.php (Real FFT from complex FFT)
 *   - https://en.wikipedia.org/wiki/Fast_Fourier_transform#Real-input_FFTs
 */

#include <stdbool.h>
#include <stddef.h>

#include "Config/config_error.h"
#include "Utils/double_helpers.h"

// MARK: - Double-Precision Real FFT (real_fft_t)

/**
 * @struct real_fft
 * @brief Main real FFT structure.
 */
typedef struct real_fft real_fft_t;

/**
 * @brief Get the time-domain length of the real FFT.
 * @param fft Pointer to the real FFT context.
 * @return The length.
 */
size_t real_fft_get_length(const real_fft_t* fft);

/**
 * @brief Get the spectrum length of the real FFT (number of complex bins).
 * @param fft Pointer to the real FFT context.
 * @return The spectrum length.
 */
size_t real_fft_get_spectrum_length(const real_fft_t* fft);

/**
 * @brief Creates a real FFT context for the specified length.
 *
 * This function chooses and instantiates the most appropriate backend
 * based on the requested length.
 *
 * @param length The time-domain length (must be even).
 * @param err Pointer to a config error struct to populate on failure.
 * @return A pointer to the created real_fft_t context, or NULL on failure.
 */
real_fft_t* real_fft_create(size_t length, config_error_t* err);

/**
 * @brief Computes the forward 2N-point real FFT.
 *
 * Produces the `N + 1` unique complex bins.
 *
 * @param fft The real FFT context.
 * @param real_in Input buffer of real samples (length >= fft->length).
 * @param spec_re Output buffer for the real parts of the spectrum (length >=
 * fft->spectrum_length).
 * @param spec_im Output buffer for the imaginary parts of the spectrum (length
 * >= fft->spectrum_length).
 */
void real_fft_forward(real_fft_t* fft, waveform_t real_in,
                      mutable_waveform_t spec_re, mutable_waveform_t spec_im);

/**
 * @brief Computes the inverse 2N-point real FFT.
 *
 * Reads the `N + 1` unique complex bins from `spec_re`/`spec_im` and writes
 * `length` real samples into `real_out`. Output is scaled by `length`.
 *
 * @param fft The real FFT context.
 * @param spec_re Input buffer for the real parts of the spectrum (length >=
 * fft->spectrum_length).
 * @param spec_im Input buffer for the imaginary parts of the spectrum (length
 * >= fft->spectrum_length).
 * @param real_out Output buffer for the reconstructed real samples (length >=
 * fft->length).
 */
void real_fft_inverse(real_fft_t* fft, waveform_t spec_re, waveform_t spec_im,
                      mutable_waveform_t real_out);

/**
 * @brief Frees the real FFT context and its backend.
 *
 * @param fft The context to destroy.
 */
void real_fft_free(real_fft_t* fft);

// MARK: - Single-Precision Real FFT (real_fftf_t)

/**
 * @struct real_fftf
 * @brief Single-precision (float) Real FFT context.
 */
typedef struct real_fftf real_fftf_t;

/**
 * @brief Get the time-domain length of the single-precision real FFT.
 * @param fft Pointer to the float real FFT context.
 * @return The length.
 */
size_t real_fftf_get_length(const real_fftf_t* fft);

/**
 * @brief Get the spectrum length of the single-precision real FFT.
 * @param fft Pointer to the float real FFT context.
 * @return The spectrum length.
 */
size_t real_fftf_get_spectrum_length(const real_fftf_t* fft);

/**
 * @brief Creates a single-precision real FFT context for the specified length.
 * @param length The time-domain length (must be even).
 * @return A pointer to the created real_fftf_t context, or NULL on failure.
 */
real_fftf_t* real_fftf_create(size_t length);

/**
 * @brief Computes the forward single-precision real FFT.
 * @param fft The float real FFT context.
 * @param real_in Input buffer of float real samples.
 * @param spec_re Output buffer for the real parts of the spectrum.
 * @param spec_im Output buffer for the imaginary parts of the spectrum.
 */
void real_fftf_forward(real_fftf_t* fft, const float* real_in, float* spec_re,
                       float* spec_im);

/**
 * @brief Computes the inverse single-precision real FFT.
 * @param fft The float real FFT context.
 * @param spec_re Input buffer for the real parts of the spectrum.
 * @param spec_im Input buffer for the imaginary parts of the spectrum.
 * @param real_out Output buffer for reconstructed real float samples. Output is
 * scaled by length.
 */
void real_fftf_inverse(real_fftf_t* fft, const float* spec_re,
                       const float* spec_im, float* real_out);

/**
 * @brief Frees the single-precision real FFT context and its backend.
 * @param fft The context to destroy.
 */
void real_fftf_free(real_fftf_t* fft);

#endif  // CLIB_FFT_REALFFT_H
