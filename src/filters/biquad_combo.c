#include "filters/biquad_combo.h"

#include <stdbool.h>
#include <stdio.h>

#include "audio/processing_parameters.h"
#include "config/config_error.h"
#include "config/filter_config_types.h"
#include "filters/biquad.h"
#include "filters/filter.h"
#include "utils/double_helpers.h"

struct biquad_combo_filter {
  char name[64];
  biquad_filter_t **sections;
  size_t num_sections;
};

typedef struct biquad_combo_filter biquad_combo_filter_t;

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define BIQUAD_COMBO_MAX_ORDER 128

// MARK: - Butterworth & Linkwitz-Riley helper calculations
/**
 * @brief Computes Q values for a Butterworth filter of a given order.
 *
 * Butterworth poles are distributed on a semi-circle in the left-half s-plane.
 * For odd orders, there is one real pole (represented here as Q = -1.0,
 * indicating a first-order section). For even orders, all poles are complex
 * conjugate pairs with Q = 1 / (2 * sin(angle)).
 *
 * @param order The filter order (must be positive).
 * @param out_q Array to store the computed Q values.
 * @param max_q Maximum capacity of the `out_q` array.
 * @return The number of Q values computed (number of biquad stages).
 */
size_t biquad_combo_butterworth_q(int order, double *out_q, size_t max_q) {
  if (order < 1 || !out_q || max_q == 0)
    return 0;
  size_t uorder = (size_t)order;
  size_t count = 0;
  for (size_t k = 0; k < uorder / 2; k++) {
    if (count >= max_q)
      break;
    double angle = M_PI / (double)uorder * ((double)k + 0.5);
    out_q[count++] = 1.0 / (2.0 * sin(angle));
  }
  if (uorder % 2 != 0 && count < max_q) {
    out_q[count++] = -1.0;
  }
  return count;
}

/**
 * @brief Computes Q values for a Linkwitz-Riley filter of a given order.
 *
 * An L-R filter is designed by cascading two Butterworth filters of half the
 * order. e.g., LR4 is two cascaded BW2.
 *
 * @param order The filter order (must be positive, typically even).
 * @param out_q Array to store the computed Q values.
 * @param max_q Maximum capacity of the `out_q` array.
 * @return The number of Q values computed (number of biquad stages).
 */
size_t biquad_combo_linkwitz_riley_q(int order, double *out_q, size_t max_q) {
  if (order % 2 != 0 || order < 2 || !out_q || max_q == 0)
    return 0;
  size_t half_order = (size_t)(order / 2);
  double *bw_q = (double *)malloc((half_order + 1) * sizeof(double));
  if (!bw_q)
    return 0;
  size_t bw_count = biquad_combo_butterworth_q(order / 2, bw_q, half_order + 1);
  if (order % 4 > 0 && bw_count > 0) {
    bw_count--;
  }
  size_t count = 0;
  for (size_t i = 0; i < bw_count; i++) {
    if (count < max_q)
      out_q[count++] = bw_q[i];
  }
  for (size_t i = 0; i < bw_count; i++) {
    if (count < max_q)
      out_q[count++] = bw_q[i];
  }
  if (order % 4 > 0 && count < max_q) {
    out_q[count++] = 0.5;
  }
  free(bw_q);
  return count;
}

/**
 * @brief Helper function to create a single biquad filter section.
 *
 * @param type The type of biquad filter (e.g., LOWPASS, HIGHPASS, PEAKING).
 * @param freq Center or cutoff frequency in Hz.
 * @param q Quality factor.
 * @param gain Gain in dB (for peaking/shelf filters).
 * @param slope Slope (for shelf filters, if steepness_type is SLOPE).
 * @param bandwidth Bandwidth in octaves (for peaking/notch, if steepness_type
 * is BANDWIDTH).
 * @param steepness_type How the filter steepness is defined (Q, Bandwidth, or
 * Slope).
 * @param sample_rate Audio sample rate in Hz.
 * @return Pointer to the created biquad_filter_t, or NULL on failure.
 */
static biquad_filter_t *create_section(const char *sec_name, biquad_type_t type,
                                       double freq, double q, double gain,
                                       double slope, double bandwidth,
                                       steepness_type_t steepness_type,
                                       int sample_rate, config_error_t *err) {
  biquad_config_t bp = {.type = type,
                        .freq = freq,
                        .q = q,
                        .gain = gain,
                        .slope = slope,
                        .bandwidth = bandwidth,
                        .steepness_type = steepness_type};
  filter_config_t cfg = {.type = FILTER_TYPE_BIQUAD, .parameters.biquad = bp};
  return (biquad_filter_t *)g_biquad_vtable.create(sec_name, &cfg, sample_rate,
                                                   0, NULL, err);
}

/**
 * @brief Validates combined biquad parameters.
 *
 * @param config High-level filter configuration.
 * @param sample_rate The sample rate in Hz.
 * @param err Pointer to store error details if validation fails.
 * @return 0 on success, -1 on failure.
 */
static int biquad_combo_config_validate(const filter_config_t *config,
                                        int sample_rate, config_error_t *err) {
  if (!config || config->type != FILTER_TYPE_BIQUAD_COMBO)
    return -1;
  const biquad_combo_config_t *params = &config->parameters.biquad_combo;
  if (!params)
    return 0;
  double nyquist = (double)sample_rate / 2.0;
  switch (params->type) {
  case BIQUAD_COMBO_TYPE_BUTTERWORTH_LOWPASS:
  case BIQUAD_COMBO_TYPE_BUTTERWORTH_HIGHPASS:
    if (!params->has_freq || params->freq <= 0.0) {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER, "Frequency must be > 0");
      return -1;
    }
    if (params->freq >= nyquist) {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                       "Frequency must be < samplerate/2");
      return -1;
    }
    if (!params->has_order || params->order <= 0) {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                       "Butterworth order must be larger than zero");
      return -1;
    }
    break;
  case BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_LOWPASS:
  case BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_HIGHPASS:
    if (!params->has_freq || params->freq <= 0.0) {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER, "Frequency must be > 0");
      return -1;
    }
    if (params->freq >= nyquist) {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                       "Frequency must be < samplerate/2");
      return -1;
    }
    if (!params->has_order || params->order <= 0 || (params->order % 2) != 0) {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                       "LR order must be an even non-zero number");
      return -1;
    }
    break;
  case BIQUAD_COMBO_TYPE_TILT:
    if (!params->has_gain) {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                       "Tilt: gain must be set");
      return -1;
    }
    if (params->gain <= -100.0) {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER, "Gain must be > -100");
      return -1;
    }
    if (params->gain >= 100.0) {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER, "Gain must be < 100");
      return -1;
    }
    if (3500.0 >= nyquist) {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                       "Frequency must be < samplerate/2");
      return -1;
    }
    break;
  case BIQUAD_COMBO_TYPE_N_POINT_PEQ: {
    if (params->bands_count < 2 || !params->bands) {
      config_error_set(
          err, CONFIG_ERR_INVALID_FILTER,
          "At least two bands are needed, for the low and high shelves");
      return -1;
    }
    size_t last = params->bands_count - 1;
    for (size_t i = 0; i < params->bands_count; i++) {
      const peq_band_t *band = &params->bands[i];
      biquad_type_t btype;
      if (i == 0) {
        btype = BIQUAD_TYPE_LOWSHELF;
      } else if (i == last) {
        btype = BIQUAD_TYPE_HIGHSHELF;
      } else {
        btype = BIQUAD_TYPE_PEAKING;
      }
      biquad_config_t bp = {.type = btype,
                            .freq = band->freq,
                            .q = band->q,
                            .gain = band->gain,
                            .slope = 0.0,
                            .bandwidth = 0.0,
                            .steepness_type = STEEPNESS_TYPE_Q};
      filter_config_t cfg = {.type = FILTER_TYPE_BIQUAD,
                             .parameters.biquad = bp};
      if (g_biquad_vtable.validate(&cfg, sample_rate, err) != 0) {
        return -1;
      }
    }
    for (size_t i = 1; i < params->bands_count; i++) {
      if (params->bands[i].freq < params->bands[i - 1].freq) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "Band frequencies must not decrease along the list");
        return -1;
      }
    }
    break;
  }
  case BIQUAD_COMBO_TYPE_GRAPHIC_EQUALIZER: {
    double f_min = params->has_freq_min ? params->freq_min : 20.0;
    double f_max = params->has_freq_max ? params->freq_max : 20000.0;
    if (f_min <= 0.0 || f_max <= 0.0) {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                       "Min and max requencies must be > 0");
      return -1;
    }
    if (f_min >= nyquist || f_max >= nyquist) {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                       "Min and max frequencies must be < samplerate/2");
      return -1;
    }
    if (f_min >= f_max) {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                       "Min frequency must be lower than max frequency");
      return -1;
    }
    for (size_t i = 0; i < params->gains_count; i++) {
      double g = params->gains[i];
      if (g < -40.0 || g > 40.0) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "Equalizer gains must be withing +- 40 dB");
        return -1;
      }
    }
    break;
  }
  }
  return 0;
}

/**
 * @brief Frees the combined biquad filter instance.
 *
 * @param instance The filter instance to free.
 */
static void biquad_combo_filter_free(void *instance) {
  biquad_combo_filter_t *filter = (biquad_combo_filter_t *)instance;
  if (!filter)
    return;
  if (filter->sections) {
    for (size_t i = 0; i < filter->num_sections; i++) {
      if (filter->sections[i] && g_biquad_vtable.free) {
        g_biquad_vtable.free(filter->sections[i]);
      }
    }
    free(filter->sections);
  }
  free(filter);
}

/**
 * @brief Creates a combined biquad filter instance.
 *
 * @param name The name of the filter.
 * @param config High-level filter configuration.
 * @param sample_rate The sample rate in Hz.
 * @param chunk_size Maximum number of frames per processing chunk.
 * @param proc_params Processing parameters.
 * @param err Pointer to a config error struct to populate on failure.
 * @return A pointer to the created filter instance, or `NULL` on failure.
 */
static void *biquad_combo_filter_create(const char *name,
                                        const filter_config_t *config,
                                        int sample_rate, size_t chunk_size,
                                        processing_parameters_t *proc_params,
                                        config_error_t *err) {
  (void)chunk_size;
  (void)proc_params;
  if (!config || config->type != FILTER_TYPE_BIQUAD_COMBO)
    return NULL;
  const biquad_combo_config_t *params = &config->parameters.biquad_combo;
  if (biquad_combo_config_validate(config, sample_rate, err) != 0)
    return NULL;
  biquad_combo_filter_t *filter =
      (biquad_combo_filter_t *)calloc(1, sizeof(biquad_combo_filter_t));
  if (!filter) {
    config_error_set(err, CONFIG_ERR_PARSE,
                     "Failed to allocate BiquadCombo filter '%s'",
                     name ? name : "");
    return NULL;
  }
  if (name) {
    strncpy(filter->name, name, sizeof(filter->name) - 1);
    filter->name[sizeof(filter->name) - 1] = '\0';
  } else {
    strcpy(filter->name, "biquad_combo");
  }

  size_t max_secs = 8;
  if (params->type == BIQUAD_COMBO_TYPE_GRAPHIC_EQUALIZER) {
    max_secs = params->gains_count > 0 ? params->gains_count : 1;
  } else if (params->type == BIQUAD_COMBO_TYPE_BUTTERWORTH_LOWPASS ||
             params->type == BIQUAD_COMBO_TYPE_BUTTERWORTH_HIGHPASS ||
             params->type == BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_LOWPASS ||
             params->type == BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_HIGHPASS) {
    if (params->order > 0) {
      size_t ord = (size_t)params->order;
      max_secs = (ord + 1) / 2 + 1;
    } else {
      max_secs = 1;
    }
  } else if (params->type == BIQUAD_COMBO_TYPE_N_POINT_PEQ) {
    max_secs = params->bands_count > 0 ? params->bands_count : 1;
  } else if (params->type == BIQUAD_COMBO_TYPE_TILT) {
    max_secs = 2;
  }

  filter->sections =
      (biquad_filter_t **)calloc(max_secs, sizeof(biquad_filter_t *));
  if (!filter->sections) {
    config_error_set(err, CONFIG_ERR_PARSE, "Failed to allocate memory");
    biquad_combo_filter_free(filter);
    return NULL;
  }

  switch (params->type) {
  case BIQUAD_COMBO_TYPE_BUTTERWORTH_LOWPASS:
  case BIQUAD_COMBO_TYPE_BUTTERWORTH_HIGHPASS: {
    bool hp = (params->type == BIQUAD_COMBO_TYPE_BUTTERWORTH_HIGHPASS);
    if (params->order <= 0) {
      biquad_combo_filter_free(filter);
      return NULL;
    }
    size_t ord = (size_t)params->order;
    size_t q_capacity = ord + 2;
    double *q_vals = (double *)malloc(q_capacity * sizeof(double));
    if (!q_vals) {
      config_error_set(err, CONFIG_ERR_PARSE, "Failed to allocate memory");
      biquad_combo_filter_free(filter);
      return NULL;
    }
    size_t nq = biquad_combo_butterworth_q(params->order, q_vals, q_capacity);
    for (size_t i = 0; i < nq; i++) {
      biquad_type_t t;
      if (q_vals[i] < 0.0) {
        t = hp ? BIQUAD_TYPE_HIGHPASS_FO : BIQUAD_TYPE_LOWPASS_FO;
      } else {
        t = hp ? BIQUAD_TYPE_HIGHPASS : BIQUAD_TYPE_LOWPASS;
      }
      char name_buf[32];
      snprintf(name_buf, sizeof(name_buf), "sec_%zu", i);
      filter->sections[filter->num_sections++] = create_section(
          name_buf, t, params->freq, q_vals[i] > 0 ? q_vals[i] : 0.707, 0.0,
          0.0, 0.0, STEEPNESS_TYPE_Q, sample_rate, err);
    }
    free(q_vals);
    break;
  }
  case BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_LOWPASS:
  case BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_HIGHPASS: {
    bool hp = (params->type == BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_HIGHPASS);
    if (params->order <= 0) {
      biquad_combo_filter_free(filter);
      return NULL;
    }
    size_t ord = (size_t)params->order;
    size_t q_capacity = ord + 2;
    double *q_vals = (double *)malloc(q_capacity * sizeof(double));
    if (!q_vals) {
      config_error_set(err, CONFIG_ERR_PARSE, "Failed to allocate memory");
      biquad_combo_filter_free(filter);
      return NULL;
    }
    size_t nq =
        biquad_combo_linkwitz_riley_q(params->order, q_vals, q_capacity);
    for (size_t i = 0; i < nq; i++) {
      biquad_type_t t = hp ? BIQUAD_TYPE_HIGHPASS : BIQUAD_TYPE_LOWPASS;
      char name_buf[32];
      snprintf(name_buf, sizeof(name_buf), "sec_%zu", i);
      filter->sections[filter->num_sections++] =
          create_section(name_buf, t, params->freq, q_vals[i], 0.0, 0.0, 0.0,
                         STEEPNESS_TYPE_Q, sample_rate, err);
    }
    free(q_vals);
    break;
  }
  // MARK: - Tilt EQ
  case BIQUAD_COMBO_TYPE_TILT: {
    double gain = params->has_gain ? params->gain : 0.0;
    double nyquist = (double)sample_rate / 2.0;
    if (110.0 < nyquist) {
      filter->sections[filter->num_sections++] = create_section(
          "low_shelf", BIQUAD_TYPE_LOWSHELF, 110.0, 0.35, -gain / 2.0, 0.0, 0.0,
          STEEPNESS_TYPE_Q, sample_rate, err);
    }
    if (3500.0 < nyquist) {
      filter->sections[filter->num_sections++] = create_section(
          "high_shelf", BIQUAD_TYPE_HIGHSHELF, 3500.0, 0.35, gain / 2.0, 0.0,
          0.0, STEEPNESS_TYPE_Q, sample_rate, err);
    }
    break;
  }
  // MARK: - Graphic EQ
  case BIQUAD_COMBO_TYPE_GRAPHIC_EQUALIZER: {
    if (params->gains_count == 0) {
      break;
    }
    size_t nb = params->gains_count;
    double fmin = params->freq_min > 0 ? params->freq_min : 20.0;
    double fmax = params->freq_max > 0 ? params->freq_max : 20000.0;
    double log_min = log2(fmin);
    double log_max = log2(fmax);
    double bw = (log_max - log_min) / (double)nb;
    for (size_t i = 0; i < nb; i++) {
      double g = params->gains[i];
      if (fabs(g) <= 0.001)
        continue;
      double log_freq = log_min + ((double)i + 0.5) * bw;
      double f = pow(2.0, log_freq);
      char name_buf[32];
      snprintf(name_buf, sizeof(name_buf), "band_%zu", i);
      filter->sections[filter->num_sections++] =
          create_section(name_buf, BIQUAD_TYPE_PEAKING, f, 0.0, g, 0.0, bw,
                         STEEPNESS_TYPE_BANDWIDTH, sample_rate, err);
    }
    break;
  }
  // MARK: - N-Point PEQ
  case BIQUAD_COMBO_TYPE_N_POINT_PEQ: {
    size_t last = params->bands_count > 0 ? params->bands_count - 1 : 0;
    for (size_t i = 0; i < params->bands_count; i++) {
      const peq_band_t *band = &params->bands[i];
      if (fabs(band->gain) <= 0.001)
        continue;
      biquad_type_t btype;
      if (i == 0) {
        btype = BIQUAD_TYPE_LOWSHELF;
      } else if (i == last) {
        btype = BIQUAD_TYPE_HIGHSHELF;
      } else {
        btype = BIQUAD_TYPE_PEAKING;
      }
      char name_buf[32];
      snprintf(name_buf, sizeof(name_buf), "peq_%zu", i);
      filter->sections[filter->num_sections++] =
          create_section(name_buf, btype, band->freq, band->q, band->gain, 0.0,
                         0.0, STEEPNESS_TYPE_Q, sample_rate, err);
    }
    break;
  }
  }

  // Validate that all sections were successfully created
  for (size_t i = 0; i < filter->num_sections; i++) {
    if (!filter->sections[i]) {
      biquad_combo_filter_free(filter);
      return NULL;
    }
  }

  return filter;
}

/**
 * @brief Processes an array of samples through the combined biquad filter.
 *
 * @param filter The filter instance.
 * @param waveform The input/output waveform buffer.
 * @param count The number of samples to process.
 */
static void biquad_combo_filter_process(void *instance,
                                        mutable_waveform_t waveform,
                                        size_t count) {
  biquad_combo_filter_t *filter = (biquad_combo_filter_t *)instance;
  if (!filter || !waveform || count == 0 || filter->num_sections == 0)
    return;
  biquad_process_mono_cascade(filter->sections, filter->num_sections, waveform,
                              count);
}

/**
 * @brief Transfers history state of nested biquad sections from src to dest.
 *
 * Resize invariant: the sections form one cascade, so section `i` only refers
 * to the same stage of the same filter while the cascade has the same length.
 * A changed section count means the combo was reconfigured into a different
 * filter (a different order, or a different combo type altogether), and the
 * per-section histories no longer describe any part of it — so nothing is
 * carried. Within a cascade of equal length, each section is still gated
 * individually by `biquad_filter_transfer_state`, which drops the history when
 * the section's own biquad type changed.
 *
 * @param dest The destination combo filter instance.
 * @param src The source combo filter instance.
 */
static void biquad_combo_filter_transfer_state(void *dest_ptr,
                                               const void *src_ptr) {
  biquad_combo_filter_t *dest = (biquad_combo_filter_t *)dest_ptr;
  const biquad_combo_filter_t *src = (const biquad_combo_filter_t *)src_ptr;
  if (!dest || !src || dest == src)
    return;
  if (dest->num_sections != src->num_sections)
    return;
  for (size_t i = 0; i < dest->num_sections; i++) {
    if (dest->sections[i] && src->sections[i] &&
        g_biquad_vtable.transfer_state) {
      g_biquad_vtable.transfer_state(dest->sections[i], src->sections[i]);
    }
  }
}

const filter_vtable_t g_biquad_combo_vtable = {
    .validate = biquad_combo_config_validate,
    .create = biquad_combo_filter_create,
    .process = biquad_combo_filter_process,
    .transfer_state = biquad_combo_filter_transfer_state,
    .free = biquad_combo_filter_free};

size_t biquad_combo_get_stage_count(const void *instance) {
  if (!instance)
    return 0;
  const biquad_combo_filter_t *combo = (const biquad_combo_filter_t *)instance;
  return combo->num_sections;
}

size_t biquad_combo_get_stages(const void *instance,
                               biquad_filter_t **out_stages,
                               size_t max_stages) {
  if (!instance || !out_stages || max_stages == 0)
    return 0;
  const biquad_combo_filter_t *combo = (const biquad_combo_filter_t *)instance;
  size_t to_copy =
      (combo->num_sections < max_stages) ? combo->num_sections : max_stages;
  for (size_t i = 0; i < to_copy; i++) {
    out_stages[i] = combo->sections[i];
  }
  return to_copy;
}

size_t biquad_combo_stages(const biquad_combo_config_t *params, int sample_rate,
                           biquad_filter_t ***out_stages, config_error_t *err) {
  if (!params || !out_stages)
    return 0;
  *out_stages = NULL;
  filter_config_t cfg = {
      .type = FILTER_TYPE_BIQUAD_COMBO,
      .parameters.biquad_combo = *params,
  };
  biquad_combo_filter_t *f =
      (biquad_combo_filter_t *)biquad_combo_filter_create(
          "combo", &cfg, sample_rate, 0, NULL, err);
  if (!f)
    return 0;
  size_t count = f->num_sections;
  *out_stages = f->sections;
  free(f);
  return count;
}
