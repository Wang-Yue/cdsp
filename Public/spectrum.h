#ifndef CDSP_PUBLIC_SPECTRUM_H
#define CDSP_PUBLIC_SPECTRUM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cdsp_pub_types.h"

/**
 * @brief Spectrum data structure containing frequency and magnitude arrays.
 */
typedef struct {
  double* frequencies; /**< Center frequencies of output bins in Hz. */
  double* magnitudes;  /**< Peak magnitude in dBFS. */
  size_t count;        /**< Number of bins. */
} cdsp_spectrum_t;

/**
 * @brief Compute a frequency spectrum snapshot from the running audio pipeline.
 *
 * @param engine Pointer to the engine.
 * @param is_capture true to analyze the capture side, false for playback side.
 * @param channel Zero-based channel index to analyze.
 * @param min_freq Lower frequency edge in Hz.
 * @param max_freq Upper frequency edge in Hz.
 * @param n_bins Requested number of frequency bins (>= 2).
 * @param out_spec Pointer to write the allocated spectrum data to.
 * @return true on success, false on failure (e.g. processing not running).
 */
bool cdsp_get_spectrum(dsp_engine_t* engine, bool is_capture, uint32_t channel,
                       double min_freq, double max_freq, size_t n_bins,
                       cdsp_spectrum_t* out_spec);

/**
 * @brief Free the dynamically allocated spectrum data.
 * @param spec Pointer to the spectrum data structure.
 */
void cdsp_free_spectrum(cdsp_spectrum_t* spec);

#endif  // CDSP_PUBLIC_SPECTRUM_H
