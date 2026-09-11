#ifndef CLIB_FILTERS_CONVOLUTION_H
#define CLIB_FILTERS_CONVOLUTION_H

/**
 * @file convolution.h
 * @brief Uniform-partitioned overlap-add FIR convolution filter.
 *
 * Stockham-style segmented overlap-add with one 2N-point real FFT per
 * chunk and an N+1-bin spectrum-domain multiply-accumulate across the
 * segment history.
 *
 * - Uses RealFFT, which stores the same N+1 unique bins as separate
 *   specRe/specIm arrays. The flat layout (DC at index 0, Nyquist
 *   at index N, both with im == 0) lets us run the spectrum
 *   multiply through vDSP_zvmulD / vDSP_zvmaD without any DC/
 *   Nyquist special-casing.
 * - RealFFT.inverse produces length * signal. The inverse does not
 *   scale, so we pre-divide coefficients by 2 * data_length to compensate.
 * - All hot-path buffers are owned by raw UnsafeMutablePointers
 *   (AudioBuffers-style) so process(waveform:) cannot trip
 *   Swift's Array CoW path that a [PrcFmt] field would.
 */

struct filter_vtable;

/**
 * @brief Opens a new coefficient-cache build pass.
 *
 * Conv filters share their (expensive) pre-transformed coefficients through a
 * process-wide cache keyed on filter name and chunk size. Upstream scopes the
 * equivalent `ConvCoeffCache` to a single build/update pass, since reusing it
 * across passes risks handing out coefficients from a stale config.
 *
 * Call this once before building or updating a pipeline. Entries created in
 * earlier passes stay alive for as long as the filters that reference them,
 * but are never handed out again.
 */
void convolution_coeff_cache_begin_build_pass(void);

extern const struct filter_vtable g_convolution_vtable;

#endif  // CLIB_FILTERS_CONVOLUTION_H
