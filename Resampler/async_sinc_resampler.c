// Asynchronous windowed-sinc resampler.
//
// Same buffer layout, same `last_index` semantics, same `t_ratio` accumulation,
// same kernel decimation — output samples agree bit-for-bit (modulo the
// FMA-reduction order in the dot product, which is on the order of a few ULPs).
//
// Memory: every internal buffer is sized at init based on `chunkSize` and
// `maxRelativeRatio`. There is **no** dynamic allocation on the hot path.

#include "async_sinc_resampler.h"

#include "Audio/audio_chunk.h"
#include "audio_resampler.h"
#include "resampler_error.h"
#include "sinc_dot_product.h"

typedef struct async_sinc_resampler async_sinc_resampler_t;

struct async_sinc_resampler {
  size_t channels;
  size_t chunk_size;
  // Filter geometry.
  size_t sinc_len;
  size_t oversampling_factor;
  sinc_interpolation_type_t interpolation;
  // ramp toward the target ratio.
  double base_ratio;
  double resample_ratio;
  double target_ratio;
  double max_relative_ratio;
  double last_index;  // tracking index
  // in the interpolator.
  double* sinc_table;
  // Per-channel input buffer. Layout:
  //   [0 .. 2*sincLen)            — history (last 2*sincLen samples of the
  //                                  previous chunk, or zeros initially)
  //   [2*sincLen .. 2*sincLen+chunkSize) — current chunk's data
  audio_buffers_t* input_buffer;
  // Pre-allocated scratch for per-frame `idx` values. Pre-computed once per
  // chunk so the per-channel loops can iterate without repeating the idx
  // accumulation.
  double* idx_scratch;
  double* frac_scratch;
  // Pre-allocated buffer for combined sinc blending across multi-channel path.
  // Sized at sinc_len + 1.
  double* combined_scratch;
  // Maximum output frames the resampler can ever produce in one call. The
  // caller uses this to size the output AudioChunk once at startup.
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
  double avg_ratio = 0.5 * resample_ratio + 0.5 * target_ratio;
  double raw =
      last_index + (double)chunk_size / avg_ratio + (double)interpolator_len;
  return (size_t)ceil(raw);
}

static inline size_t calculate_output_size(
    size_t chunk_size, double resample_ratio, double target_ratio,
    double last_index, size_t interpolator_len, fixed_async_t fixed) {
  if (fixed == FIXED_ASYNC_OUTPUT) {
    return chunk_size;
  }
  double avg_ratio = 0.5 * resample_ratio + 0.5 * target_ratio;
  double raw =
      ((double)chunk_size - (double)(interpolator_len + 1) - last_index) *
      avg_ratio;
  return (size_t)floor(raw);
}

static void async_sinc_resampler_free(async_sinc_resampler_t* resampler) {
  if (!resampler) return;
  if (resampler->input_buffer) audio_buffers_free(resampler->input_buffer);
  free(resampler->sinc_table);
  free(resampler->idx_scratch);
  free(resampler->frac_scratch);
  free(resampler->combined_scratch);
  free(resampler);
}

static void async_sinc_resampler_set_relative_ratio(async_sinc_resampler_t* resampler,
                                              double multiplier) {
  if (!resampler) return;
  double min_ratio = 1.0 / resampler->max_relative_ratio;
  if (multiplier < min_ratio) multiplier = min_ratio;
  if (multiplier > resampler->max_relative_ratio)
    multiplier = resampler->max_relative_ratio;
  resampler->target_ratio = resampler->base_ratio * multiplier;
}

static double async_sinc_resampler_get_ratio(const async_sinc_resampler_t* resampler) {
  return resampler ? resampler->resample_ratio : 1.0;
}

static size_t async_sinc_resampler_get_max_output_frames(
    const async_sinc_resampler_t* resampler) {
  return resampler ? resampler->max_output_frames : 0;
}

static size_t async_sinc_resampler_get_chunk_size(
    const async_sinc_resampler_t* resampler) {
  return resampler ? resampler->chunk_size : 0;
}

static size_t async_sinc_resampler_get_channels(
    const async_sinc_resampler_t* resampler) {
  return resampler ? resampler->channels : 0;
}



typedef struct {
  int idx;
  int sub;
} adjust_point_t;

/**
 * @brief Adjusts the input buffer index and fractional subindex for
 * oversampling lookup.
 *
 * Handles wrap-around logic when the subindex (fractional part + offset) goes
 * out of bounds of the oversampling factor.
 *
 * @param start The starting integer index in the input buffer.
 * @param frac The current fractional index (0 to factor - 1).
 * @param sub The offset to apply to the fractional index (e.g., -1, 0, 1, 2 for
 * cubic interp).
 * @param factor The oversampling factor.
 * @return An adjust_point_t struct containing the adjusted integer index and
 * subindex.
 */
static inline adjust_point_t adjust_point(int start, int frac, int sub,
                                          int factor) {
  int index = start;
  int subindex = frac + sub;
  if (subindex < 0) {
    subindex += factor;
    index -= 1;
  } else if (subindex >= factor) {
    subindex -= factor;
    index += 1;
  }
  return (adjust_point_t){index, subindex};
}

/**
 * @brief Resamples the input using nearest-neighbor interpolation.
 *
 * This is the fastest but lowest quality interpolation mode. It uses the
 * closest sinc filter phase without interpolating between phases.
 *
 * @param resampler Pointer to the resampler instance.
 * @param output_frames Number of output frames to generate.
 * @param output Pointer to the output audio chunk.
 */
static void run_nearest(async_sinc_resampler_t* resampler, size_t output_frames,
                        audio_chunk_t* output) {
  size_t s_len = resampler->sinc_len;
  size_t two_s_len = 2 * s_len;
  int factor = (int)resampler->oversampling_factor;
  double factor_d = (double)factor;
  const double* table = resampler->sinc_table;
  const double* idx_buf = resampler->idx_scratch;

  for (size_t ch = 0; ch < resampler->channels; ch++) {
    const double* buf = audio_buffers_get_channel(resampler->input_buffer, ch);
    double* out = audio_chunk_get_channel(output, ch);
    if (!buf || !out) continue;
    for (size_t frame = 0; frame < output_frames; frame++) {
      double idx = idx_buf[frame];
      double idx_floor = floor(idx);
      int start_idx = (int)idx_floor;
      int subindex = (int)round((idx - idx_floor) * factor_d);
      if (subindex >= factor) {
        subindex -= factor;
        start_idx += 1;
      }

      double y = sinc_dot_product(buf + start_idx + two_s_len,
                                  table + subindex * s_len, s_len);
      out[frame] = y;
    }
  }
}

/**
 * @brief Resamples the input using cubic interpolation across four adjacent
 * sinc filter phases.
 *
 * Employs multi-channel combined sinc blending when channels >= 2 to evaluate
 * 1 dot product per channel.
 *
 * @param resampler Pointer to the resampler instance.
 * @param output_frames Number of output frames to generate.
 * @param output Pointer to the output audio chunk.
 */
static void run_cubic(async_sinc_resampler_t* resampler, size_t output_frames,
                      audio_chunk_t* output) {
  size_t s_len = resampler->sinc_len;
  size_t two_s_len = 2 * s_len;
  int factor = (int)resampler->oversampling_factor;
  double factor_d = (double)factor;
  const double* table = resampler->sinc_table;
  const double* idx_buf = resampler->idx_scratch;
  const double* frac_buf = resampler->frac_scratch;

  if (resampler->channels >= 2) {
    double* combined = resampler->combined_scratch;
    for (size_t frame = 0; frame < output_frames; frame++) {
      double idx = idx_buf[frame];
      double idx_floor = floor(idx);
      int start_idx = (int)idx_floor;
      int frac = (int)floor((idx - idx_floor) * factor_d);
      double x = frac_buf[frame];

      // 4 (idx, sub) pairs at sub = -1, 0, 1, 2.
      adjust_point_t pts[4] = {adjust_point(start_idx, frac, -1, factor),
                               adjust_point(start_idx, frac, 0, factor),
                               adjust_point(start_idx, frac, 1, factor),
                               adjust_point(start_idx, frac, 2, factor)};

      int min_idx = pts[0].idx;
      if (pts[1].idx < min_idx) min_idx = pts[1].idx;
      if (pts[2].idx < min_idx) min_idx = pts[2].idx;
      if (pts[3].idx < min_idx) min_idx = pts[3].idx;

      // interp_cubic weights (asynchro_sinc.rs:118-128).
      double x2 = x * x;
      double x3 = x2 * x;
      double w[4] = {-1.0 / 3.0 * x + 0.5 * x2 - 1.0 / 6.0 * x3,
                     1.0 - 0.5 * x - x2 + 0.5 * x3, x + 0.5 * x2 - 0.5 * x3,
                     -1.0 / 6.0 * x + 1.0 / 6.0 * x3};

      memset(combined, 0, (s_len + 1) * sizeof(double));
      for (int k = 0; k < 4; k++) {
        int shift = pts[k].idx - min_idx;
        const double* sinc_row = table + pts[k].sub * s_len;
        double weight = w[k];
        for (size_t p = 0; p < s_len; p++) {
          combined[shift + p] += weight * sinc_row[p];
        }
      }

      size_t base_offset = (size_t)(min_idx + (int)two_s_len);
      for (size_t ch = 0; ch < resampler->channels; ch++) {
        const double* buf =
            audio_buffers_get_channel(resampler->input_buffer, ch);
        double* out = audio_chunk_get_channel(output, ch);
        if (!buf || !out) continue;
        double dot = sinc_dot_product(buf + base_offset, combined, s_len);
        out[frame] = dot + combined[s_len] * buf[base_offset + s_len];
      }
    }
  } else {
    for (size_t ch = 0; ch < resampler->channels; ch++) {
      const double* buf =
          audio_buffers_get_channel(resampler->input_buffer, ch);
      double* out = audio_chunk_get_channel(output, ch);
      if (!buf || !out) continue;
      for (size_t frame = 0; frame < output_frames; frame++) {
        double idx = idx_buf[frame];
        double idx_floor = floor(idx);
        int start_idx = (int)idx_floor;
        int frac = (int)floor((idx - idx_floor) * factor_d);
        double frac_offset = frac_buf[frame];

        // 4 (idx, sub) pairs at sub = -1, 0, 1, 2.
        adjust_point_t p0t = adjust_point(start_idx, frac, -1, factor);
        adjust_point_t p1t = adjust_point(start_idx, frac, 0, factor);
        adjust_point_t p2t = adjust_point(start_idx, frac, 1, factor);
        adjust_point_t p3t = adjust_point(start_idx, frac, 2, factor);

        double p0 = sinc_dot_product(buf + p0t.idx + two_s_len,
                                     table + p0t.sub * s_len, s_len);
        double p1 = sinc_dot_product(buf + p1t.idx + two_s_len,
                                     table + p1t.sub * s_len, s_len);
        double p2 = sinc_dot_product(buf + p2t.idx + two_s_len,
                                     table + p2t.sub * s_len, s_len);
        double p3 = sinc_dot_product(buf + p3t.idx + two_s_len,
                                     table + p3t.sub * s_len, s_len);

        // interp_cubic (asynchro_sinc.rs:118-128).
        double a0 = p1;
        double a1 = -1.0 / 3.0 * p0 - 0.5 * p1 + p2 - 1.0 / 6.0 * p3;
        double a2 = 0.5 * (p0 + p2) - p1;
        double a3 = 0.5 * (p1 - p2) + 1.0 / 6.0 * (p3 - p0);
        double x = frac_offset;
        double x2 = x * x;
        double x3 = x2 * x;
        out[frame] = a0 + a1 * x + a2 * x2 + a3 * x3;
      }
    }
  }
}

/**
 * @brief Resamples the input using quadratic interpolation across three
 * adjacent sinc filter phases.
 *
 * Employs multi-channel combined sinc blending when channels > 2.
 *
 * @param resampler Pointer to the resampler instance.
 * @param output_frames Number of output frames to generate.
 * @param output Pointer to the output audio chunk.
 */
static void run_quadratic(async_sinc_resampler_t* resampler,
                          size_t output_frames, audio_chunk_t* output) {
  size_t s_len = resampler->sinc_len;
  size_t two_s_len = 2 * s_len;
  int factor = (int)resampler->oversampling_factor;
  double factor_d = (double)factor;
  const double* table = resampler->sinc_table;
  const double* idx_buf = resampler->idx_scratch;
  const double* frac_buf = resampler->frac_scratch;

  if (resampler->channels > 2) {
    double* combined = resampler->combined_scratch;
    for (size_t frame = 0; frame < output_frames; frame++) {
      double idx = idx_buf[frame];
      double idx_floor = floor(idx);
      int start_idx = (int)idx_floor;
      int frac = (int)floor((idx - idx_floor) * factor_d);
      double x = frac_buf[frame];

      // get_nearest_times_3: sub = 0, 1, 2.
      adjust_point_t pts[3] = {adjust_point(start_idx, frac, 0, factor),
                               adjust_point(start_idx, frac, 1, factor),
                               adjust_point(start_idx, frac, 2, factor)};

      int min_idx = pts[0].idx;
      if (pts[1].idx < min_idx) min_idx = pts[1].idx;
      if (pts[2].idx < min_idx) min_idx = pts[2].idx;

      // interp_quad weights (asynchro_sinc.rs:145-154).
      double x2 = x * x;
      double w[3] = {0.5 * (2.0 - 3.0 * x + x2), 0.5 * (4.0 * x - 2.0 * x2),
                     0.5 * (x2 - x)};

      memset(combined, 0, (s_len + 1) * sizeof(double));
      for (int k = 0; k < 3; k++) {
        int shift = pts[k].idx - min_idx;
        const double* sinc_row = table + pts[k].sub * s_len;
        double weight = w[k];
        for (size_t p = 0; p < s_len; p++) {
          combined[shift + p] += weight * sinc_row[p];
        }
      }

      size_t base_offset = (size_t)(min_idx + (int)two_s_len);
      for (size_t ch = 0; ch < resampler->channels; ch++) {
        const double* buf =
            audio_buffers_get_channel(resampler->input_buffer, ch);
        double* out = audio_chunk_get_channel(output, ch);
        if (!buf || !out) continue;
        double dot = sinc_dot_product(buf + base_offset, combined, s_len);
        out[frame] = dot + combined[s_len] * buf[base_offset + s_len];
      }
    }
  } else {
    for (size_t ch = 0; ch < resampler->channels; ch++) {
      const double* buf =
          audio_buffers_get_channel(resampler->input_buffer, ch);
      double* out = audio_chunk_get_channel(output, ch);
      if (!buf || !out) continue;
      for (size_t frame = 0; frame < output_frames; frame++) {
        double idx = idx_buf[frame];
        double idx_floor = floor(idx);
        int start_idx = (int)idx_floor;
        int frac = (int)floor((idx - idx_floor) * factor_d);
        double frac_offset = frac_buf[frame];

        // get_nearest_times_3: sub = 0, 1, 2.
        adjust_point_t p0t = adjust_point(start_idx, frac, 0, factor);
        adjust_point_t p1t = adjust_point(start_idx, frac, 1, factor);
        adjust_point_t p2t = adjust_point(start_idx, frac, 2, factor);

        double p0 = sinc_dot_product(buf + p0t.idx + two_s_len,
                                     table + p0t.sub * s_len, s_len);
        double p1 = sinc_dot_product(buf + p0t.idx + two_s_len,
                                     table + p1t.sub * s_len, s_len);
        double p2 = sinc_dot_product(buf + p0t.idx + two_s_len,
                                     table + p2t.sub * s_len, s_len);

        // interp_quad (asynchro_sinc.rs:145-154).
        double a2 = p0 - 2.0 * p1 + p2;
        double a1 = -3.0 * p0 + 4.0 * p1 - p2;
        double a0 = 2.0 * p0;
        double x = frac_offset;
        double x2 = x * x;
        out[frame] = 0.5 * (a0 + a1 * x + a2 * x2);
      }
    }
  }
}

/**
 * @brief Resamples the input using linear interpolation between two adjacent
 * sinc filter phases.
 *
 * Employs multi-channel combined sinc blending when channels > 2.
 *
 * @param resampler Pointer to the resampler instance.
 * @param output_frames Number of output frames to generate.
 * @param output Pointer to the output audio chunk.
 */
static void run_linear(async_sinc_resampler_t* resampler, size_t output_frames,
                       audio_chunk_t* output) {
  size_t s_len = resampler->sinc_len;
  size_t two_s_len = 2 * s_len;
  int factor = (int)resampler->oversampling_factor;
  double factor_d = (double)factor;
  const double* table = resampler->sinc_table;
  const double* idx_buf = resampler->idx_scratch;
  const double* frac_buf = resampler->frac_scratch;

  if (resampler->channels > 2) {
    double* combined = resampler->combined_scratch;
    for (size_t frame = 0; frame < output_frames; frame++) {
      double idx = idx_buf[frame];
      double idx_floor = floor(idx);
      int start_idx = (int)idx_floor;
      int frac = (int)floor((idx - idx_floor) * factor_d);
      double x = frac_buf[frame];

      // get_nearest_times_2: sub = 0, 1.
      adjust_point_t pts[2] = {adjust_point(start_idx, frac, 0, factor),
                               adjust_point(start_idx, frac, 1, factor)};

      int min_idx = pts[0].idx;
      if (pts[1].idx < min_idx) min_idx = pts[1].idx;

      // interp_lin weights.
      double w[2] = {1.0 - x, x};

      memset(combined, 0, (s_len + 1) * sizeof(double));
      for (int k = 0; k < 2; k++) {
        int shift = pts[k].idx - min_idx;
        const double* sinc_row = table + pts[k].sub * s_len;
        double weight = w[k];
        for (size_t p = 0; p < s_len; p++) {
          combined[shift + p] += weight * sinc_row[p];
        }
      }

      size_t base_offset = (size_t)(min_idx + (int)two_s_len);
      for (size_t ch = 0; ch < resampler->channels; ch++) {
        const double* buf =
            audio_buffers_get_channel(resampler->input_buffer, ch);
        double* out = audio_chunk_get_channel(output, ch);
        if (!buf || !out) continue;
        double dot = sinc_dot_product(buf + base_offset, combined, s_len);
        out[frame] = dot + combined[s_len] * buf[base_offset + s_len];
      }
    }
  } else {
    for (size_t ch = 0; ch < resampler->channels; ch++) {
      const double* buf =
          audio_buffers_get_channel(resampler->input_buffer, ch);
      double* out = audio_chunk_get_channel(output, ch);
      if (!buf || !out) continue;
      for (size_t frame = 0; frame < output_frames; frame++) {
        double idx = idx_buf[frame];
        double idx_floor = floor(idx);
        int start_idx = (int)idx_floor;
        int frac = (int)floor((idx - idx_floor) * factor_d);
        double frac_offset = frac_buf[frame];

        // get_nearest_times_2: sub = 0, 1.
        adjust_point_t p0t = adjust_point(start_idx, frac, 0, factor);
        adjust_point_t p1t = adjust_point(start_idx, frac, 1, factor);

        double p0 = sinc_dot_product(buf + p0t.idx + two_s_len,
                                     table + p0t.sub * s_len, s_len);
        double p1 = sinc_dot_product(buf + p1t.idx + two_s_len,
                                     table + p1t.sub * s_len, s_len);

        // interp_lin: y0 + x * (y1 - y0).
        out[frame] = p0 + frac_offset * (p1 - p0);
      }
    }
  }
}

static resampler_error_t async_sinc_resampler_process(
    async_sinc_resampler_t* resampler, const audio_chunk_t* input,
    audio_chunk_t* output) {
  if (!resampler || !input || !output) return RESAMPLER_ERR_INVALID_PARAMETER;
  size_t valid_frames = audio_chunk_get_valid_frames(input);
  if (valid_frames > resampler->max_input_frames) {
    return RESAMPLER_ERR_INPUT_SIZE_MISMATCH;
  }
  if (audio_chunk_get_channels(output) != resampler->channels) {
    return RESAMPLER_ERR_CHANNEL_COUNT_MISMATCH;
  }
  size_t output_frames = resampler->needed_output_size;
  if (output_frames == 0) {
    resampler->last_index -= (double)resampler->needed_input_size;
    resampler->resample_ratio = resampler->target_ratio;

    resampler->needed_input_size =
        calculate_input_size(resampler->chunk_size, resampler->resample_ratio,
                             resampler->target_ratio, resampler->last_index,
                             resampler->sinc_len, resampler->fixed);
    resampler->needed_output_size =
        calculate_output_size(resampler->chunk_size, resampler->resample_ratio,
                              resampler->target_ratio, resampler->last_index,
                              resampler->sinc_len, resampler->fixed);

    audio_chunk_set_valid_frames(output, 0);
    return RESAMPLER_OK;
  }
  if (audio_chunk_get_frames(output) < output_frames) {
    return RESAMPLER_ERR_OUTPUT_BUFFER_TOO_SMALL;
  }

  // Shift buffer, write new data, run inner.
  size_t s_len = resampler->sinc_len;
  size_t two_s_len = 2 * s_len;

  for (size_t ch = 0; ch < resampler->channels; ch++) {
    double* base = audio_buffers_get_channel(resampler->input_buffer, ch);
    if (!base) continue;
    memmove(base, base + resampler->current_buffer_fill,
            two_s_len * sizeof(double));
  }
  // Copy the new input chunk data immediately after the history.
  for (size_t ch = 0; ch < resampler->channels; ch++) {
    const double* src_ptr = audio_chunk_get_channel(input, ch);
    double* dst_ptr = audio_buffers_get_channel(resampler->input_buffer, ch);
    if (!src_ptr || !dst_ptr) continue;
    memcpy(dst_ptr + two_s_len, src_ptr, valid_frames * sizeof(double));
    if (valid_frames < resampler->needed_input_size) {
      memset(dst_ptr + two_s_len + valid_frames, 0,
             (resampler->needed_input_size - valid_frames) * sizeof(double));
    }
  }

  resampler->current_buffer_fill = resampler->needed_input_size;

  double t_ratio_start = 1.0 / resampler->resample_ratio;
  double t_ratio_end = 1.0 / resampler->target_ratio;
  double t_ratio_increment =
      (t_ratio_end - t_ratio_start) / (double)output_frames;
  double factor_d = (double)resampler->oversampling_factor;

  double t_ratio = t_ratio_start;
  double idx = resampler->last_index;
  for (size_t frame = 0; frame < output_frames; frame++) {
    t_ratio += t_ratio_increment;
    idx += t_ratio;
    resampler->idx_scratch[frame] = idx;
    double scaled = idx * factor_d;
    resampler->frac_scratch[frame] = scaled - floor(scaled);
  }
  double final_idx = idx;

  // Inner loop, specialised per interpolation mode.
  switch (resampler->interpolation) {
    case SINC_INTERPOLATION_NEAREST:
      run_nearest(resampler, output_frames, output);
      break;
    case SINC_INTERPOLATION_LINEAR:
      run_linear(resampler, output_frames, output);
      break;
    case SINC_INTERPOLATION_QUADRATIC:
      run_quadratic(resampler, output_frames, output);
      break;
    case SINC_INTERPOLATION_CUBIC:
      run_cubic(resampler, output_frames, output);
      break;
    default:
      run_cubic(resampler, output_frames, output);
      break;
  }

  // Update state for next chunk.
  resampler->last_index = final_idx - (double)resampler->needed_input_size;
  resampler->resample_ratio = resampler->target_ratio;

  size_t prev_needed_input_size = resampler->needed_input_size;

  resampler->needed_input_size = calculate_input_size(
      resampler->chunk_size, resampler->resample_ratio, resampler->target_ratio,
      resampler->last_index, resampler->sinc_len, resampler->fixed);
  resampler->needed_output_size = calculate_output_size(
      resampler->chunk_size, resampler->resample_ratio, resampler->target_ratio,
      resampler->last_index, resampler->sinc_len, resampler->fixed);

  size_t valid_out = (output_frames * valid_frames) / prev_needed_input_size;
  audio_chunk_set_valid_frames(output, valid_out);
  return RESAMPLER_OK;
}

static size_t async_sinc_resampler_get_input_frames_next(
    const async_sinc_resampler_t* resampler) {
  return resampler ? resampler->needed_input_size : 0;
}

static size_t async_sinc_resampler_get_output_frames_next(
    const async_sinc_resampler_t* resampler) {
  return resampler ? resampler->needed_output_size : 0;
}

audio_resampler_t* async_sinc_resampler_create(
    size_t channels, size_t input_rate, size_t output_rate, size_t sinc_len,
    size_t oversampling_factor, sinc_interpolation_type_t interpolation,
    window_function_t window, double f_cutoff, bool has_f_cutoff,
    size_t chunk_size, double max_relative_ratio, fixed_async_t fixed,
    config_error_t* err) {
  if (channels == 0) {
    config_error_set(err, CONFIG_ERR_VALIDATION,
                     "AsyncSincResampler: channels must be positive");
    return NULL;
  }
  if (chunk_size == 0) {
    config_error_set(err, CONFIG_ERR_VALIDATION,
                     "AsyncSincResampler: chunk_size must be positive");
    return NULL;
  }
  if (input_rate == 0 || output_rate == 0) {
    config_error_set(err, CONFIG_ERR_VALIDATION,
                     "AsyncSincResampler: rates must be positive");
    return NULL;
  }
  if (oversampling_factor == 0) {
    config_error_set(
        err, CONFIG_ERR_VALIDATION,
        "AsyncSincResampler: oversampling_factor must be positive");
    return NULL;
  }
  if (max_relative_ratio < 1.0) max_relative_ratio = 1.1;

  async_sinc_resampler_t* resampler =
      (async_sinc_resampler_t*)calloc(1, sizeof(async_sinc_resampler_t));
  if (!resampler) {
    config_error_set(err, CONFIG_ERR_PARSE,
                     "Failed to allocate AsyncSincResampler");
    return NULL;
  }

  resampler->channels = channels;
  resampler->chunk_size = chunk_size;
  resampler->fixed = fixed;
  resampler->sinc_len = sinc_len;
  resampler->oversampling_factor = oversampling_factor;
  resampler->interpolation = interpolation;
  resampler->base_ratio = (double)output_rate / (double)input_rate;
  resampler->resample_ratio = resampler->base_ratio;
  resampler->target_ratio = resampler->base_ratio;
  resampler->max_relative_ratio = max_relative_ratio;
  resampler->last_index = -((double)sinc_len - 1.0);

  float base_cutoff =
      has_f_cutoff ? (float)f_cutoff : calculate_cutoff_f32(sinc_len, window);
  float fc_f32 = resampler->base_ratio >= 1.0
                     ? base_cutoff
                     : base_cutoff * (float)resampler->base_ratio;
  double fc = (double)fc_f32;

  resampler->sinc_table =
      make_sinc_table(sinc_len, oversampling_factor, window, fc);
  if (!resampler->sinc_table) {
    config_error_set(err, CONFIG_ERR_PARSE,
                     "Failed to build AsyncSincResampler sinc table");
    async_sinc_resampler_free(resampler);
    return NULL;
  }

  double min_ratio_abs = resampler->base_ratio / max_relative_ratio;
  if (fixed == FIXED_ASYNC_INPUT) {
    resampler->max_input_frames = chunk_size;
  } else {
    double raw_max_in = ((double)chunk_size) / min_ratio_abs + 2.0 +
                        (double)resampler->sinc_len / 2.0;
    resampler->max_input_frames = (size_t)ceil(raw_max_in) + 16;
  }

  if (resampler->max_input_frames > SIZE_MAX - 2 * sinc_len) {
    config_error_set(err, CONFIG_ERR_VALIDATION,
                     "AsyncSincResampler: max_input_frames is out of bounds");
    async_sinc_resampler_free(resampler);
    return NULL;
  }

  size_t buf_len = resampler->max_input_frames + 2 * sinc_len;
  resampler->input_buffer = audio_buffers_create(channels, buf_len);
  if (!resampler->input_buffer) {
    config_error_set(err, CONFIG_ERR_PARSE,
                     "Failed to allocate AsyncSincResampler input buffer");
    async_sinc_resampler_free(resampler);
    return NULL;
  }

  resampler->needed_input_size = calculate_input_size(
      chunk_size, resampler->resample_ratio, resampler->target_ratio,
      resampler->last_index, resampler->sinc_len, fixed);
  resampler->needed_output_size = calculate_output_size(
      chunk_size, resampler->resample_ratio, resampler->target_ratio,
      resampler->last_index, resampler->sinc_len, fixed);
  resampler->current_buffer_fill = resampler->needed_input_size;

  if (fixed == FIXED_ASYNC_OUTPUT) {
    resampler->max_output_frames = chunk_size;
  } else {
    double most_neg_last_index = -((double)sinc_len - 1.0);
    double max_ratio_abs = resampler->base_ratio * max_relative_ratio;
    double raw_max =
        ((double)chunk_size - (double)(sinc_len + 1) - most_neg_last_index) *
        max_ratio_abs;

    if (isnan(raw_max) || isinf(raw_max) || raw_max < 0.0 ||
        raw_max > (double)(SIZE_MAX - 32)) {
      config_error_set(
          err, CONFIG_ERR_VALIDATION,
          "AsyncSincResampler: calculated maximum output size is invalid");
      async_sinc_resampler_free(resampler);
      return NULL;
    }
    resampler->max_output_frames = (size_t)(ceil(raw_max)) + 16;
  }

  resampler->idx_scratch =
      (double*)calloc(resampler->max_output_frames, sizeof(double));
  resampler->frac_scratch =
      (double*)calloc(resampler->max_output_frames, sizeof(double));
  resampler->combined_scratch = (double*)calloc(sinc_len + 1, sizeof(double));
  if (!resampler->idx_scratch || !resampler->frac_scratch ||
      !resampler->combined_scratch) {
    config_error_set(err, CONFIG_ERR_PARSE,
                     "Failed to allocate AsyncSincResampler scratch buffers");
    async_sinc_resampler_free(resampler);
    return NULL;
  }

  audio_resampler_t* wrap =
      (audio_resampler_t*)calloc(1, sizeof(audio_resampler_t));
  if (!wrap) {
    async_sinc_resampler_free(resampler);
    return NULL;
  }
  wrap->type = RESAMPLER_IMPL_ASYNC_SINC;
  wrap->impl = resampler;
  wrap->process =
      (resampler_error_t (*)(void*, const audio_chunk_t*, audio_chunk_t*))
          async_sinc_resampler_process;
  wrap->set_relative_ratio =
      (void (*)(void*, double))async_sinc_resampler_set_relative_ratio;
  wrap->get_ratio = (double (*)(const void*))async_sinc_resampler_get_ratio;
  wrap->get_max_output_frames =
      (size_t (*)(const void*))async_sinc_resampler_get_max_output_frames;
  wrap->get_chunk_size =
      (size_t (*)(const void*))async_sinc_resampler_get_chunk_size;
  wrap->get_input_frames_next =
      (size_t (*)(const void*))async_sinc_resampler_get_input_frames_next;
  wrap->get_output_frames_next =
      (size_t (*)(const void*))async_sinc_resampler_get_output_frames_next;
  wrap->get_channels =
      (size_t (*)(const void*))async_sinc_resampler_get_channels;
  wrap->free = (void (*)(void*))async_sinc_resampler_free;
  return wrap;
}

audio_resampler_t* async_sinc_resampler_create_from_profile(
    size_t channels, size_t input_rate, size_t output_rate,
    resampler_profile_t profile, size_t chunk_size, double max_relative_ratio,
    config_error_t* err) {
  size_t sinc_len = 192;
  size_t oversampling_factor = 512;
  window_function_t window = WINDOW_FUNCTION_BLACKMAN_HARRIS2;
  sinc_interpolation_type_t interpolation = SINC_INTERPOLATION_QUADRATIC;

  switch (profile) {
    case RESAMPLER_PROFILE_VERY_FAST:
      sinc_len = 64;
      oversampling_factor = 1024;
      window = WINDOW_FUNCTION_HANN2;
      interpolation = SINC_INTERPOLATION_LINEAR;
      break;
    case RESAMPLER_PROFILE_FAST:
      sinc_len = 128;
      oversampling_factor = 1024;
      window = WINDOW_FUNCTION_BLACKMAN2;
      interpolation = SINC_INTERPOLATION_LINEAR;
      break;
    case RESAMPLER_PROFILE_BALANCED:
      sinc_len = 192;
      oversampling_factor = 512;
      window = WINDOW_FUNCTION_BLACKMAN_HARRIS2;
      interpolation = SINC_INTERPOLATION_QUADRATIC;
      break;
    case RESAMPLER_PROFILE_ACCURATE:
      sinc_len = 256;
      oversampling_factor = 256;
      window = WINDOW_FUNCTION_BLACKMAN_HARRIS2;
      interpolation = SINC_INTERPOLATION_CUBIC;
      break;
    default:
      break;
  }

  return async_sinc_resampler_create(
      channels, input_rate, output_rate, sinc_len, oversampling_factor,
      interpolation, window, 0.0, false, chunk_size, max_relative_ratio,
      FIXED_ASYNC_OUTPUT, err);
}


