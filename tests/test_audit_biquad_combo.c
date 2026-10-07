// Regression tests for audit report 01 section 2 (biquad_combo.c) and the
// cross-report items 05 §3.3, 06 §5.4-5.6.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "config/config_gen.h"
#include "filters/biquad.h"
#include "filters/biquad_combo.h"
#include "filters/biquad_internal.h"
#include "filters/filter.h"
#include "test_support.h"

static filter_config_t combo_cfg(biquad_combo_config_t p) {
  filter_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.type = FILTER_TYPE_BIQUAD_COMBO;
  cfg.parameters.biquad_combo = p;
  return cfg;
}

static void impulse_response(void *combo, double *out, size_t n) {
  memset(out, 0, n * sizeof(double));
  out[0] = 1.0;
  g_biquad_combo_vtable.process(combo, out, n);
}

// 2.1: validate and create resolve GraphicEqualizer band edges the same way
// (from the has_* flags, as upstream's map_or defaults).
TEST(AuditCombo_GraphicEqFreqEdgesFromFlags) {
  double gains[4] = {3.0, -2.0, 1.5, 4.0};
  biquad_combo_config_t unset = {.type = BIQUAD_COMBO_TYPE_GRAPHIC_EQUALIZER,
                                 .gains = gains,
                                 .gains_count = 4,
                                 // Values present without their flags are
                                 // ignored by validate, so create must too.
                                 .freq_min = 100.0,
                                 .freq_max = 10000.0};
  biquad_combo_config_t defaults = {.type = BIQUAD_COMBO_TYPE_GRAPHIC_EQUALIZER,
                                    .gains = gains,
                                    .gains_count = 4,
                                    .has_freq_min = true,
                                    .freq_min = 20.0,
                                    .has_freq_max = true,
                                    .freq_max = 20000.0};
  filter_config_t c1 = combo_cfg(unset), c2 = combo_cfg(defaults);
  ASSERT_EQ(0, g_biquad_combo_vtable.validate(&c1, 48000, NULL));
  void *a = g_biquad_combo_vtable.create("a", &c1, 48000, 0, NULL, NULL);
  void *b = g_biquad_combo_vtable.create("b", &c2, 48000, 0, NULL, NULL);
  ASSERT_TRUE(a && b);
  double ra[64], rb[64];
  impulse_response(a, ra, 64);
  impulse_response(b, rb, 64);
  for (size_t i = 0; i < 64; i++)
    ASSERT_TRUE(ra[i] == rb[i]);
  g_biquad_combo_vtable.free(a);
  g_biquad_combo_vtable.free(b);
}

// 2.5: zero expanded stages -> 0 and NULL (no leaked allocation; run under
// ASan/LSan to catch a regression).
TEST(AuditCombo_StagesZeroCountReturnsNull) {
  double gains[3] = {0.0, 0.0005, -0.0002};
  biquad_combo_config_t p = {.type = BIQUAD_COMBO_TYPE_GRAPHIC_EQUALIZER,
                             .gains = gains,
                             .gains_count = 3};
  biquad_filter_t **stages = (biquad_filter_t **)(uintptr_t)1;
  size_t n = biquad_combo_stages(&p, 48000, &stages, NULL);
  ASSERT_EQ(0, n);
  ASSERT_TRUE(stages == NULL);

  gains[1] = 3.0;
  n = biquad_combo_stages(&p, 48000, &stages, NULL);
  ASSERT_EQ(1, n);
  ASSERT_TRUE(stages != NULL);
  g_biquad_vtable.free(stages[0]);
  free(stages);
}

// 2.4: validate expands every combo type into its sections and validates them,
// so a config validate accepts is one create can build (and vice versa).
TEST(AuditCombo_ValidateMatchesCreate) {
  // A Butterworth cutoff so low that cos(w0) rounds to 1.0: the sections are
  // numerically unstable. Previously validate accepted it and create failed.
  biquad_combo_config_t tiny = {.type = BIQUAD_COMBO_TYPE_BUTTERWORTH_LOWPASS,
                                .has_freq = true,
                                .freq = 1e-6,
                                .has_order = true,
                                .order = 4};
  filter_config_t c = combo_cfg(tiny);
  ASSERT_NE(0, g_biquad_combo_vtable.validate(&c, 48000, NULL));
  ASSERT_TRUE(g_biquad_combo_vtable.create("x", &c, 48000, 0, NULL, NULL) ==
              NULL);

  // Normal configs of every type still validate and create.
  biquad_combo_config_t bw5 = {.type = BIQUAD_COMBO_TYPE_BUTTERWORTH_HIGHPASS,
                               .has_freq = true,
                               .freq = 80.0,
                               .has_order = true,
                               .order = 5};
  biquad_combo_config_t lr8 = {.type = BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_LOWPASS,
                               .has_freq = true,
                               .freq = 2000.0,
                               .has_order = true,
                               .order = 8};
  biquad_combo_config_t tilt = {
      .type = BIQUAD_COMBO_TYPE_TILT, .has_gain = true, .gain = 6.0};
  double gains[10] = {1, -2, 3, 0, 0, 4, -5, 6, 0.5, -1};
  biquad_combo_config_t geq = {.type = BIQUAD_COMBO_TYPE_GRAPHIC_EQUALIZER,
                               .gains = gains,
                               .gains_count = 10};
  biquad_combo_config_t all[] = {bw5, lr8, tilt, geq};
  size_t expect_sections[] = {3, 4, 2, 8};
  for (size_t i = 0; i < 4; i++) {
    c = combo_cfg(all[i]);
    ASSERT_EQ(0, g_biquad_combo_vtable.validate(&c, 48000, NULL));
    void *f = g_biquad_combo_vtable.create("x", &c, 48000, 0, NULL, NULL);
    ASSERT_TRUE(f != NULL);
    ASSERT_EQ(expect_sections[i], biquad_combo_get_stage_count(f));
    g_biquad_combo_vtable.free(f);
  }
}

// 06 §5.5 (INTENTIONAL): upstream builds Tilt's 3500 Hz shelf even when it is
// at/above nyquist; cdsp leaves that shelf out instead of building an aliased
// section, and validate accepts what create builds.
TEST(AuditCombo_TiltLowSampleRateOmitsHighShelf) {
  biquad_combo_config_t tilt = {
      .type = BIQUAD_COMBO_TYPE_TILT, .has_gain = true, .gain = 6.0};
  filter_config_t c = combo_cfg(tilt);
  ASSERT_EQ(0, g_biquad_combo_vtable.validate(&c, 6000, NULL));
  void *f = g_biquad_combo_vtable.create("x", &c, 6000, 0, NULL, NULL);
  ASSERT_TRUE(f != NULL);
  ASSERT_EQ(1, biquad_combo_get_stage_count(f));
  g_biquad_combo_vtable.free(f);
}

// 06 §5.6 (FALSE POSITIVE): upstream accepts an empty GraphicEqualizer (zero
// stages); so does cdsp, and the combo is a passthrough.
TEST(AuditCombo_GraphicEqEmptyGainsIsPassthrough) {
  biquad_combo_config_t geq = {.type = BIQUAD_COMBO_TYPE_GRAPHIC_EQUALIZER};
  filter_config_t c = combo_cfg(geq);
  ASSERT_EQ(0, g_biquad_combo_vtable.validate(&c, 48000, NULL));
  void *f = g_biquad_combo_vtable.create("x", &c, 48000, 0, NULL, NULL);
  ASSERT_TRUE(f != NULL);
  ASSERT_EQ(0, biquad_combo_get_stage_count(f));
  double w[4] = {0.5, -0.25, 1.0, 0.0};
  g_biquad_combo_vtable.process(f, w, 4);
  ASSERT_TRUE(w[0] == 0.5 && w[1] == -0.25 && w[2] == 1.0);
  g_biquad_combo_vtable.free(f);
}

static void *make_geq(double *gains, size_t n) {
  biquad_combo_config_t p = {.type = BIQUAD_COMBO_TYPE_GRAPHIC_EQUALIZER,
                             .gains = gains,
                             .gains_count = n};
  filter_config_t c = combo_cfg(p);
  return g_biquad_combo_vtable.create("geq", &c, 48000, 0, NULL, NULL);
}

// 2.2 / 05 §3.3: flattening one band while boosting another keeps the section
// count; the old band's history must not be poured into the new band.
TEST(AuditCombo_TransferStateDoesNotCrossBands) {
  double g_src[4] = {3.0, 0.0, 0.0, 0.0};
  double g_dst[4] = {0.0, 0.0, 0.0, 3.0};
  void *src = make_geq(g_src, 4);
  void *dst = make_geq(g_dst, 4);
  ASSERT_TRUE(src && dst);
  ASSERT_EQ(1, biquad_combo_get_stage_count(src));
  ASSERT_EQ(1, biquad_combo_get_stage_count(dst));
  double w[32];
  impulse_response(src, w, 32);
  g_biquad_combo_vtable.transfer_state(dst, src);
  double tail[16] = {0};
  g_biquad_combo_vtable.process(dst, tail, 16);
  for (size_t i = 0; i < 16; i++)
    ASSERT_TRUE(tail[i] == 0.0);
  g_biquad_combo_vtable.free(src);
  g_biquad_combo_vtable.free(dst);
}

// 2.2: a band present in both carries its own state even when its position in
// the cascade moved because another band was flattened.
TEST(AuditCombo_TransferStateMatchesBandByName) {
  double g_src[4] = {3.0, 0.0, 4.0, 0.0};
  double g_dst[4] = {0.0, 0.0, 4.0, 0.0};
  void *src = make_geq(g_src, 4);
  void *dst = make_geq(g_dst, 4);
  ASSERT_TRUE(src && dst);
  double w[32];
  impulse_response(src, w, 32);
  g_biquad_combo_vtable.transfer_state(dst, src);
  biquad_filter_t *ss[2], *ds[1];
  ASSERT_EQ(2, biquad_combo_get_stages(src, ss, 2));
  ASSERT_EQ(1, biquad_combo_get_stages(dst, ds, 1));
  ASSERT_STR_EQ("band_2", biquad_filter_get_name(ss[1]));
  ASSERT_STR_EQ("band_2", biquad_filter_get_name(ds[0]));
  // Same coefficients, so the ring-estimate scale is 1: an exact copy.
  ASSERT_TRUE(ds[0]->z1 == ss[1]->z1 && ds[0]->z2 == ss[1]->z2);
  ASSERT_TRUE(ds[0]->z1 != ss[0]->z1);
  g_biquad_combo_vtable.free(src);
  g_biquad_combo_vtable.free(dst);
}

// 2.2: a different combo subtype with the same section count carries nothing.
TEST(AuditCombo_TransferStateRequiresSameSubtype) {
  biquad_combo_config_t bw4 = {.type = BIQUAD_COMBO_TYPE_BUTTERWORTH_LOWPASS,
                               .has_freq = true,
                               .freq = 1000.0,
                               .has_order = true,
                               .order = 4};
  biquad_combo_config_t lr4 = bw4;
  lr4.type = BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_LOWPASS;
  filter_config_t c1 = combo_cfg(bw4), c2 = combo_cfg(lr4);
  void *src = g_biquad_combo_vtable.create("a", &c1, 48000, 0, NULL, NULL);
  void *dst = g_biquad_combo_vtable.create("b", &c2, 48000, 0, NULL, NULL);
  ASSERT_TRUE(src && dst);
  ASSERT_EQ(biquad_combo_get_stage_count(src),
            biquad_combo_get_stage_count(dst));
  double w[32];
  impulse_response(src, w, 32);
  g_biquad_combo_vtable.transfer_state(dst, src);
  double tail[16] = {0};
  g_biquad_combo_vtable.process(dst, tail, 16);
  for (size_t i = 0; i < 16; i++)
    ASSERT_TRUE(tail[i] == 0.0);
  g_biquad_combo_vtable.free(src);
  g_biquad_combo_vtable.free(dst);
}

TEST_MAIN()
