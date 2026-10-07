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
 * - Uses the FFTW3-backed real_fft_t, which stores the N+1 unique bins as
 *   interleaved complex_t (DC at index 0, Nyquist at index N, both with
 *   im == 0), so the spectrum multiply-accumulate runs through the
 *   NEON / AVX+FMA kernels in utils/double_helpers.h without any DC /
 *   Nyquist special-casing.
 * - real_fft_inverse produces length * signal. The inverse does not
 *   scale, so the coefficients are pre-divided by 2 * chunk_size.
 * - All hot-path buffers are preallocated (64-byte aligned) at creation;
 *   process performs no allocation.
 */

struct filter_vtable;

/**
 * @brief Opens a new coefficient-cache build pass.
 *
 * Conv filters share their (expensive) pre-transformed coefficients through a
 * process-wide cache keyed on the full filter name, the coefficient parameters
 * and chunk size. Upstream scopes the
 * equivalent `ConvCoeffCache` to a single build/update pass, since reusing it
 * across passes risks handing out coefficients from a stale config.
 *
 * Call this once before building or updating a pipeline. Entries created in
 * earlier passes stay alive for as long as the filters that reference them,
 * but are never handed out again.
 */
void convolution_coeff_cache_begin_build_pass(void);

extern const struct filter_vtable g_convolution_vtable;

#endif // CLIB_FILTERS_CONVOLUTION_H
