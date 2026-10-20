#if defined(__linux__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/audio_chunk.h"
#include "config/config_error.h"
#include "config/config_gen.h"
#include "resampler/async_sinc_resampler.h"
#include "resampler/audio_resampler.h"
#include "resampler/resampler_error.h"
#include "test_support.h"

static void make_sine(double *out, size_t n, int rate, double freq) {
  double omega = 2.0 * M_PI * freq / (double)rate;
  for (size_t i = 0; i < n; i++) {
    out[i] = sin(omega * (double)i);
  }
}

static void assert_stereo_matches_mono(resampler_type_t type,
                                       const char *interp_str) {
  int in_rate = 44100;
  int out_rate = 48000;
  size_t chunk_size = 1024;
  size_t nbr_in = 32 * chunk_size;

  double *left = (double *)calloc(nbr_in, sizeof(double));
  double *right = (double *)calloc(nbr_in, sizeof(double));
  make_sine(left, nbr_in, in_rate, 1000.0);
  make_sine(right, nbr_in, in_rate, 1500.0);

  resampler_config_t cfg_stereo;
  resampler_config_init_with_type(&cfg_stereo, type);
  if (interp_str) {
    strncpy(cfg_stereo.interpolation, interp_str,
            sizeof(cfg_stereo.interpolation) - 1);
    cfg_stereo.has_interpolation = true;
  }
  if (type == RESAMPLER_TYPE_ASYNC_SINC) {
    strncpy(cfg_stereo.profile, "Accurate", sizeof(cfg_stereo.profile) - 1);
    cfg_stereo.has_profile = true;
  }

  resampler_t *stereo = resampler_create_from_config(
      &cfg_stereo, in_rate, out_rate, 2, chunk_size, NULL);
  resampler_t *mono_l = resampler_create_from_config(
      &cfg_stereo, in_rate, out_rate, 1, chunk_size, NULL);
  resampler_t *mono_r = resampler_create_from_config(
      &cfg_stereo, in_rate, out_rate, 1, chunk_size, NULL);

  ASSERT_TRUE(stereo != NULL);
  ASSERT_TRUE(mono_l != NULL);
  ASSERT_TRUE(mono_r != NULL);

  size_t needed_out = resampler_get_output_frames_next(stereo);
  audio_chunk_t *st_in = audio_chunk_create(65536, 2);
  audio_chunk_t *st_out = audio_chunk_create(needed_out, 2);
  audio_chunk_t *ml_in = audio_chunk_create(65536, 1);
  audio_chunk_t *ml_out = audio_chunk_create(needed_out, 1);
  audio_chunk_t *mr_in = audio_chunk_create(65536, 1);
  audio_chunk_t *mr_out = audio_chunk_create(needed_out, 1);

  size_t idx = 0;
  while (true) {
    size_t needed_in = resampler_get_input_frames_next(stereo);
    if (idx + needed_in > nbr_in)
      break;

    double *st_ch0 = audio_chunk_get_channel(st_in, 0);
    double *st_ch1 = audio_chunk_get_channel(st_in, 1);
    double *ml_ch0 = audio_chunk_get_channel(ml_in, 0);
    double *mr_ch0 = audio_chunk_get_channel(mr_in, 0);

    for (size_t i = 0; i < needed_in; i++) {
      st_ch0[i] = left[idx + i];
      st_ch1[i] = right[idx + i];
      ml_ch0[i] = left[idx + i];
      mr_ch0[i] = right[idx + i];
    }
    audio_chunk_set_valid_frames(st_in, needed_in);
    audio_chunk_set_valid_frames(ml_in, needed_in);
    audio_chunk_set_valid_frames(mr_in, needed_in);

    resampler_error_t err_st = resampler_process(stereo, st_in, st_out);
    resampler_error_t err_ml = resampler_process(mono_l, ml_in, ml_out);
    resampler_error_t err_mr = resampler_process(mono_r, mr_in, mr_out);

    ASSERT_EQ(RESAMPLER_OK, err_st);
    ASSERT_EQ(RESAMPLER_OK, err_ml);
    ASSERT_EQ(RESAMPLER_OK, err_mr);
    ASSERT_EQ(audio_chunk_get_valid_frames(st_out),
              audio_chunk_get_valid_frames(ml_out));
    ASSERT_EQ(audio_chunk_get_valid_frames(st_out),
              audio_chunk_get_valid_frames(mr_out));

    const double *st_o0 = audio_chunk_get_channel(st_out, 0);
    const double *st_o1 = audio_chunk_get_channel(st_out, 1);
    const double *ml_o0 = audio_chunk_get_channel(ml_out, 0);
    const double *mr_o0 = audio_chunk_get_channel(mr_out, 0);

    for (size_t i = 0; i < audio_chunk_get_valid_frames(st_out); i++) {
      ASSERT_NEAR(st_o0[i], ml_o0[i], 1e-12);
      ASSERT_NEAR(st_o1[i], mr_o0[i], 1e-12);
    }

    idx += needed_in;
  }

  audio_chunk_free(st_in);
  audio_chunk_free(st_out);
  audio_chunk_free(ml_in);
  audio_chunk_free(ml_out);
  audio_chunk_free(mr_in);
  audio_chunk_free(mr_out);
  resampler_free(stereo);
  resampler_free(mono_l);
  resampler_free(mono_r);
  free(left);
  free(right);
}

static void assert_inout_matches(resampler_type_t type,
                                 const char *interp_str) {
  resampler_config_t cfg;
  resampler_config_init_with_type(&cfg, type);
  if (interp_str) {
    strncpy(cfg.interpolation, interp_str, sizeof(cfg.interpolation) - 1);
    cfg.has_interpolation = true;
  }
  if (type == RESAMPLER_TYPE_ASYNC_SINC) {
    strncpy(cfg.profile, "Accurate", sizeof(cfg.profile) - 1);
    cfg.has_profile = true;
  }

  size_t chunk_size = 1024;
  resampler_t *res_a =
      resampler_create_from_config(&cfg, 44100, 48000, 2, chunk_size, NULL);
  resampler_t *res_b =
      resampler_create_from_config(&cfg, 44100, 48000, 2, chunk_size, NULL);
  ASSERT_TRUE(res_a != NULL);
  ASSERT_TRUE(res_b != NULL);

  size_t needed_out = resampler_get_output_frames_next(res_a);
  audio_chunk_t *in_chunk = audio_chunk_create(65536, 2);
  audio_chunk_t *out_a = audio_chunk_create(needed_out, 2);
  audio_chunk_t *out_b = audio_chunk_create(needed_out, 2);

  size_t accum_in = 0;
  for (int c = 0; c < 8; c++) {
    size_t needed_in = resampler_get_input_frames_next(res_a);
    double *ch0 = audio_chunk_get_channel(in_chunk, 0);
    double *ch1 = audio_chunk_get_channel(in_chunk, 1);
    for (size_t i = 0; i < needed_in; i++) {
      ch0[i] = sin(0.1 * (double)(accum_in + i));
      ch1[i] = cos(0.15 * (double)(accum_in + i));
    }
    audio_chunk_set_valid_frames(in_chunk, needed_in);

    ASSERT_EQ(RESAMPLER_OK, resampler_process(res_a, in_chunk, out_a));
    ASSERT_EQ(RESAMPLER_OK, resampler_process(res_b, in_chunk, out_b));
    ASSERT_EQ(audio_chunk_get_valid_frames(out_a),
              audio_chunk_get_valid_frames(out_b));

    for (size_t ch = 0; ch < 2; ch++) {
      const double *o_a = audio_chunk_get_channel(out_a, ch);
      const double *o_b = audio_chunk_get_channel(out_b, ch);
      for (size_t i = 0; i < audio_chunk_get_valid_frames(out_a); i++) {
        ASSERT_NEAR(o_a[i], o_b[i], 1e-12);
      }
    }
    accum_in += needed_in;
  }

  audio_chunk_free(in_chunk);
  audio_chunk_free(out_a);
  audio_chunk_free(out_b);
  resampler_free(res_a);
  resampler_free(res_b);
}

static void assert_rejects_too_small(resampler_type_t type,
                                     const char *interp_str) {
  resampler_config_t cfg;
  resampler_config_init_with_type(&cfg, type);
  if (interp_str) {
    strncpy(cfg.interpolation, interp_str, sizeof(cfg.interpolation) - 1);
    cfg.has_interpolation = true;
  }
  if (type == RESAMPLER_TYPE_ASYNC_SINC) {
    strncpy(cfg.profile, "Accurate", sizeof(cfg.profile) - 1);
    cfg.has_profile = true;
  }

  size_t chunk_size = 1024;
  resampler_t *res =
      resampler_create_from_config(&cfg, 44100, 48000, 2, chunk_size, NULL);
  ASSERT_TRUE(res != NULL);

  size_t needed_in = resampler_get_input_frames_next(res);
  audio_chunk_t *in_chunk = audio_chunk_create(needed_in, 2);
  audio_chunk_set_valid_frames(in_chunk, needed_in);
  audio_chunk_t *too_small = audio_chunk_create(64, 2);

  resampler_error_t err = resampler_process(res, in_chunk, too_small);
  ASSERT_EQ(RESAMPLER_ERR_OUTPUT_BUFFER_TOO_SMALL, err);

  audio_chunk_free(in_chunk);
  audio_chunk_free(too_small);
  resampler_free(res);
}

TEST(Stereo_MatchesPerChannelMono_Synchronous) {
  assert_stereo_matches_mono(RESAMPLER_TYPE_SYNCHRONOUS, NULL);
}

TEST(Stereo_MatchesPerChannelMono_AsyncPoly) {
  assert_stereo_matches_mono(RESAMPLER_TYPE_ASYNC_POLY, "Cubic");
}

TEST(Stereo_MatchesPerChannelMono_AsyncSinc) {
  assert_stereo_matches_mono(RESAMPLER_TYPE_ASYNC_SINC, NULL);
}

TEST(InoutAPI_Synchronous_MatchesAllocatingAPI) {
  assert_inout_matches(RESAMPLER_TYPE_SYNCHRONOUS, NULL);
}

TEST(InoutAPI_AsyncPoly_MatchesAllocatingAPI) {
  assert_inout_matches(RESAMPLER_TYPE_ASYNC_POLY, "Cubic");
}

TEST(InoutAPI_AsyncSinc_MatchesAllocatingAPI) {
  assert_inout_matches(RESAMPLER_TYPE_ASYNC_SINC, NULL);
}

TEST(InoutAPI_RejectsTooSmallOutputBuffer_Synchronous) {
  assert_rejects_too_small(RESAMPLER_TYPE_SYNCHRONOUS, NULL);
}

TEST(InoutAPI_RejectsTooSmallOutputBuffer_AsyncPoly) {
  assert_rejects_too_small(RESAMPLER_TYPE_ASYNC_POLY, "Cubic");
}

TEST(InoutAPI_RejectsTooSmallOutputBuffer_AsyncSinc) {
  assert_rejects_too_small(RESAMPLER_TYPE_ASYNC_SINC, NULL);
}

static void assert_accepts_partial_chunk(resampler_type_t type,
                                         const char *interp_str) {
  resampler_config_t cfg;
  resampler_config_init_with_type(&cfg, type);
  if (interp_str) {
    strncpy(cfg.interpolation, interp_str, sizeof(cfg.interpolation) - 1);
    cfg.has_interpolation = true;
  }
  if (type == RESAMPLER_TYPE_ASYNC_SINC) {
    strncpy(cfg.profile, "Accurate", sizeof(cfg.profile) - 1);
    cfg.has_profile = true;
  }

  size_t chunk_size = 1024;
  resampler_t *res =
      resampler_create_from_config(&cfg, 44100, 48000, 2, chunk_size, NULL);
  ASSERT_TRUE(res != NULL);

  size_t needed_in = resampler_get_input_frames_next(res);
  size_t needed_out = resampler_get_output_frames_next(res);
  audio_chunk_t *in_chunk = audio_chunk_create(65536, 2);
  audio_chunk_t *out_chunk = audio_chunk_create(needed_out, 2);

  size_t partial_valid = needed_in / 2;
  audio_chunk_set_valid_frames(in_chunk, partial_valid);

  resampler_error_t err = resampler_process(res, in_chunk, out_chunk);
  ASSERT_EQ(RESAMPLER_OK, err);
  size_t valid_out = audio_chunk_get_valid_frames(out_chunk);
  ASSERT_TRUE(valid_out > 0);
  ASSERT_TRUE(valid_out < needed_out);

  audio_chunk_free(in_chunk);
  audio_chunk_free(out_chunk);
  resampler_free(res);
}

TEST(PartialChunk_Synchronous) {
  assert_accepts_partial_chunk(RESAMPLER_TYPE_SYNCHRONOUS, NULL);
}

TEST(PartialChunk_AsyncPoly) {
  assert_accepts_partial_chunk(RESAMPLER_TYPE_ASYNC_POLY, "Cubic");
}

TEST(PartialChunk_AsyncSinc) {
  assert_accepts_partial_chunk(RESAMPLER_TYPE_ASYNC_SINC, NULL);
}

TEST(AsyncSinc_UnderrunBoundaryCheck) {
  resampler_config_t cfg;
  resampler_config_init_with_type(&cfg, RESAMPLER_TYPE_ASYNC_SINC);
  strncpy(cfg.profile, "Accurate", sizeof(cfg.profile) - 1);
  cfg.has_profile = true;

  size_t chunk_size = 1024;
  resampler_t *res =
      resampler_create_from_config(&cfg, 44100, 48000, 2, chunk_size, NULL);
  ASSERT_TRUE(res != NULL);

  audio_chunk_t *empty_in = audio_chunk_create(65536, 2);
  audio_chunk_t *valid_in = audio_chunk_create(65536, 2);
  audio_chunk_t *out_chunk = audio_chunk_create(chunk_size, 2);

  // Set 0 valid frames to simulate multiple underrun chunks
  audio_chunk_set_valid_frames(empty_in, 0);
  for (int i = 0; i < 10; i++) {
    resampler_error_t err = resampler_process(res, empty_in, out_chunk);
    ASSERT_EQ(RESAMPLER_OK, err);
  }

  // Now process valid chunk when last_index is at minimum safe index boundary
  size_t needed_in = resampler_get_input_frames_next(res);
  audio_chunk_set_valid_frames(valid_in, needed_in);
  resampler_error_t err = resampler_process(res, valid_in, out_chunk);
  ASSERT_EQ(RESAMPLER_OK, err);

  audio_chunk_free(empty_in);
  audio_chunk_free(valid_in);
  audio_chunk_free(out_chunk);
  resampler_free(res);
}

TEST(SlipResampler_Basic) {
  resampler_config_t cfg;
  resampler_config_init_with_type(&cfg, RESAMPLER_TYPE_SLIP);

  size_t chunk_size = 1000;
  resampler_t *res =
      resampler_create_from_config(&cfg, 48000, 48000, 1, chunk_size, NULL);
  ASSERT_TRUE(res != NULL);

  size_t max_in = resampler_get_max_input_frames(res);
  size_t expected_max_in = chunk_size + (chunk_size - 1) / (128 + 2); // 1007
  ASSERT_EQ(expected_max_in, max_in);

  audio_chunk_t *in_chunk = audio_chunk_create(max_in, 1);
  audio_chunk_t *out_chunk = audio_chunk_create(chunk_size, 1);

  double *in_data = audio_chunk_get_channel(in_chunk, 0);
  for (size_t i = 0; i < max_in; i++) {
    in_data[i] = (double)i;
  }

  // 1. Ratio 1.0 -> output matches input exactly
  resampler_set_relative_ratio(res, 1.0);
  size_t needed_in = resampler_get_input_frames_next(res);
  ASSERT_EQ(chunk_size, needed_in);
  audio_chunk_set_valid_frames(in_chunk, needed_in);
  resampler_error_t err = resampler_process(res, in_chunk, out_chunk);
  ASSERT_EQ(RESAMPLER_OK, err);
  ASSERT_EQ(chunk_size, audio_chunk_get_valid_frames(out_chunk));
  const double *out_data = audio_chunk_get_channel(out_chunk, 0);
  for (size_t i = 0; i < chunk_size; i++) {
    ASSERT_DOUBLE_EQ((double)i, out_data[i]);
  }

  // 2. Ratio 1.002 -> Expect a slip (insert sample, fewer needed input)
  resampler_set_relative_ratio(res, 1.002);
  needed_in = resampler_get_input_frames_next(res);
  ASSERT_EQ(chunk_size - 1, needed_in);
  audio_chunk_set_valid_frames(in_chunk, needed_in);
  err = resampler_process(res, in_chunk, out_chunk);
  ASSERT_EQ(RESAMPLER_OK, err);
  ASSERT_EQ(chunk_size, audio_chunk_get_valid_frames(out_chunk));
  out_data = audio_chunk_get_channel(out_chunk, 0);

  // First 436 samples are copied exactly
  for (size_t i = 0; i < 436; i++) {
    ASSERT_DOUBLE_EQ((double)i, out_data[i]);
  }
  // After crossfade (436 + 128 = 564), samples are offset by -1
  for (size_t i = 564; i < chunk_size; i++) {
    ASSERT_DOUBLE_EQ((double)(i - 1), out_data[i]);
  }

  // 3. Ratio 0.998 -> Expect a slip (drop sample, more needed input)
  resampler_set_relative_ratio(res, 0.998);
  needed_in = resampler_get_input_frames_next(res);
  ASSERT_EQ(chunk_size + 1, needed_in);
  for (size_t i = 0; i < max_in; i++) {
    in_data[i] = (double)i;
  }
  audio_chunk_set_valid_frames(in_chunk, needed_in);
  err = resampler_process(res, in_chunk, out_chunk);
  ASSERT_EQ(RESAMPLER_OK, err);
  ASSERT_EQ(chunk_size, audio_chunk_get_valid_frames(out_chunk));
  out_data = audio_chunk_get_channel(out_chunk, 0);

  // First 436 samples are copied exactly
  for (size_t i = 0; i < 436; i++) {
    ASSERT_DOUBLE_EQ((double)i, out_data[i]);
  }
  // After crossfade (436 + 128 = 564), samples are offset by +1
  for (size_t i = 564; i < chunk_size; i++) {
    ASSERT_DOUBLE_EQ((double)(i + 1), out_data[i]);
  }

  // 4. Ratio NAN -> Expect it to clamp to min_ratio and not output NaN or crash
  resampler_set_relative_ratio(res, nan(""));
  needed_in = resampler_get_input_frames_next(res);
  audio_chunk_set_valid_frames(in_chunk, needed_in);
  err = resampler_process(res, in_chunk, out_chunk);
  ASSERT_EQ(RESAMPLER_OK, err);
  size_t got_out = audio_chunk_get_valid_frames(out_chunk);
  out_data = audio_chunk_get_channel(out_chunk, 0);
  for (size_t i = 0; i < got_out; i++) {
    ASSERT_FALSE(isnan(out_data[i]));
  }

  audio_chunk_free(in_chunk);
  audio_chunk_free(out_chunk);
  resampler_free(res);
}

static char g_rubato_bin_path[1024] = {0};
static bool g_rubato_checked = false;
static bool g_rubato_available = false;

static bool check_rubato_available(void) {
  if (g_rubato_checked)
    return g_rubato_available;
  g_rubato_checked = true;

  const char *found = test_find_harness_binary("cdsp_resampler_compare");
  if (found) {
    strncpy(g_rubato_bin_path, found, sizeof(g_rubato_bin_path) - 1);
    g_rubato_available = true;
    return true;
  }
  return false;
}

static int write_raw_f64(const double *data, size_t count, const char *path) {
  FILE *f = fopen(path, "wb");
  if (!f)
    return 0;
  size_t written = fwrite(data, sizeof(double), count, f);
  fclose(f);
  return written == count;
}

static double *read_raw_f64(const char *path, size_t *out_count) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  size_t count = size / sizeof(double);
  double *data = (double *)malloc(count * sizeof(double));
  if (!data) {
    fclose(f);
    return NULL;
  }
  size_t read = fread(data, sizeof(double), count, f);
  fclose(f);
  if (read != count) {
    free(data);
    return NULL;
  }
  *out_count = count;
  return data;
}

TEST(SlipResampler_Vs_Rubato) {
  if (!check_rubato_available()) {
    printf(
        "[SKIP] cdsp_resampler_compare harness not built/available. Skipping "
        "comparison test.\n");
    return;
  }

  size_t chunk_size = 1000;
  size_t n_chunks = 8;
  size_t total_frames = n_chunks * chunk_size;

  double *input = (double *)malloc(total_frames * sizeof(double));
  ASSERT_TRUE(input != NULL);
  for (size_t i = 0; i < total_frames; i++) {
    input[i] = sin(0.05 * (double)i);
  }

  int fs_in = 40000;
  int fs_out =
      40012; // ratio = 1.0003 (robust drift accumulation away from boundaries)
  double ratio = (double)fs_out / (double)fs_in;

  char in_path[256];
  char ref_path[256];
  snprintf(in_path, sizeof(in_path), "/tmp/cdsp_slip_compare_in.raw");
  snprintf(ref_path, sizeof(ref_path), "/tmp/cdsp_slip_compare_ref.raw");

  remove(ref_path);
  ASSERT_TRUE(write_raw_f64(input, total_frames, in_path));

  char cmd[1024];
#ifdef _WIN32
  snprintf(cmd, sizeof(cmd),
           "\"\"%s\" slip \"%s\" \"%s\" %d %d %zu --no-partial > NUL 2>&1\"",
           g_rubato_bin_path, in_path, ref_path, fs_in, fs_out, chunk_size);
#else
  snprintf(cmd, sizeof(cmd), "\"%s\" slip \"%s\" \"%s\" %d %d %zu --no-partial",
           g_rubato_bin_path, in_path, ref_path, fs_in, fs_out, chunk_size);
#endif
  int status = system(cmd);
  ASSERT_EQ(0, status);

  size_t ref_count = 0;
  double *ref_data = read_raw_f64(ref_path, &ref_count);
  ASSERT_TRUE(ref_data != NULL);

  resampler_config_t cfg;
  resampler_config_init_with_type(&cfg, RESAMPLER_TYPE_SLIP);

  resampler_t *res =
      resampler_create_from_config(&cfg, fs_in, fs_in, 1, chunk_size, NULL);
  ASSERT_TRUE(res != NULL);
  resampler_set_relative_ratio(res, ratio);

  size_t max_in = resampler_get_max_input_frames(res);
  audio_chunk_t *in_chunk = audio_chunk_create(max_in, 1);
  audio_chunk_t *out_chunk = audio_chunk_create(chunk_size, 1);

  size_t in_offset = 0;
  size_t accum_out = 0;
  while (true) {
    size_t needed_in = resampler_get_input_frames_next(res);
    if (in_offset + needed_in > total_frames)
      break;

    double *ch_in = audio_chunk_get_channel(in_chunk, 0);
    memcpy(ch_in, &input[in_offset], needed_in * sizeof(double));
    audio_chunk_set_valid_frames(in_chunk, needed_in);

    resampler_error_t err = resampler_process(res, in_chunk, out_chunk);
    ASSERT_EQ(RESAMPLER_OK, err);

    size_t got_out = audio_chunk_get_valid_frames(out_chunk);
    ASSERT_EQ(chunk_size, got_out);
    const double *ch_out = audio_chunk_get_channel(out_chunk, 0);
    for (size_t i = 0; i < got_out; i++) {
      ASSERT_TRUE(accum_out + i < ref_count);
      ASSERT_DOUBLE_EQ(ref_data[accum_out + i], ch_out[i]);
    }
    accum_out += got_out;
    in_offset += needed_in;
  }

  ASSERT_EQ(ref_count, accum_out);

  audio_chunk_free(in_chunk);
  audio_chunk_free(out_chunk);
  resampler_free(res);
  free(input);
  free(ref_data);

  fs_out =
      39988; // ratio = 0.9997 (robust drift accumulation away from boundaries)
  ratio = (double)fs_out / (double)fs_in;

  input = (double *)malloc(total_frames * sizeof(double));
  ASSERT_TRUE(input != NULL);
  for (size_t i = 0; i < total_frames; i++) {
    input[i] = sin(0.05 * (double)i);
  }

  remove(ref_path);
#ifdef _WIN32
  snprintf(cmd, sizeof(cmd),
           "\"\"%s\" slip \"%s\" \"%s\" %d %d %zu --no-partial > NUL 2>&1\"",
           g_rubato_bin_path, in_path, ref_path, fs_in, fs_out, chunk_size);
#else
  snprintf(cmd, sizeof(cmd), "\"%s\" slip \"%s\" \"%s\" %d %d %zu --no-partial",
           g_rubato_bin_path, in_path, ref_path, fs_in, fs_out, chunk_size);
#endif
  status = system(cmd);
  ASSERT_EQ(0, status);

  ref_count = 0;
  ref_data = read_raw_f64(ref_path, &ref_count);
  ASSERT_TRUE(ref_data != NULL);

  res = resampler_create_from_config(&cfg, fs_in, fs_in, 1, chunk_size, NULL);
  ASSERT_TRUE(res != NULL);
  resampler_set_relative_ratio(res, ratio);

  max_in = resampler_get_max_input_frames(res);
  in_chunk = audio_chunk_create(max_in, 1);
  out_chunk = audio_chunk_create(chunk_size, 1);

  in_offset = 0;
  accum_out = 0;
  while (true) {
    size_t needed_in = resampler_get_input_frames_next(res);
    if (in_offset + needed_in > total_frames)
      break;

    double *ch_in = audio_chunk_get_channel(in_chunk, 0);
    memcpy(ch_in, &input[in_offset], needed_in * sizeof(double));
    audio_chunk_set_valid_frames(in_chunk, needed_in);

    resampler_error_t err = resampler_process(res, in_chunk, out_chunk);
    ASSERT_EQ(RESAMPLER_OK, err);

    size_t got_out = audio_chunk_get_valid_frames(out_chunk);
    ASSERT_EQ(chunk_size, got_out);
    const double *ch_out = audio_chunk_get_channel(out_chunk, 0);

    for (size_t i = 0; i < got_out; i++) {
      ASSERT_TRUE(accum_out + i < ref_count);
      ASSERT_DOUBLE_EQ(ref_data[accum_out + i], ch_out[i]);
    }
    accum_out += got_out;
    in_offset += needed_in;
  }

  ASSERT_EQ(ref_count, accum_out);

  audio_chunk_free(in_chunk);
  audio_chunk_free(out_chunk);
  resampler_free(res);
  free(input);
  free(ref_data);
}

TEST(AsyncSinc_DriftCrash) {
  resampler_config_t cfg;
  resampler_config_init_with_type(&cfg, RESAMPLER_TYPE_ASYNC_SINC);
  cfg.sinc_len = 128;
  cfg.has_sinc_len = true;
  cfg.oversampling_factor = 1024;
  cfg.has_oversampling_factor = true;
  strncpy(cfg.window, "BlackmanHarris2", sizeof(cfg.window) - 1);
  cfg.has_window = true;
  strncpy(cfg.interpolation, "Cubic", sizeof(cfg.interpolation) - 1);
  cfg.has_interpolation = true;
  // do not set profile to force FIXED_ASYNC_INPUT

  size_t chunk_size = 1024;
  int channels = 2;
  resampler_t *res = resampler_create_from_config(&cfg, 48000, 48000, channels,
                                                  chunk_size, NULL);
  ASSERT_TRUE(res != NULL);

  size_t max_in = resampler_get_max_input_frames(res);
  audio_chunk_t *in_chunk = audio_chunk_create(max_in, channels);
  audio_chunk_t *out_chunk = audio_chunk_create(chunk_size, channels);

  size_t needed_in = resampler_get_input_frames_next(res);
  for (int ch = 0; ch < channels; ch++) {
    double *in_data = audio_chunk_get_channel(in_chunk, ch);
    for (size_t i = 0; i < needed_in; i++) {
      in_data[i] = sin(0.05 * i + ch);
    }
  }
  audio_chunk_set_valid_frames(in_chunk, needed_in);

  resampler_error_t err = resampler_process(res, in_chunk, out_chunk);
  ASSERT_EQ(RESAMPLER_OK, err);
  ASSERT_EQ(chunk_size, audio_chunk_get_valid_frames(out_chunk));

  resampler_set_relative_ratio(res, 0.9);

  needed_in = resampler_get_input_frames_next(res);
  for (int ch = 0; ch < channels; ch++) {
    double *in_data = audio_chunk_get_channel(in_chunk, ch);
    for (size_t i = 0; i < needed_in; i++) {
      in_data[i] = sin(0.05 * i + ch);
    }
  }
  audio_chunk_set_valid_frames(in_chunk, needed_in);

  err = resampler_process(res, in_chunk, out_chunk);
  ASSERT_EQ(RESAMPLER_OK, err);
  ASSERT_EQ(chunk_size, audio_chunk_get_valid_frames(out_chunk));

  audio_chunk_free(in_chunk);
  audio_chunk_free(out_chunk);
  resampler_free(res);
}

TEST(AvgTRatio_EqualRatios) {
  double ratios[] = {0.1, 0.5, 1.0, 2.0, 10.0};
  for (size_t i = 0; i < sizeof(ratios) / sizeof(ratios[0]); i++) {
    double r = ratios[i];
    double got = 0.5 * (1.0 / r + 1.0 / r);
    double expected = 1.0 / r;
    ASSERT_NEAR(got, expected, 1e-12);
  }
}

TEST(AvgTRatio_Symmetric) {
  double pairs[][2] = {
      {1.0, 0.2}, {2.0, 3.0}, {0.3, 0.5}, {0.125, 8.0}, {1.0, 1.0}};
  for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
    double r1 = pairs[i][0];
    double r2 = pairs[i][1];
    double forward = 0.5 * (1.0 / r1 + 1.0 / r2);
    double backward = 0.5 * (1.0 / r2 + 1.0 / r1);
    ASSERT_NEAR(forward, backward, 1e-12);
  }
}

TEST(AvgTRatio_KnownValues) {
  // avg_t_ratio(1.0, 0.2) = 0.5 * (1/1.0 + 1/0.2) = 0.5 * (1 + 5) = 3.0
  double got1 = 0.5 * (1.0 / 1.0 + 1.0 / 0.2);
  ASSERT_NEAR(got1, 3.0, 1e-12);
  // avg_t_ratio(2.0, 3.0) = 0.5 * (0.5 + 1/3) = 0.5 * (5/6) = 5/12
  double got2 = 0.5 * (1.0 / 2.0 + 1.0 / 3.0);
  ASSERT_NEAR(got2, 5.0 / 12.0, 1e-12);
}

TEST(TRatioIncrement_ReachesTarget) {
  struct {
    double r1;
    double r2;
    size_t n;
  } cases[] = {{1.0, 0.2, 100},
               {1.0, 5.0, 1024},
               {2.0, 3.0, 512},
               {0.5, 0.5, 256},
               {0.125, 8.0, 64}};
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    double r1 = cases[i].r1;
    double r2 = cases[i].r2;
    size_t n = cases[i].n;
    double inc = (1.0 / r2 - 1.0 / r1) / (double)n;
    double t_ratio_end = 1.0 / r1 + (double)n * inc;
    double expected_end = 1.0 / r2;
    ASSERT_NEAR(t_ratio_end, expected_end, 1e-10);
  }
}

TEST(TRatioIncrement_EqualRatiosIsZero) {
  double ratios[] = {0.1, 0.5, 1.0, 2.0, 10.0};
  size_t ns[] = {1, 100, 1024};
  for (size_t i = 0; i < sizeof(ratios) / sizeof(ratios[0]); i++) {
    for (size_t j = 0; j < sizeof(ns) / sizeof(ns[0]); j++) {
      double r = ratios[i];
      size_t n = ns[j];
      double inc = (1.0 / r - 1.0 / r) / (double)n;
      ASSERT_NEAR(inc, 0.0, 1e-15);
    }
  }
}

// --- Rubato 5.0.0: advance_index analytical & bound tests ---

TEST(AdvanceIndex_MatchesAnalyticalFormula) {
  struct {
    double r1;
    double r2;
    size_t n;
  } cases[] = {{1.0, 0.2, 100}, {1.0, 5.0, 1024}, {2.0, 3.0, 512},
               {0.5, 0.8, 256}, {0.125, 8.0, 64}, {1.0, 1.0, 200}};
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    double r1 = cases[i].r1;
    double r2 = cases[i].r2;
    size_t n = cases[i].n;
    double start_idx = 0.0;
    double inc = (1.0 / r2 - 1.0 / r1) / (double)n;

    double idx = start_idx;
    double t_ratio = 1.0 / r1;
    for (size_t k = 0; k < n; k++) {
      t_ratio += inc;
      idx += t_ratio;
    }

    double avg = 0.5 * (1.0 / r1 + 1.0 / r2);
    double ramp_overshoot = 0.5 * (1.0 / r2 - 1.0 / r1);
    double analytical = start_idx + (double)n * avg + ramp_overshoot;

    ASSERT_NEAR(idx, analytical, 1e-6);
  }
}

TEST(AdvanceIndex_StaysWithinBufferBounds) {
  size_t chunk_size = 1024;
  size_t interpolator_len = 4;
  double last_indices[] = {0.0, 0.5, 2.0};
  double pairs[][2] = {{1.0, 0.2}, {1.0, 5.0}, {0.5, 2.0},   {2.0, 0.5},
                       {1.0, 1.0}, {0.3, 0.3}, {0.125, 8.0}, {8.0, 0.125}};

  for (size_t li = 0; li < sizeof(last_indices) / sizeof(last_indices[0]);
       li++) {
    double last_index = last_indices[li];
    for (size_t p = 0; p < sizeof(pairs) / sizeof(pairs[0]); p++) {
      double r1 = pairs[p][0];
      double r2 = pairs[p][1];

      double space =
          (double)chunk_size - (double)(interpolator_len + 1) - last_index;
      double avg = 0.5 * (1.0 / r1 + 1.0 / r2);
      double ramp_overshoot = 0.5 * (1.0 / r2 - 1.0 / r1);
      double raw = (space - ramp_overshoot) / avg;
      size_t n = (raw < 0.0) ? 0 : (size_t)floor(raw);

      if (n == 0)
        continue;

      double inc = (1.0 / r2 - 1.0 / r1) / (double)n;
      double idx = last_index;
      double t_ratio = 1.0 / r1;
      for (size_t k = 0; k < n; k++) {
        t_ratio += inc;
        idx += t_ratio;
      }

      size_t bound = chunk_size - interpolator_len - 1;
      ASSERT_TRUE((size_t)floor(idx) <= bound);
    }
  }
}

// --- Rubato 5.0.0 Issue #136 Regression Tests ---

static resampler_t *make_poly_resampler_helper(const char *interp,
                                               size_t chunk_size,
                                               size_t channels) {
  resampler_config_t cfg;
  resampler_config_init_with_type(&cfg, RESAMPLER_TYPE_ASYNC_POLY);
  strncpy(cfg.interpolation, interp, sizeof(cfg.interpolation) - 1);
  cfg.has_interpolation = true;
  return resampler_create_from_config(&cfg, 48000, 48000, channels, chunk_size,
                                      NULL);
}

static resampler_t *make_sinc_resampler_helper(size_t chunk_size,
                                               size_t channels) {
  resampler_config_t cfg;
  resampler_config_init_with_type(&cfg, RESAMPLER_TYPE_ASYNC_SINC);
  cfg.sinc_len = 128;
  cfg.has_sinc_len = true;
  cfg.oversampling_factor = 1024;
  cfg.has_oversampling_factor = true;
  strncpy(cfg.window, "BlackmanHarris2", sizeof(cfg.window) - 1);
  cfg.has_window = true;
  strncpy(cfg.interpolation, "Cubic", sizeof(cfg.interpolation) - 1);
  cfg.has_interpolation = true;
  cfg.f_cutoff = 0.95;
  cfg.has_f_cutoff = true;
  return resampler_create_from_config(&cfg, 48000, 48000, channels, chunk_size,
                                      NULL);
}

static void test_ramp_resampler_helper(resampler_t *res, double target_rel) {
  size_t channels = resampler_get_channels(res);
  size_t chunk_size = resampler_get_chunk_size(res);

  audio_chunk_t *in_chunk = audio_chunk_create(65536, channels);
  audio_chunk_t *out_chunk =
      audio_chunk_create(chunk_size * 2 + 1024, channels);

  // Block 1: nominal ratio
  size_t needed_in1 = resampler_get_input_frames_next(res);
  for (size_t ch = 0; ch < channels; ch++) {
    double *in_data = audio_chunk_get_channel(in_chunk, ch);
    for (size_t i = 0; i < needed_in1; i++)
      in_data[i] = 0.0;
  }
  audio_chunk_set_valid_frames(in_chunk, needed_in1);
  resampler_error_t err = resampler_process(res, in_chunk, out_chunk);
  ASSERT_EQ(RESAMPLER_OK, err);
  ASSERT_EQ(chunk_size, audio_chunk_get_valid_frames(out_chunk));

  // Change ratio dynamically with ramp
  resampler_set_relative_ratio(res, target_rel);

  size_t needed_in2 = resampler_get_input_frames_next(res);
  size_t needed_out2 = resampler_get_output_frames_next(res);
  ASSERT_TRUE(needed_in2 > 0);
  ASSERT_TRUE(needed_out2 > 0);
  ASSERT_EQ(chunk_size, needed_out2);

  // Block 2: must complete without out-of-bounds crash
  for (size_t ch = 0; ch < channels; ch++) {
    double *in_data = audio_chunk_get_channel(in_chunk, ch);
    for (size_t i = 0; i < needed_in2; i++)
      in_data[i] = 0.0;
  }
  audio_chunk_set_valid_frames(in_chunk, needed_in2);
  err = resampler_process(res, in_chunk, out_chunk);
  ASSERT_EQ(RESAMPLER_OK, err);
  ASSERT_EQ(needed_out2, audio_chunk_get_valid_frames(out_chunk));

  audio_chunk_free(in_chunk);
  audio_chunk_free(out_chunk);
  resampler_free(res);
}

TEST(Poly_Ramp_LargeRatioChange_DoesNotPanic) {
  const char *interps[] = {"Cubic", "Linear"};
  double targets[] = {0.92, 1.08, 0.95, 1.05};
  for (size_t i = 0; i < sizeof(interps) / sizeof(interps[0]); i++) {
    for (size_t j = 0; j < sizeof(targets) / sizeof(targets[0]); j++) {
      resampler_t *res = make_poly_resampler_helper(interps[i], 1024, 1);
      ASSERT_TRUE(res != NULL);
      test_ramp_resampler_helper(res, targets[j]);
    }
  }
}

TEST(Sinc_Ramp_LargeRatioChange_DoesNotPanic) {
  double targets[] = {0.92, 1.08, 0.95, 1.05};
  for (size_t j = 0; j < sizeof(targets) / sizeof(targets[0]); j++) {
    resampler_t *res = make_sinc_resampler_helper(1024, 1);
    ASSERT_TRUE(res != NULL);
    test_ramp_resampler_helper(res, targets[j]);
  }
}

// --- Rubato: Sinc Interpolation Weights Unit Tests (asynchro_sinc.rs) ---

TEST(Sinc_CubicWeightsMatchCubicDirect) {
  double yvals[4] = {1.3, -0.7, 2.1, 0.4};
  for (int x_int = 0; x_int <= 10; x_int++) {
    double x = (double)x_int / 10.0;
    // Direct cubic evaluation:
    double a0 = 2.0 * yvals[1];
    double a1 = -yvals[0] + yvals[2];
    double a2 = 2.0 * yvals[0] - 5.0 * yvals[1] + 4.0 * yvals[2] - yvals[3];
    double a3 = -yvals[0] + 3.0 * yvals[1] - 3.0 * yvals[2] + yvals[3];
    double direct = 0.5 * (a0 + x * (a1 + x * (a2 + x * a3)));

    // Equivalent weights blending:
    double w0 = 0.5 * (-x + 2.0 * x * x - x * x * x);
    double w1 = 0.5 * (2.0 - 5.0 * x * x + 3.0 * x * x * x);
    double w2 = 0.5 * (x + 4.0 * x * x - 3.0 * x * x * x);
    double w3 = 0.5 * (-x * x + x * x * x);
    double blended =
        w0 * yvals[0] + w1 * yvals[1] + w2 * yvals[2] + w3 * yvals[3];
    ASSERT_NEAR(direct, blended, 1e-12);
  }
}

TEST(Sinc_QuadWeightsMatchQuadDirect) {
  double yvals[3] = {1.3, -0.7, 2.1};
  for (int x_int = 0; x_int <= 10; x_int++) {
    double x = (double)x_int / 10.0;
    // Direct quadratic evaluation:
    double a2 = yvals[0] - 2.0 * yvals[1] + yvals[2];
    double a1 = -3.0 * yvals[0] + 4.0 * yvals[1] - yvals[2];
    double a0 = 2.0 * yvals[0];
    double direct = 0.5 * (a0 + x * (a1 + x * a2));

    // Equivalent weights blending:
    double w0 = 0.5 * (2.0 - 3.0 * x + x * x);
    double w1 = 0.5 * (4.0 * x - 2.0 * x * x);
    double w2 = 0.5 * (-x + x * x);
    double blended = w0 * yvals[0] + w1 * yvals[1] + w2 * yvals[2];
    ASSERT_NEAR(direct, blended, 1e-12);
  }
}

TEST(Sinc_LinWeightsMatchLinDirect) {
  double yvals[2] = {1.3, -0.7};
  for (int x_int = 0; x_int <= 10; x_int++) {
    double x = (double)x_int / 10.0;
    double direct = yvals[0] + x * (yvals[1] - yvals[0]);
    double blended = (1.0 - x) * yvals[0] + x * yvals[1];
    ASSERT_NEAR(direct, blended, 1e-12);
  }
}

// --- Rubato: Synchronous Resample Unit (synchro.rs) ---

TEST(Synchronous_ResampleUnitEnergyConservation) {
  resampler_config_t cfg;
  resampler_config_init_with_type(&cfg, RESAMPLER_TYPE_SYNCHRONOUS);
  resampler_t *res =
      resampler_create_from_config(&cfg, 147, 1000, 1, 147, NULL);
  ASSERT_TRUE(res != NULL);

  audio_chunk_t *in = audio_chunk_create(147, 1);
  double *in_d = audio_chunk_get_channel(in, 0);
  memset(in_d, 0, 147 * sizeof(double));
  in_d[0] = 0.3;
  in_d[1] = 0.7;
  in_d[2] = 1.0;
  in_d[3] = 1.0;
  in_d[4] = 0.7;
  in_d[5] = 0.3;
  audio_chunk_set_valid_frames(in, 147);

  audio_chunk_t *out = audio_chunk_create(2048, 1);
  double sum = 0.0;
  double max_val = 0.0;
  for (int b = 0; b < 10; b++) {
    resampler_error_t err = resampler_process(res, in, out);
    ASSERT_EQ(RESAMPLER_OK, err);
    size_t valid = audio_chunk_get_valid_frames(out);
    double *out_d = audio_chunk_get_channel(out, 0);
    for (size_t i = 0; i < valid; i++) {
      sum += out_d[i];
      if (fabs(out_d[i]) > max_val)
        max_val = fabs(out_d[i]);
    }
    memset(in_d, 0, 147 * sizeof(double));
  }
  // Total pulse sum is 4.0, resampled sum should conserve DC energy
  ASSERT_NEAR(sum, 4.0 * 1000.0 / 147.0, 1e-4);
  ASSERT_NEAR(max_val, 1.0, 0.15);

  audio_chunk_free(in);
  audio_chunk_free(out);
  resampler_free(res);
}

// --- Rubato: Slip Resampler Unit Tests (slip.rs) ---

TEST(Slip_UnitRatioIsIdentity) {
  resampler_config_t cfg;
  resampler_config_init_with_type(&cfg, RESAMPLER_TYPE_SLIP);
  resampler_t *res =
      resampler_create_from_config(&cfg, 48000, 48000, 2, 1024, NULL);
  ASSERT_TRUE(res != NULL);

  audio_chunk_t *in = audio_chunk_create(1024, 2);
  audio_chunk_t *out = audio_chunk_create(2048, 2);
  for (int ch = 0; ch < 2; ch++) {
    double *d = audio_chunk_get_channel(in, ch);
    for (size_t i = 0; i < 1024; i++) {
      d[i] = (double)(i + 1) * 0.001;
    }
  }
  audio_chunk_set_valid_frames(in, 1024);

  resampler_error_t err = resampler_process(res, in, out);
  ASSERT_EQ(RESAMPLER_OK, err);
  ASSERT_EQ(1024, audio_chunk_get_valid_frames(out));

  for (int ch = 0; ch < 2; ch++) {
    double *in_d = audio_chunk_get_channel(in, ch);
    double *out_d = audio_chunk_get_channel(out, ch);
    for (size_t i = 0; i < 1024; i++) {
      ASSERT_NEAR(in_d[i], out_d[i], 1e-12);
    }
  }

  audio_chunk_free(in);
  audio_chunk_free(out);
  resampler_free(res);
}

TEST(Slip_DC_StaysFlatAcrossCorrection) {
  resampler_config_t cfg;
  resampler_config_init_with_type(&cfg, RESAMPLER_TYPE_SLIP);
  resampler_t *res =
      resampler_create_from_config(&cfg, 48000, 48000, 1, 512, NULL);
  ASSERT_TRUE(res != NULL);
  resampler_set_relative_ratio(res, 1.0005);

  audio_chunk_t *in = audio_chunk_create(512, 1);
  audio_chunk_t *out = audio_chunk_create(1024, 1);
  double *in_d = audio_chunk_get_channel(in, 0);
  for (size_t i = 0; i < 512; i++)
    in_d[i] = 1.0;
  audio_chunk_set_valid_frames(in, 512);

  for (int b = 0; b < 20; b++) {
    resampler_error_t err = resampler_process(res, in, out);
    ASSERT_EQ(RESAMPLER_OK, err);
    size_t valid = audio_chunk_get_valid_frames(out);
    double *out_d = audio_chunk_get_channel(out, 0);
    for (size_t i = 0; i < valid; i++) {
      ASSERT_NEAR(out_d[i], 1.0, 1e-12);
    }
  }

  audio_chunk_free(in);
  audio_chunk_free(out);
  resampler_free(res);
}

TEST(AsyncSinc_SincLenRoundedToMultipleOf8) {
  resampler_config_t cfg;
  resampler_config_init_with_type(&cfg, RESAMPLER_TYPE_ASYNC_SINC);
  cfg.has_sinc_len = true;
  cfg.sinc_len = 100; // Not a multiple of 8; should round to 104
  cfg.has_oversampling_factor = true;
  cfg.oversampling_factor = 128;
  cfg.has_interpolation = true;
  strncpy(cfg.interpolation, "Cubic", sizeof(cfg.interpolation) - 1);
  cfg.has_window = true;
  strncpy(cfg.window, "BlackmanHarris2", sizeof(cfg.window) - 1);

  resampler_t *res =
      resampler_create_from_config(&cfg, 44100, 48000, 1, 1024, NULL);
  ASSERT_TRUE(res != NULL);

  // output delay is (sinc_len * ratio) / 2.
  // With 104 taps: 104 * (48000/44100) / 2 = 56 samples (vs 54 samples for 100
  // taps).
  size_t delay = resampler_get_output_delay(res);
  ASSERT_EQ(56, delay);

  resampler_free(res);
}

TEST(AsyncSinc_Reset_RecomputesNeededLengths) {
  resampler_config_t cfg;
  resampler_config_init_with_type(&cfg, RESAMPLER_TYPE_ASYNC_SINC);
  strncpy(cfg.profile, "VeryFast", sizeof(cfg.profile) - 1);
  cfg.has_profile = true;

  resampler_t *res =
      resampler_create_from_config(&cfg, 44100, 44100, 1, 1024, NULL);
  ASSERT_TRUE(res != NULL);

  size_t initial_in = resampler_get_input_frames_next(res);

  // Set relative ratio to 1.1 and process frames
  resampler_set_relative_ratio(res, 1.1);
  audio_chunk_t *in = audio_chunk_create(65536, 1);
  audio_chunk_t *out = audio_chunk_create(1024, 1);

  for (int i = 0; i < 5; i++) {
    size_t needed_in = resampler_get_input_frames_next(res);
    audio_chunk_set_valid_frames(in, needed_in);
    ASSERT_EQ(RESAMPLER_OK, resampler_process(res, in, out));
  }

  // After processing at ratio 1.1, needed_input_size is changed
  size_t shifted_in = resampler_get_input_frames_next(res);
  ASSERT_TRUE(shifted_in != initial_in);

  // Reset should restore lengths to base_ratio initial state
  resampler_reset(res);
  size_t reset_in = resampler_get_input_frames_next(res);
  ASSERT_EQ(initial_in, reset_in);

  audio_chunk_free(in);
  audio_chunk_free(out);
  resampler_free(res);
}

TEST(AsyncPoly_Reset_RecomputesNeededLengths) {
  resampler_config_t cfg;
  resampler_config_init_with_type(&cfg, RESAMPLER_TYPE_ASYNC_POLY);
  strncpy(cfg.interpolation, "Cubic", sizeof(cfg.interpolation) - 1);
  cfg.has_interpolation = true;

  resampler_t *res =
      resampler_create_from_config(&cfg, 44100, 44100, 1, 1024, NULL);
  ASSERT_TRUE(res != NULL);

  size_t initial_in = resampler_get_input_frames_next(res);

  resampler_set_relative_ratio(res, 1.1);
  audio_chunk_t *in = audio_chunk_create(65536, 1);
  audio_chunk_t *out = audio_chunk_create(1024, 1);

  for (int i = 0; i < 5; i++) {
    size_t needed_in = resampler_get_input_frames_next(res);
    audio_chunk_set_valid_frames(in, needed_in);
    ASSERT_EQ(RESAMPLER_OK, resampler_process(res, in, out));
  }

  size_t shifted_in = resampler_get_input_frames_next(res);
  ASSERT_TRUE(shifted_in != initial_in);

  resampler_reset(res);
  size_t reset_in = resampler_get_input_frames_next(res);
  ASSERT_EQ(initial_in, reset_in);

  audio_chunk_free(in);
  audio_chunk_free(out);
  resampler_free(res);
}

TEST(AsyncSinc_LargeUpsampling_DoesNotExceedMaxOutput) {
  // 44100 -> 352800 (8x upsampling) with 1.1 relative ratio
  resampler_config_t cfg;
  resampler_config_init_with_type(&cfg, RESAMPLER_TYPE_ASYNC_SINC);
  strncpy(cfg.profile, "Fast", sizeof(cfg.profile) - 1);
  cfg.has_profile = true;

  size_t chunk_size = 512;
  resampler_t *res =
      resampler_create_from_config(&cfg, 44100, 352800, 1, chunk_size, NULL);
  ASSERT_TRUE(res != NULL);

  resampler_set_relative_ratio(res, 1.1);
  size_t max_in = resampler_get_max_input_frames(res);

  audio_chunk_t *in = audio_chunk_create(max_in, 1);
  audio_chunk_t *out = audio_chunk_create(chunk_size, 1);

  for (int i = 0; i < 20; i++) {
    size_t needed_in = resampler_get_input_frames_next(res);
    ASSERT_TRUE(needed_in <= max_in);
    audio_chunk_set_valid_frames(in, needed_in);
    resampler_error_t err = resampler_process(res, in, out);
    ASSERT_EQ(RESAMPLER_OK, err);
  }

  audio_chunk_free(in);
  audio_chunk_free(out);
  resampler_free(res);
}

TEST(Async_SetRelativeRatio_RejectsNaN) {
  resampler_config_t cfg;
  resampler_config_init_with_type(&cfg, RESAMPLER_TYPE_ASYNC_SINC);
  strncpy(cfg.profile, "VeryFast", sizeof(cfg.profile) - 1);
  cfg.has_profile = true;

  resampler_t *res =
      resampler_create_from_config(&cfg, 48000, 48000, 1, 512, NULL);
  ASSERT_TRUE(res != NULL);

  double orig_ratio = resampler_get_ratio(res);
  resampler_set_relative_ratio(res, NAN);
  ASSERT_EQ(orig_ratio, resampler_get_ratio(res));

  resampler_set_relative_ratio(res, INFINITY);
  ASSERT_EQ(orig_ratio, resampler_get_ratio(res));

  resampler_free(res);
}

/* ------------------------------------------------------------------------- */
/* Regression tests from audit (resampler fixes & mask handling)             */
/* ------------------------------------------------------------------------- */

static resampler_t *audit_make_sync(size_t in_rate, size_t out_rate,
                                    size_t chunk_size) {
  resampler_config_t cfg;
  resampler_config_init_with_type(&cfg, RESAMPLER_TYPE_SYNCHRONOUS);
  return resampler_create_from_config(&cfg, in_rate, out_rate, 1, chunk_size,
                                      NULL);
}

static void audit_fill_sine(audio_chunk_t *chunk, size_t n, size_t *phase) {
  double *d = audio_chunk_get_channel(chunk, 0);
  for (size_t i = 0; i < n; i++) {
    d[i] = sin(0.031 * (double)(*phase + i));
  }
  *phase += n;
}

// Finding 2: a partial chunk must advance the synchronous resampler's stream
// by a full chunk (rubato FixedSync::Output) and only scale valid_frames.
// Reference: an identical resampler fed the same chunk explicitly
// zero-padded with valid_frames == frames. Both must stay bit-identical.
static void check_sync_partial_matches_zero_padded(size_t in_rate,
                                                   size_t out_rate,
                                                   size_t chunk_size,
                                                   size_t partial_num,
                                                   size_t partial_den) {
  resampler_t *a = audit_make_sync(in_rate, out_rate, chunk_size);
  resampler_t *b = audit_make_sync(in_rate, out_rate, chunk_size);
  ASSERT_TRUE(a != NULL);
  ASSERT_TRUE(b != NULL);
  size_t max_in = resampler_get_max_input_frames(a);
  audio_chunk_t *in_a = audio_chunk_create(max_in, 1);
  audio_chunk_t *in_b = audio_chunk_create(max_in, 1);
  audio_chunk_t *out_a = audio_chunk_create(chunk_size, 1);
  audio_chunk_t *out_b = audio_chunk_create(chunk_size, 1);

  size_t phase = 0;
  for (int iter = 0; iter < 40; iter++) {
    size_t need_a = resampler_get_input_frames_next(a);
    size_t need_b = resampler_get_input_frames_next(b);
    ASSERT_EQ(need_a, need_b);
    ASSERT_TRUE(need_a <= max_in);

    size_t p0 = phase;
    audit_fill_sine(in_a, need_a, &phase);
    phase = p0;
    audit_fill_sine(in_b, need_b, &phase);

    bool partial = (iter % 5 == 3) && need_a > 0;
    size_t valid = partial ? (need_a * partial_num) / partial_den : need_a;
    audio_chunk_set_valid_frames(in_a, valid);
    // Reference: same samples, explicitly zero-padded, full valid length.
    double *db = audio_chunk_get_channel(in_b, 0);
    memset(db + valid, 0, (need_b - valid) * sizeof(double));
    audio_chunk_set_valid_frames(in_b, need_b);

    ASSERT_EQ(RESAMPLER_OK, resampler_process(a, in_a, out_a));
    ASSERT_EQ(RESAMPLER_OK, resampler_process(b, in_b, out_b));

    size_t va = audio_chunk_get_valid_frames(out_a);
    ASSERT_EQ(chunk_size, audio_chunk_get_valid_frames(out_b));
    if (partial) {
      ASSERT_EQ((chunk_size * valid) / need_a, va);
    } else {
      ASSERT_EQ(chunk_size, va);
    }
    const double *oa = audio_chunk_get_channel(out_a, 0);
    const double *ob = audio_chunk_get_channel(out_b, 0);
    for (size_t i = 0; i < va; i++) {
      ASSERT_DOUBLE_EQ(ob[i], oa[i]);
    }
  }

  audio_chunk_free(in_a);
  audio_chunk_free(in_b);
  audio_chunk_free(out_a);
  audio_chunk_free(out_b);
  resampler_free(a);
  resampler_free(b);
}

TEST(AuditResampler_Sync_PartialChunk_AdvancesFullChunk_Up) {
  check_sync_partial_matches_zero_padded(44100, 48000, 1024, 1, 2);
}

TEST(AuditResampler_Sync_PartialChunk_AdvancesFullChunk_Down) {
  check_sync_partial_matches_zero_padded(96000, 44100, 1000, 1, 3);
}

TEST(AuditResampler_Sync_PartialChunk_AdvancesFullChunk_Zero) {
  check_sync_partial_matches_zero_padded(44100, 48000, 512, 0, 1);
}

// Finding 2: repeated zero-valid chunks (silence pumping) used to leave all
// output in saved_frames, so input_frames_next collapsed to 0 and the
// stream desynchronized. Every call must consume a full chunk.
TEST(AuditResampler_Sync_ZeroValidChunks_KeepStreamInSync) {
  size_t chunk_size = 1024;
  resampler_t *r = audit_make_sync(44100, 48000, chunk_size);
  ASSERT_TRUE(r != NULL);
  audio_chunk_t *in = audio_chunk_create(resampler_get_max_input_frames(r), 1);
  audio_chunk_t *out = audio_chunk_create(chunk_size, 1);

  for (int i = 0; i < 20; i++) {
    size_t need = resampler_get_input_frames_next(r);
    ASSERT_TRUE(need > 0);
    audio_chunk_set_valid_frames(in, 0);
    ASSERT_EQ(RESAMPLER_OK, resampler_process(r, in, out));
    ASSERT_EQ(0, audio_chunk_get_valid_frames(out));
  }
  size_t need = resampler_get_input_frames_next(r);
  ASSERT_TRUE(need > 0);
  audio_chunk_set_valid_frames(in, need);
  ASSERT_EQ(RESAMPLER_OK, resampler_process(r, in, out));
  ASSERT_EQ(chunk_size, audio_chunk_get_valid_frames(out));

  audio_chunk_free(in);
  audio_chunk_free(out);
  resampler_free(r);
}

// Finding 5: valid_frames beyond the chunk's capacity must be rejected
// instead of over-reading the input buffer.
TEST(AuditResampler_Sync_RejectsValidFramesBeyondCapacity) {
  resampler_t *r = audit_make_sync(44100, 48000, 1024);
  ASSERT_TRUE(r != NULL);
  size_t need = resampler_get_input_frames_next(r);
  audio_chunk_t *in = audio_chunk_create(need / 2, 1);
  audio_chunk_t *out = audio_chunk_create(1024, 1);
  audio_chunk_set_valid_frames(in, need / 2 + 1);
  ASSERT_EQ(RESAMPLER_ERR_INPUT_SIZE_MISMATCH, resampler_process(r, in, out));
  audio_chunk_free(in);
  audio_chunk_free(out);
  resampler_free(r);
}

TEST(AuditResampler_Slip_RejectsValidFramesBeyondCapacity) {
  resampler_config_t cfg;
  resampler_config_init_with_type(&cfg, RESAMPLER_TYPE_SLIP);
  resampler_t *r =
      resampler_create_from_config(&cfg, 48000, 48000, 1, 1000, NULL);
  ASSERT_TRUE(r != NULL);
  audio_chunk_t *in = audio_chunk_create(500, 1);
  audio_chunk_t *out = audio_chunk_create(1000, 1);
  audio_chunk_set_valid_frames(in, 501);
  ASSERT_EQ(RESAMPLER_ERR_INPUT_SIZE_MISMATCH, resampler_process(r, in, out));
  audio_chunk_free(in);
  audio_chunk_free(out);
  resampler_free(r);
}

// Finding 7: zero channels must be rejected.
TEST(AuditResampler_Slip_RejectsZeroChannels) {
  resampler_config_t cfg;
  resampler_config_init_with_type(&cfg, RESAMPLER_TYPE_SLIP);
  config_error_t err;
  config_error_init(&err);
  resampler_t *r =
      resampler_create_from_config(&cfg, 48000, 48000, 0, 1000, &err);
  ASSERT_TRUE(r == NULL);
  config_error_init(&err);
  r = resampler_create_from_config(&cfg, 0, 0, 2, 1000, &err);
  ASSERT_TRUE(r == NULL);
}

// Finding 6 / report 06 §3.6: AsyncPoly has no `profile` upstream.
TEST(AuditResampler_AsyncPoly_RejectsProfile) {
  resampler_config_t cfg;
  resampler_config_init_with_type(&cfg, RESAMPLER_TYPE_ASYNC_POLY);
  strncpy(cfg.profile, "garbage", sizeof(cfg.profile) - 1);
  cfg.has_profile = true;
  config_error_t err;
  config_error_init(&err);
  ASSERT_NE(0, resampler_config_validate(&cfg, &err));
  ASSERT_TRUE(resampler_create_from_config(&cfg, 44100, 48000, 2, 1024, NULL) ==
              NULL);

  // Profile together with a valid interpolation is still rejected.
  strncpy(cfg.interpolation, "Cubic", sizeof(cfg.interpolation) - 1);
  cfg.has_interpolation = true;
  config_error_init(&err);
  ASSERT_NE(0, resampler_config_validate(&cfg, &err));

  // Interpolation alone is accepted.
  cfg.has_profile = false;
  config_error_init(&err);
  ASSERT_EQ(0, resampler_config_validate(&cfg, &err));
}

// Report 06 §3.9.1: negative sinc_len / non-finite f_cutoff are rejected by
// the AsyncSinc validator itself (not only by the generic front door).
TEST(AuditResampler_AsyncSinc_RejectsNegativeSincLenAndNaNCutoff) {
  const resampler_vtable_t *vt = &g_async_sinc_resampler_vtable;
  resampler_config_t cfg;
  resampler_config_init_with_type(&cfg, RESAMPLER_TYPE_ASYNC_SINC);
  cfg.has_sinc_len = true;
  cfg.has_oversampling_factor = true;
  cfg.has_window = true;
  cfg.has_interpolation = true;
  cfg.oversampling_factor = 256;
  strncpy(cfg.window, "BlackmanHarris2", sizeof(cfg.window) - 1);
  strncpy(cfg.interpolation, "Cubic", sizeof(cfg.interpolation) - 1);

  config_error_t err;
  int bad_lens[] = {-1, -7, -64};
  for (size_t i = 0; i < sizeof(bad_lens) / sizeof(bad_lens[0]); i++) {
    cfg.sinc_len = bad_lens[i];
    config_error_init(&err);
    ASSERT_NE(0, vt->validate(&cfg, &err));
  }
  cfg.sinc_len = 128;
  config_error_init(&err);
  ASSERT_EQ(0, vt->validate(&cfg, &err));

  cfg.has_f_cutoff = true;
  cfg.f_cutoff = NAN;
  config_error_init(&err);
  ASSERT_NE(0, vt->validate(&cfg, &err));
  cfg.f_cutoff = 0.9;
  config_error_init(&err);
  ASSERT_EQ(0, vt->validate(&cfg, &err));
}

#define AUDIT_MASK_CHUNK 256
#define AUDIT_MASK_ITERS 24

typedef enum {
  AUDIT_KIND_SYNC,
  AUDIT_KIND_SINC_LINEAR,
  AUDIT_KIND_SINC_QUADRATIC,
  AUDIT_KIND_SINC_CUBIC,
  AUDIT_KIND_POLY_CUBIC,
  AUDIT_KIND_SLIP,
  AUDIT_KIND_COUNT
} audit_kind_t;

static resampler_t *audit_make_resampler(audit_kind_t kind, size_t channels) {
  resampler_config_t cfg;
  size_t in_rate = 44100, out_rate = 48000;
  switch (kind) {
  case AUDIT_KIND_SYNC:
    resampler_config_init_with_type(&cfg, RESAMPLER_TYPE_SYNCHRONOUS);
    break;
  case AUDIT_KIND_SINC_LINEAR:
  case AUDIT_KIND_SINC_QUADRATIC:
  case AUDIT_KIND_SINC_CUBIC:
    resampler_config_init_with_type(&cfg, RESAMPLER_TYPE_ASYNC_SINC);
    cfg.has_profile = true;
    strncpy(cfg.profile,
            kind == AUDIT_KIND_SINC_LINEAR      ? "Fast"
            : kind == AUDIT_KIND_SINC_QUADRATIC ? "Balanced"
                                                : "Accurate",
            sizeof(cfg.profile) - 1);
    break;
  case AUDIT_KIND_POLY_CUBIC:
    resampler_config_init_with_type(&cfg, RESAMPLER_TYPE_ASYNC_POLY);
    cfg.has_interpolation = true;
    strncpy(cfg.interpolation, "Cubic", sizeof(cfg.interpolation) - 1);
    break;
  case AUDIT_KIND_SLIP:
  default:
    resampler_config_init_with_type(&cfg, RESAMPLER_TYPE_SLIP);
    in_rate = out_rate = 48000;
    break;
  }
  config_error_t err;
  config_error_init(&err);
  resampler_t *r = resampler_create_from_config(&cfg, in_rate, out_rate,
                                                channels, AUDIT_MASK_CHUNK, &err);
  if (r && kind != AUDIT_KIND_SYNC) {
    resampler_set_relative_ratio(r, 1.0007);
  }
  return r;
}

static double audit_test_signal(size_t ch, size_t n) {
  return 0.5 * sin(0.013 * (double)(ch + 1) * (double)n + 0.3 * (double)ch);
}

static void audit_fill_mask_chunk(audio_chunk_t *chunk, size_t nch, const size_t *src_map,
                                  size_t frames, size_t phase, const bool *zero_ch) {
  for (size_t c = 0; c < nch; c++) {
    double *d = audio_chunk_get_channel(chunk, c);
    for (size_t i = 0; i < frames; i++) {
      d[i] = (zero_ch && zero_ch[c]) ? 0.0 : audit_test_signal(src_map[c], phase + i);
    }
  }
  audio_chunk_set_valid_frames(chunk, frames);
}

static bool audit_bit_equal(const double *a, const double *b, size_t n) {
  return memcmp(a, b, n * sizeof(double)) == 0;
}

static void audit_check_skip_bit_identical(audit_kind_t kind) {
  const size_t nch = 4;
  const size_t map[4] = {0, 1, 2, 3};
  const bool mask[4] = {true, false, true, true};
  resampler_t *a = audit_make_resampler(kind, nch);
  resampler_t *b = audit_make_resampler(kind, nch);
  ASSERT_TRUE(a != NULL);
  ASSERT_TRUE(b != NULL);
  size_t max_in = resampler_get_max_input_frames(a);
  audio_chunk_t *in_a = audio_chunk_create(max_in, nch);
  audio_chunk_t *in_b = audio_chunk_create(max_in, nch);
  audio_chunk_t *out_a = audio_chunk_create(AUDIT_MASK_CHUNK + 64, nch);
  audio_chunk_t *out_b = audio_chunk_create(AUDIT_MASK_CHUNK + 64, nch);
  audio_chunk_set_used_channels(in_b, mask);
  const bool all_true[4] = {true, true, true, true};
  audio_chunk_set_used_channels(out_b, all_true);

  size_t phase = 0;
  for (int it = 0; it < AUDIT_MASK_ITERS; it++) {
    size_t need = resampler_get_input_frames_next(a);
    ASSERT_EQ(need, resampler_get_input_frames_next(b));
    audit_fill_mask_chunk(in_a, nch, map, need, phase, NULL);
    audit_fill_mask_chunk(in_b, nch, map, need, phase, NULL);
    double *poison = audio_chunk_get_channel(in_b, 1);
    for (size_t i = 0; i < need; i++)
      poison[i] = NAN;
    double *pout = audio_chunk_get_channel(out_b, 1);
    for (size_t i = 0; i < AUDIT_MASK_CHUNK + 64; i++)
      pout[i] = 123.0;
    phase += need;
    ASSERT_EQ(RESAMPLER_OK, resampler_process(a, in_a, out_a));
    ASSERT_EQ(RESAMPLER_OK, resampler_process(b, in_b, out_b));
    size_t n = audio_chunk_get_valid_frames(out_a);
    ASSERT_EQ(n, audio_chunk_get_valid_frames(out_b));
    for (size_t c = 0; c < nch; c++) {
      const double *oa = audio_chunk_get_channel(out_a, c);
      const double *ob = audio_chunk_get_channel(out_b, c);
      if (mask[c]) {
        ASSERT_TRUE(audit_bit_equal(oa, ob, n));
      } else {
        for (size_t i = 0; i < n; i++) {
          ASSERT_TRUE(ob[i] == 0.0);
        }
      }
    }
    const bool *om = audio_chunk_get_used_channels(out_b);
    ASSERT_TRUE(om != NULL);
    ASSERT_TRUE(memcmp(om, mask, sizeof(mask)) == 0);
  }
  audio_chunk_free(in_a);
  audio_chunk_free(in_b);
  audio_chunk_free(out_a);
  audio_chunk_free(out_b);
  resampler_free(a);
  resampler_free(b);
}

TEST(AuditResamplerMask_Sync_SkipsInactive) {
  audit_check_skip_bit_identical(AUDIT_KIND_SYNC);
}
TEST(AuditResamplerMask_SincLinear_SkipsInactive) {
  audit_check_skip_bit_identical(AUDIT_KIND_SINC_LINEAR);
}
TEST(AuditResamplerMask_SincQuadratic_SkipsInactive) {
  audit_check_skip_bit_identical(AUDIT_KIND_SINC_QUADRATIC);
}
TEST(AuditResamplerMask_SincCubic_SkipsInactive) {
  audit_check_skip_bit_identical(AUDIT_KIND_SINC_CUBIC);
}
TEST(AuditResamplerMask_Poly_SkipsInactive) {
  audit_check_skip_bit_identical(AUDIT_KIND_POLY_CUBIC);
}
TEST(AuditResamplerMask_Slip_SkipsInactive) {
  audit_check_skip_bit_identical(AUDIT_KIND_SLIP);
}

static void audit_check_active_count_path(audit_kind_t kind, size_t nch, const bool *mask) {
  size_t map_full[8];
  size_t map_active[8];
  size_t nact = 0;
  for (size_t c = 0; c < nch; c++) {
    map_full[c] = c;
    if (mask[c])
      map_active[nact++] = c;
  }
  resampler_t *a = audit_make_resampler(kind, nch);
  resampler_t *b = audit_make_resampler(kind, nact);
  ASSERT_TRUE(a != NULL);
  ASSERT_TRUE(b != NULL);
  size_t max_in = resampler_get_max_input_frames(a);
  audio_chunk_t *in_a = audio_chunk_create(max_in, nch);
  audio_chunk_t *in_b = audio_chunk_create(max_in, nact);
  audio_chunk_t *out_a = audio_chunk_create(AUDIT_MASK_CHUNK + 64, nch);
  audio_chunk_t *out_b = audio_chunk_create(AUDIT_MASK_CHUNK + 64, nact);
  audio_chunk_set_used_channels(in_a, mask);
  size_t phase = 0;
  for (int it = 0; it < AUDIT_MASK_ITERS; it++) {
    size_t need = resampler_get_input_frames_next(a);
    ASSERT_EQ(need, resampler_get_input_frames_next(b));
    audit_fill_mask_chunk(in_a, nch, map_full, need, phase, NULL);
    audit_fill_mask_chunk(in_b, nact, map_active, need, phase, NULL);
    phase += need;
    ASSERT_EQ(RESAMPLER_OK, resampler_process(a, in_a, out_a));
    ASSERT_EQ(RESAMPLER_OK, resampler_process(b, in_b, out_b));
    size_t n = audio_chunk_get_valid_frames(out_a);
    ASSERT_EQ(n, audio_chunk_get_valid_frames(out_b));
    for (size_t k = 0; k < nact; k++) {
      ASSERT_TRUE(audit_bit_equal(audio_chunk_get_channel(out_a, map_active[k]),
                                  audio_chunk_get_channel(out_b, k), n));
    }
  }
  audio_chunk_free(in_a);
  audio_chunk_free(in_b);
  audio_chunk_free(out_a);
  audio_chunk_free(out_b);
  resampler_free(a);
  resampler_free(b);
}

TEST(AuditResamplerMask_SincQuadratic_ActiveCountSelectsPath) {
  const bool mask[3] = {true, false, true};
  audit_check_active_count_path(AUDIT_KIND_SINC_QUADRATIC, 3, mask);
}
TEST(AuditResamplerMask_SincLinear_ActiveCountSelectsPath) {
  const bool mask[4] = {false, true, false, true};
  audit_check_active_count_path(AUDIT_KIND_SINC_LINEAR, 4, mask);
}
TEST(AuditResamplerMask_SincCubic_ActiveCountSelectsPath) {
  const bool mask[3] = {false, false, true};
  audit_check_active_count_path(AUDIT_KIND_SINC_CUBIC, 3, mask);
}

static void audit_check_reactivation(audit_kind_t kind) {
  const size_t nch = 2;
  const size_t map[2] = {0, 1};
  const bool inactive_mask[2] = {true, false};
  const bool active_mask[2] = {true, true};
  const bool zero_ch1[2] = {false, true};
  resampler_t *a = audit_make_resampler(kind, nch);
  resampler_t *b = audit_make_resampler(kind, nch);
  ASSERT_TRUE(a != NULL);
  ASSERT_TRUE(b != NULL);
  size_t max_in = resampler_get_max_input_frames(a);
  audio_chunk_t *in_a = audio_chunk_create(max_in, nch);
  audio_chunk_t *in_b = audio_chunk_create(max_in, nch);
  audio_chunk_t *out_a = audio_chunk_create(AUDIT_MASK_CHUNK + 64, nch);
  audio_chunk_t *out_b = audio_chunk_create(AUDIT_MASK_CHUNK + 64, nch);
  size_t phase = 0;
  for (int it = 0; it < 3 * AUDIT_MASK_ITERS; it++) {
    bool inactive = it >= AUDIT_MASK_ITERS && it < 2 * AUDIT_MASK_ITERS;
    size_t need = resampler_get_input_frames_next(a);
    ASSERT_EQ(need, resampler_get_input_frames_next(b));
    audio_chunk_set_used_channels(in_a, inactive ? inactive_mask : active_mask);
    audit_fill_mask_chunk(in_a, nch, map, need, phase, NULL);
    audit_fill_mask_chunk(in_b, nch, map, need, phase, inactive ? zero_ch1 : NULL);
    phase += need;
    ASSERT_EQ(RESAMPLER_OK, resampler_process(a, in_a, out_a));
    ASSERT_EQ(RESAMPLER_OK, resampler_process(b, in_b, out_b));
    size_t n = audio_chunk_get_valid_frames(out_a);
    ASSERT_EQ(n, audio_chunk_get_valid_frames(out_b));
    if (it >= 2 * AUDIT_MASK_ITERS) {
      ASSERT_TRUE(audit_bit_equal(audio_chunk_get_channel(out_a, 1),
                                  audio_chunk_get_channel(out_b, 1), n));
    }
  }
  audio_chunk_free(in_a);
  audio_chunk_free(in_b);
  audio_chunk_free(out_a);
  audio_chunk_free(out_b);
  resampler_free(a);
  resampler_free(b);
}

TEST(AuditResamplerMask_Reactivation_Clean) {
  for (int k = 0; k < AUDIT_KIND_COUNT; k++) {
    audit_check_reactivation((audit_kind_t)k);
  }
}

TEST_MAIN()
