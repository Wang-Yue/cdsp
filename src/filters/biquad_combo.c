#include "filters/biquad_combo.h"

#include <stdbool.h>
#include <stdio.h>

#include "audio/processing_parameters.h"
#include "config/config_error.h"
#include "config/config_gen.h"
#include "filters/biquad.h"
#include "filters/filter.h"
#include "utils/double_helpers.h"

struct biquad_combo_filter {
  char name[128];
  biquad_filter_t **sections;
  size_t num_sections;
  biquad_combo_type_t type; /**< Combo subtype the sections were built for. */
  size_t layout_count;      /**< Order (BW/LR), band count (GEQ/NPointPeq), or 2
                               (Tilt): what a section's index/name refers to. */
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
 * @brief GraphicEqualizer band edges, shared by validate and create.
 *
 * Matches upstream `GraphicEqualizerParameters::freq_min()/freq_max()`
 * (`map_or(20.0 / 20000.0)`): the configured value when present, else the
 * default. Validate and create must agree, so both use these.
 */
static inline double graphic_eq_freq_min(const biquad_combo_config_t *params) {
  return params->has_freq_min ? params->freq_min : 20.0;
}

static inline double graphic_eq_freq_max(const biquad_combo_config_t *params) {
  return params->has_freq_max ? params->freq_max : 20000.0;
}

/**
 * @brief Callback receiving one expanded section of a combo.
 *
 * @return 0 to continue, -1 to abort the expansion.
 */
typedef int (*combo_section_fn)(void *ctx, const char *sec_name,
                                const filter_config_t *section_cfg,
                                int sample_rate, config_error_t *err);

static filter_config_t make_section_cfg(biquad_type_t type, double freq,
                                        double q, double gain, double bandwidth,
                                        steepness_type_t steepness_type) {
  biquad_config_t bp = {.type = type,
                        .freq = freq,
                        .q = q,
                        .gain = gain,
                        .slope = 0.0,
                        .bandwidth = bandwidth,
                        .steepness_type = steepness_type};
  filter_config_t cfg = {.type = FILTER_TYPE_BIQUAD, .parameters.biquad = bp};
  return cfg;
}

/**
 * @brief Expands a combo into its biquad sections, in cascade order.
 *
 * Single source of truth for the section list, used both by validation (each
 * section is run through the biquad validator, so validate rejects exactly
 * what create would fail on) and by creation. Config-time only.
 *
 * @return 0 on success, -1 if allocation failed or `fn` aborted.
 */
static int combo_for_each_section(const biquad_combo_config_t *params,
                                  int sample_rate, combo_section_fn fn,
                                  void *ctx, config_error_t *err) {
  char name_buf[32];
  switch (params->type) {
  case BIQUAD_COMBO_TYPE_BUTTERWORTH_LOWPASS:
  case BIQUAD_COMBO_TYPE_BUTTERWORTH_HIGHPASS:
  case BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_LOWPASS:
  case BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_HIGHPASS: {
    bool bw = (params->type == BIQUAD_COMBO_TYPE_BUTTERWORTH_LOWPASS ||
               params->type == BIQUAD_COMBO_TYPE_BUTTERWORTH_HIGHPASS);
    bool hp = (params->type == BIQUAD_COMBO_TYPE_BUTTERWORTH_HIGHPASS ||
               params->type == BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_HIGHPASS);
    if (params->order <= 0)
      return -1;
    size_t q_capacity = (size_t)params->order + 2;
    double *q_vals = (double *)malloc(q_capacity * sizeof(double));
    if (!q_vals) {
      config_error_set(err, CONFIG_ERR_PARSE, "Failed to allocate memory");
      return -1;
    }
    size_t nq =
        bw ? biquad_combo_butterworth_q(params->order, q_vals, q_capacity)
           : biquad_combo_linkwitz_riley_q(params->order, q_vals, q_capacity);
    int rc = 0;
    for (size_t i = 0; i < nq && rc == 0; i++) {
      biquad_type_t t;
      double q = q_vals[i];
      if (q < 0.0) {
        // Butterworth odd order: the real pole is a first-order section.
        t = hp ? BIQUAD_TYPE_HIGHPASS_FO : BIQUAD_TYPE_LOWPASS_FO;
        q = 0.707;
      } else {
        t = hp ? BIQUAD_TYPE_HIGHPASS : BIQUAD_TYPE_LOWPASS;
      }
      snprintf(name_buf, sizeof(name_buf), "sec_%zu", i);
      filter_config_t cfg =
          make_section_cfg(t, params->freq, q, 0.0, 0.0, STEEPNESS_TYPE_Q);
      rc = fn(ctx, name_buf, &cfg, sample_rate, err);
    }
    free(q_vals);
    return rc;
  }
  // MARK: - Tilt EQ
  case BIQUAD_COMBO_TYPE_TILT: {
    // Upstream builds both shelves unconditionally, even when a shelf
    // frequency is at or above nyquist (sample rates <= 7000 Hz), which gives
    // a meaningless aliased section. cdsp leaves out a shelf that does not fit
    // below nyquist instead.
    double gain = params->has_gain ? params->gain : 0.0;
    double nyquist = (double)sample_rate / 2.0;
    if (110.0 < nyquist) {
      filter_config_t cfg =
          make_section_cfg(BIQUAD_TYPE_LOWSHELF, 110.0, 0.35, -gain / 2.0, 0.0,
                           STEEPNESS_TYPE_Q);
      if (fn(ctx, "low_shelf", &cfg, sample_rate, err) != 0)
        return -1;
    }
    if (3500.0 < nyquist) {
      filter_config_t cfg =
          make_section_cfg(BIQUAD_TYPE_HIGHSHELF, 3500.0, 0.35, gain / 2.0, 0.0,
                           STEEPNESS_TYPE_Q);
      if (fn(ctx, "high_shelf", &cfg, sample_rate, err) != 0)
        return -1;
    }
    return 0;
  }
  // MARK: - Graphic EQ
  case BIQUAD_COMBO_TYPE_GRAPHIC_EQUALIZER: {
    size_t nb = params->gains_count;
    if (nb == 0 || !params->gains)
      return 0;
    double log_min = log2(graphic_eq_freq_min(params));
    double log_max = log2(graphic_eq_freq_max(params));
    double bw = (log_max - log_min) / (double)nb;
    for (size_t i = 0; i < nb; i++) {
      double g = params->gains[i];
      if (fabs(g) <= 0.001)
        continue;
      double log_freq = log_min + ((double)i + 0.5) * bw;
      double f = pow(2.0, log_freq);
      snprintf(name_buf, sizeof(name_buf), "band_%zu", i);
      filter_config_t cfg = make_section_cfg(BIQUAD_TYPE_PEAKING, f, 0.0, g, bw,
                                             STEEPNESS_TYPE_BANDWIDTH);
      if (fn(ctx, name_buf, &cfg, sample_rate, err) != 0)
        return -1;
    }
    return 0;
  }
  // MARK: - N-Point PEQ
  case BIQUAD_COMBO_TYPE_N_POINT_PEQ: {
    if (!params->bands)
      return 0;
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
      snprintf(name_buf, sizeof(name_buf), "peq_%zu", i);
      filter_config_t cfg = make_section_cfg(btype, band->freq, band->q,
                                             band->gain, 0.0, STEEPNESS_TYPE_Q);
      if (fn(ctx, name_buf, &cfg, sample_rate, err) != 0)
        return -1;
    }
    return 0;
  }
  }
  return 0;
}

static int section_validate_cb(void *ctx, const char *sec_name,
                               const filter_config_t *section_cfg,
                               int sample_rate, config_error_t *err) {
  (void)ctx;
  (void)sec_name;
  return g_biquad_vtable.validate(section_cfg, sample_rate, err);
}

static int section_count_cb(void *ctx, const char *sec_name,
                            const filter_config_t *section_cfg, int sample_rate,
                            config_error_t *err) {
  (void)sec_name;
  (void)section_cfg;
  (void)sample_rate;
  (void)err;
  (*(size_t *)ctx)++;
  return 0;
}

typedef struct {
  biquad_filter_t **sections;
  size_t capacity;
  size_t count;
} section_sink_t;

static int section_create_cb(void *ctx, const char *sec_name,
                             const filter_config_t *section_cfg,
                             int sample_rate, config_error_t *err) {
  section_sink_t *sink = (section_sink_t *)ctx;
  if (sink->count >= sink->capacity)
    return -1;
  biquad_filter_t *sec = (biquad_filter_t *)g_biquad_vtable.create(
      sec_name, section_cfg, sample_rate, 0, NULL, err);
  if (!sec)
    return -1;
  sink->sections[sink->count++] = sec;
  return 0;
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
    double f_min = graphic_eq_freq_min(params);
    double f_max = graphic_eq_freq_max(params);
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
  // Expand into the biquad sections create will build and validate each one
  // (frequency vs nyquist, stability, finiteness), so validation rejects
  // exactly what creation would fail on. Upstream only does this for
  // NPointPeq because its stages() is infallible and never checks stability.
  if (combo_for_each_section(params, sample_rate, section_validate_cb, NULL,
                             err) != 0)
    return -1;
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
 * @brief What a section name refers to within a combo subtype.
 *
 * Section names encode the stage index (`sec_N`) or band index (`band_N`,
 * `peq_N`). They only identify the same stage/band of the same filter while
 * the order or band count is unchanged.
 */
static size_t combo_layout_count(const biquad_combo_config_t *params) {
  switch (params->type) {
  case BIQUAD_COMBO_TYPE_BUTTERWORTH_LOWPASS:
  case BIQUAD_COMBO_TYPE_BUTTERWORTH_HIGHPASS:
  case BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_LOWPASS:
  case BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_HIGHPASS:
    return params->order > 0 ? (size_t)params->order : 0;
  case BIQUAD_COMBO_TYPE_GRAPHIC_EQUALIZER:
    return params->gains_count;
  case BIQUAD_COMBO_TYPE_N_POINT_PEQ:
    return params->bands_count;
  case BIQUAD_COMBO_TYPE_TILT:
    return 2;
  }
  return 0;
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

  size_t total = 0;
  if (combo_for_each_section(params, sample_rate, section_count_cb, &total,
                             err) != 0) {
    biquad_combo_filter_free(filter);
    return NULL;
  }
  filter->sections = (biquad_filter_t **)calloc(total > 0 ? total : 1,
                                                sizeof(biquad_filter_t *));
  if (!filter->sections) {
    config_error_set(err, CONFIG_ERR_PARSE, "Failed to allocate memory");
    biquad_combo_filter_free(filter);
    return NULL;
  }
  section_sink_t sink = {
      .sections = filter->sections, .capacity = total, .count = 0};
  int rc = combo_for_each_section(params, sample_rate, section_create_cb, &sink,
                                  err);
  filter->num_sections = sink.count;
  if (rc != 0) {
    biquad_combo_filter_free(filter);
    return NULL;
  }
  filter->type = params->type;
  filter->layout_count = combo_layout_count(params);

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
 * Resize invariant: a section's name (`sec_N`, `band_N`, `peq_N`, `low_shelf`,
 * `high_shelf`) only refers to the same stage or band of the same filter while
 * the combo keeps its subtype and its order / band count. A changed subtype or
 * layout means the combo was reconfigured into a different filter, and the
 * per-section histories no longer describe any part of it — so nothing is
 * carried.
 *
 * Within an unchanged layout, state is carried between sections of the same
 * name, each transferred and scaled by `biquad_filter_transfer_state` based on
 * the ratio of ring estimates between the source and destination coefficients.
 * Matching by name rather than by position matters because flat
 * GraphicEqualizer / NPointPeq bands (|gain| <= 0.001 dB) are left out of the
 * cascade: flattening one band while boosting another keeps the section count
 * but shifts positions, and positional matching would pour one band's history
 * into an unrelated band. A section with no same-named source starts from
 * zero state, as upstream does for every reconfigured combo.
 *
 * Allocation-free.
 *
 * @param dest The destination combo filter instance.
 * @param src The source combo filter instance.
 */
static void biquad_combo_filter_transfer_state(void *dest_ptr,
                                               const void *src_ptr) {
  biquad_combo_filter_t *dest = (biquad_combo_filter_t *)dest_ptr;
  const biquad_combo_filter_t *src = (const biquad_combo_filter_t *)src_ptr;
  if (!dest || !src || dest == src || !g_biquad_vtable.transfer_state)
    return;
  if (dest->type != src->type || dest->layout_count != src->layout_count)
    return;
  // Sections are emitted in ascending index order in both, so a single forward
  // scan over src finds every match (O(n) overall).
  size_t j = 0;
  for (size_t i = 0; i < dest->num_sections; i++) {
    const char *dname = biquad_filter_get_name(dest->sections[i]);
    if (!dname)
      continue;
    for (size_t k = j; k < src->num_sections; k++) {
      const char *sname = biquad_filter_get_name(src->sections[k]);
      if (sname && strcmp(dname, sname) == 0) {
        g_biquad_vtable.transfer_state(dest->sections[i], src->sections[k]);
        j = k + 1;
        break;
      }
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
  if (count == 0) {
    // Zero stages (e.g. every GraphicEqualizer/NPointPeq band flat): return
    // NULL rather than an allocation a count-0 caller would never free.
    free(f->sections);
    free(f);
    return 0;
  }
  *out_stages = f->sections;
  free(f);
  return count;
}
