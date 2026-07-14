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

  audio_resampler_t* stereo = audio_resampler_create_from_config(
      &cfg_stereo, in_rate, out_rate, 2, chunk_size, NULL);
  audio_resampler_t* mono_l = audio_resampler_create_from_config(
      &cfg_stereo, in_rate, out_rate, 1, chunk_size, NULL);
  audio_resampler_t* mono_r = audio_resampler_create_from_config(
      &cfg_stereo, in_rate, out_rate, 1, chunk_size, NULL);

  ASSERT_TRUE(stereo != NULL);
  ASSERT_TRUE(mono_l != NULL);
  ASSERT_TRUE(mono_r != NULL);

  size_t max_out_st = audio_resampler_get_max_output_frames(stereo);
  size_t max_out_m = audio_resampler_get_max_output_frames(mono_l);

  audio_chunk_t* st_in = audio_chunk_create(65536, 2);
  audio_chunk_t* st_out = audio_chunk_create(max_out_st, 2);
  audio_chunk_t* ml_in = audio_chunk_create(65536, 1);
  audio_chunk_t* ml_out = audio_chunk_create(max_out_m, 1);
  audio_chunk_t* mr_in = audio_chunk_create(65536, 1);
  audio_chunk_t* mr_out = audio_chunk_create(max_out_m, 1);

  size_t idx = 0;
  while (true) {
    size_t needed_in = audio_resampler_get_input_frames_next(stereo);
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

    resampler_error_t err_st = audio_resampler_process(stereo, st_in, st_out);
    resampler_error_t err_ml = audio_resampler_process(mono_l, ml_in, ml_out);
    resampler_error_t err_mr = audio_resampler_process(mono_r, mr_in, mr_out);

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
  audio_resampler_free(stereo);
  audio_resampler_free(mono_l);
  audio_resampler_free(mono_r);
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
  audio_resampler_t* res_a = audio_resampler_create_from_config(
      &cfg, 44100, 48000, 2, chunk_size, NULL);
  audio_resampler_t* res_b = audio_resampler_create_from_config(
      &cfg, 44100, 48000, 2, chunk_size, NULL);
  ASSERT_TRUE(res_a != NULL);
  ASSERT_TRUE(res_b != NULL);

  size_t max_out = audio_resampler_get_max_output_frames(res_a);
  audio_chunk_t* in_chunk = audio_chunk_create(65536, 2);
  audio_chunk_t* out_a = audio_chunk_create(max_out, 2);
  audio_chunk_t* out_b = audio_chunk_create(max_out, 2);

  size_t accum_in = 0;
  for (int c = 0; c < 8; c++) {
    size_t needed_in = audio_resampler_get_input_frames_next(res_a);
    double* ch0 = audio_chunk_get_channel(in_chunk, 0);
    double* ch1 = audio_chunk_get_channel(in_chunk, 1);
    for (size_t i = 0; i < needed_in; i++) {
      ch0[i] = sin(0.1 * (double)(accum_in + i));
      ch1[i] = cos(0.15 * (double)(accum_in + i));
    }
    audio_chunk_set_valid_frames(in_chunk, needed_in);

    ASSERT_EQ(RESAMPLER_OK, audio_resampler_process(res_a, in_chunk, out_a));
    ASSERT_EQ(RESAMPLER_OK, audio_resampler_process(res_b, in_chunk, out_b));
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
  audio_resampler_free(res_a);
  audio_resampler_free(res_b);
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
  audio_resampler_t* res = audio_resampler_create_from_config(
      &cfg, 44100, 48000, 2, chunk_size, NULL);
  ASSERT_TRUE(res != NULL);

  size_t needed_in = audio_resampler_get_input_frames_next(res);
  audio_chunk_t* in_chunk = audio_chunk_create(needed_in, 2);
  audio_chunk_set_valid_frames(in_chunk, needed_in);
  audio_chunk_t* too_small = audio_chunk_create(64, 2);

  resampler_error_t err = audio_resampler_process(res, in_chunk, too_small);
  ASSERT_EQ(RESAMPLER_ERR_OUTPUT_BUFFER_TOO_SMALL, err);

  audio_chunk_free(in_chunk);
  audio_chunk_free(too_small);
  audio_resampler_free(res);
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
  audio_resampler_t* res = audio_resampler_create_from_config(
      &cfg, 44100, 48000, 2, chunk_size, NULL);
  ASSERT_TRUE(res != NULL);

  size_t needed_in = audio_resampler_get_input_frames_next(res);
  size_t max_out = audio_resampler_get_max_output_frames(res);
  audio_chunk_t* in_chunk = audio_chunk_create(65536, 2);
  audio_chunk_t* out_chunk = audio_chunk_create(max_out, 2);

  size_t partial_valid = needed_in / 2;
  audio_chunk_set_valid_frames(in_chunk, partial_valid);

  resampler_error_t err = audio_resampler_process(res, in_chunk, out_chunk);
  ASSERT_EQ(RESAMPLER_OK, err);
  size_t valid_out = audio_chunk_get_valid_frames(out_chunk);
  ASSERT_TRUE(valid_out > 0);
  ASSERT_TRUE(valid_out < max_out);

  audio_chunk_free(in_chunk);
  audio_chunk_free(out_chunk);
  audio_resampler_free(res);
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
  audio_resampler_t* res = audio_resampler_create_from_config(
      &cfg, 44100, 48000, 2, chunk_size, NULL);
  ASSERT_TRUE(res != NULL);

  size_t max_out = audio_resampler_get_max_output_frames(res);
  audio_chunk_t* empty_in = audio_chunk_create(65536, 2);
  audio_chunk_t* valid_in = audio_chunk_create(65536, 2);
  audio_chunk_t* out_chunk = audio_chunk_create(max_out, 2);

  // Set 0 valid frames to simulate multiple underrun chunks
  audio_chunk_set_valid_frames(empty_in, 0);
  for (int i = 0; i < 10; i++) {
    resampler_error_t err = audio_resampler_process(res, empty_in, out_chunk);
    ASSERT_EQ(RESAMPLER_OK, err);
  }

  // Now process valid chunk when last_index is at minimum safe index boundary
  size_t needed_in = audio_resampler_get_input_frames_next(res);
  audio_chunk_set_valid_frames(valid_in, needed_in);
  resampler_error_t err = audio_resampler_process(res, valid_in, out_chunk);
  ASSERT_EQ(RESAMPLER_OK, err);

  audio_chunk_free(empty_in);
  audio_chunk_free(valid_in);
  audio_chunk_free(out_chunk);
  audio_resampler_free(res);
}

TEST_MAIN()
