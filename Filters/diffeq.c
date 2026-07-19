#include "diffeq.h"

#include "filter.h"

struct diffeq_filter {
  char name[64];
  double* x;
  double* y;
  double* a;
  double* b;
  size_t a_count;
  size_t b_count;
  size_t idx_x;
  size_t idx_y;
};

typedef struct diffeq_filter diffeq_filter_t;

#include <math.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Free the difference equation filter instance and its associated
 * resources.
 *
 * @param filter The difference equation filter instance to free.
 */
static void diffeq_filter_free(void* instance) {
  diffeq_filter_t* filter = (diffeq_filter_t*)instance;
  if (!filter) return;
  if (filter->x) free(filter->x);
  if (filter->y) free(filter->y);
  if (filter->a) free(filter->a);
  if (filter->b) free(filter->b);
  free(filter);
}

/**
 * @brief Validates difference equation filter parameters.
 *
 * @param config Pointer to the difference equation configuration to validate.
 * @param sample_rate The sample rate.
 * @param err Pointer to a config error struct to populate on failure.
 * @return 0 on success, -1 on failure.
 */
static int diffeq_config_validate(const filter_config_t* config,
                                  int sample_rate, config_error_t* err) {
  (void)sample_rate;
  if (!config || config->type != FILTER_TYPE_DIFF_EQ) return -1;
  const diffeq_config_t* params = &config->parameters.diff_eq;
  if (params && params->a && params->a_count > 0) {
    if (params->a[0] == 0.0 || !isfinite(params->a[0])) {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                       "DiffEq filter a[0] must be non-zero and finite");
      return -1;
    }
  }
  return 0;
}

/**
 * @brief Create a new difference equation filter.
 *
 * @param name The name of the filter.
 * @param config The difference equation configuration.
 * @param sample_rate The sample rate.
 * @param chunk_size Maximum number of frames per processing chunk.
 * @param proc_params Processing parameters.
 * @param err Optional pointer to receive configuration error detail on failure.
 * @return A pointer to the created difference equation filter, or NULL on
 * failure.
 */
static void* diffeq_filter_create(const char* name,
                                  const filter_config_t* config,
                                  int sample_rate, size_t chunk_size,
                                  processing_parameters_t* proc_params,
                                  config_error_t* err) {
  (void)sample_rate;
  (void)chunk_size;
  (void)proc_params;
  if (!config || config->type != FILTER_TYPE_DIFF_EQ) return NULL;
  const diffeq_config_t* params = &config->parameters.diff_eq;
  if (diffeq_config_validate(config, 0, err) != 0) return NULL;
  diffeq_filter_t* filter =
      (diffeq_filter_t*)calloc(1, sizeof(diffeq_filter_t));
  if (!filter) return NULL;
  if (name) {
    strncpy(filter->name, name, sizeof(filter->name) - 1);
    filter->name[sizeof(filter->name) - 1] = '\0';
  } else {
    strcpy(filter->name, "diffeq");
  }

  size_t a_cnt =
      (params && params->a && params->a_count > 0) ? params->a_count : 1;
  size_t b_cnt =
      (params && params->b && params->b_count > 0) ? params->b_count : 1;

  filter->a_count = a_cnt;
  filter->b_count = b_cnt;

  filter->a = (double*)calloc(a_cnt, sizeof(double));
  filter->b = (double*)calloc(b_cnt, sizeof(double));
  filter->x = (double*)calloc(b_cnt, sizeof(double));
  filter->y = (double*)calloc(a_cnt, sizeof(double));

  if (!filter->a || !filter->b || !filter->x || !filter->y) {
    diffeq_filter_free(filter);
    return NULL;
  }

  if (params && params->a && params->a_count > 0) {
    memcpy(filter->a, params->a, a_cnt * sizeof(double));
  } else {
    filter->a[0] = 1.0;
  }
  if (params && params->b && params->b_count > 0) {
    memcpy(filter->b, params->b, b_cnt * sizeof(double));
  } else {
    filter->b[0] = 1.0;
  }

  // Normalize by a[0]
  if (filter->a[0] != 0.0 && filter->a[0] != 1.0) {
    double scale = 1.0 / filter->a[0];
    for (size_t i = 0; i < a_cnt; i++) filter->a[i] *= scale;
    for (size_t i = 0; i < b_cnt; i++) filter->b[i] *= scale;
  }

  filter->idx_x = 0;
  filter->idx_y = 0;
  return filter;
}

/**
 * @brief Process a block of samples in-place.
 *
 * @param filter The difference equation filter instance.
 * @param waveform The input/output waveform buffer.
 * @param count The number of samples to process.
 */
static void diffeq_filter_process(void* instance,
                                  mutable_waveform_t waveform, size_t count) {
  diffeq_filter_t* filter = (diffeq_filter_t*)instance;
  if (!filter || !waveform || count == 0) return;
  size_t nb = filter->b_count;
  size_t na = filter->a_count;
  double* x = filter->x;
  double* y = filter->y;
  const double* a = filter->a;
  const double* b = filter->b;
  size_t idx_x = filter->idx_x;
  size_t idx_y = filter->idx_y;

  // Process each sample through the difference equation:
  // y[n] = b[0]*x[n] + b[1]*x[n-1] + ... + b[N]*x[n-N] - a[1]*y[n-1] - ... -
  // a[M]*y[n-M] x and y are implemented as circular buffers to store historical
  // samples.
  for (size_t i = 0; i < count; i++) {
    // Advance circular buffer write indices
    idx_x++;
    if (idx_x >= nb) idx_x = 0;
    idx_y++;
    if (idx_y >= na) idx_y = 0;

    // Store current input sample
    x[idx_x] = waveform[i];

    double out = 0.0;
    // Compute feedforward part: sum(b[n] * x[n-i])
    int ptr_x = (int)idx_x;
    for (size_t n = 0; n < nb; n++) {
      out += b[n] * x[ptr_x];
      ptr_x--;
      if (ptr_x < 0) ptr_x = (int)nb - 1;
    }
    // Compute feedback part: sum(a[p] * y[p-j])
    int ptr_y = (int)idx_y - 1;
    if (ptr_y < 0) ptr_y = (int)na - 1;
    for (size_t p = 1; p < na; p++) {
      out -= a[p] * y[ptr_y];
      ptr_y--;
      if (ptr_y < 0) ptr_y = (int)na - 1;
    }

    // Store current output sample and update waveform
    y[idx_y] = out;
    waveform[i] = out;
  }

  for (size_t k = 0; k < nb; k++) {
    if (!isnormal(x[k])) x[k] = 0.0;
  }
  for (size_t k = 0; k < na; k++) {
    if (!isnormal(y[k])) y[k] = 0.0;
  }

  filter->idx_x = idx_x;
  filter->idx_y = idx_y;
}

static void diffeq_filter_transfer_state(void* dest_ptr,
                                          const void* src_ptr) {
  diffeq_filter_t* dest = (diffeq_filter_t*)dest_ptr;
  const diffeq_filter_t* src = (const diffeq_filter_t*)src_ptr;
  if (!dest || !src || dest == src) return;

  // Transfer input history x
  if (dest->x && dest->b_count > 0 && src->x && src->b_count > 0) {
    size_t dest_bc = dest->b_count;
    size_t src_bc = src->b_count;
    size_t copy_len = dest_bc < src_bc ? dest_bc : src_bc;

    memset(dest->x, 0, dest_bc * sizeof(double));

    size_t src_start_idx = (src->idx_x + src_bc - copy_len + 1) % src_bc;
    size_t dest_start_idx = dest_bc - copy_len;

    for (size_t i = 0; i < copy_len; i++) {
      size_t src_idx = (src_start_idx + i) % src_bc;
      size_t dest_idx = dest_start_idx + i;
      dest->x[dest_idx] = src->x[src_idx];
    }
    dest->idx_x = dest_bc - 1;
  }

  // Transfer output history y
  if (dest->y && dest->a_count > 0 && src->y && src->a_count > 0) {
    size_t dest_ac = dest->a_count;
    size_t src_ac = src->a_count;
    size_t copy_len = dest_ac < src_ac ? dest_ac : src_ac;

    memset(dest->y, 0, dest_ac * sizeof(double));

    size_t src_start_idx = (src->idx_y + src_ac - copy_len + 1) % src_ac;
    size_t dest_start_idx = dest_ac - copy_len;

    for (size_t i = 0; i < copy_len; i++) {
      size_t src_idx = (src_start_idx + i) % src_ac;
      size_t dest_idx = dest_start_idx + i;
      dest->y[dest_idx] = src->y[src_idx];
    }
    dest->idx_y = dest_ac - 1;
  }
}

const filter_vtable_t g_diffeq_vtable = {
    .validate = diffeq_config_validate,
    .create = diffeq_filter_create,
    .process = diffeq_filter_process,
    .transfer_state = diffeq_filter_transfer_state,
    .free = diffeq_filter_free};
