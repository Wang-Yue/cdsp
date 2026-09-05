#include "Filters/biquad.h"

#include "Audio/processing_parameters.h"
#include "Config/config_error.h"
#include "Config/filter_config_types.h"
#include "Filters/biquad_internal.h"
#include "Filters/filter.h"
#include "Utils/double_helpers.h"

/**
 * @brief Returns coefficients for a passthrough filter (identity / no effect).
 *
 * @return Biquad coefficients representing an identity filter (b0=1, all others
 * 0).
 */
static inline biquad_coefficients_t biquad_coefficients_passthrough(void) {
  return (biquad_coefficients_t){1.0, 0.0, 0.0, 0.0, 0.0};
}

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/**
 * @brief Validates stability of a biquad filter using the Jury pole triangle
 * condition.
 *
 * Checks if poles lie strictly inside the unit circle:
 * \f$ |a_2| < 1.0 \land |a_1| < a_2 + 1.0 \f$
 *
 * @param coeffs Pointer to the biquad coefficients structure.
 * @return true if the filter is strictly stable, false otherwise.
 */
static inline bool is_stable(const biquad_coefficients_t* coeffs) {
  if (!coeffs) return false;
  return fabs(coeffs->a2) < 1.0 && (fabs(coeffs->a1) < (coeffs->a2 + 1.0));
}

/**
 * @brief Computes low-level transfer function coefficients from high-level
 * parameters.
 *
 * Calculates b0, b1, b2, a1, a2 based on parameter type (lowpass, highpass,
 * shelving, peaking, allpass, notch, Linkwitz transform, etc.) and sample rate.
 *
 * @param params High-level biquad parameters.
 * @param sample_rate Audio sample rate in Hz.
 * @param out_coeffs Pointer to store computed transfer coefficients.
 * @return true if computation succeeded and coefficients are stable, false
 * otherwise.
 */
static bool biquad_coefficients_compute(const biquad_config_t* params,
                                        int sample_rate,
                                        biquad_coefficients_t* out_coeffs) {
  if (!params || !out_coeffs || sample_rate <= 0) return false;

  double fs = (double)sample_rate;
  double freq = params->freq > 0 ? params->freq : 1000.0;
  double gain = params->gain;
  double q = params->q > 0 ? params->q : 0.707;

  double w0 = 0.0;
  double cos_w0 = 0.0;
  double sin_w0 = 0.0;
  double A = 1.0;
  double alpha = 0.0;

  bool needs_w0 = (params->type != BIQUAD_TYPE_FREE &&
                   params->type != BIQUAD_TYPE_GENERAL_NOTCH &&
                   params->type != BIQUAD_TYPE_LINKWITZ_TRANSFORM);

  if (needs_w0) {
    w0 = 2.0 * M_PI * freq / fs;
    cos_w0 = cos(w0);
    sin_w0 = sin(w0);
    A = double_from_db(gain / 2.0);

    if (fabs(sin_w0) < 1e-12) sin_w0 = 1e-12;
    if (A < 1e-12) A = 1e-12;

    // Compute alpha directly based on steepness_type (Bandwidth, Slope, or Q)
    if (params->steepness_type == STEEPNESS_TYPE_BANDWIDTH) {
      double bw = params->bandwidth;
      alpha = sin_w0 * sinh(log(2.0) / 2.0 * bw * w0 / sin_w0);
    } else if (params->steepness_type == STEEPNESS_TYPE_SLOPE) {
      double slope_s = params->slope / 12.0;
      if (fabs(slope_s) < 1e-12) slope_s = 1e-12;
      double term = (A + 1.0 / A) * (1.0 / slope_s - 1.0) + 2.0;
      alpha = sin_w0 / 2.0 * sqrt(term > 1e-12 ? term : 1e-12);
    } else {
      if (fabs(q) < 1e-12) q = 1e-12;
      alpha = sin_w0 / (2.0 * q);
    }
  }

  double b0 = 0, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;

  switch (params->type) {
    case BIQUAD_TYPE_FREE:
      b0 = params->b0;
      b1 = params->b1;
      b2 = params->b2;
      a0 = 1.0;
      a1 = params->a1;
      a2 = params->a2;
      break;

    case BIQUAD_TYPE_GENERAL_NOTCH: {
      // General notch filter allows independent control of notch frequency and
      // pole frequency. Uses bilinear transform.
      double freq_z = params->freq_notch > 0 ? params->freq_notch : 1000.0;
      double freq_p = params->freq_pole > 0 ? params->freq_pole : 1000.0;
      double q_p = params->q_p;
      bool normalize = params->normalize_at_dc;
      double tn_z = tan(M_PI * freq_z / fs);
      double tn_p = tan(M_PI * freq_p / fs);
      double alpha_p = tn_p / q_p;
      double tn2_p = tn_p * tn_p;
      double tn2_z = tn_z * tn_z;
      // Optional normalization to ensure 0 dB gain at DC.
      double gain_norm = normalize ? (tn2_p / tn2_z) : 1.0;
      b0 = gain_norm * (1.0 + tn2_z);
      b1 = -2.0 * gain_norm * (1.0 - tn2_z);
      b2 = gain_norm * (1.0 + tn2_z);
      a0 = 1.0 + alpha_p + tn2_p;
      a1 = -2.0 + 2.0 * tn2_p;
      a2 = 1.0 - alpha_p + tn2_p;
      break;
    }

    case BIQUAD_TYPE_LINKWITZ_TRANSFORM: {
      // Linkwitz Transform compensates for the low frequency roll-off of a
      // speaker in a sealed box and replaces it with a new target response
      // (lower Fc, different Q). Act: actual speaker parameters. Target:
      // desired parameters.
      double freq_act = params->freq_act > 0 ? params->freq_act : 50.0;
      double q_act = params->q_act > 0 ? params->q_act : 0.707;
      double freq_target = params->freq_target > 0 ? params->freq_target : 25.0;
      double q_target = params->q_target > 0 ? params->q_target : 0.707;
      double d0i = pow(2.0 * M_PI * freq_act, 2);
      double d1i = (2.0 * M_PI * freq_act) / q_act;
      double c0i = pow(2.0 * M_PI * freq_target, 2);
      double c1i = (2.0 * M_PI * freq_target) / q_target;
      double fc = (freq_target + freq_act) / 2.0;
      double gn = 2.0 * M_PI * fc / tan(M_PI * fc / fs);
      double gn2 = gn * gn;
      double cci = c0i + gn * c1i + gn2;
      b0 = (d0i + gn * d1i + gn2) / cci;
      b1 = 2.0 * (d0i - gn2) / cci;
      b2 = (d0i - gn * d1i + gn2) / cci;
      a0 = 1.0;
      a1 = 2.0 * (c0i - gn2) / cci;
      a2 = (c0i - gn * c1i + gn2) / cci;
      break;
    }

    case BIQUAD_TYPE_PEAKING:
      b0 = 1.0 + alpha * A;
      b1 = -2.0 * cos_w0;
      b2 = 1.0 - alpha * A;
      a0 = 1.0 + alpha / A;
      a1 = -2.0 * cos_w0;
      a2 = 1.0 - alpha / A;
      break;

    case BIQUAD_TYPE_LOWSHELF:
      b0 = A * ((A + 1.0) - (A - 1.0) * cos_w0 + 2.0 * sqrt(A) * alpha);
      b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cos_w0);
      b2 = A * ((A + 1.0) - (A - 1.0) * cos_w0 - 2.0 * sqrt(A) * alpha);
      a0 = (A + 1.0) + (A - 1.0) * cos_w0 + 2.0 * sqrt(A) * alpha;
      a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cos_w0);
      a2 = (A + 1.0) + (A - 1.0) * cos_w0 - 2.0 * sqrt(A) * alpha;
      break;

    case BIQUAD_TYPE_HIGHSHELF:
      b0 = A * ((A + 1.0) + (A - 1.0) * cos_w0 + 2.0 * sqrt(A) * alpha);
      b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cos_w0);
      b2 = A * ((A + 1.0) + (A - 1.0) * cos_w0 - 2.0 * sqrt(A) * alpha);
      a0 = (A + 1.0) - (A - 1.0) * cos_w0 + 2.0 * sqrt(A) * alpha;
      a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cos_w0);
      a2 = (A + 1.0) - (A - 1.0) * cos_w0 - 2.0 * sqrt(A) * alpha;
      break;

    case BIQUAD_TYPE_LOWPASS:
      b0 = (1.0 - cos_w0) / 2.0;
      b1 = 1.0 - cos_w0;
      b2 = (1.0 - cos_w0) / 2.0;
      a0 = 1.0 + alpha;
      a1 = -2.0 * cos_w0;
      a2 = 1.0 - alpha;
      break;

    case BIQUAD_TYPE_HIGHPASS:
      b0 = (1.0 + cos_w0) / 2.0;
      b1 = -(1.0 + cos_w0);
      b2 = (1.0 + cos_w0) / 2.0;
      a0 = 1.0 + alpha;
      a1 = -2.0 * cos_w0;
      a2 = 1.0 - alpha;
      break;

    case BIQUAD_TYPE_NOTCH:
      b0 = 1.0;
      b1 = -2.0 * cos_w0;
      b2 = 1.0;
      a0 = 1.0 + alpha;
      a1 = -2.0 * cos_w0;
      a2 = 1.0 - alpha;
      break;

    case BIQUAD_TYPE_BANDPASS:
      b0 = alpha;
      b1 = 0.0;
      b2 = -alpha;
      a0 = 1.0 + alpha;
      a1 = -2.0 * cos_w0;
      a2 = 1.0 - alpha;
      break;

    case BIQUAD_TYPE_ALLPASS:
      b0 = 1.0 - alpha;
      b1 = -2.0 * cos_w0;
      b2 = 1.0 + alpha;
      a0 = 1.0 + alpha;
      a1 = -2.0 * cos_w0;
      a2 = 1.0 - alpha;
      break;

    case BIQUAD_TYPE_LOWPASS_FO:
      b0 = sin_w0;
      b1 = sin_w0;
      b2 = 0.0;
      a0 = sin_w0 + 1.0 + cos_w0;
      a1 = sin_w0 - 1.0 - cos_w0;
      a2 = 0.0;
      break;

    case BIQUAD_TYPE_HIGHPASS_FO:
      b0 = 1.0 + cos_w0;
      b1 = -1.0 - cos_w0;
      b2 = 0.0;
      a0 = sin_w0 + 1.0 + cos_w0;
      a1 = sin_w0 - 1.0 - cos_w0;
      a2 = 0.0;
      break;

    case BIQUAD_TYPE_LOWSHELF_FO:
      b0 = A * sin_w0 + 1.0 + cos_w0;
      b1 = A * sin_w0 - 1.0 - cos_w0;
      b2 = 0.0;
      a0 = (1.0 / A) * sin_w0 + 1.0 + cos_w0;
      a1 = (1.0 / A) * sin_w0 - 1.0 - cos_w0;
      a2 = 0.0;
      break;

    case BIQUAD_TYPE_HIGHSHELF_FO:
      b0 = sin_w0 + A + A * cos_w0;
      b1 = sin_w0 - A - A * cos_w0;
      b2 = 0.0;
      a0 = sin_w0 + (1.0 / A) + (1.0 / A) * cos_w0;
      a1 = sin_w0 - (1.0 / A) - (1.0 / A) * cos_w0;
      a2 = 0.0;
      break;

    case BIQUAD_TYPE_ALLPASS_FO:
      b0 = sin_w0 - 1.0 - cos_w0;
      b1 = sin_w0 + 1.0 + cos_w0;
      b2 = 0.0;
      a0 = sin_w0 + 1.0 + cos_w0;
      a1 = sin_w0 - 1.0 - cos_w0;
      a2 = 0.0;
      break;
  }

  if (a0 == 0.0) return false;
  out_coeffs->b0 = b0 / a0;
  out_coeffs->b1 = b1 / a0;
  out_coeffs->b2 = b2 / a0;
  out_coeffs->a1 = a1 / a0;
  out_coeffs->a2 = a2 / a0;

  return is_stable(out_coeffs);
}

static bool biquad_config_check_stability(const biquad_config_t* params,
                                          int sample_rate) {
  if (!params || sample_rate <= 0) return false;
  biquad_coefficients_t dummy_coeffs;
  return biquad_coefficients_compute(params, sample_rate, &dummy_coeffs);
}

/**
 * @brief Frees the biquad filter instance.
 *
 * @param filter The filter instance to free.
 */
static void biquad_filter_free(void* instance) {
  biquad_filter_t* filter = (biquad_filter_t*)instance;
  if (!filter) return;
  free(filter);
}

/**
 * @brief Validates high-level biquad filter parameters and checks stability.
 *
 * @param config High-level filter configuration.
 * @param sample_rate Audio sample rate in Hz.
 * @param err Pointer to a config error struct to populate on failure.
 * @return 0 on success, -1 on failure.
 */
static int biquad_config_validate(const filter_config_t* config,
                                  int sample_rate, config_error_t* err) {
  if (!config || config->type != FILTER_TYPE_BIQUAD) return -1;
  const biquad_config_t* params = &config->parameters.biquad;
  double nyquist = (double)sample_rate / 2.0;

  // 0. Enforce steepness type constraints
  if (params->type == BIQUAD_TYPE_LOWPASS ||
      params->type == BIQUAD_TYPE_HIGHPASS) {
    if (params->steepness_type != STEEPNESS_TYPE_Q) {
      if (err) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "Lowpass/Highpass only supports Q steepness type");
      }
      return -1;
    }
  }
  if (params->type == BIQUAD_TYPE_PEAKING ||
      params->type == BIQUAD_TYPE_BANDPASS ||
      params->type == BIQUAD_TYPE_NOTCH ||
      params->type == BIQUAD_TYPE_ALLPASS) {
    if (params->steepness_type == STEEPNESS_TYPE_SLOPE) {
      if (err) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "Peaking/Bandpass/Notch/Allpass does not support "
                         "Slope steepness type");
      }
      return -1;
    }
  }
  if (params->type == BIQUAD_TYPE_HIGHSHELF ||
      params->type == BIQUAD_TYPE_LOWSHELF) {
    if (params->steepness_type == STEEPNESS_TYPE_BANDWIDTH) {
      if (err) {
        config_error_set(
            err, CONFIG_ERR_INVALID_FILTER,
            "Highshelf/Lowshelf does not support Bandwidth steepness type");
      }
      return -1;
    }
  }

  // 1. Check Frequency (matching Rust validate_config match block 1)
  bool has_standard_freq = (params->type == BIQUAD_TYPE_HIGHPASS ||
                            params->type == BIQUAD_TYPE_LOWPASS ||
                            params->type == BIQUAD_TYPE_HIGHPASS_FO ||
                            params->type == BIQUAD_TYPE_LOWPASS_FO ||
                            params->type == BIQUAD_TYPE_PEAKING ||
                            params->type == BIQUAD_TYPE_HIGHSHELF ||
                            params->type == BIQUAD_TYPE_LOWSHELF ||
                            params->type == BIQUAD_TYPE_HIGHSHELF_FO ||
                            params->type == BIQUAD_TYPE_LOWSHELF_FO ||
                            params->type == BIQUAD_TYPE_NOTCH ||
                            params->type == BIQUAD_TYPE_BANDPASS ||
                            params->type == BIQUAD_TYPE_ALLPASS ||
                            params->type == BIQUAD_TYPE_ALLPASS_FO);

  if (has_standard_freq) {
    if (params->freq <= 0.0) {
      if (err) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "Frequency must be > 0");
      }
      return -1;
    }
    if (params->freq >= nyquist) {
      if (err) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "Frequency must be < samplerate/2");
      }
      return -1;
    }
  }

  // 2. Check Q (matching Rust validate_config match block 2)
  bool check_q_val = false;
  double q_to_check = 0.0;
  if (params->type == BIQUAD_TYPE_HIGHPASS ||
      params->type == BIQUAD_TYPE_LOWPASS) {
    check_q_val = true;
    q_to_check = params->q;
  } else if (params->type == BIQUAD_TYPE_PEAKING ||
             params->type == BIQUAD_TYPE_NOTCH ||
             params->type == BIQUAD_TYPE_BANDPASS ||
             params->type == BIQUAD_TYPE_ALLPASS ||
             params->type == BIQUAD_TYPE_HIGHSHELF ||
             params->type == BIQUAD_TYPE_LOWSHELF) {
    if (params->steepness_type == STEEPNESS_TYPE_Q) {
      check_q_val = true;
      q_to_check = params->q;
    }
  } else if (params->type == BIQUAD_TYPE_GENERAL_NOTCH) {
    check_q_val = true;
    q_to_check = params->q_p;
  }

  if (check_q_val && q_to_check <= 0.0) {
    if (err) {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER, "Q must be > 0");
    }
    return -1;
  }

  // 3. Check Bandwidth (matching Rust validate_config match block 3)
  bool check_bw_val = false;
  if (params->type == BIQUAD_TYPE_PEAKING ||
      params->type == BIQUAD_TYPE_NOTCH ||
      params->type == BIQUAD_TYPE_BANDPASS ||
      params->type == BIQUAD_TYPE_ALLPASS) {
    if (params->steepness_type == STEEPNESS_TYPE_BANDWIDTH) {
      check_bw_val = true;
    }
  }
  if (check_bw_val && params->bandwidth <= 0.0) {
    if (err) {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER, "Bandwidth must be > 0");
    }
    return -1;
  }

  // 4. Check Slope (matching Rust validate_config match block 4)
  bool check_slope_val = false;
  if (params->type == BIQUAD_TYPE_HIGHSHELF ||
      params->type == BIQUAD_TYPE_LOWSHELF) {
    if (params->steepness_type == STEEPNESS_TYPE_SLOPE) {
      check_slope_val = true;
    }
  }
  if (check_slope_val) {
    if (params->slope <= 0.0) {
      if (err) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER, "Slope must be > 0");
      }
      return -1;
    }
    if (params->slope > 12.0) {
      if (err) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "Slope must be <= 12.0");
      }
      return -1;
    }
  }

  // 5. Check LT (matching Rust validate_config LT block)
  if (params->type == BIQUAD_TYPE_LINKWITZ_TRANSFORM) {
    if (params->freq_act <= 0.0 || params->freq_target <= 0.0) {
      if (err) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "Frequency must be > 0");
      }
      return -1;
    }
    if (params->freq_act >= nyquist || params->freq_target >= nyquist) {
      if (err) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "Frequency must be < samplerate/2");
      }
      return -1;
    }
    if (params->q_act <= 0.0 || params->q_target <= 0.0) {
      if (err) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER, "Q must be > 0");
      }
      return -1;
    }
  }

  // 6. Check GeneralNotch frequencies (matching Rust GeneralNotch block)
  if (params->type == BIQUAD_TYPE_GENERAL_NOTCH) {
    if (params->freq_pole <= 0.0 || params->freq_notch <= 0.0) {
      if (err) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "Pole and zero frequencies must be > 0");
      }
      return -1;
    }
    if (params->freq_pole >= nyquist || params->freq_notch >= nyquist) {
      if (err) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "Pole and zero frequencies must be < samplerate/2");
      }
      return -1;
    }
  }

  // 7. Check Stability (matching Rust stability check)
  if (sample_rate > 0) {
    if (!biquad_config_check_stability(params, sample_rate)) {
      if (err) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "Unstable filter specified");
      }
      return -1;
    }
  }

  return 0;
}

/**
 * @brief Creates a biquad filter instance directly from filter configuration.
 *
 * Computes coefficients and validates filter stability. If config is NULL,
 * creates a passthrough filter. If the filter is unstable or parameters are
 * invalid, sets err and returns NULL.
 *
 * @param name The name of the filter (for debugging/identification).
 * @param config High-level filter configuration (NULL for passthrough identity
 * filter).
 * @param sample_rate Sample rate in Hz.
 * @param chunk_size Maximum number of frames per processing chunk.
 * @param proc_params Processing parameters.
 * @param err Pointer to a config error struct to populate on failure.
 * @return A pointer to the created filter instance, or `NULL` on failure.
 */
static void* biquad_filter_create(const char* name,
                                  const filter_config_t* config,
                                  int sample_rate, size_t chunk_size,
                                  processing_parameters_t* proc_params,
                                  config_error_t* err) {
  (void)chunk_size;
  (void)proc_params;
  const biquad_config_t* params = NULL;
  if (config) {
    if (config->type != FILTER_TYPE_BIQUAD) return NULL;
    params = &config->parameters.biquad;
    if (biquad_config_validate(config, sample_rate, err) != 0) return NULL;
  }
  biquad_filter_t* filter =
      (biquad_filter_t*)calloc(1, sizeof(biquad_filter_t));
  if (!filter) {
    config_error_set(err, CONFIG_ERR_PARSE,
                     "Failed to allocate biquad filter '%s'", name ? name : "");
    return NULL;
  }
  if (name) {
    strncpy(filter->name, name, sizeof(filter->name) - 1);
    filter->name[sizeof(filter->name) - 1] = '\0';
  } else {
    strcpy(filter->name, "biquad");
  }

  if (!params) {
    filter->type = (biquad_type_t)0;
    filter->coeffs = biquad_coefficients_passthrough();
  } else {
    filter->type = params->type;
    if (!biquad_coefficients_compute(params, sample_rate, &filter->coeffs)) {
      config_error_set(
          err, CONFIG_ERR_INVALID_FILTER,
          "Failed to compute coefficients or filter is unstable for '%s'",
          filter->name);
      biquad_filter_free(filter);
      return NULL;
    }
  }

  filter->z1 = 0.0;
  filter->z2 = 0.0;
  filter->neg_a1 = -filter->coeffs.a1;
  filter->neg_a2 = -filter->coeffs.a2;
  return filter;
}

/**
 * @brief Processes an array of samples through the biquad filter.
 *
 * In-place processing.
 *
 * @param filter The filter instance.
 * @param waveform The input/output waveform buffer.
 * @param count The number of samples to process.
 */
static void biquad_filter_process(void* instance, mutable_waveform_t waveform,
                                  size_t count) {
  biquad_filter_t* filter = (biquad_filter_t*)instance;
  if (!filter || !waveform || count == 0) return;

  // Direct Form II Transposed (DF2T) implementation, optimized with FMA.
  double b0 = filter->coeffs.b0;
  double b1 = filter->coeffs.b1;
  double b2 = filter->coeffs.b2;
  double neg_a1 = filter->neg_a1;
  double neg_a2 = filter->neg_a2;
  double z1 = filter->z1;
  double z2 = filter->z2;
  for (size_t i = 0; i < count; i++) {
    double in = waveform[i];
    double out = b0 * in + z1;
    double tmp = b1 * in + z2;
    z1 = neg_a1 * out + tmp;
    z2 = b2 * in + neg_a2 * out;
    waveform[i] = out;
  }
  if (fpclassify(z1) == FP_SUBNORMAL) z1 = 0.0;
  if (fpclassify(z2) == FP_SUBNORMAL) z2 = 0.0;
  filter->z1 = z1;
  filter->z2 = z2;
}

double biquad_filter_process_single(biquad_filter_t* filter, double sample) {
  if (!filter) return sample;

  double b0 = filter->coeffs.b0;
  double b1 = filter->coeffs.b1;
  double b2 = filter->coeffs.b2;
  double out = b0 * sample + filter->z1;
  double tmp = b1 * sample + filter->z2;
  filter->z1 = filter->neg_a1 * out + tmp;
  filter->z2 = b2 * sample + filter->neg_a2 * out;
  return out;
}

biquad_filter_t* biquad_filter_clone(const biquad_filter_t* src) {
  if (!src) return NULL;
  biquad_filter_t* dst = (biquad_filter_t*)calloc(1, sizeof(biquad_filter_t));
  if (!dst) return NULL;
  memcpy(dst, src, sizeof(biquad_filter_t));
  dst->z1 = 0.0;
  dst->z2 = 0.0;
  return dst;
}

// ============================================================================
// 2D Systolic Biquad Canon
// ============================================================================

#define BIQUAD_MAX_CHANNELS 4
#define BIQUAD_MAX_DEPTH 8
#define BIQUAD_WIDTH_BUDGET 8

static void biquad_choose_split(size_t channels, size_t depth,
                                size_t* out_group, size_t* out_stages) {
  size_t stages = depth;
  if (stages < 1) stages = 1;
  if (stages > BIQUAD_MAX_DEPTH) stages = BIQUAD_MAX_DEPTH;

  size_t group = channels;
  if (group < 1) group = 1;
  if (group > BIQUAD_MAX_CHANNELS) group = BIQUAD_MAX_CHANNELS;

  size_t budget_cap = BIQUAD_WIDTH_BUDGET / stages;
  if (budget_cap < 1) budget_cap = 1;
  if (group > budget_cap) group = budget_cap;

  if (out_group) *out_group = group;
  if (out_stages) *out_stages = stages;
}

#define CANON_STAGE(c, k, in_val)                    \
  do {                                               \
    double _in = (in_val);                           \
    double _out = b0[c][k] * _in + s1[c][k];         \
    double _tmp = b1[c][k] * _in + s2[c][k];         \
    s1[c][k] = neg_a1[c][k] * _out + _tmp;           \
    s2[c][k] = b2[c][k] * _in + neg_a2[c][k] * _out; \
    pipe[c][k] = _out;                               \
  } while (0)

#define DEFINE_2D_CANON_KERNEL(C, S)                                     \
  _Static_assert((S) >= 1, "biquad cascade depth S must be at least 1"); \
  _Static_assert((C) >= 1, "channel count C must be at least 1");        \
  static void biquad_canon_kernel_##C##_##S(                             \
      biquad_filter_t*** cascades, double** waveforms,                   \
      const size_t* channel_of, const size_t* members, size_t start,     \
      size_t n) {                                                        \
    double b0[C][S], b1[C][S], b2[C][S], neg_a1[C][S], neg_a2[C][S];     \
    double s1[C][S], s2[C][S];                                           \
    double* waves[C];                                                    \
    for (size_t c = 0; c < C; c++) {                                     \
      size_t mem = members[c];                                           \
      waves[c] = waveforms[channel_of[mem]];                             \
      for (size_t k = 0; k < S; k++) {                                   \
        biquad_filter_t* f = cascades[mem][start + k];                   \
        b0[c][k] = f->coeffs.b0;                                         \
        b1[c][k] = f->coeffs.b1;                                         \
        b2[c][k] = f->coeffs.b2;                                         \
        neg_a1[c][k] = f->neg_a1;                                        \
        neg_a2[c][k] = f->neg_a2;                                        \
        s1[c][k] = f->z1;                                                \
        s2[c][k] = f->z2;                                                \
      }                                                                  \
    }                                                                    \
    double pipe[C][S];                                                   \
    memset(pipe, 0, sizeof(pipe));                                       \
    size_t ramp = (S - 1 < n) ? (S - 1) : n;                             \
    for (size_t i = 0; i < ramp; i++) {                                  \
      for (size_t k = i; k >= 1; k--) {                                  \
        for (size_t c = 0; c < C; c++) {                                 \
          CANON_STAGE(c, k, pipe[c][k - 1]);                             \
        }                                                                \
      }                                                                  \
      for (size_t c = 0; c < C; c++) {                                   \
        CANON_STAGE(c, 0, waves[c][i]);                                  \
      }                                                                  \
    }                                                                    \
    for (size_t i = S - 1; i < n; i++) {                                 \
      for (size_t k = S - 1; k >= 1; k--) {                              \
        for (size_t c = 0; c < C; c++) {                                 \
          CANON_STAGE(c, k, pipe[c][k - 1]);                             \
        }                                                                \
      }                                                                  \
      for (size_t c = 0; c < C; c++) {                                   \
        CANON_STAGE(c, 0, waves[c][i]);                                  \
      }                                                                  \
      for (size_t c = 0; c < C; c++) {                                   \
        waves[c][i - (S - 1)] = pipe[c][S - 1];                          \
      }                                                                  \
    }                                                                    \
    for (size_t i = n; i < n + S - 1; i++) {                             \
      size_t first = i - n + 1;                                          \
      size_t last = (S - 1 < i) ? (S - 1) : i;                           \
      for (size_t k = last; k >= first; k--) {                           \
        for (size_t c = 0; c < C; c++) {                                 \
          CANON_STAGE(c, k, pipe[c][k - 1]);                             \
        }                                                                \
      }                                                                  \
      if (i + 1 >= S) {                                                  \
        for (size_t c = 0; c < C; c++) {                                 \
          waves[c][i + 1 - S] = pipe[c][S - 1];                          \
        }                                                                \
      }                                                                  \
    }                                                                    \
    for (size_t c = 0; c < C; c++) {                                     \
      size_t mem = members[c];                                           \
      for (size_t k = 0; k < S; k++) {                                   \
        if (fpclassify(s1[c][k]) == FP_SUBNORMAL) s1[c][k] = 0.0;        \
        if (fpclassify(s2[c][k]) == FP_SUBNORMAL) s2[c][k] = 0.0;        \
        cascades[mem][start + k]->z1 = s1[c][k];                         \
        cascades[mem][start + k]->z2 = s2[c][k];                         \
      }                                                                  \
    }                                                                    \
  }

#define DEFINE_ALL_DEPTHS(C)   \
  DEFINE_2D_CANON_KERNEL(C, 1) \
  DEFINE_2D_CANON_KERNEL(C, 2) \
  DEFINE_2D_CANON_KERNEL(C, 3) \
  DEFINE_2D_CANON_KERNEL(C, 4) \
  DEFINE_2D_CANON_KERNEL(C, 5) \
  DEFINE_2D_CANON_KERNEL(C, 6) \
  DEFINE_2D_CANON_KERNEL(C, 7) \
  DEFINE_2D_CANON_KERNEL(C, 8)

DEFINE_ALL_DEPTHS(1)
DEFINE_ALL_DEPTHS(2)
DEFINE_ALL_DEPTHS(3)
DEFINE_ALL_DEPTHS(4)

#undef DEFINE_ALL_DEPTHS

static void dispatch_pass(biquad_filter_t*** cascades, double** waveforms,
                          const size_t* channel_of, const size_t* members,
                          size_t members_count, size_t start, size_t depth,
                          size_t n_frames) {
#define DISPATCH_DEPTH(C)                                                   \
  switch (depth) {                                                          \
    case 1:                                                                 \
      biquad_canon_kernel_##C##_1(cascades, waveforms, channel_of, members, \
                                  start, n_frames);                         \
      break;                                                                \
    case 2:                                                                 \
      biquad_canon_kernel_##C##_2(cascades, waveforms, channel_of, members, \
                                  start, n_frames);                         \
      break;                                                                \
    case 3:                                                                 \
      biquad_canon_kernel_##C##_3(cascades, waveforms, channel_of, members, \
                                  start, n_frames);                         \
      break;                                                                \
    case 4:                                                                 \
      biquad_canon_kernel_##C##_4(cascades, waveforms, channel_of, members, \
                                  start, n_frames);                         \
      break;                                                                \
    case 5:                                                                 \
      biquad_canon_kernel_##C##_5(cascades, waveforms, channel_of, members, \
                                  start, n_frames);                         \
      break;                                                                \
    case 6:                                                                 \
      biquad_canon_kernel_##C##_6(cascades, waveforms, channel_of, members, \
                                  start, n_frames);                         \
      break;                                                                \
    case 7:                                                                 \
      biquad_canon_kernel_##C##_7(cascades, waveforms, channel_of, members, \
                                  start, n_frames);                         \
      break;                                                                \
    case 8:                                                                 \
      biquad_canon_kernel_##C##_8(cascades, waveforms, channel_of, members, \
                                  start, n_frames);                         \
      break;                                                                \
    default:                                                                \
      break;                                                                \
  }

  switch (members_count) {
    case 1:
      DISPATCH_DEPTH(1);
      break;
    case 2:
      DISPATCH_DEPTH(2);
      break;
    case 3:
      DISPATCH_DEPTH(3);
      break;
    case 4:
      DISPATCH_DEPTH(4);
      break;
    default:
      break;
  }
#undef DISPATCH_DEPTH
}

void biquad_process_cascades(biquad_filter_t*** cascades, double** waveforms,
                             const size_t* channel_of, const size_t* live,
                             size_t live_count, size_t cascade_depth,
                             size_t n_frames) {
  if (!cascades || !waveforms || !channel_of || !live || live_count == 0 ||
      cascade_depth == 0 || n_frames == 0)
    return;

  size_t depth = cascade_depth;
  size_t max_group, max_stages;
  biquad_choose_split(live_count, depth, &max_group, &max_stages);

  size_t start = 0;
  while (start < depth) {
    size_t take = depth - start;
    if (take > max_stages) take = max_stages;
    size_t group, dummy;
    biquad_choose_split(live_count, take, &group, &dummy);

    for (size_t m = 0; m < live_count; m += group) {
      size_t chunk_len = live_count - m;
      if (chunk_len > group) chunk_len = group;
      dispatch_pass(cascades, waveforms, channel_of, &live[m], chunk_len, start,
                    take, n_frames);
    }
    start += take;
  }
}

void biquad_process_mono_cascade(biquad_filter_t** stages, size_t num_stages,
                                 double* waveform, size_t n_frames) {
  if (!stages || num_stages == 0 || !waveform || n_frames == 0) return;

  size_t max_group, max_stages;
  biquad_choose_split(1, num_stages, &max_group, &max_stages);
  (void)max_group;

  biquad_filter_t** cascade_ptr = stages;
  biquad_filter_t*** cascades_wrap = &cascade_ptr;
  double* waveform_ptr = waveform;
  double** waveforms_wrap = &waveform_ptr;
  size_t channel_of[1] = {0};
  size_t members[1] = {0};

  size_t start = 0;
  while (start < num_stages) {
    size_t take = num_stages - start;
    if (take > max_stages) take = max_stages;
    dispatch_pass(cascades_wrap, waveforms_wrap, channel_of, members, 1, start,
                  take, n_frames);
    start += take;
  }
}

void biquad_filter_update_parameters(biquad_filter_t* filter,
                                     const filter_config_t* config,
                                     int sample_rate) {
  if (!filter || !config) return;
  if (config->type != FILTER_TYPE_BIQUAD) return;
  biquad_coefficients_t new_coeffs;
  if (biquad_coefficients_compute(&config->parameters.biquad, sample_rate,
                                  &new_coeffs)) {
    filter->coeffs = new_coeffs;
    filter->neg_a1 = -new_coeffs.a1;
    filter->neg_a2 = -new_coeffs.a2;
  }
}

/**
 * @brief Transfers internal history state (delay line registers) from src to
 * dest.
 *
 * @param dest The destination biquad filter instance.
 * @param src The source biquad filter instance.
 */
static void biquad_filter_transfer_state(void* dest_ptr, const void* src_ptr) {
  biquad_filter_t* dest = (biquad_filter_t*)dest_ptr;
  const biquad_filter_t* src = (const biquad_filter_t*)src_ptr;
  if (!dest || !src) return;

  if (dest->type != src->type) {
    dest->z1 = 0.0;
    dest->z2 = 0.0;
    return;
  }

  dest->z1 = src->z1;
  dest->z2 = src->z2;
}

const char* biquad_filter_get_name(const biquad_filter_t* filter) {
  return filter ? filter->name : NULL;
}

const filter_vtable_t g_biquad_vtable = {
    .validate = biquad_config_validate,
    .create = biquad_filter_create,
    .process = biquad_filter_process,
    .transfer_state = biquad_filter_transfer_state,
    .free = biquad_filter_free};
