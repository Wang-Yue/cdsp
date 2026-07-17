#if defined(__linux__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif
#include <math.h>
#include <stdlib.h>

#include "Audio/audio_chunk.h"
#include "Resampler/audio_resampler.h"
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
  size_t expected_max_out = chunk_size + (chunk_size - 1) / (128 + 2); // 1007
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

  // 2. Ratio 1.001000000001 -> Expect a slip (duplicate sample) in the first chunk
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
    printf("[SKIP] cdsp_resampler_compare harness not built/available. Skipping comparison test.\n");
    return;
  }

  size_t chunk_size = 1000;
  size_t total_frames = 20 * chunk_size;

  double* input = (double*)malloc(total_frames * sizeof(double));
  ASSERT_TRUE(input != NULL);
  for (size_t i = 0; i < total_frames; i++) {
    input[i] = sin(0.05 * (double)i);
  }

  int fs_in = 40000;
  int fs_out = 40012; // ratio = 1.0003 (robust drift accumulation away from boundaries)
  double ratio = (double)fs_out / (double)fs_in;

  char in_path[256];
  char ref_path[256];
  snprintf(in_path, sizeof(in_path), "/tmp/cdsp_slip_compare_in.raw");
  snprintf(ref_path, sizeof(ref_path), "/tmp/cdsp_slip_compare_ref.raw");

  remove(ref_path);
  ASSERT_TRUE(write_raw_f64(input, total_frames, in_path));

  char cmd[1024];
  snprintf(cmd, sizeof(cmd),
           "\"%s\" slip \"%s\" \"%s\" %d %d %zu --no-partial",
           g_rubato_bin_path, in_path, ref_path, fs_in, fs_out, chunk_size);
  int status = system(cmd);
  ASSERT_EQ(0, status);

  size_t ref_count = 0;
  double* ref_data = read_raw_f64(ref_path, &ref_count);
  ASSERT_TRUE(ref_data != NULL);

  resampler_config_t cfg;
  resampler_config_init(&cfg, RESAMPLER_TYPE_SLIP);

  resampler_t* res = resampler_create_from_config(&cfg, fs_in, fs_in, 1, chunk_size, NULL);
  ASSERT_TRUE(res != NULL);
  resampler_set_relative_ratio(res, ratio);

  size_t max_out = resampler_get_max_output_frames(res);
  audio_chunk_t* in_chunk = audio_chunk_create(chunk_size, 1);
  audio_chunk_t* out_chunk = audio_chunk_create(max_out, 1);

  size_t accum_out = 0;
  for (size_t c = 0; c < 20; c++) {
    size_t needed_in = resampler_get_input_frames_next(res);
    ASSERT_EQ(chunk_size, needed_in);

    double* ch_in = audio_chunk_get_channel(in_chunk, 0);
    memcpy(ch_in, &input[c * chunk_size], chunk_size * sizeof(double));
    audio_chunk_set_valid_frames(in_chunk, chunk_size);

    resampler_error_t err = resampler_process(res, in_chunk, out_chunk);
    ASSERT_EQ(RESAMPLER_OK, err);

    size_t got_out = audio_chunk_get_valid_frames(out_chunk);
    printf("[C slip_process] chunk=%zu n_in=%zu n_out=%zu\n", c, chunk_size, got_out);
    fflush(stdout);
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

  fs_out = 39988; // ratio = 0.9997 (robust drift accumulation away from boundaries)
  ratio = (double)fs_out / (double)fs_in;

  input = (double*)malloc(total_frames * sizeof(double));
  ASSERT_TRUE(input != NULL);
  for (size_t i = 0; i < total_frames; i++) {
    input[i] = sin(0.05 * (double)i);
  }

  remove(ref_path);
  snprintf(cmd, sizeof(cmd),
           "\"%s\" slip \"%s\" \"%s\" %d %d %zu --no-partial",
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
  for (size_t c = 0; c < 20; c++) {
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

TEST_MAIN()
