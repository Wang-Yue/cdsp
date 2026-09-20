#ifndef CLIB_FILTERS_BIQUAD_H
#define CLIB_FILTERS_BIQUAD_H

/**
 * @file biquad.h
 * @brief Biquad filter creation, filtering operations, and lifecycle
 * management.
 */

#include <stdbool.h>
#include <stddef.h>

typedef struct filter_config_t filter_config_t;

typedef enum {
  BIQUAD_TYPE_FREE,
  BIQUAD_TYPE_HIGHPASS,
  BIQUAD_TYPE_LOWPASS,
  BIQUAD_TYPE_HIGHPASS_FO,
  BIQUAD_TYPE_LOWPASS_FO,
  BIQUAD_TYPE_HIGHSHELF,
  BIQUAD_TYPE_LOWSHELF,
  BIQUAD_TYPE_HIGHSHELF_FO,
  BIQUAD_TYPE_LOWSHELF_FO,
  BIQUAD_TYPE_PEAKING,
  BIQUAD_TYPE_NOTCH,
  BIQUAD_TYPE_BANDPASS,
  BIQUAD_TYPE_ALLPASS,
  BIQUAD_TYPE_ALLPASS_FO,
  BIQUAD_TYPE_GENERAL_NOTCH,
  BIQUAD_TYPE_LINKWITZ_TRANSFORM
} biquad_type_t;

typedef struct biquad_filter biquad_filter_t;

/**
 * @brief Runs multi-channel biquad cascades using the 2D systolic canon.
 */
void biquad_process_cascades(biquad_filter_t ***cascades, double **waveforms,
                             const size_t *channel_of, const size_t *live,
                             size_t live_count, size_t cascade_depth,
                             size_t n_frames);

/**
 * @brief Runs a single-channel biquad cascade using the systolic canon.
 */
void biquad_process_mono_cascade(biquad_filter_t **stages, size_t num_stages,
                                 double *waveform, size_t n_frames);

/**
 * @brief Clones a biquad filter instance (copying coefficients and state).
 */
biquad_filter_t *biquad_filter_clone(const biquad_filter_t *src);

/**
 * @brief Processes a single sample through the biquad filter.
 */
double biquad_filter_process_single(biquad_filter_t *filter, double sample);

/**
 * @brief Updates the filter parameters from a new configuration.
 * @return True if parameters were successfully updated, false if invalid or
 * unstable.
 */
bool biquad_filter_update_parameters(biquad_filter_t *filter,
                                     const filter_config_t *config,
                                     int sample_rate);

/**
 * @brief Gets the name of the biquad filter section.
 */
const char *biquad_filter_get_name(const biquad_filter_t *filter);

struct filter_vtable;

extern const struct filter_vtable g_biquad_vtable;

#endif // CLIB_FILTERS_BIQUAD_H
