#ifndef CLIB_FILTERS_BIQUAD_INTERNAL_H
#define CLIB_FILTERS_BIQUAD_INTERNAL_H

/**
 * @file biquad_internal.h
 * @brief Internal structural definitions for biquad filter instances.
 */

#include "Config/filter_config_types.h"
#include "Filters/biquad.h"

typedef struct {
  double b0;
  double b1;
  double b2;
  double a1;
  double a2;
} biquad_coefficients_t;

struct biquad_filter {
  char name[64];
  biquad_type_t type;
  biquad_coefficients_t coeffs;
  double z1, z2;
  double neg_a1, neg_a2;
};

#endif  // CLIB_FILTERS_BIQUAD_INTERNAL_H
