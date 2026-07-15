#include "biquad.h"

#ifdef ENABLE_ACCELERATE
#include <Accelerate/Accelerate.h>
#endif

/**
 * @struct biquad_coefficients_t
 * @brief Structure holding the transfer function coefficients of a biquad
 * filter.
 *
 * The transfer function is defined as:
 * \f$ H(z) = \frac{b_0 + b_1 z^{-1} + b_2 z^{-2}}{1 + a_1 z^{-1} + a_2 z^{-2}}
 * \f$
 */

typedef struct {
  double b0; /**< Numerator coefficient for \f$z^0\f$ */
  double b1; /**< Numerator coefficient for \f$z^{-1}\f$ */
  double b2; /**< Numerator coefficient for \f$z^{-2}\f$ */
  double a1; /**< Denominator coefficient for \f$z^{-1}\f$ */
  double a2; /**< Denominator coefficient for \f$z^{-2}\f$ */
} biquad_coefficients_t;

/**
 * @brief Returns coefficients for a passthrough filter (identity / no effect).
 *
 * @return Biquad coefficients representing an identity filter (b0=1, all others
 * 0).
 */
static inline biquad_coefficients_t biquad_coefficients_passthrough(void) {
  return (biquad_coefficients_t){1.0, 0.0, 0.0, 0.0, 0.0};
}

struct biquad_filter {
  char name[64];
  biquad_coefficients_t coeffs;
#ifdef ENABLE_ACCELERATE
  vDSP_biquadm_SetupD setup;
  double coeffs_array[5];
#else
  double z1, z2;
  double neg_a1, neg_a2;
#endif
};

#include <math.h>
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
    A = pow(10.0, gain / 40.0);

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
      double q_p =
          params->q_p > 0 ? params->q_p : (params->q > 0 ? params->q : 0.5);
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

biquad_filter_t* biquad_filter_create(const char* name,
                                      const biquad_config_t* params,
                                      int sample_rate, config_error_t* err) {
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
    filter->coeffs = biquad_coefficients_passthrough();
  } else {
    if (!biquad_coefficients_compute(params, sample_rate, &filter->coeffs)) {
      config_error_set(
          err, CONFIG_ERR_INVALID_FILTER,
          "Failed to compute coefficients or filter is unstable for '%s'",
          filter->name);
      biquad_filter_free(filter);
      return NULL;
    }
  }

#ifdef ENABLE_ACCELERATE
  filter->coeffs_array[0] = filter->coeffs.b0;
  filter->coeffs_array[1] = filter->coeffs.b1;
  filter->coeffs_array[2] = filter->coeffs.b2;
  filter->coeffs_array[3] = filter->coeffs.a1;
  filter->coeffs_array[4] = filter->coeffs.a2;
  filter->setup = vDSP_biquadm_CreateSetupD(filter->coeffs_array, 1, 1);
  if (!filter->setup) {
    config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                     "Failed to initialize vDSP biquad setup for filter '%s'",
                     filter->name);
    biquad_filter_free(filter);
    return NULL;
  }
#else
  filter->z1 = 0.0;
  filter->z2 = 0.0;
  filter->neg_a1 = -filter->coeffs.a1;
  filter->neg_a2 = -filter->coeffs.a2;
#endif
  return filter;
}

void biquad_filter_process(biquad_filter_t* filter, mutable_waveform_t waveform,
                           size_t count) {
  if (!filter || !waveform || count == 0) return;
#ifdef ENABLE_ACCELERATE
  if (!filter->setup) return;
  const double* signal_ptr = waveform;
  double* output_ptr = waveform;
  vDSP_biquadmD(filter->setup, &signal_ptr, 1, &output_ptr, 1, count);
#else
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
  if (!isnormal(z1)) z1 = 0.0;
  if (!isnormal(z2)) z2 = 0.0;
  filter->z1 = z1;
  filter->z2 = z2;
#endif
}

double biquad_filter_process_single(biquad_filter_t* filter, double sample) {
  if (!filter) return sample;
#ifdef ENABLE_ACCELERATE
  if (!filter->setup) return sample;
  double in_val = sample;
  double out_val = 0.0;
  const double* signal_ptr = &in_val;
  double* dest_ptr = &out_val;
  vDSP_biquadmD(filter->setup, &signal_ptr, 1, &dest_ptr, 1, 1);
  return out_val;
#else
  double b0 = filter->coeffs.b0;
  double b1 = filter->coeffs.b1;
  double b2 = filter->coeffs.b2;
  double out = b0 * sample + filter->z1;
  double tmp = b1 * sample + filter->z2;
  filter->z1 = filter->neg_a1 * out + tmp;
  filter->z2 = b2 * sample + filter->neg_a2 * out;
  if (!isnormal(filter->z1)) filter->z1 = 0.0;
  if (!isnormal(filter->z2)) filter->z2 = 0.0;
  return out;
#endif
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

#ifdef ENABLE_ACCELERATE
    if (filter->setup) {
      filter->coeffs_array[0] = new_coeffs.b0;
      filter->coeffs_array[1] = new_coeffs.b1;
      filter->coeffs_array[2] = new_coeffs.b2;
      filter->coeffs_array[3] = new_coeffs.a1;
      filter->coeffs_array[4] = new_coeffs.a2;
      vDSP_biquadm_SetCoefficientsDoubleD(filter->setup, filter->coeffs_array,
                                          0, 0, 1, 1);
    }
#else
    filter->neg_a1 = -new_coeffs.a1;
    filter->neg_a2 = -new_coeffs.a2;
#endif
  }
}

void biquad_filter_transfer_state(biquad_filter_t* dest,
                                  const biquad_filter_t* src) {
  if (!dest || !src) return;
#ifdef ENABLE_ACCELERATE
  if (dest->setup && src->setup) {
    vDSP_biquadm_CopyStateD(dest->setup, src->setup);
  }
#else
  dest->z1 = src->z1;
  dest->z2 = src->z2;
#endif
}

void biquad_filter_free(biquad_filter_t* filter) {
  if (!filter) return;
#ifdef ENABLE_ACCELERATE
  if (filter->setup) {
    vDSP_biquadm_DestroySetupD(filter->setup);
  }
#endif
  free(filter);
}

int biquad_config_validate(const biquad_config_t* params,
                            int sample_rate, config_error_t* err) {
  if (!params) return -1;
  double nyquist = (double)sample_rate / 2.0;
  if (params->type != BIQUAD_TYPE_FREE &&
      params->type != BIQUAD_TYPE_LINKWITZ_TRANSFORM &&
      params->type != BIQUAD_TYPE_GENERAL_NOTCH) {
    if (params->freq <= 0.0 || params->freq >= nyquist) {
      if (err)
        config_error_set(err, CONFIG_ERR_INVALID_FILTER, "freq out of range");
      return -1;
    }
  }
  if (params->type == BIQUAD_TYPE_GENERAL_NOTCH) {
    if (params->freq_notch <= 0.0 || params->freq_notch >= nyquist ||
        params->freq_pole <= 0.0 || params->freq_pole >= nyquist) {
      if (err)
        config_error_set(err, CONFIG_ERR_INVALID_FILTER, "freq out of range");
      return -1;
    }
    if (params->q_p <= 0.0) {
      if (err)
        config_error_set(err, CONFIG_ERR_INVALID_FILTER, "q out of range");
      return -1;
    }
  }
  if (params->type == BIQUAD_TYPE_LINKWITZ_TRANSFORM) {
    if (params->freq_act <= 0.0 || params->freq_act >= nyquist ||
        params->freq_target <= 0.0 || params->freq_target >= nyquist) {
      if (err)
        config_error_set(err, CONFIG_ERR_INVALID_FILTER, "freq out of range");
      return -1;
    }
    if (params->q_act <= 0.0 || params->q_target <= 0.0) {
      if (err)
        config_error_set(err, CONFIG_ERR_INVALID_FILTER, "q out of range");
      return -1;
    }
  }
  if (params->type == BIQUAD_TYPE_PEAKING ||
      params->type == BIQUAD_TYPE_LOWPASS ||
      params->type == BIQUAD_TYPE_HIGHPASS ||
      params->type == BIQUAD_TYPE_BANDPASS ||
      params->type == BIQUAD_TYPE_NOTCH ||
      params->type == BIQUAD_TYPE_ALLPASS ||
      params->type == BIQUAD_TYPE_GENERAL_NOTCH ||
      params->type == BIQUAD_TYPE_HIGHSHELF ||
      params->type == BIQUAD_TYPE_LOWSHELF) {
    if (params->q <= 0.0 && params->bandwidth <= 0.0 && params->slope <= 0.0) {
      if (err)
        config_error_set(err, CONFIG_ERR_INVALID_FILTER, "q out of range");
      return -1;
    }
  }
  if (params->type == BIQUAD_TYPE_HIGHSHELF ||
      params->type == BIQUAD_TYPE_LOWSHELF) {
    if (params->steepness_type == STEEPNESS_TYPE_SLOPE) {
      if (params->slope <= 0.0 || params->slope > 12.0) {
        if (err)
          config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                           "slope out of range");
        return -1;
      }
    }
  }
  if (sample_rate > 0) {
    if (!biquad_config_check_stability(params, sample_rate)) {
      if (err)
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "Unstable or invalid biquad filter specified");
      return -1;
    }
  }
  return 0;
}
