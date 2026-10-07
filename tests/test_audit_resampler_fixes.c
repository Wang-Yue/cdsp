// Regression tests for audit report 09 (resampler) fixes.
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

static resampler_t *make_sync(size_t in_rate, size_t out_rate,
                              size_t chunk_size) {
  resampler_config_t cfg;
  resampler_config_init_with_type(&cfg, RESAMPLER_TYPE_SYNCHRONOUS);
  return resampler_create_from_config(&cfg, in_rate, out_rate, 1, chunk_size,
                                      NULL);
}

static void fill_sine(audio_chunk_t *chunk, size_t n, size_t *phase) {
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
  resampler_t *a = make_sync(in_rate, out_rate, chunk_size);
  resampler_t *b = make_sync(in_rate, out_rate, chunk_size);
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
    fill_sine(in_a, need_a, &phase);
    phase = p0;
    fill_sine(in_b, need_b, &phase);

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
  resampler_t *r = make_sync(44100, 48000, chunk_size);
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
  resampler_t *r = make_sync(44100, 48000, 1024);
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

TEST_MAIN()
