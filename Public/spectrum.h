#ifndef CDSP_PUBLIC_SPECTRUM_H
#define CDSP_PUBLIC_SPECTRUM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cdsp_export.h"
#include "cdsp_pub_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Spectrum data structure containing frequency and magnitude arrays.
 *
 * The `frequencies` and `magnitudes` arrays are allocated by the caller (each
 * with size >= `n_bins` floats).
 */
typedef struct {
  float* frequencies; /**< Center frequencies of output bins in Hz
                         (caller-allocated). */
  float* magnitudes;  /**< Peak magnitude in dBFS (caller-allocated). */
  size_t count;       /**< Actual number of bins populated. */
} cdsp_spectrum_t;

/**
 * @brief Compute a frequency spectrum snapshot from the running audio pipeline.
 *
 * The caller allocates the arrays in `out_spec` (`frequencies` and
 * `magnitudes`) with at least `n_bins` elements.
 *
 * @param engine Pointer to the engine.
 * @param side Side to analyze (capture or playback).
 * @param channel Optional zero-based channel index to analyze (NULL for
 * all/default).
 * @param min_freq Lower frequency edge in Hz.
 * @param max_freq Upper frequency edge in Hz.
 * @param n_bins Requested number of frequency bins (>= 2).
 * @param out_spec Pointer to the caller-allocated spectrum structure.
 * @return true on success, false on failure (e.g. processing not running).
 */
CDSP_API bool cdsp_get_spectrum(dsp_engine_t* engine, cdsp_spectrum_side_t side,
                                const uint32_t* channel, float min_freq,
                                float max_freq, size_t n_bins,
                                cdsp_spectrum_t* out_spec);

#ifdef __cplusplus
}
#endif

#endif  // CDSP_PUBLIC_SPECTRUM_H
