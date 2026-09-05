#include "Filters/biquad_combo.h"

#include <stdbool.h>
#include <stdio.h>

#include "Audio/processing_parameters.h"
#include "Config/config_error.h"
#include "Config/filter_config_types.h"
#include "Filters/biquad.h"
#include "Filters/biquad_canon.h"
#include "Filters/filter.h"
#include "Utils/double_helpers.h"

struct biquad_combo_filter {
  char name[64];
  biquad_filter_t** sections;
  size_t num_sections;
  void* canon_filter;
};

typedef struct biquad_combo_filter biquad_combo_filter_t;

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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
size_t biquad_combo_butterworth_q(int order, double* out_q, size_t max_q) {
  if (order < 1 || !out_q || max_q == 0) return 0;
  size_t count = 0;
  for (int k = 0; k < order / 2; k++) {
    if (count >= max_q) break;
    double angle = M_PI / (double)order * ((double)k + 0.5);
    out_q[count++] = 1.0 / (2.0 * sin(angle));
  }
  if (order % 2 != 0 && count < max_q) {
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
size_t biquad_combo_linkwitz_riley_q(int order, double* out_q, size_t max_q) {
  if (order % 2 != 0 || order < 2 || !out_q || max_q == 0) return 0;
  double bw_q[16];
  size_t bw_count = biquad_combo_butterworth_q(order / 2, bw_q, 16);
  if (order % 4 > 0 && bw_count > 0) {
    bw_count--;
  }
  size_t count = 0;
  for (size_t i = 0; i < bw_count; i++) {
    if (count < max_q) out_q[count++] = bw_q[i];
  }
  for (size_t i = 0; i < bw_count; i++) {
    if (count < max_q) out_q[count++] = bw_q[i];
  }
  if (order % 4 > 0 && count < max_q) {
    out_q[count++] = 0.5;
  }
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
static biquad_filter_t* create_section(const char* sec_name, biquad_type_t type,
                                       double freq, double q, double gain,
                                       double slope, double bandwidth,
                                       steepness_type_t steepness_type,
                                       int sample_rate, config_error_t* err) {
  biquad_config_t bp = {.type = type,
                        .freq = freq,
                        .q = q,
                        .gain = gain,
                        .slope = slope,
                        .bandwidth = bandwidth,
                        .steepness_type = steepness_type};
  filter_config_t cfg = {.type = FILTER_TYPE_BIQUAD, .parameters.biquad = bp};
  return (biquad_filter_t*)g_biquad_vtable.create(sec_name, &cfg, sample_rate,
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
static int biquad_combo_config_validate(const filter_config_t* config,
                                        int sample_rate, config_error_t* err) {
  if (sample_rate <= 0) {
    config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                     "BiquadCombo: sample_rate must be greater than 0, got %d",
                     sample_rate);
    return -1;
  }
  if (!config || config->type != FILTER_TYPE_BIQUAD_COMBO) return -1;
  const biquad_combo_config_t* params = &config->parameters.biquad_combo;
  if (!params) return 0;
  double nyquist = (double)sample_rate / 2.0;
  switch (params->type) {
    case BIQUAD_COMBO_TYPE_BUTTERWORTH_LOWPASS:
    case BIQUAD_COMBO_TYPE_BUTTERWORTH_HIGHPASS:
      if (!params->has_freq || params->freq <= 0.0) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "BiquadCombo: freq must be > 0, got %g", params->freq);
        return -1;
      }
      if (params->freq >= nyquist) {
        config_error_set(
            err, CONFIG_ERR_INVALID_FILTER,
            "BiquadCombo: freq must be less than Nyquist (%g), got %g", nyquist,
            params->freq);
        return -1;
      }
      if (!params->has_order || params->order <= 0 || params->order > 64) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "BiquadCombo: order must be between 1 and 64, got %d",
                         params->order);
        return -1;
      }
      break;
    case BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_LOWPASS:
    case BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_HIGHPASS:
      if (!params->has_freq || params->freq <= 0.0) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "BiquadCombo: freq must be > 0, got %g", params->freq);
        return -1;
      }
      if (params->freq >= nyquist) {
        config_error_set(
            err, CONFIG_ERR_INVALID_FILTER,
            "BiquadCombo: freq must be less than Nyquist (%g), got %g", nyquist,
            params->freq);
        return -1;
      }
      if (!params->has_order || params->order <= 0 || params->order > 64 ||
          (params->order % 2) != 0) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "Linkwitz-Riley order must be an even number between "
                         "2 and 64, got %d",
                         params->order);
        return -1;
      }
      break;
    case BIQUAD_COMBO_TYPE_TILT:
      if (!params->has_gain) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "Tilt: gain must be set");
        return -1;
      }
      if (params->gain <= -100.0 || params->gain >= 100.0) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "Tilt: gain must be between -100 and 100 dB, got %g",
                         params->gain);
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
      for (size_t i = 0; i < params->bands_count; i++) {
        const peq_band_t* b = &params->bands[i];
        if (b->freq <= 0.0) {
          config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                           "Frequency must be > 0");
          return -1;
        }
        if (b->freq >= nyquist) {
          config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                           "Frequency must be < samplerate/2");
          return -1;
        }
        if (b->q <= 0.0) {
          config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                           "Q-value must be > 0");
          return -1;
        }
        if (i > 0 && b->freq < params->bands[i - 1].freq) {
          config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                           "Band frequencies must not decrease along the list");
          return -1;
        }
      }
      break;
    }
    case BIQUAD_COMBO_TYPE_GRAPHIC_EQUALIZER: {
      if (!params->gains || params->gains_count <= 0) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "GraphicEqualizer: gains must be non-empty");
        return -1;
      }
      double f_min = params->has_freq_min ? params->freq_min : 20.0;
      double f_max = params->has_freq_max ? params->freq_max : 20000.0;
      if (f_min <= 0.0 || f_max <= 0.0) {
        config_error_set(
            err, CONFIG_ERR_INVALID_FILTER,
            "GraphicEqualizer: min and max frequencies must be > 0");
        return -1;
      }
      if (f_min >= nyquist || f_max >= nyquist) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "GraphicEqualizer: min and max frequencies must be "
                         "less than Nyquist (%g)",
                         nyquist);
        return -1;
      }
      if (f_min >= f_max) {
        config_error_set(
            err, CONFIG_ERR_INVALID_FILTER,
            "GraphicEqualizer: min frequency must be lower than max frequency");
        return -1;
      }
      for (size_t i = 0; i < params->gains_count; i++) {
        double g = params->gains[i];
        if (g < -40.0 || g > 40.0) {
          config_error_set(
              err, CONFIG_ERR_INVALID_FILTER,
              "GraphicEqualizer: gain[%zu]=%g out of bounds [-40, +40]", i, g);
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
static void biquad_combo_filter_free(void* instance) {
  biquad_combo_filter_t* filter = (biquad_combo_filter_t*)instance;
  if (!filter) return;
  if (filter->canon_filter) {
    g_biquad_canon_vtable.free(filter->canon_filter);
  }
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
static void* biquad_combo_filter_create(const char* name,
                                        const filter_config_t* config,
                                        int sample_rate, size_t chunk_size,
                                        processing_parameters_t* proc_params,
                                        config_error_t* err) {
  (void)chunk_size;
  (void)proc_params;
  if (!config || config->type != FILTER_TYPE_BIQUAD_COMBO) return NULL;
  const biquad_combo_config_t* params = &config->parameters.biquad_combo;
  if (biquad_combo_config_validate(config, sample_rate, err) != 0) return NULL;
  biquad_combo_filter_t* filter =
      (biquad_combo_filter_t*)calloc(1, sizeof(biquad_combo_filter_t));
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
    max_secs = (params->order + 1) / 2 + 1;
  } else if (params->type == BIQUAD_COMBO_TYPE_N_POINT_PEQ) {
    max_secs = params->bands_count > 0 ? params->bands_count : 1;
  } else if (params->type == BIQUAD_COMBO_TYPE_TILT) {
    max_secs = 2;
  }

  filter->sections =
      (biquad_filter_t**)calloc(max_secs, sizeof(biquad_filter_t*));
  if (!filter->sections) {
    config_error_set(err, CONFIG_ERR_PARSE, "Failed to allocate memory");
    biquad_combo_filter_free(filter);
    return NULL;
  }

  switch (params->type) {
    case BIQUAD_COMBO_TYPE_BUTTERWORTH_LOWPASS:
    case BIQUAD_COMBO_TYPE_BUTTERWORTH_HIGHPASS: {
      bool hp = (params->type == BIQUAD_COMBO_TYPE_BUTTERWORTH_HIGHPASS);
      double q_vals[32];
      size_t nq = biquad_combo_butterworth_q(params->order, q_vals, 32);
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
      break;
    }
    case BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_LOWPASS:
    case BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_HIGHPASS: {
      bool hp = (params->type == BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_HIGHPASS);
      double q_vals[32];
      size_t nq = biquad_combo_linkwitz_riley_q(params->order, q_vals, 32);
      for (size_t i = 0; i < nq; i++) {
        biquad_type_t t = hp ? BIQUAD_TYPE_HIGHPASS : BIQUAD_TYPE_LOWPASS;
        char name_buf[32];
        snprintf(name_buf, sizeof(name_buf), "sec_%zu", i);
        filter->sections[filter->num_sections++] =
            create_section(name_buf, t, params->freq, q_vals[i], 0.0, 0.0, 0.0,
                           STEEPNESS_TYPE_Q, sample_rate, err);
      }
      break;
    }
    // MARK: - Tilt EQ
    case BIQUAD_COMBO_TYPE_TILT: {
      double gain = params->has_gain ? params->gain : 0.0;
      filter->sections[filter->num_sections++] = create_section(
          "low_shelf", BIQUAD_TYPE_LOWSHELF, 110.0, 0.35, -gain / 2.0, 0.0, 0.0,
          STEEPNESS_TYPE_Q, sample_rate, err);
      filter->sections[filter->num_sections++] = create_section(
          "high_shelf", BIQUAD_TYPE_HIGHSHELF, 3500.0, 0.35, gain / 2.0, 0.0,
          0.0, STEEPNESS_TYPE_Q, sample_rate, err);
      break;
    }
    // MARK: - Graphic EQ
    case BIQUAD_COMBO_TYPE_GRAPHIC_EQUALIZER: {
      size_t nb = params->gains_count > 0 ? params->gains_count : 1;
      double fmin = params->freq_min > 0 ? params->freq_min : 20.0;
      double fmax = params->freq_max > 0 ? params->freq_max : 20000.0;
      double log_min = log2(fmin);
      double log_max = log2(fmax);
      double bw = (log_max - log_min) / (double)nb;
      for (size_t i = 0; i < nb; i++) {
        double g = params->gains[i];
        if (fabs(g) <= 0.001) continue;
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
        const peq_band_t* band = &params->bands[i];
        if (fabs(band->gain) <= 0.001) continue;
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
            create_section(name_buf, btype, band->freq, band->q, band->gain,
                           0.0, 0.0, STEEPNESS_TYPE_Q, sample_rate, err);
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

  filter_config_t canon_cfg = {
      .type = FILTER_TYPE_BIQUAD_CANON,
      .parameters.biquad_canon =
          {
              .sections = filter->sections,
              .num_sections = filter->num_sections,
              .owns_sections = false,
          },
  };
  filter->canon_filter =
      g_biquad_canon_vtable.create(name, &canon_cfg, sample_rate, 0, NULL, err);
  if (!filter->canon_filter) {
    biquad_combo_filter_free(filter);
    return NULL;
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
static void biquad_combo_filter_process(void* instance,
                                        mutable_waveform_t waveform,
                                        size_t count) {
  biquad_combo_filter_t* filter = (biquad_combo_filter_t*)instance;
  if (!filter || !waveform || count == 0) return;
  g_biquad_canon_vtable.process(filter->canon_filter, waveform, count);
}

/**
 * @brief Transfers history state of nested biquad sections from src to dest.
 *
 * @param dest The destination combo filter instance.
 * @param src The source combo filter instance.
 */
static void biquad_combo_filter_transfer_state(void* dest_ptr,
                                               const void* src_ptr) {
  biquad_combo_filter_t* dest = (biquad_combo_filter_t*)dest_ptr;
  const biquad_combo_filter_t* src = (const biquad_combo_filter_t*)src_ptr;
  if (!dest || !src) return;
  if (dest->canon_filter && src->canon_filter) {
    g_biquad_canon_vtable.transfer_state(dest->canon_filter, src->canon_filter);
  }
}

const filter_vtable_t g_biquad_combo_vtable = {
    .validate = biquad_combo_config_validate,
    .create = biquad_combo_filter_create,
    .process = biquad_combo_filter_process,
    .transfer_state = biquad_combo_filter_transfer_state,
    .free = biquad_combo_filter_free};
