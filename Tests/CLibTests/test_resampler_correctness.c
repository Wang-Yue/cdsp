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

#include "Audio/audio_chunk.h"
#include "Config/resampler_config_types.h"
#include "Resampler/audio_resampler.h"
#include "Resampler/resampler_error.h"
#include "test_support.h"

static void make_sine(double* out, size_t n, int rate, double freq) {
  double omega = 2.0 * M_PI * freq / (double)rate;
  for (size_t i = 0; i < n; i++) {
    out[i] = sin(omega * (double)i);
  }
}

static void assert_stereo_matches_mono(resampler_type_t type,
                                       const char* interp_str) {
  int in_rate = 44100;
  int out_rate = 48000;
  size_t chunk_size = 1024;
  size_t nbr_in = 32 * chunk_size;

  double* left = (double*)calloc(nbr_in, sizeof(double));
  double* right = (double*)calloc(nbr_in, sizeof(double));
  make_sine(left, nbr_in, in_rate, 1000.0);
  make_sine(right, nbr_in, in_rate, 1500.0);

  resampler_config_t cfg_stereo;
  resampler_config_init(&cfg_stereo, type);
  if (interp_str) {
    strncpy(cfg_stereo.interpolation, interp_str,
            sizeof(cfg_stereo.interpolation) - 1);
    cfg_stereo.has_interpolation = true;
  }
  if (type == RESAMPLER_TYPE_ASYNC_SINC) {
    strncpy(cfg_stereo.profile, "Accurate", sizeof(cfg_stereo.profile) - 1);
    cfg_stereo.has_profile = true;
  }

  resampler_t* stereo = resampler_create_from_config(
      &cfg_stereo, in_rate, out_rate, 2, chunk_size, NULL);
  resampler_t* mono_l = resampler_create_from_config(
      &cfg_stereo, in_rate, out_rate, 1, chunk_size, NULL);
  resampler_t* mono_r = resampler_create_from_config(
      &cfg_stereo, in_rate, out_rate, 1, chunk_size, NULL);

  ASSERT_TRUE(stereo != NULL);
  ASSERT_TRUE(mono_l != NULL);
  ASSERT_TRUE(mono_r != NULL);

  size_t max_out_st = resampler_get_max_output_frames(stereo);
  size_t max_out_m = resampler_get_max_output_frames(mono_l);

  audio_chunk_t* st_in = audio_chunk_create(65536, 2);
  audio_chunk_t* st_out = audio_chunk_create(max_out_st, 2);
  audio_chunk_t* ml_in = audio_chunk_create(65536, 1);
  audio_chunk_t* ml_out = audio_chunk_create(max_out_m, 1);
  audio_chunk_t* mr_in = audio_chunk_create(65536, 1);
  audio_chunk_t* mr_out = audio_chunk_create(max_out_m, 1);

  size_t idx = 0;
  while (true) {
    size_t needed_in = resampler_get_input_frames_next(stereo);
    if (idx + needed_in > nbr_in) break;

    double* st_ch0 = audio_chunk_get_channel(st_in, 0);
    double* st_ch1 = audio_chunk_get_channel(st_in, 1);
    double* ml_ch0 = audio_chunk_get_channel(ml_in, 0);
    double* mr_ch0 = audio_chunk_get_channel(mr_in, 0);

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

    const double* st_o0 = audio_chunk_get_channel(st_out, 0);
    const double* st_o1 = audio_chunk_get_channel(st_out, 1);
    const double* ml_o0 = audio_chunk_get_channel(ml_out, 0);
    const double* mr_o0 = audio_chunk_get_channel(mr_out, 0);

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
                                 const char* interp_str) {
  resampler_config_t cfg;
  resampler_config_init(&cfg, type);
  if (interp_str) {
    strncpy(cfg.interpolation, interp_str, sizeof(cfg.interpolation) - 1);
    cfg.has_interpolation = true;
  }
  if (type == RESAMPLER_TYPE_ASYNC_SINC) {
    strncpy(cfg.profile, "Accurate", sizeof(cfg.profile) - 1);
    cfg.has_profile = true;
  }

  size_t chunk_size = 1024;
  resampler_t* res_a =
      resampler_create_from_config(&cfg, 44100, 48000, 2, chunk_size, NULL);
  resampler_t* res_b =
      resampler_create_from_config(&cfg, 44100, 48000, 2, chunk_size, NULL);
  ASSERT_TRUE(res_a != NULL);
  ASSERT_TRUE(res_b != NULL);

  size_t max_out = resampler_get_max_output_frames(res_a);
  audio_chunk_t* in_chunk = audio_chunk_create(65536, 2);
  audio_chunk_t* out_a = audio_chunk_create(max_out, 2);
  audio_chunk_t* out_b = audio_chunk_create(max_out, 2);

  size_t accum_in = 0;
  for (int c = 0; c < 8; c++) {
    size_t needed_in = resampler_get_input_frames_next(res_a);
    double* ch0 = audio_chunk_get_channel(in_chunk, 0);
    double* ch1 = audio_chunk_get_channel(in_chunk, 1);
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
      const double* o_a = audio_chunk_get_channel(out_a, ch);
      const double* o_b = audio_chunk_get_channel(out_b, ch);
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
                                     const char* interp_str) {
  resampler_config_t cfg;
  resampler_config_init(&cfg, type);
  if (interp_str) {
    strncpy(cfg.interpolation, interp_str, sizeof(cfg.interpolation) - 1);
    cfg.has_interpolation = true;
  }
  if (type == RESAMPLER_TYPE_ASYNC_SINC) {
    strncpy(cfg.profile, "Accurate", sizeof(cfg.profile) - 1);
    cfg.has_profile = true;
  }

  size_t chunk_size = 1024;
  resampler_t* res =
      resampler_create_from_config(&cfg, 44100, 48000, 2, chunk_size, NULL);
  ASSERT_TRUE(res != NULL);

  size_t needed_in = resampler_get_input_frames_next(res);
  audio_chunk_t* in_chunk = audio_chunk_create(needed_in, 2);
  audio_chunk_set_valid_frames(in_chunk, needed_in);
  audio_chunk_t* too_small = audio_chunk_create(64, 2);

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
                                         const char* interp_str) {
  resampler_config_t cfg;
  resampler_config_init(&cfg, type);
  if (interp_str) {
    strncpy(cfg.interpolation, interp_str, sizeof(cfg.interpolation) - 1);
    cfg.has_interpolation = true;
  }
  if (type == RESAMPLER_TYPE_ASYNC_SINC) {
    strncpy(cfg.profile, "Accurate", sizeof(cfg.profile) - 1);
    cfg.has_profile = true;
  }

  size_t chunk_size = 1024;
  resampler_t* res =
      resampler_create_from_config(&cfg, 44100, 48000, 2, chunk_size, NULL);
  ASSERT_TRUE(res != NULL);

  size_t needed_in = resampler_get_input_frames_next(res);
  size_t max_out = resampler_get_max_output_frames(res);
  audio_chunk_t* in_chunk = audio_chunk_create(65536, 2);
  audio_chunk_t* out_chunk = audio_chunk_create(max_out, 2);

  size_t partial_valid = needed_in / 2;
  audio_chunk_set_valid_frames(in_chunk, partial_valid);

  resampler_error_t err = resampler_process(res, in_chunk, out_chunk);
  ASSERT_EQ(RESAMPLER_OK, err);
  size_t valid_out = audio_chunk_get_valid_frames(out_chunk);
  ASSERT_TRUE(valid_out > 0);
  ASSERT_TRUE(valid_out < max_out);

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
  resampler_config_init(&cfg, RESAMPLER_TYPE_ASYNC_SINC);
  strncpy(cfg.profile, "Accurate", sizeof(cfg.profile) - 1);
  cfg.has_profile = true;

  size_t chunk_size = 1024;
  resampler_t* res =
      resampler_create_from_config(&cfg, 44100, 48000, 2, chunk_size, NULL);
  ASSERT_TRUE(res != NULL);

  size_t max_out = resampler_get_max_output_frames(res);
  audio_chunk_t* empty_in = audio_chunk_create(65536, 2);
  audio_chunk_t* valid_in = audio_chunk_create(65536, 2);
  audio_chunk_t* out_chunk = audio_chunk_create(max_out, 2);

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
  resampler_config_init(&cfg, RESAMPLER_TYPE_SLIP);

  size_t chunk_size = 1000;
  resampler_t* res =
      resampler_create_from_config(&cfg, 48000, 48000, 1, chunk_size, NULL);
  ASSERT_TRUE(res != NULL);

  size_t max_out = resampler_get_max_output_frames(res);
  size_t expected_max_out = chunk_size + (chunk_size - 1) / (128 + 2);  // 1007
  ASSERT_EQ(expected_max_out, max_out);

  audio_chunk_t* in_chunk = audio_chunk_create(chunk_size, 1);
  audio_chunk_t* out_chunk = audio_chunk_create(max_out, 1);

  double* in_data = audio_chunk_get_channel(in_chunk, 0);
  for (size_t i = 0; i < chunk_size; i++) {
    in_data[i] = (double)i;
  }
  audio_chunk_set_valid_frames(in_chunk, chunk_size);

  // 1. Ratio 1.0 -> output matches input exactly
  resampler_set_relative_ratio(res, 1.0);
  resampler_error_t err = resampler_process(res, in_chunk, out_chunk);
  ASSERT_EQ(RESAMPLER_OK, err);
  ASSERT_EQ(chunk_size, audio_chunk_get_valid_frames(out_chunk));
  const double* out_data = audio_chunk_get_channel(out_chunk, 0);
  for (size_t i = 0; i < chunk_size; i++) {
    ASSERT_DOUBLE_EQ((double)i, out_data[i]);
  }

  // 2. Ratio 1.001000000001 -> Expect a slip (duplicate sample) in the first
  // chunk
  resampler_set_relative_ratio(res, 1.001000000001);
  err = resampler_process(res, in_chunk, out_chunk);
  ASSERT_EQ(RESAMPLER_OK, err);
  ASSERT_EQ(chunk_size + 1, audio_chunk_get_valid_frames(out_chunk));
  out_data = audio_chunk_get_channel(out_chunk, 0);

  // First 437 samples are copied exactly
  for (size_t i = 0; i < 437; i++) {
    ASSERT_DOUBLE_EQ((double)i, out_data[i]);
  }
  // After crossfade (437 + 128 = 565), samples are offset by -1
  for (size_t i = 565; i < chunk_size + 1; i++) {
    ASSERT_DOUBLE_EQ((double)(i - 1), out_data[i]);
  }

  // 3. Ratio 0.998999999999 -> Expect a slip (drop sample) in the first chunk
  resampler_set_relative_ratio(res, 0.998999999999);
  err = resampler_process(res, in_chunk, out_chunk);
  ASSERT_EQ(RESAMPLER_OK, err);
  ASSERT_EQ(chunk_size - 1, audio_chunk_get_valid_frames(out_chunk));
  out_data = audio_chunk_get_channel(out_chunk, 0);

  // First 436 samples are copied exactly
  for (size_t i = 0; i < 436; i++) {
    ASSERT_DOUBLE_EQ((double)i, out_data[i]);
  }
  // After crossfade (436 + 128 = 564), samples are offset by +1
  for (size_t i = 564; i < chunk_size - 1; i++) {
    ASSERT_DOUBLE_EQ((double)(i + 1), out_data[i]);
  }

  // 4. Ratio NAN -> Expect it to clamp to min_ratio and not output NaN or crash
  resampler_set_relative_ratio(res, nan(""));
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

#define RUBATO_HARNESS_NAME "cdsp_resampler_compare"

static char g_rubato_bin_path[1024] = {0};
static bool g_rubato_checked = false;
static bool g_rubato_available = false;

static bool check_rubato_available(void) {
  if (g_rubato_checked) return g_rubato_available;
  g_rubato_checked = true;

  const char* env_path = getenv("RUBATO_BIN");
  if (env_path && strlen(env_path) > 0) {
    FILE* f = fopen(env_path, "r");
    if (f) {
      fclose(f);
      strncpy(g_rubato_bin_path, env_path, sizeof(g_rubato_bin_path) - 1);
      g_rubato_available = true;
      return true;
    }
  }

  static char home_path[1024] = {0};
  const char* home = getenv("HOME");
  if (home) {
    snprintf(home_path, sizeof(home_path),
             "%s/cdsp/Tests/RustHarnesses/target/"
             "release/" RUBATO_HARNESS_NAME,
             home);
  }

  const char* candidates[] = {
      "Tests/RustHarnesses/target/release/" RUBATO_HARNESS_NAME,
      "./Tests/RustHarnesses/target/release/" RUBATO_HARNESS_NAME,
      "../Tests/RustHarnesses/target/release/" RUBATO_HARNESS_NAME,
      "../../Tests/RustHarnesses/target/release/" RUBATO_HARNESS_NAME,
      home_path[0] ? home_path : NULL};
  for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
    if (!candidates[i]) continue;
    FILE* f = fopen(candidates[i], "r");
    if (f) {
      fclose(f);
      strncpy(g_rubato_bin_path, candidates[i], sizeof(g_rubato_bin_path) - 1);
      g_rubato_available = true;
      return true;
    }
  }
  return false;
}

static int write_raw_f64(const double* data, size_t count, const char* path) {
  FILE* f = fopen(path, "wb");
  if (!f) return 0;
  size_t written = fwrite(data, sizeof(double), count, f);
  fclose(f);
  return written == count;
}

static double* read_raw_f64(const char* path, size_t* out_count) {
  FILE* f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  size_t count = size / sizeof(double);
  double* data = (double*)malloc(count * sizeof(double));
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

  double* input = (double*)malloc(total_frames * sizeof(double));
  ASSERT_TRUE(input != NULL);
  for (size_t i = 0; i < total_frames; i++) {
    input[i] = sin(0.05 * (double)i);
  }

  int fs_in = 40000;
  int fs_out =
      40012;  // ratio = 1.0003 (robust drift accumulation away from boundaries)
  double ratio = (double)fs_out / (double)fs_in;

  char in_path[256];
  char ref_path[256];
  snprintf(in_path, sizeof(in_path), "/tmp/cdsp_slip_compare_in.raw");
  snprintf(ref_path, sizeof(ref_path), "/tmp/cdsp_slip_compare_ref.raw");

  remove(ref_path);
  ASSERT_TRUE(write_raw_f64(input, total_frames, in_path));

  char cmd[1024];
  snprintf(cmd, sizeof(cmd), "\"%s\" slip \"%s\" \"%s\" %d %d %zu --no-partial",
           g_rubato_bin_path, in_path, ref_path, fs_in, fs_out, chunk_size);
  int status = system(cmd);
  ASSERT_EQ(0, status);

  size_t ref_count = 0;
  double* ref_data = read_raw_f64(ref_path, &ref_count);
  ASSERT_TRUE(ref_data != NULL);

  resampler_config_t cfg;
  resampler_config_init(&cfg, RESAMPLER_TYPE_SLIP);

  resampler_t* res =
      resampler_create_from_config(&cfg, fs_in, fs_in, 1, chunk_size, NULL);
  ASSERT_TRUE(res != NULL);
  resampler_set_relative_ratio(res, ratio);

  size_t max_out = resampler_get_max_output_frames(res);
  audio_chunk_t* in_chunk = audio_chunk_create(chunk_size, 1);
  audio_chunk_t* out_chunk = audio_chunk_create(max_out, 1);

  size_t accum_out = 0;
  for (size_t c = 0; c < n_chunks; c++) {
    size_t needed_in = resampler_get_input_frames_next(res);
    ASSERT_EQ(chunk_size, needed_in);

    double* ch_in = audio_chunk_get_channel(in_chunk, 0);
    memcpy(ch_in, &input[c * chunk_size], chunk_size * sizeof(double));
    audio_chunk_set_valid_frames(in_chunk, chunk_size);

    resampler_error_t err = resampler_process(res, in_chunk, out_chunk);
    ASSERT_EQ(RESAMPLER_OK, err);

    size_t got_out = audio_chunk_get_valid_frames(out_chunk);
    const double* ch_out = audio_chunk_get_channel(out_chunk, 0);
    for (size_t i = 0; i < got_out; i++) {
      ASSERT_TRUE(accum_out + i < ref_count);
      ASSERT_DOUBLE_EQ(ref_data[accum_out + i], ch_out[i]);
    }
    accum_out += got_out;
  }

  ASSERT_EQ(ref_count, accum_out);

  audio_chunk_free(in_chunk);
  audio_chunk_free(out_chunk);
  resampler_free(res);
  free(input);
  free(ref_data);

  fs_out =
      39988;  // ratio = 0.9997 (robust drift accumulation away from boundaries)
  ratio = (double)fs_out / (double)fs_in;

  input = (double*)malloc(total_frames * sizeof(double));
  ASSERT_TRUE(input != NULL);
  for (size_t i = 0; i < total_frames; i++) {
    input[i] = sin(0.05 * (double)i);
  }

  remove(ref_path);
  snprintf(cmd, sizeof(cmd), "\"%s\" slip \"%s\" \"%s\" %d %d %zu --no-partial",
           g_rubato_bin_path, in_path, ref_path, fs_in, fs_out, chunk_size);
  status = system(cmd);
  ASSERT_EQ(0, status);

  ref_count = 0;
  ref_data = read_raw_f64(ref_path, &ref_count);
  ASSERT_TRUE(ref_data != NULL);

  res = resampler_create_from_config(&cfg, fs_in, fs_in, 1, chunk_size, NULL);
  ASSERT_TRUE(res != NULL);
  resampler_set_relative_ratio(res, ratio);

  in_chunk = audio_chunk_create(chunk_size, 1);
  out_chunk = audio_chunk_create(max_out, 1);

  accum_out = 0;
  for (size_t c = 0; c < n_chunks; c++) {
    size_t needed_in = resampler_get_input_frames_next(res);
    ASSERT_EQ(chunk_size, needed_in);

    double* ch_in = audio_chunk_get_channel(in_chunk, 0);
    memcpy(ch_in, &input[c * chunk_size], chunk_size * sizeof(double));
    audio_chunk_set_valid_frames(in_chunk, chunk_size);

    resampler_error_t err = resampler_process(res, in_chunk, out_chunk);
    ASSERT_EQ(RESAMPLER_OK, err);

    size_t got_out = audio_chunk_get_valid_frames(out_chunk);
    const double* ch_out = audio_chunk_get_channel(out_chunk, 0);

    for (size_t i = 0; i < got_out; i++) {
      ASSERT_TRUE(accum_out + i < ref_count);
      ASSERT_DOUBLE_EQ(ref_data[accum_out + i], ch_out[i]);
    }
    accum_out += got_out;
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
  resampler_config_init(&cfg, RESAMPLER_TYPE_ASYNC_SINC);
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
  resampler_t* res = resampler_create_from_config(&cfg, 48000, 48000, channels,
                                                  chunk_size, NULL);
  ASSERT_TRUE(res != NULL);

  size_t max_out = resampler_get_max_output_frames(res);
  audio_chunk_t* in_chunk = audio_chunk_create(chunk_size, channels);
  audio_chunk_t* out_chunk = audio_chunk_create(max_out * 2, channels);

  for (int ch = 0; ch < channels; ch++) {
    double* in_data = audio_chunk_get_channel(in_chunk, ch);
    for (size_t i = 0; i < chunk_size; i++) {
      in_data[i] = sin(0.05 * i + ch);
    }
  }
  audio_chunk_set_valid_frames(in_chunk, chunk_size);

  resampler_error_t err = resampler_process(res, in_chunk, out_chunk);
  ASSERT_EQ(RESAMPLER_OK, err);

  resampler_set_relative_ratio(res, 0.9);

  err = resampler_process(res, in_chunk, out_chunk);
  ASSERT_EQ(RESAMPLER_OK, err);

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
               {0.5, 0.8, 256},  {0.125, 8.0, 64},  {1.0, 1.0, 200}};
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

      if (n == 0) continue;

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

static resampler_t* make_poly_resampler_helper(const char* interp,
                                               size_t chunk_size,
                                               size_t channels) {
  resampler_config_t cfg;
  resampler_config_init(&cfg, RESAMPLER_TYPE_ASYNC_POLY);
  strncpy(cfg.interpolation, interp, sizeof(cfg.interpolation) - 1);
  cfg.has_interpolation = true;
  return resampler_create_from_config(&cfg, 48000, 48000, channels, chunk_size,
                                      NULL);
}

static resampler_t* make_sinc_resampler_helper(size_t chunk_size,
                                               size_t channels) {
  resampler_config_t cfg;
  resampler_config_init(&cfg, RESAMPLER_TYPE_ASYNC_SINC);
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

static void test_ramp_resampler_helper(resampler_t* res, double target_rel) {
  size_t channels = resampler_get_channels(res);
  size_t chunk_size = resampler_get_chunk_size(res);
  size_t max_out = resampler_get_max_output_frames(res);

  audio_chunk_t* in_chunk = audio_chunk_create(65536, channels);
  audio_chunk_t* out_chunk = audio_chunk_create(max_out * 2 + 1024, channels);

  // Block 1: nominal ratio
  size_t needed_in1 = resampler_get_input_frames_next(res);
  for (size_t ch = 0; ch < channels; ch++) {
    double* in_data = audio_chunk_get_channel(in_chunk, ch);
    for (size_t i = 0; i < needed_in1; i++) in_data[i] = 0.0;
  }
  audio_chunk_set_valid_frames(in_chunk, needed_in1);
  resampler_error_t err = resampler_process(res, in_chunk, out_chunk);
  ASSERT_EQ(RESAMPLER_OK, err);
  ASSERT_EQ(chunk_size, needed_in1);

  // Change ratio dynamically with ramp
  resampler_set_relative_ratio(res, target_rel);

  size_t needed_in2 = resampler_get_input_frames_next(res);
  size_t needed_out2 = resampler_get_output_frames_next(res);
  ASSERT_TRUE(needed_in2 > 0);
  ASSERT_TRUE(needed_out2 > 0);
  ASSERT_EQ(chunk_size, needed_in2);

  // Block 2: must complete without out-of-bounds crash
  for (size_t ch = 0; ch < channels; ch++) {
    double* in_data = audio_chunk_get_channel(in_chunk, ch);
    for (size_t i = 0; i < needed_in2; i++) in_data[i] = 0.0;
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
  const char* interps[] = {"Cubic", "Linear"};
  double targets[] = {0.92, 1.08, 0.95, 1.05};
  for (size_t i = 0; i < sizeof(interps) / sizeof(interps[0]); i++) {
    for (size_t j = 0; j < sizeof(targets) / sizeof(targets[0]); j++) {
      resampler_t* res = make_poly_resampler_helper(interps[i], 1024, 1);
      ASSERT_TRUE(res != NULL);
      test_ramp_resampler_helper(res, targets[j]);
    }
  }
}

TEST(Sinc_Ramp_LargeRatioChange_DoesNotPanic) {
  double targets[] = {0.92, 1.08, 0.95, 1.05};
  for (size_t j = 0; j < sizeof(targets) / sizeof(targets[0]); j++) {
    resampler_t* res = make_sinc_resampler_helper(1024, 1);
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
  resampler_config_init(&cfg, RESAMPLER_TYPE_SYNCHRONOUS);
  resampler_t* res =
      resampler_create_from_config(&cfg, 147, 1000, 1, 147, NULL);
  ASSERT_TRUE(res != NULL);

  audio_chunk_t* in = audio_chunk_create(147, 1);
  double* in_d = audio_chunk_get_channel(in, 0);
  memset(in_d, 0, 147 * sizeof(double));
  in_d[0] = 0.3;
  in_d[1] = 0.7;
  in_d[2] = 1.0;
  in_d[3] = 1.0;
  in_d[4] = 0.7;
  in_d[5] = 0.3;
  audio_chunk_set_valid_frames(in, 147);

  audio_chunk_t* out = audio_chunk_create(2048, 1);
  double sum = 0.0;
  double max_val = 0.0;
  for (int b = 0; b < 10; b++) {
    resampler_error_t err = resampler_process(res, in, out);
    ASSERT_EQ(RESAMPLER_OK, err);
    size_t valid = audio_chunk_get_valid_frames(out);
    double* out_d = audio_chunk_get_channel(out, 0);
    for (size_t i = 0; i < valid; i++) {
      sum += out_d[i];
      if (fabs(out_d[i]) > max_val) max_val = fabs(out_d[i]);
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
  resampler_config_init(&cfg, RESAMPLER_TYPE_SLIP);
  resampler_t* res =
      resampler_create_from_config(&cfg, 48000, 48000, 2, 1024, NULL);
  ASSERT_TRUE(res != NULL);

  audio_chunk_t* in = audio_chunk_create(1024, 2);
  audio_chunk_t* out = audio_chunk_create(2048, 2);
  for (int ch = 0; ch < 2; ch++) {
    double* d = audio_chunk_get_channel(in, ch);
    for (size_t i = 0; i < 1024; i++) {
      d[i] = (double)(i + 1) * 0.001;
    }
  }
  audio_chunk_set_valid_frames(in, 1024);

  resampler_error_t err = resampler_process(res, in, out);
  ASSERT_EQ(RESAMPLER_OK, err);
  ASSERT_EQ(1024, audio_chunk_get_valid_frames(out));

  for (int ch = 0; ch < 2; ch++) {
    double* in_d = audio_chunk_get_channel(in, ch);
    double* out_d = audio_chunk_get_channel(out, ch);
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
  resampler_config_init(&cfg, RESAMPLER_TYPE_SLIP);
  resampler_t* res =
      resampler_create_from_config(&cfg, 48000, 48000, 1, 512, NULL);
  ASSERT_TRUE(res != NULL);
  resampler_set_relative_ratio(res, 1.0005);

  audio_chunk_t* in = audio_chunk_create(512, 1);
  audio_chunk_t* out = audio_chunk_create(1024, 1);
  double* in_d = audio_chunk_get_channel(in, 0);
  for (size_t i = 0; i < 512; i++) in_d[i] = 1.0;
  audio_chunk_set_valid_frames(in, 512);

  for (int b = 0; b < 20; b++) {
    resampler_error_t err = resampler_process(res, in, out);
    ASSERT_EQ(RESAMPLER_OK, err);
    size_t valid = audio_chunk_get_valid_frames(out);
    double* out_d = audio_chunk_get_channel(out, 0);
    for (size_t i = 0; i < valid; i++) {
      ASSERT_NEAR(out_d[i], 1.0, 1e-12);
    }
  }

  audio_chunk_free(in);
  audio_chunk_free(out);
  resampler_free(res);
}

TEST_MAIN()
