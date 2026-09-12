#include "Filters/diffeq.h"

#include "Audio/processing_parameters.h"
#include "Config/config_error.h"
#include "Config/filter_config_types.h"
#include "Filters/filter.h"
#include "Utils/double_helpers.h"

struct diffeq_filter {
  char name[64];
  double* s;    /**< Filter state, size = order */
  double* a;    /**< Feedback coefficients, a[0]=1.0, size = len */
  double* b;    /**< Feedforward coefficients, size = len */
  size_t order; /**< Filter order (len - 1) */
  size_t len;   /**< Maximum of a_count and b_count */
};

typedef struct diffeq_filter diffeq_filter_t;

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Check that the poles of the filter are inside the unit circle.
 *
 * Implements the Schur-Cohn step-down recursion test.
 * The denominator polynomial is peeled down one order at a time, and the
 * reflection coefficient of each step is the highest order coefficient of the
 * polynomial at that step. All poles are inside the unit circle if and only if
 * |reflection| < 1.0 at every step.
 *
 * @param a Array of scaled denominator coefficients with a[0] == 1.0.
 * @param count Number of coefficients in array a.
 * @return true if all poles are inside the unit circle, false otherwise.
 */
static bool poles_inside_unit_circle(const double* a, size_t count) {
  if (count <= 1) return true;
  double* coeffs = (double*)malloc(count * sizeof(double));
  if (!coeffs) return false;
  memcpy(coeffs, a, count * sizeof(double));
  double* prev = (double*)malloc(count * sizeof(double));
  if (!prev) {
    free(coeffs);
    return false;
  }

  bool stable = true;
  for (size_t order = count - 1; order >= 1; order--) {
    double reflection = coeffs[order];
    if (fabs(reflection) >= 1.0) {
      stable = false;
      break;
    }
    double scale = 1.0 - reflection * reflection;
    memcpy(prev, coeffs, (order + 1) * sizeof(double));
    for (size_t n = 1; n < order; n++) {
      coeffs[n] = (prev[n] - reflection * prev[order - n]) / scale;
    }
  }

  free(prev);
  free(coeffs);
  return stable;
}

/**
 * @brief Free the difference equation filter instance and its associated
 * resources.
 *
 * @param instance Pointer to diffeq_filter_t instance.
 */
static void diffeq_filter_free(void* instance) {
  diffeq_filter_t* filter = (diffeq_filter_t*)instance;
  if (!filter) return;
  if (filter->s) free(filter->s);
  if (filter->a) free(filter->a);
  if (filter->b) free(filter->b);
  free(filter);
}

/**
 * @brief Validates difference equation filter parameters.
 *
 * @param config Pointer to the difference equation configuration to validate.
 * @param sample_rate The sample rate (unused).
 * @param err Pointer to a config error struct to populate on failure.
 * @return 0 on success, -1 on failure.
 */
static int diffeq_config_validate(const filter_config_t* config,
                                  int sample_rate, config_error_t* err) {
  (void)sample_rate;
  if (!config || config->type != FILTER_TYPE_DIFF_EQ) return -1;
  const diffeq_config_t* params = &config->parameters.diff_eq;
  if (!params) return 0;

  if (params->a && params->a_count > 0) {
    for (size_t i = 0; i < params->a_count; i++) {
      if (!isfinite(params->a[i])) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "All coefficients must be finite numbers");
        return -1;
      }
    }
  }
  if (params->b && params->b_count > 0) {
    for (size_t i = 0; i < params->b_count; i++) {
      if (!isfinite(params->b[i])) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "All coefficients must be finite numbers");
        return -1;
      }
    }
  }

  if (!params->a || params->a_count == 0) {
    // Defaults to a single unity coefficient, which gives a stable FIR filter.
    return 0;
  }

  if (params->a[0] == 0.0) {
    config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                     "The first 'a' coefficient must not be zero");
    return -1;
  }

  double a0 = params->a[0];
  double* scaled = (double*)malloc(params->a_count * sizeof(double));
  if (!scaled) {
    config_error_set(err, CONFIG_ERR_PARSE, "Allocation failure");
    return -1;
  }
  for (size_t i = 0; i < params->a_count; i++) {
    scaled[i] = params->a[i] / a0;
  }

  bool stable = poles_inside_unit_circle(scaled, params->a_count);
  free(scaled);

  if (!stable) {
    config_error_set(
        err, CONFIG_ERR_INVALID_FILTER,
        "Unstable filter, the 'a' coefficients give poles on or outside the "
        "unit circle");
    return -1;
  }

  return 0;
}

/**
 * @brief Create a new difference equation filter.
 *
 * @param name The name of the filter.
 * @param config The difference equation configuration.
 * @param sample_rate The sample rate (unused).
 * @param chunk_size Maximum number of frames per processing chunk (unused).
 * @param proc_params Processing parameters (unused).
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
  size_t len = a_cnt > b_cnt ? a_cnt : b_cnt;

  filter->len = len;
  filter->order = (len > 0) ? len - 1 : 0;

  filter->a = (double*)calloc(len, sizeof(double));
  filter->b = (double*)calloc(len, sizeof(double));
  if (filter->order > 0) {
    filter->s = (double*)calloc(filter->order, sizeof(double));
  }

  if (!filter->a || !filter->b || (filter->order > 0 && !filter->s)) {
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

  // Normalize coefficients by a[0] so a[0] becomes 1.0
  double a0 = filter->a[0];
  if (isfinite(a0) && a0 != 0.0 && a0 != 1.0) {
    double scale = 1.0 / a0;
    for (size_t i = 0; i < len; i++) {
      filter->a[i] *= scale;
      filter->b[i] *= scale;
    }
  }

  return filter;
}

#define DEFINE_DIFFEQ_ORDER_KERNEL(ORDER)                                   \
  static void diffeq_process_block_##ORDER(                                 \
      diffeq_filter_t* filter, mutable_waveform_t waveform, size_t count) { \
    double s[ORDER];                                                        \
    memcpy(s, filter->s, ORDER * sizeof(double));                           \
    const double* a = &filter->a[1];                                        \
    const double* b = &filter->b[1];                                        \
    double b0 = filter->b[0];                                               \
    for (size_t i = 0; i < count; i++) {                                    \
      double input = waveform[i];                                           \
      double out = b0 * input + s[0];                                       \
      for (size_t n = 0; n + 1 < ORDER; n++) {                              \
        s[n] = b[n] * input - a[n] * out + s[n + 1];                        \
      }                                                                     \
      s[ORDER - 1] = b[ORDER - 1] * input - a[ORDER - 1] * out;             \
      waveform[i] = out;                                                    \
    }                                                                       \
    memcpy(filter->s, s, ORDER * sizeof(double));                           \
  }

DEFINE_DIFFEQ_ORDER_KERNEL(1)
DEFINE_DIFFEQ_ORDER_KERNEL(2)
DEFINE_DIFFEQ_ORDER_KERNEL(3)
DEFINE_DIFFEQ_ORDER_KERNEL(4)
DEFINE_DIFFEQ_ORDER_KERNEL(5)
DEFINE_DIFFEQ_ORDER_KERNEL(6)
DEFINE_DIFFEQ_ORDER_KERNEL(7)
DEFINE_DIFFEQ_ORDER_KERNEL(8)

static void diffeq_process_block_any(diffeq_filter_t* filter,
                                     mutable_waveform_t waveform,
                                     size_t count) {
  size_t order = filter->order;
  double* s = filter->s;
  const double* a = &filter->a[1];
  const double* b = &filter->b[1];
  double b0 = filter->b[0];
  for (size_t i = 0; i < count; i++) {
    double input = waveform[i];
    double out = b0 * input + s[0];
    for (size_t n = 0; n < order - 1; n++) {
      s[n] = b[n] * input - a[n] * out + s[n + 1];
    }
    s[order - 1] = b[order - 1] * input - a[order - 1] * out;
    waveform[i] = out;
  }
}

/**
 * @brief Process a block of samples in-place using Direct Form 2 Transposed.
 *
 * @param instance The difference equation filter instance.
 * @param waveform The input/output waveform buffer.
 * @param count The number of samples to process.
 */
static void diffeq_filter_process(void* instance, mutable_waveform_t waveform,
                                  size_t count) {
  diffeq_filter_t* filter = (diffeq_filter_t*)instance;
  if (!filter || !waveform || count == 0) return;

  switch (filter->order) {
    case 0: {
      double b0 = filter->b[0];
      for (size_t i = 0; i < count; i++) {
        waveform[i] *= b0;
      }
      break;
    }
    case 1:
      diffeq_process_block_1(filter, waveform, count);
      break;
    case 2:
      diffeq_process_block_2(filter, waveform, count);
      break;
    case 3:
      diffeq_process_block_3(filter, waveform, count);
      break;
    case 4:
      diffeq_process_block_4(filter, waveform, count);
      break;
    case 5:
      diffeq_process_block_5(filter, waveform, count);
      break;
    case 6:
      diffeq_process_block_6(filter, waveform, count);
      break;
    case 7:
      diffeq_process_block_7(filter, waveform, count);
      break;
    case 8:
      diffeq_process_block_8(filter, waveform, count);
      break;
    default:
      diffeq_process_block_any(filter, waveform, count);
      break;
  }

  // Flush subnormals
  for (size_t k = 0; k < filter->order; k++) {
    if (fpclassify(filter->s[k]) == FP_SUBNORMAL) {
      filter->s[k] = 0.0;
    }
  }
}

static void diffeq_filter_transfer_state(void* dest_ptr, const void* src_ptr) {
  diffeq_filter_t* dest = (diffeq_filter_t*)dest_ptr;
  const diffeq_filter_t* src = (const diffeq_filter_t*)src_ptr;
  if (!dest || !src || dest == src) return;

  if (dest->s && src->s && dest->order > 0 && src->order > 0) {
    size_t copy_len = dest->order < src->order ? dest->order : src->order;
    memcpy(dest->s, src->s, copy_len * sizeof(double));
    if (dest->order > copy_len) {
      memset(&dest->s[copy_len], 0, (dest->order - copy_len) * sizeof(double));
    }
  }
}

const filter_vtable_t g_diffeq_vtable = {
    .validate = diffeq_config_validate,
    .create = diffeq_filter_create,
    .process = diffeq_filter_process,
    .transfer_state = diffeq_filter_transfer_state,
    .free = diffeq_filter_free};
