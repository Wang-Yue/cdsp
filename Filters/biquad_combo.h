#ifndef CLIB_FILTERS_BIQUAD_COMBO_H
#define CLIB_FILTERS_BIQUAD_COMBO_H

/**
 * @file biquad_combo.h
 * @brief Combined biquad filters (e.g., Linkwitz-Riley, Butterworth, Tilt EQ,
 * Graphic EQ).
 */

#include <stddef.h>

#include "Config/config_error.h"
#include "Config/filter_config_types.h"
#include "Filters/biquad.h"

struct filter_vtable;

extern const struct filter_vtable g_biquad_combo_vtable;

/**
 * @brief Returns the number of stages in a BiquadCombo filter instance.
 *
 * @param instance Pointer to the biquad combo filter instance.
 * @return Number of biquad stages.
 */
size_t biquad_combo_get_stage_count(const void* instance);

/**
 * @brief Retrieves underlying biquad filter stages from a BiquadCombo filter
 * instance.
 *
 * @param instance Pointer to the biquad combo filter instance.
 * @param[out] out_stages Destination array to receive biquad filter pointers.
 * @param max_stages Maximum number of pointers that can be stored in
 * out_stages.
 * @return Number of biquad stage pointers written.
 */
size_t biquad_combo_get_stages(const void* instance,
                               biquad_filter_t** out_stages, size_t max_stages);

/**
 * @brief Expands a BiquadCombo configuration into its individual biquad stages.
 *
 * @param params Pointer to the biquad combo config parameters.
 * @param sample_rate Audio sample rate in Hz.
 * @param[out] out_stages Pointer to receive the allocated array of biquad
 * filter pointers.
 * @param[out] err Optional error struct to receive error details.
 * @return Number of stages built.
 */
size_t biquad_combo_stages(const biquad_combo_config_t* params, int sample_rate,
                           biquad_filter_t*** out_stages, config_error_t* err);

#endif  // CLIB_FILTERS_BIQUAD_COMBO_H
