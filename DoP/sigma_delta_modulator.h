/**
 * @file sigma_delta_modulator.h
 * @brief Sigma-delta modulator for DSD oversampling.
 */

#ifndef CLIB_DOP_SIGMA_DELTA_MODULATOR_H
#define CLIB_DOP_SIGMA_DELTA_MODULATOR_H

#include <stdint.h>

#include "Config/engine_config_types.h"

/**
 * @brief Sigma-delta modulator state.
 *
 * Heap-backed fixed storage for the modulator state.
 * `non_trellis_state[0..7]` is slot 0, `non_trellis_state[8..15]` is slot 1;
 * `idx` selects which slot is current.
 *
 * `cached_a` and `cached_g` mirror the selected filter's coefficients
 * to avoid re-copying the filter structure in the hot loop.
 */
struct sigma_delta_modulator {
  /** Index of the current state slot (0 or 1). */
  int idx;
  /** Previous output value. */
  double prev_y;
  /** State storage for the filter (two slots of 8 doubles each). */
  double non_trellis_state[16];
  /** Cached 'a' coefficients of the filter. */
  double cached_a[8];
  /** Cached 'g' coefficients of the filter. */
  double cached_g[8];
  /** Cached filter order. */
  int cached_order;
  /** Name of the filter. */
  sdm_filter_t name;
  /** Sampling frequency. */
  uint32_t freq;
};

typedef struct sigma_delta_modulator sigma_delta_modulator_t;

/**
 * @brief Create a sigma-delta modulator.
 *
 * @param filter_name Name of the filter to use.
 * @param freq Sampling frequency.
 * @return Pointer to the created modulator instance, or NULL on failure.
 */
sigma_delta_modulator_t* sigma_delta_modulator_create(sdm_filter_t filter_name,
                                                      uint32_t freq);

/**
 * @brief Initialize a sigma-delta modulator instance.
 *
 * @param mod Pointer to the modulator instance to initialize.
 * @param filter_name Name of the filter to use.
 * @param freq Sampling frequency.
 */
void sigma_delta_modulator_init(sigma_delta_modulator_t* mod,
                                sdm_filter_t filter_name, uint32_t freq);

/**
 * @brief Process a single sample through the modulator (inlined for performance).
 *
 * @param mod Pointer to the modulator instance.
 * @param x Input sample.
 * @return True if modulated DSD bit is high (1), false if low (0).
 */
static inline bool sigma_delta_modulator_sample(sigma_delta_modulator_t* mod,
                                                double x) {
  if (!mod) return false;
  int current_idx = mod->idx;
  double* s = &mod->non_trellis_state[current_idx * 8];
  double* d = &mod->non_trellis_state[(current_idx ^ 1) * 8];
  const double* a = mod->cached_a;
  const double* g = mod->cached_g;
  double y = mod->prev_y;
  bool bit;

  if (mod->cached_order == 6) {
    d[0] = s[0] - g[0] * s[1] + x - y;
    double v = x + a[0] * d[0];
    d[1] = s[1] + s[0] - g[1] * s[2];
    v += a[1] * d[1];
    d[2] = s[2] + s[1] - g[2] * s[3];
    v += a[2] * d[2];
    d[3] = s[3] + s[2] - g[3] * s[4];
    v += a[3] * d[3];
    d[4] = s[4] + s[3] - g[4] * s[5];
    v += a[4] * d[4];
    d[5] = s[5] + s[4];
    v += a[5] * d[5];
    bit = (v >= 0.0);
  } else if (mod->cached_order == 4) {
    d[0] = s[0] - g[0] * s[1] + x - y;
    double v = x + a[0] * d[0];
    d[1] = s[1] + s[0] - g[1] * s[2];
    v += a[1] * d[1];
    d[2] = s[2] + s[1] - g[2] * s[3];
    v += a[2] * d[2];
    d[3] = s[3] + s[2];
    v += a[3] * d[3];
    bit = (v >= 0.0);
  } else if (mod->cached_order == 5) {
    d[0] = s[0] - g[0] * s[1] + x - y;
    double v = x + a[0] * d[0];
    d[1] = s[1] + s[0] - g[1] * s[2];
    v += a[1] * d[1];
    d[2] = s[2] + s[1] - g[2] * s[3];
    v += a[2] * d[2];
    d[3] = s[3] + s[2] - g[3] * s[4];
    v += a[3] * d[3];
    d[4] = s[4] + s[3];
    v += a[4] * d[4];
    bit = (v >= 0.0);
  } else if (mod->cached_order == 7) {
    d[0] = s[0] - g[0] * s[1] + x - y;
    double v = x + a[0] * d[0];
    d[1] = s[1] + s[0] - g[1] * s[2];
    v += a[1] * d[1];
    d[2] = s[2] + s[1] - g[2] * s[3];
    v += a[2] * d[2];
    d[3] = s[3] + s[2] - g[3] * s[4];
    v += a[3] * d[3];
    d[4] = s[4] + s[3] - g[4] * s[5];
    v += a[4] * d[4];
    d[5] = s[5] + s[4] - g[5] * s[6];
    v += a[5] * d[5];
    d[6] = s[6] + s[5];
    v += a[6] * d[6];
    bit = (v >= 0.0);
  } else if (mod->cached_order == 8) {
    d[0] = s[0] - g[0] * s[1] + x - y;
    double v = x + a[0] * d[0];
    d[1] = s[1] + s[0] - g[1] * s[2];
    v += a[1] * d[1];
    d[2] = s[2] + s[1] - g[2] * s[3];
    v += a[2] * d[2];
    d[3] = s[3] + s[2] - g[3] * s[4];
    v += a[3] * d[3];
    d[4] = s[4] + s[3] - g[4] * s[5];
    v += a[4] * d[4];
    d[5] = s[5] + s[4] - g[5] * s[6];
    v += a[5] * d[5];
    d[6] = s[6] + s[5] - g[6] * s[7];
    v += a[6] * d[6];
    d[7] = s[7] + s[6];
    v += a[7] * d[7];
    bit = (v >= 0.0);
  } else {
    d[0] = s[0] - g[0] * s[1] + x - y;
    double v = x + a[0] * d[0];
    int i = 1;
    while (i < mod->cached_order - 1) {
      d[i] = s[i] + s[i - 1] - g[i] * s[i + 1];
      v += a[i] * d[i];
      i++;
    }
    d[i] = s[i] + s[i - 1];
    v += a[i] * d[i];
    bit = (v >= 0.0);
  }

  mod->idx = current_idx ^ 1;
  mod->prev_y = bit ? 1.0 : -1.0;
  return bit;
}

/**
 * @brief Free the sigma-delta modulator.
 *
 * @param mod Pointer to the modulator instance to free.
 */
void sigma_delta_modulator_free(sigma_delta_modulator_t* mod);

#endif  // CLIB_DOP_SIGMA_DELTA_MODULATOR_H
