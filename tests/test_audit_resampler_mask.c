// Regression tests for audit report 09 Finding 4: resamplers honour the input
// chunk's used-channel mask (upstream rubato active_channels_mask).
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
#include "resampler/audio_resampler.h"
#include "resampler/resampler_error.h"
#include "test_support.h"

#define CHUNK 256
#define ITERS 24

typedef enum {
  KIND_SYNC,
  KIND_SINC_LINEAR,
  KIND_SINC_QUADRATIC,
  KIND_SINC_CUBIC,
  KIND_POLY_CUBIC,
  KIND_SLIP,
  KIND_COUNT
} kind_t;

static resampler_t *make_resampler(kind_t kind, size_t channels) {
  resampler_config_t cfg;
  size_t in_rate = 44100, out_rate = 48000;
  switch (kind) {
  case KIND_SYNC:
    resampler_config_init_with_type(&cfg, RESAMPLER_TYPE_SYNCHRONOUS);
    break;
  case KIND_SINC_LINEAR:
  case KIND_SINC_QUADRATIC:
  case KIND_SINC_CUBIC:
    resampler_config_init_with_type(&cfg, RESAMPLER_TYPE_ASYNC_SINC);
    cfg.has_profile = true;
    strncpy(cfg.profile,
            kind == KIND_SINC_LINEAR      ? "Fast"
            : kind == KIND_SINC_QUADRATIC ? "Balanced"
                                          : "Accurate",
            sizeof(cfg.profile) - 1);
    break;
  case KIND_POLY_CUBIC:
    resampler_config_init_with_type(&cfg, RESAMPLER_TYPE_ASYNC_POLY);
    cfg.has_interpolation = true;
    strncpy(cfg.interpolation, "Cubic", sizeof(cfg.interpolation) - 1);
    break;
  case KIND_SLIP:
  default:
    resampler_config_init_with_type(&cfg, RESAMPLER_TYPE_SLIP);
    in_rate = out_rate = 48000;
    break;
  }
  config_error_t err;
  config_error_init(&err);
  resampler_t *r = resampler_create_from_config(&cfg, in_rate, out_rate,
                                                channels, CHUNK, &err);
  if (r && kind != KIND_SYNC) {
    resampler_set_relative_ratio(r, 1.0007);
  }
  return r;
}

static double test_signal(size_t ch, size_t n) {
  return 0.5 * sin(0.013 * (double)(ch + 1) * (double)n + 0.3 * (double)ch);
}

// Fill `chunk` (channels `nch`) with signal for source channels `src_map`.
static void fill(audio_chunk_t *chunk, size_t nch, const size_t *src_map,
                 size_t frames, size_t phase, const bool *zero_ch) {
  for (size_t c = 0; c < nch; c++) {
    double *d = audio_chunk_get_channel(chunk, c);
    for (size_t i = 0; i < frames; i++) {
      d[i] = (zero_ch && zero_ch[c]) ? 0.0 : test_signal(src_map[c], phase + i);
    }
  }
  audio_chunk_set_valid_frames(chunk, frames);
}

static bool bit_equal(const double *a, const double *b, size_t n) {
  return memcmp(a, b, n * sizeof(double)) == 0;
}

// Inactive channels are skipped and zero-filled, the mask is propagated, and
// the active channels stay bit-identical to an unmasked run (4 channels with
// one inactive keeps the async-sinc combined path either way: 3 > 2).
static void check_skip_bit_identical(kind_t kind) {
  const size_t nch = 4;
  const size_t map[4] = {0, 1, 2, 3};
  const bool mask[4] = {true, false, true, true};
  resampler_t *a = make_resampler(kind, nch);
  resampler_t *b = make_resampler(kind, nch);
  ASSERT_TRUE(a != NULL);
  ASSERT_TRUE(b != NULL);
  size_t max_in = resampler_get_max_input_frames(a);
  audio_chunk_t *in_a = audio_chunk_create(max_in, nch);
  audio_chunk_t *in_b = audio_chunk_create(max_in, nch);
  audio_chunk_t *out_a = audio_chunk_create(CHUNK + 64, nch);
  audio_chunk_t *out_b = audio_chunk_create(CHUNK + 64, nch);
  audio_chunk_set_used_channels(in_b, mask);
  const bool all_true[4] = {true, true, true, true};
  audio_chunk_set_used_channels(out_b, all_true); // pre-allocated mask

  size_t phase = 0;
  for (int it = 0; it < ITERS; it++) {
    size_t need = resampler_get_input_frames_next(a);
    ASSERT_EQ(need, resampler_get_input_frames_next(b));
    fill(in_a, nch, map, need, phase, NULL);
    fill(in_b, nch, map, need, phase, NULL);
    // Poison the inactive input/output channel: must not leak anywhere.
    double *poison = audio_chunk_get_channel(in_b, 1);
    for (size_t i = 0; i < need; i++)
      poison[i] = NAN;
    double *pout = audio_chunk_get_channel(out_b, 1);
    for (size_t i = 0; i < CHUNK + 64; i++)
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
        ASSERT_TRUE(bit_equal(oa, ob, n));
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
  check_skip_bit_identical(KIND_SYNC);
}
TEST(AuditResamplerMask_SincLinear_SkipsInactive) {
  check_skip_bit_identical(KIND_SINC_LINEAR);
}
TEST(AuditResamplerMask_SincQuadratic_SkipsInactive) {
  check_skip_bit_identical(KIND_SINC_QUADRATIC);
}
TEST(AuditResamplerMask_SincCubic_SkipsInactive) {
  check_skip_bit_identical(KIND_SINC_CUBIC);
}
TEST(AuditResamplerMask_Poly_SkipsInactive) {
  check_skip_bit_identical(KIND_POLY_CUBIC);
}
TEST(AuditResamplerMask_Slip_SkipsInactive) {
  check_skip_bit_identical(KIND_SLIP);
}

// The async-sinc combined/per-channel path choice uses the ACTIVE channel
// count (rubato asynchro_sinc.rs). A masked N-channel resampler must be
// bit-identical to an unmasked resampler built with only the active channels.
static void check_active_count_path(kind_t kind, size_t nch, const bool *mask) {
  size_t map_full[8];
  size_t map_active[8];
  size_t nact = 0;
  for (size_t c = 0; c < nch; c++) {
    map_full[c] = c;
    if (mask[c])
      map_active[nact++] = c;
  }
  resampler_t *a = make_resampler(kind, nch);
  resampler_t *b = make_resampler(kind, nact);
  ASSERT_TRUE(a != NULL);
  ASSERT_TRUE(b != NULL);
  size_t max_in = resampler_get_max_input_frames(a);
  audio_chunk_t *in_a = audio_chunk_create(max_in, nch);
  audio_chunk_t *in_b = audio_chunk_create(max_in, nact);
  audio_chunk_t *out_a = audio_chunk_create(CHUNK + 64, nch);
  audio_chunk_t *out_b = audio_chunk_create(CHUNK + 64, nact);
  audio_chunk_set_used_channels(in_a, mask);
  size_t phase = 0;
  for (int it = 0; it < ITERS; it++) {
    size_t need = resampler_get_input_frames_next(a);
    ASSERT_EQ(need, resampler_get_input_frames_next(b));
    fill(in_a, nch, map_full, need, phase, NULL);
    fill(in_b, nact, map_active, need, phase, NULL);
    phase += need;
    ASSERT_EQ(RESAMPLER_OK, resampler_process(a, in_a, out_a));
    ASSERT_EQ(RESAMPLER_OK, resampler_process(b, in_b, out_b));
    size_t n = audio_chunk_get_valid_frames(out_a);
    ASSERT_EQ(n, audio_chunk_get_valid_frames(out_b));
    for (size_t k = 0; k < nact; k++) {
      ASSERT_TRUE(bit_equal(audio_chunk_get_channel(out_a, map_active[k]),
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
  const bool mask[3] = {true, false, true}; // 2 active: per-channel path
  check_active_count_path(KIND_SINC_QUADRATIC, 3, mask);
}
TEST(AuditResamplerMask_SincLinear_ActiveCountSelectsPath) {
  const bool mask[4] = {false, true, false, true};
  check_active_count_path(KIND_SINC_LINEAR, 4, mask);
}
TEST(AuditResamplerMask_SincCubic_ActiveCountSelectsPath) {
  const bool mask[3] = {false, false, true}; // 1 active: per-channel path
  check_active_count_path(KIND_SINC_CUBIC, 3, mask);
}

// A channel re-activated after being inactive must start from clean (zero)
// state: its output must match a reference that was fed silence on that
// channel while it was inactive.
static void check_reactivation(kind_t kind) {
  const size_t nch = 2;
  const size_t map[2] = {0, 1};
  const bool inactive_mask[2] = {true, false};
  const bool active_mask[2] = {true, true};
  const bool zero_ch1[2] = {false, true};
  resampler_t *a = make_resampler(kind, nch);
  resampler_t *b = make_resampler(kind, nch);
  ASSERT_TRUE(a != NULL);
  ASSERT_TRUE(b != NULL);
  size_t max_in = resampler_get_max_input_frames(a);
  audio_chunk_t *in_a = audio_chunk_create(max_in, nch);
  audio_chunk_t *in_b = audio_chunk_create(max_in, nch);
  audio_chunk_t *out_a = audio_chunk_create(CHUNK + 64, nch);
  audio_chunk_t *out_b = audio_chunk_create(CHUNK + 64, nch);
  size_t phase = 0;
  for (int it = 0; it < 3 * ITERS; it++) {
    // Active for the first third (so history is non-zero), inactive for the
    // second third, active again for the last third.
    bool inactive = it >= ITERS && it < 2 * ITERS;
    size_t need = resampler_get_input_frames_next(a);
    ASSERT_EQ(need, resampler_get_input_frames_next(b));
    audio_chunk_set_used_channels(in_a, inactive ? inactive_mask : active_mask);
    fill(in_a, nch, map, need, phase, NULL);
    fill(in_b, nch, map, need, phase, inactive ? zero_ch1 : NULL);
    phase += need;
    ASSERT_EQ(RESAMPLER_OK, resampler_process(a, in_a, out_a));
    ASSERT_EQ(RESAMPLER_OK, resampler_process(b, in_b, out_b));
    size_t n = audio_chunk_get_valid_frames(out_a);
    ASSERT_EQ(n, audio_chunk_get_valid_frames(out_b));
    if (it >= 2 * ITERS) {
      ASSERT_TRUE(bit_equal(audio_chunk_get_channel(out_a, 1),
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
  for (int k = 0; k < KIND_COUNT; k++) {
    check_reactivation((kind_t)k);
  }
}

TEST_MAIN()
