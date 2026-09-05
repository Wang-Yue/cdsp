/**
 * @file biquad_canon.c
 * @brief Implementation of the internal biquad canon cascade filter.
 */

#include "Filters/biquad_canon.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "Filters/biquad.h"
#include "Filters/biquad_internal.h"
#include "Filters/filter.h"

struct biquad_canon_filter {
  char name[64];
  biquad_filter_t** sections;
  size_t num_sections;
  bool owns_sections;
};

typedef struct biquad_canon_filter biquad_canon_filter_t;

#define CANON_STAGE(k, in_val)              \
  do {                                      \
    double _in = (in_val);                  \
    double _out = b0[k] * _in + z1[k];      \
    double _tmp = b1[k] * _in + z2[k];      \
    z1[k] = neg_a1[k] * _out + _tmp;        \
    z2[k] = b2[k] * _in + neg_a2[k] * _out; \
    pipe[k] = _out;                         \
  } while (0)

#define DEFINE_CANON_KERNEL(S)                                                 \
  static void biquad_canon_kernel_##S(biquad_filter_t** filters, size_t start, \
                                      double* waveform, size_t n) {            \
    double b0[S], b1[S], b2[S], neg_a1[S], neg_a2[S], z1[S], z2[S];            \
    for (size_t k = 0; k < S; k++) {                                           \
      biquad_filter_t* f = filters[start + k];                                 \
      b0[k] = f->coeffs.b0;                                                    \
      b1[k] = f->coeffs.b1;                                                    \
      b2[k] = f->coeffs.b2;                                                    \
      neg_a1[k] = f->neg_a1;                                                   \
      neg_a2[k] = f->neg_a2;                                                   \
      z1[k] = f->z1;                                                           \
      z2[k] = f->z2;                                                           \
    }                                                                          \
    double pipe[S];                                                            \
    memset(pipe, 0, sizeof(pipe));                                             \
    size_t ramp = (S - 1 < n) ? (S - 1) : n;                                   \
    for (size_t i = 0; i < ramp; i++) {                                        \
      for (size_t k = i; k >= 1; k--) {                                        \
        CANON_STAGE(k, pipe[k - 1]);                                           \
      }                                                                        \
      CANON_STAGE(0, waveform[i]);                                             \
    }                                                                          \
    for (size_t i = S - 1; i < n; i++) {                                       \
      for (size_t k = S - 1; k >= 1; k--) {                                    \
        CANON_STAGE(k, pipe[k - 1]);                                           \
      }                                                                        \
      CANON_STAGE(0, waveform[i]);                                             \
      waveform[i - (S - 1)] = pipe[S - 1];                                     \
    }                                                                          \
    for (size_t i = n; i < n + S - 1; i++) {                                   \
      size_t first = i - n + 1;                                                \
      size_t last = (S - 1 < i) ? (S - 1) : i;                                 \
      for (size_t k = last; k >= first; k--) {                                 \
        CANON_STAGE(k, pipe[k - 1]);                                           \
      }                                                                        \
      if (i + 1 >= S) {                                                        \
        waveform[i - (S - 1)] = pipe[S - 1];                                   \
      }                                                                        \
    }                                                                          \
    for (size_t k = 0; k < S; k++) {                                           \
      if (fpclassify(z1[k]) == FP_SUBNORMAL) z1[k] = 0.0;                      \
      if (fpclassify(z2[k]) == FP_SUBNORMAL) z2[k] = 0.0;                      \
      filters[start + k]->z1 = z1[k];                                          \
      filters[start + k]->z2 = z2[k];                                          \
    }                                                                          \
  }

DEFINE_CANON_KERNEL(1)
DEFINE_CANON_KERNEL(2)
DEFINE_CANON_KERNEL(3)
DEFINE_CANON_KERNEL(4)
DEFINE_CANON_KERNEL(5)
DEFINE_CANON_KERNEL(6)
DEFINE_CANON_KERNEL(7)
DEFINE_CANON_KERNEL(8)

static void biquad_cascade_canon_process(biquad_filter_t** filters,
                                         size_t num_filters,
                                         mutable_waveform_t waveform,
                                         size_t count) {
  if (!filters || num_filters == 0 || !waveform || count == 0) return;
  size_t start = 0;
  while (start < num_filters) {
    size_t take = num_filters - start;
    if (take > 8) take = 8;
    switch (take) {
      case 8:
        biquad_canon_kernel_8(filters, start, waveform, count);
        break;
      case 7:
        biquad_canon_kernel_7(filters, start, waveform, count);
        break;
      case 6:
        biquad_canon_kernel_6(filters, start, waveform, count);
        break;
      case 5:
        biquad_canon_kernel_5(filters, start, waveform, count);
        break;
      case 4:
        biquad_canon_kernel_4(filters, start, waveform, count);
        break;
      case 3:
        biquad_canon_kernel_3(filters, start, waveform, count);
        break;
      case 2:
        biquad_canon_kernel_2(filters, start, waveform, count);
        break;
      case 1:
        biquad_canon_kernel_1(filters, start, waveform, count);
        break;
    }
    start += take;
  }
}

static void biquad_canon_filter_process(void* instance,
                                        mutable_waveform_t waveform,
                                        size_t count) {
  biquad_canon_filter_t* filter = (biquad_canon_filter_t*)instance;
  if (!filter || !waveform || count == 0) return;
  biquad_cascade_canon_process(filter->sections, filter->num_sections, waveform,
                               count);
}

static void biquad_canon_filter_transfer_state(void* dest_ptr,
                                               const void* src_ptr) {
  biquad_canon_filter_t* dest = (biquad_canon_filter_t*)dest_ptr;
  const biquad_canon_filter_t* src = (const biquad_canon_filter_t*)src_ptr;
  if (!dest || !src) return;

  for (size_t i = 0; i < dest->num_sections; i++) {
    const char* dest_name = biquad_filter_get_name(dest->sections[i]);
    if (!dest_name) continue;
    for (size_t j = 0; j < src->num_sections; j++) {
      const char* src_name = biquad_filter_get_name(src->sections[j]);
      if (src_name && strcmp(dest_name, src_name) == 0) {
        g_biquad_vtable.transfer_state(dest->sections[i], src->sections[j]);
        break;
      }
    }
  }
}

static void biquad_canon_filter_free(void* instance) {
  biquad_canon_filter_t* filter = (biquad_canon_filter_t*)instance;
  if (!filter) return;

  if (filter->owns_sections && filter->sections) {
    for (size_t i = 0; i < filter->num_sections; i++) {
      if (filter->sections[i]) {
        g_biquad_vtable.free(filter->sections[i]);
      }
    }
  }
  if (filter->sections) {
    free(filter->sections);
  }
  free(filter);
}

static int biquad_canon_filter_validate(const filter_config_t* config,
                                        int sample_rate, config_error_t* err) {
  (void)sample_rate;
  if (!config) {
    if (err) config_error_set(err, CONFIG_ERR_INVALID_FILTER, "Null config");
    return -1;
  }
  if (config->type != FILTER_TYPE_BIQUAD_CANON) {
    if (err)
      config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                       "Invalid filter type for BiquadCanon");
    return -1;
  }
  const biquad_canon_config_t* cfg = &config->parameters.biquad_canon;
  if (cfg->num_sections > 0 && !cfg->sections) {
    if (err)
      config_error_set(err, CONFIG_ERR_INVALID_FILTER, "Null sections array");
    return -1;
  }
  return 0;
}

static void* biquad_canon_filter_create(const char* name,
                                        const filter_config_t* config,
                                        int sample_rate, size_t chunk_size,
                                        processing_parameters_t* proc_params,
                                        config_error_t* err) {
  (void)sample_rate;
  (void)chunk_size;
  (void)proc_params;
  if (biquad_canon_filter_validate(config, sample_rate, err) != 0) {
    return NULL;
  }
  const biquad_canon_config_t* cfg = &config->parameters.biquad_canon;

  biquad_canon_filter_t* canon =
      (biquad_canon_filter_t*)calloc(1, sizeof(biquad_canon_filter_t));
  if (!canon) {
    if (err) config_error_set(err, CONFIG_ERR_PARSE, "Allocation failure");
    return NULL;
  }

  if (name && name[0] != '\0') {
    strncpy(canon->name, name, sizeof(canon->name) - 1);
  } else {
    strncpy(canon->name, "biquad_canon", sizeof(canon->name) - 1);
  }

  canon->num_sections = cfg->num_sections;
  canon->owns_sections = cfg->owns_sections;

  if (cfg->num_sections > 0) {
    canon->sections =
        (biquad_filter_t**)calloc(cfg->num_sections, sizeof(biquad_filter_t*));
    if (!canon->sections) {
      if (err) config_error_set(err, CONFIG_ERR_PARSE, "Allocation failure");
      free(canon);
      return NULL;
    }
    for (size_t i = 0; i < cfg->num_sections; i++) {
      canon->sections[i] = cfg->sections[i];
    }
  }

  return canon;
}

const filter_vtable_t g_biquad_canon_vtable = {
    .validate = biquad_canon_filter_validate,
    .create = biquad_canon_filter_create,
    .process = biquad_canon_filter_process,
    .transfer_state = biquad_canon_filter_transfer_state,
    .free = biquad_canon_filter_free,
};
