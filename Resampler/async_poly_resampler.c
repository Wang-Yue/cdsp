// Asynchronous polynomial resampler.
//
// Same buffer layout, same `last_index`
// semantics, same `t_ratio` accumulation, same Newton-form polynomial
// formulas — output samples agree bit-for-bit.
//
// No anti-aliasing; for
// quality use `AsyncSincResampler`.
//
// Memory: every internal buffer is sized at init based on `chunkSize` and
// `maxRelativeRatio`. There is **no** dynamic allocation on the hot path.

#include "async_poly_resampler.h"

#include <string.h>
#include <strings.h>

#include "Audio/audio_chunk.h"
#include "audio_resampler.h"
#include "resampler_error.h"

typedef enum {
  POLY_INTERPOLATION_LINEAR = 0,
  POLY_INTERPOLATION_CUBIC,
  POLY_INTERPOLATION_QUINTIC,
  POLY_INTERPOLATION_SEPTIC,
  POLY_INTERPOLATION_LAST
} poly_interpolation_t;

static poly_interpolation_t poly_interpolation_from_string(const char* str) {
  if (!str) return POLY_INTERPOLATION_CUBIC;
  if (strcasecmp(str, "Linear") == 0) return POLY_INTERPOLATION_LINEAR;
  if (strcasecmp(str, "Cubic") == 0) return POLY_INTERPOLATION_CUBIC;
  if (strcasecmp(str, "Quintic") == 0) return POLY_INTERPOLATION_QUINTIC;
  if (strcasecmp(str, "Septic") == 0) return POLY_INTERPOLATION_SEPTIC;
  return POLY_INTERPOLATION_LAST;
}

static inline int poly_interpolation_nbr_points(poly_interpolation_t interp) {
  switch (interp) {
    case POLY_INTERPOLATION_LINEAR:
      return 2;
    case POLY_INTERPOLATION_CUBIC:
      return 4;
    case POLY_INTERPOLATION_QUINTIC:
      return 6;
    case POLY_INTERPOLATION_SEPTIC:
      return 8;
    default:
      return 4;
  }
}

static void* async_poly_resampler_create_from_profile(
    size_t channels, size_t input_rate, size_t output_rate,
    resampler_profile_t profile, size_t chunk_size, double max_relative_ratio,
    fixed_async_t fixed, config_error_t* err);

typedef struct async_poly_resampler async_poly_resampler_t;

struct async_poly_resampler {
  size_t channels;
  size_t chunk_size;
  poly_interpolation_t interpolation;
  size_t interpolator_len;  // = nbr_points
  // Ratio bookkeeping.
  double base_ratio;
  double resample_ratio;
  double target_ratio;
  double max_relative_ratio;
  double last_index;  // tracking index
  // Per-channel input buffer. Layout:
  //   [0 .. 2*nbr_points)            — history padding zone
  //   [2*nbr_points .. 2*nbr_points+chunkSize) — current chunk
  audio_buffers_t* input_buffer;
  // Pre-allocated per-frame scratch. `start_idx_scratch` holds the integer
  // floor of `idx`, computed once when `frac_scratch` is built — saving the
  // inner loops a floor() + int cast per output frame.
  int* start_idx_scratch;
  double* frac_scratch;
  size_t max_output_frames;
  fixed_async_t fixed;
  size_t needed_input_size;
  size_t needed_output_size;
  size_t current_buffer_fill;
  size_t max_input_frames;
};

#include <math.h>
#include <stdlib.h>
#include <string.h>

static inline size_t calculate_input_size(
    size_t chunk_size, double resample_ratio, double target_ratio,
    double last_index, size_t interpolator_len, fixed_async_t fixed) {
  if (fixed == FIXED_ASYNC_INPUT) {
    return chunk_size;
  }
  double inv_r1 = 1.0 / resample_ratio;
  double inv_r2 = 1.0 / target_ratio;
  double avg_t_ratio = 0.5 * (inv_r1 + inv_r2);
  double raw =
      last_index + (double)chunk_size * avg_t_ratio + (double)interpolator_len;
  return (size_t)ceil(raw);
}

static inline size_t calculate_output_size(
    size_t chunk_size, double resample_ratio, double target_ratio,
    double last_index, size_t interpolator_len, fixed_async_t fixed) {
  if (fixed == FIXED_ASYNC_OUTPUT) {
    return chunk_size;
  }
  double inv_r1 = 1.0 / resample_ratio;
  double inv_r2 = 1.0 / target_ratio;
  double avg_t_ratio = 0.5 * (inv_r1 + inv_r2);
  double raw =
      ((double)chunk_size - (double)(interpolator_len + 1) - last_index) /
      avg_t_ratio;
  return (size_t)floor(raw);
}

static void async_poly_resampler_update_lengths(
    async_poly_resampler_t* resampler) {
  resampler->needed_input_size = calculate_input_size(
      resampler->chunk_size, resampler->resample_ratio, resampler->target_ratio,
      resampler->last_index, resampler->interpolator_len, resampler->fixed);
  resampler->needed_output_size = calculate_output_size(
      resampler->chunk_size, resampler->resample_ratio, resampler->target_ratio,
      resampler->last_index, resampler->interpolator_len, resampler->fixed);
}

static void async_poly_resampler_free(void* impl) {
  async_poly_resampler_t* resampler = (async_poly_resampler_t*)impl;
  if (!resampler) return;
  if (resampler->input_buffer) audio_buffers_free(resampler->input_buffer);
  free(resampler->start_idx_scratch);
  free(resampler->frac_scratch);
  free(resampler);
}

static void async_poly_resampler_set_relative_ratio(void* impl,
                                                    double multiplier) {
  async_poly_resampler_t* resampler = (async_poly_resampler_t*)impl;
  if (!resampler) return;
  double min_ratio = 1.0 / resampler->max_relative_ratio;
  if (multiplier < min_ratio) multiplier = min_ratio;
  if (multiplier > resampler->max_relative_ratio)
    multiplier = resampler->max_relative_ratio;
  resampler->target_ratio = resampler->base_ratio * multiplier;
  async_poly_resampler_update_lengths(resampler);
}

static double async_poly_resampler_get_ratio(const void* impl) {
  const async_poly_resampler_t* resampler = (const async_poly_resampler_t*)impl;
  return resampler ? resampler->resample_ratio : 1.0;
}

static size_t async_poly_resampler_get_max_output_frames(const void* impl) {
  const async_poly_resampler_t* resampler = (const async_poly_resampler_t*)impl;
  return resampler ? resampler->max_output_frames : 0;
}

static size_t async_poly_resampler_get_chunk_size(const void* impl) {
  const async_poly_resampler_t* resampler = (const async_poly_resampler_t*)impl;
  return resampler ? resampler->chunk_size : 0;
}

static size_t async_poly_resampler_get_channels(const void* impl) {
  const async_poly_resampler_t* resampler = (const async_poly_resampler_t*)impl;
  return resampler ? resampler->channels : 0;
}

/**
 * @brief Performs linear polynomial interpolation on input buffer.
 *
 * Loops over channels and output frames, interpolating between the two nearest
 * samples.
 *
 * @param resampler Pointer to the resampler instance.
 * @param output_frames Number of output frames to generate.
 * @param output Output chunk to store interpolated samples.
 */
static void run_linear(async_poly_resampler_t* resampler, size_t output_frames,
                       audio_chunk_t* output) {
  size_t n_len = resampler->interpolator_len;
  size_t two_n_len = 2 * n_len;
  const int* idx_buf = resampler->start_idx_scratch;
  const double* frac_buf = resampler->frac_scratch;

  for (size_t ch = 0; ch < resampler->channels; ch++) {
    const double* buf = audio_buffers_get_channel(resampler->input_buffer, ch);
    double* out = audio_chunk_get_channel(output, ch);
    if (!buf || !out) continue;
    for (size_t frame = 0; frame < output_frames; frame++) {
      double x = frac_buf[frame];
      const double* base = buf + idx_buf[frame] + two_n_len;
      double y0 = base[0];
      double y1 = base[1];
      out[frame] = y0 + x * (y1 - y0);
    }
  }
}

/**
 * @brief Performs cubic polynomial interpolation (4-point Lagrange/Newton
 * form).
 *
 * @param resampler Pointer to the resampler instance.
 * @param output_frames Number of output frames to generate.
 * @param output Output chunk to store interpolated samples.
 */
static void run_cubic(async_poly_resampler_t* resampler, size_t output_frames,
                      audio_chunk_t* output) {
  size_t n_len = resampler->interpolator_len;  // 4
  size_t base_offset = 2 * n_len - 1;          // 7
  const int* idx_buf = resampler->start_idx_scratch;
  const double* frac_buf = resampler->frac_scratch;

  for (size_t ch = 0; ch < resampler->channels; ch++) {
    const double* buf = audio_buffers_get_channel(resampler->input_buffer, ch);
    double* out = audio_chunk_get_channel(output, ch);
    if (!buf || !out) continue;
    for (size_t frame = 0; frame < output_frames; frame++) {
      double x = frac_buf[frame];
      const double* base = buf + idx_buf[frame] + base_offset;
      double y0 = base[0];
      double y1 = base[1];
      double y2 = base[2];
      double y3 = base[3];

      double a0 = y1;
      double a1 = -1.0 / 3.0 * y0 - 0.5 * y1 + y2 - 1.0 / 6.0 * y3;
      double a2 = 0.5 * (y0 + y2) - y1;
      double a3 = 0.5 * (y1 - y2) + 1.0 / 6.0 * (y3 - y0);

      out[frame] = a0 + x * (a1 + x * (a2 + x * a3));
    }
  }
}

/**
 * @brief Performs quintic polynomial interpolation (6-point Lagrange/Newton
 * form).
 *
 * @param resampler Pointer to the resampler instance.
 * @param output_frames Number of output frames to generate.
 * @param output Output chunk to store interpolated samples.
 */
static void run_quintic(async_poly_resampler_t* resampler, size_t output_frames,
                        audio_chunk_t* output) {
  size_t n_len = resampler->interpolator_len;  // 6
  size_t base_offset = 2 * n_len - 2;          // 10
  const int* idx_buf = resampler->start_idx_scratch;
  const double* frac_buf = resampler->frac_scratch;

  for (size_t ch = 0; ch < resampler->channels; ch++) {
    const double* buf = audio_buffers_get_channel(resampler->input_buffer, ch);
    double* out = audio_chunk_get_channel(output, ch);
    if (!buf || !out) continue;
    for (size_t frame = 0; frame < output_frames; frame++) {
      double x = frac_buf[frame];
      const double* base = buf + idx_buf[frame] + base_offset;
      double a = base[0];
      double b = base[1];
      double c = base[2];
      double d = base[3];
      double e = base[4];
      double f = base[5];

      double k5 = -a + 5.0 * b - 10.0 * c + 10.0 * d - 5.0 * e + f;
      double k4 = 5.0 * a - 20.0 * b + 30.0 * c - 20.0 * d + 5.0 * e;
      double k3 = -5.0 * a - 5.0 * b + 50.0 * c - 70.0 * d + 35.0 * e - 5.0 * f;
      double k2 = -5.0 * a + 80.0 * b - 150.0 * c + 80.0 * d - 5.0 * e;
      double k1 =
          6.0 * a - 60.0 * b - 40.0 * c + 120.0 * d - 30.0 * e + 4.0 * f;
      double k0 = 120.0 * c;

      double val = k0 + x * (k1 + x * (k2 + x * (k3 + x * (k4 + x * k5))));

      out[frame] = (1.0 / 120.0) * val;
    }
  }
}

/**
 * @brief Performs septic polynomial interpolation (8-point Lagrange/Newton
 * form).
 *
 * Uses Horner's method to evaluate the polynomial coefficients efficiently.
 *
 * @param resampler Pointer to the resampler instance.
 * @param output_frames Number of output frames to generate.
 * @param output Output chunk to store interpolated samples.
 */
static void run_septic(async_poly_resampler_t* resampler, size_t output_frames,
                       audio_chunk_t* output) {
  size_t n_len = resampler->interpolator_len;  // 8
  size_t base_offset = 2 * n_len - 3;          // 13
  const int* idx_buf = resampler->start_idx_scratch;
  const double* frac_buf = resampler->frac_scratch;

  for (size_t ch = 0; ch < resampler->channels; ch++) {
    const double* buf = audio_buffers_get_channel(resampler->input_buffer, ch);
    double* out = audio_chunk_get_channel(output, ch);
    if (!buf || !out) continue;
    for (size_t frame = 0; frame < output_frames; frame++) {
      double x = frac_buf[frame];
      const double* base = buf + idx_buf[frame] + base_offset;
      double a = base[0];
      double b = base[1];
      double c = base[2];
      double d = base[3];
      double e = base[4];
      double f = base[5];
      double g = base[6];
      double h = base[7];

      double k7 = -a + 7.0 * b - 21.0 * c + 35.0 * d - 35.0 * e + 21.0 * f -
                  7.0 * g + h;
      double k6 = 7.0 * a - 42.0 * b + 105.0 * c - 140.0 * d + 105.0 * e -
                  42.0 * f + 7.0 * g;
      double k5 = -7.0 * a - 14.0 * b + 189.0 * c - 490.0 * d + 595.0 * e -
                  378.0 * f + 119.0 * g - 14.0 * h;
      double k4 = -35.0 * a + 420.0 * b - 1365.0 * c + 1960.0 * d - 1365.0 * e +
                  420.0 * f - 35.0 * g;
      double k3 = 56.0 * a - 497.0 * b + 336.0 * c + 1715.0 * d - 3080.0 * e +
                  1869.0 * f - 448.0 * g + 49.0 * h;
      double k2 = 28.0 * a - 378.0 * b + 3780.0 * c - 6860.0 * d + 3780.0 * e -
                  378.0 * f + 28.0 * g;
      double k1 = -48.0 * a + 504.0 * b - 3024.0 * c - 1260.0 * d + 5040.0 * e -
                  1512.0 * f + 336.0 * g - 36.0 * h;
      double k0 = 5040.0 * d;

      // Horner's method
      double val =
          k0 +
          x * (k1 +
               x * (k2 + x * (k3 + x * (k4 + x * (k5 + x * (k6 + x * k7))))));

      out[frame] = (1.0 / 5040.0) * val;
    }
  }
}

static resampler_error_t async_poly_resampler_process(
    void* impl, const audio_chunk_t* input, audio_chunk_t* output) {
  async_poly_resampler_t* resampler = (async_poly_resampler_t*)impl;
  if (!resampler || !input || !output) return RESAMPLER_ERR_INVALID_PARAMETER;
  size_t valid_frames = audio_chunk_get_valid_frames(input);
  if (valid_frames > resampler->max_input_frames ||
      resampler->needed_input_size > resampler->max_input_frames) {
    return RESAMPLER_ERR_INPUT_SIZE_MISMATCH;
  }
  if (audio_chunk_get_channels(input) != resampler->channels) {
    return RESAMPLER_ERR_CHANNEL_COUNT_MISMATCH;
  }
  if (audio_chunk_get_channels(output) != resampler->channels) {
    return RESAMPLER_ERR_CHANNEL_COUNT_MISMATCH;
  }
  size_t output_frames = resampler->needed_output_size;
  if (output_frames > resampler->max_output_frames) {
    output_frames = resampler->max_output_frames;
  }

  size_t n_len = resampler->interpolator_len;
  size_t two_n_len = 2 * n_len;

  // Shift buffer + write new chunk wait-free and crash-safe.
  for (size_t ch = 0; ch < resampler->channels; ch++) {
    double* base = audio_buffers_get_channel(resampler->input_buffer, ch);
    if (!base) continue;
    memmove(base, base + resampler->current_buffer_fill,
            two_n_len * sizeof(double));
  }
  for (size_t ch = 0; ch < resampler->channels; ch++) {
    const double* src_ptr = audio_chunk_get_channel(input, ch);
    double* dst_ptr = audio_buffers_get_channel(resampler->input_buffer, ch);
    if (!src_ptr || !dst_ptr) continue;
    memcpy(dst_ptr + two_n_len, src_ptr, valid_frames * sizeof(double));
    if (valid_frames < resampler->needed_input_size) {
      memset(dst_ptr + two_n_len + valid_frames, 0,
             (resampler->needed_input_size - valid_frames) * sizeof(double));
    }
  }

  resampler->current_buffer_fill = resampler->needed_input_size;

  if (output_frames == 0) {
    resampler->last_index -= (double)resampler->needed_input_size;
    resampler->resample_ratio = resampler->target_ratio;
    async_poly_resampler_update_lengths(resampler);
    audio_chunk_set_valid_frames(output, 0);
    return RESAMPLER_OK;
  }

  if (audio_chunk_get_frames(output) < output_frames) {
    return RESAMPLER_ERR_OUTPUT_BUFFER_TOO_SMALL;
  }

  double t_ratio_start = 1.0 / resampler->resample_ratio;
  double t_ratio_end = 1.0 / resampler->target_ratio;
  double t_ratio_increment =
      (t_ratio_end - t_ratio_start) / (double)output_frames;

  double t_ratio = t_ratio_start;
  double idx = resampler->last_index;
  for (size_t frame = 0; frame < output_frames; frame++) {
    t_ratio += t_ratio_increment;
    idx += t_ratio;
    double idx_floor = floor(idx);
    resampler->start_idx_scratch[frame] = (int)idx_floor;
    resampler->frac_scratch[frame] = idx - idx_floor;
  }
  double final_idx = idx;

  switch (resampler->interpolation) {
    case POLY_INTERPOLATION_LINEAR:
      run_linear(resampler, output_frames, output);
      break;
    case POLY_INTERPOLATION_CUBIC:
      run_cubic(resampler, output_frames, output);
      break;
    case POLY_INTERPOLATION_QUINTIC:
      run_quintic(resampler, output_frames, output);
      break;
    case POLY_INTERPOLATION_SEPTIC:
      run_septic(resampler, output_frames, output);
      break;
    default:
      run_cubic(resampler, output_frames, output);
      break;
  }

  resampler->last_index = final_idx - (double)resampler->needed_input_size;
  resampler->resample_ratio = resampler->target_ratio;

  size_t prev_needed_input_size = resampler->needed_input_size;
  async_poly_resampler_update_lengths(resampler);

  size_t valid_out = (output_frames * valid_frames) / prev_needed_input_size;
  audio_chunk_set_valid_frames(output, valid_out);
  return RESAMPLER_OK;
}

static size_t async_poly_resampler_get_input_frames_next(const void* impl) {
  const async_poly_resampler_t* resampler = (const async_poly_resampler_t*)impl;
  return resampler ? resampler->needed_input_size : 0;
}

static size_t async_poly_resampler_get_output_frames_next(const void* impl) {
  const async_poly_resampler_t* resampler = (const async_poly_resampler_t*)impl;
  return resampler ? resampler->needed_output_size : 0;
}

static void* async_poly_resampler_create_impl(
    size_t channels, size_t input_rate, size_t output_rate,
    poly_interpolation_t interpolation, size_t chunk_size,
    double max_relative_ratio, fixed_async_t fixed, config_error_t* err) {
  if (channels == 0) {
    config_error_set(err, CONFIG_ERR_VALIDATION,
                     "AsyncPolyResampler: channels must be positive");
    return NULL;
  }
  if (chunk_size == 0) {
    config_error_set(err, CONFIG_ERR_VALIDATION,
                     "AsyncPolyResampler: chunk_size must be positive");
    return NULL;
  }
  if (input_rate == 0 || output_rate == 0) {
    config_error_set(err, CONFIG_ERR_VALIDATION,
                     "AsyncPolyResampler: rates must be positive");
    return NULL;
  }
  if (max_relative_ratio < 1.0) max_relative_ratio = 1.1;

  async_poly_resampler_t* resampler =
      (async_poly_resampler_t*)calloc(1, sizeof(async_poly_resampler_t));
  if (!resampler) {
    config_error_set(err, CONFIG_ERR_PARSE,
                     "Failed to allocate AsyncPolyResampler");
    return NULL;
  }

  resampler->channels = channels;
  resampler->chunk_size = chunk_size;
  resampler->fixed = fixed;
  resampler->interpolation = interpolation;
  resampler->interpolator_len =
      (size_t)poly_interpolation_nbr_points(interpolation);
  resampler->base_ratio = (double)output_rate / (double)input_rate;
  resampler->resample_ratio = resampler->base_ratio;
  resampler->target_ratio = resampler->base_ratio;
  resampler->max_relative_ratio = max_relative_ratio;
  resampler->last_index = -((double)resampler->interpolator_len / 2.0);

  double min_ratio_abs = resampler->base_ratio / max_relative_ratio;
  if (fixed == FIXED_ASYNC_INPUT) {
    resampler->max_input_frames = chunk_size;
  } else {
    double raw_max_in = ((double)chunk_size) / min_ratio_abs + 2.0 +
                        (double)resampler->interpolator_len / 2.0;
    resampler->max_input_frames = (size_t)ceil(raw_max_in) + 16;
  }

  if (resampler->max_input_frames >
      SIZE_MAX - 2 * resampler->interpolator_len) {
    config_error_set(err, CONFIG_ERR_VALIDATION,
                     "AsyncPolyResampler: max_input_frames is out of bounds");
    async_poly_resampler_free(resampler);
    return NULL;
  }

  size_t buf_len =
      resampler->max_input_frames + 2 * resampler->interpolator_len;
  resampler->input_buffer = audio_buffers_create(channels, buf_len);
  if (!resampler->input_buffer) {
    config_error_set(err, CONFIG_ERR_PARSE,
                     "Failed to allocate AsyncPolyResampler input buffer");
    async_poly_resampler_free(resampler);
    return NULL;
  }

  async_poly_resampler_update_lengths(resampler);
  resampler->current_buffer_fill = resampler->needed_input_size;

  if (fixed == FIXED_ASYNC_OUTPUT) {
    resampler->max_output_frames = chunk_size;
  } else {
    double most_neg_last_index = -((double)resampler->interpolator_len / 2.0);
    double max_ratio_abs = resampler->base_ratio * max_relative_ratio;
    double raw_max =
        ((double)chunk_size - (double)(resampler->interpolator_len + 1) -
         most_neg_last_index) *
        max_ratio_abs;

    if (isnan(raw_max) || isinf(raw_max) || raw_max < 0.0 ||
        raw_max > (double)(SIZE_MAX - 32)) {
      config_error_set(
          err, CONFIG_ERR_VALIDATION,
          "AsyncPolyResampler: calculated maximum output size is invalid");
      async_poly_resampler_free(resampler);
      return NULL;
    }
    resampler->max_output_frames = (size_t)(ceil(raw_max)) + 16;
  }
  resampler->start_idx_scratch =
      (int*)calloc(resampler->max_output_frames, sizeof(int));
  resampler->frac_scratch =
      (double*)calloc(resampler->max_output_frames, sizeof(double));
  if (!resampler->start_idx_scratch || !resampler->frac_scratch) {
    config_error_set(err, CONFIG_ERR_PARSE,
                     "Failed to allocate AsyncPolyResampler scratch buffers");
    async_poly_resampler_free(resampler);
    return NULL;
  }

  return resampler;
}

/**
 * @brief Validates async polynomial resampler parameters.
 *
 * @param config Pointer to the resampler configuration to validate.
 * @param err Pointer to a config error struct to populate on failure.
 * @return 0 on success, -1 on failure.
 */
static int async_poly_resampler_config_validate(
    const resampler_config_t* config, config_error_t* err) {
  if (!config || config->type != RESAMPLER_TYPE_ASYNC_POLY) return -1;
  if (config->has_interpolation) {
    if (poly_interpolation_from_string(config->interpolation) ==
        POLY_INTERPOLATION_LAST) {
      config_error_set(err, CONFIG_ERR_VALIDATION,
                       "AsyncPoly: invalid interpolation type %s",
                       config->interpolation);
      return -1;
    }
  }
  if (config->has_profile) {
    if (strcasecmp(config->profile, "VeryFast") != 0 &&
        strcasecmp(config->profile, "Fast") != 0 &&
        strcasecmp(config->profile, "Balanced") != 0 &&
        strcasecmp(config->profile, "Accurate") != 0) {
      config_error_set(err, CONFIG_ERR_VALIDATION,
                       "AsyncPoly: invalid profile %s", config->profile);
      return -1;
    }
  }
  return 0;
}

/**
 * @brief Creates a new async polynomial resampler.
 *
 * @param config Resampler configuration parameters.
 * @param input_rate Input sample rate in Hz.
 * @param output_rate Output sample rate in Hz.
 * @param channels Number of audio channels.
 * @param chunk_size Fixed number of input/output frames per processing chunk.
 * @param err Pointer to a config error struct to populate on failure.
 * @return Pointer to newly allocated audio_resampler_t wrapper, or NULL on
 * failure.
 */
static void* async_poly_resampler_create(const resampler_config_t* config,
                                         size_t input_rate, size_t output_rate,
                                         size_t channels, size_t chunk_size,
                                         config_error_t* err) {
  if (!config || config->type != RESAMPLER_TYPE_ASYNC_POLY) return NULL;

  fixed_async_t fixed_mode = FIXED_ASYNC_INPUT;
  if (config->has_interpolation) {
    poly_interpolation_t interp =
        poly_interpolation_from_string(config->interpolation);
    return async_poly_resampler_create_impl(channels, input_rate, output_rate,
                                            interp, chunk_size, 1.1, fixed_mode,
                                            err);
  } else {
    resampler_profile_t prof = RESAMPLER_PROFILE_BALANCED;
    if (config->has_profile) {
      prof = resampler_profile_from_string(config->profile);
    }
    return async_poly_resampler_create_from_profile(
        channels, input_rate, output_rate, prof, chunk_size, 1.1, fixed_mode,
        err);
  }
}

const resampler_vtable_t g_async_poly_resampler_vtable = {
    .validate = async_poly_resampler_config_validate,
    .create = async_poly_resampler_create,
    .process = async_poly_resampler_process,
    .set_relative_ratio = async_poly_resampler_set_relative_ratio,
    .get_ratio = async_poly_resampler_get_ratio,
    .get_max_output_frames = async_poly_resampler_get_max_output_frames,
    .get_chunk_size = async_poly_resampler_get_chunk_size,
    .get_input_frames_next = async_poly_resampler_get_input_frames_next,
    .get_output_frames_next = async_poly_resampler_get_output_frames_next,
    .get_channels = async_poly_resampler_get_channels,
    .free = async_poly_resampler_free};

static void* async_poly_resampler_create_from_profile(
    size_t channels, size_t input_rate, size_t output_rate,
    resampler_profile_t profile, size_t chunk_size, double max_relative_ratio,
    fixed_async_t fixed, config_error_t* err) {
  poly_interpolation_t interp = POLY_INTERPOLATION_CUBIC;
  switch (profile) {
    case RESAMPLER_PROFILE_VERY_FAST:
      interp = POLY_INTERPOLATION_LINEAR;
      break;
    case RESAMPLER_PROFILE_FAST:
      interp = POLY_INTERPOLATION_CUBIC;
      break;
    case RESAMPLER_PROFILE_BALANCED:
      interp = POLY_INTERPOLATION_QUINTIC;
      break;
    case RESAMPLER_PROFILE_ACCURATE:
      interp = POLY_INTERPOLATION_SEPTIC;
      break;
    default:
      break;
  }
  return async_poly_resampler_create_impl(channels, input_rate, output_rate,
                                          interp, chunk_size,
                                          max_relative_ratio, fixed, err);
}
