// Regression tests for audit report 01 (biquad / biquad_combo / diffeq).
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "config/config_gen.h"
#include "filters/biquad.h"
#include "filters/biquad_internal.h"
#include "filters/filter.h"
#include "test_support.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static biquad_filter_t *audit_make_biquad(biquad_config_t p, int fs) {
  filter_config_t cfg = {.type = FILTER_TYPE_BIQUAD, .parameters.biquad = p};
  return (biquad_filter_t *)g_biquad_vtable.create("audit", &cfg, fs, 0, NULL,
                                                   NULL);
}

// 1.1: first-order sections must not lose precision near Nyquist. With the old
// sin/(1+cos) form, 1+cos(w0) rounded to exactly 0 and HighpassFO/LowpassFO
// were rejected as unstable.
TEST(AuditBiquad_FirstOrderNearNyquist) {
  const int fs = 48000;
  const double freq = 24000.0 * (1.0 - 1e-10);
  biquad_type_t types[] = {BIQUAD_TYPE_LOWPASS_FO, BIQUAD_TYPE_HIGHPASS_FO,
                           BIQUAD_TYPE_ALLPASS_FO, BIQUAD_TYPE_LOWSHELF_FO,
                           BIQUAD_TYPE_HIGHSHELF_FO};
  for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
    biquad_config_t p = {.type = types[i], .freq = freq, .gain = 6.0};
    biquad_filter_t *f = audit_make_biquad(p, fs);
    ASSERT_TRUE(f != NULL);
    if (types[i] == BIQUAD_TYPE_HIGHPASS_FO) {
      // b0 = 1 / (1 + tn) must remain a non-zero number.
      ASSERT_TRUE(f->coeffs.b0 > 0.0);
    }
    if (types[i] == BIQUAD_TYPE_LOWPASS_FO) {
      // Unity DC gain: (b0 + b1) / (1 + a1).
      double dc = (f->coeffs.b0 + f->coeffs.b1) / (1.0 + f->coeffs.a1);
      ASSERT_NEAR(1.0, dc, 1e-9);
    }
    g_biquad_vtable.free(f);
  }
}

// 1.1: coefficients match upstream's tan(w0/2) formulation.
TEST(AuditBiquad_FirstOrderMatchesUpstreamTanForm) {
  const int fs = 44100;
  const double freq = 1234.5;
  const double tn = tan(M_PI * freq / fs);
  const double ampl = pow(10.0, 4.5 / 40.0);

  biquad_config_t lp = {.type = BIQUAD_TYPE_LOWPASS_FO, .freq = freq};
  biquad_filter_t *f = audit_make_biquad(lp, fs);
  ASSERT_TRUE(f != NULL);
  ASSERT_NEAR(tn / (1.0 + tn), f->coeffs.b0, 1e-15);
  ASSERT_NEAR(-(1.0 - tn) / (1.0 + tn), f->coeffs.a1, 1e-15);
  g_biquad_vtable.free(f);

  biquad_config_t ls = {
      .type = BIQUAD_TYPE_LOWSHELF_FO, .freq = freq, .gain = 4.5};
  f = audit_make_biquad(ls, fs);
  ASSERT_TRUE(f != NULL);
  ASSERT_NEAR((ampl * ampl * tn + ampl) / (tn + ampl), f->coeffs.b0, 1e-14);
  ASSERT_NEAR((ampl * ampl * tn - ampl) / (tn + ampl), f->coeffs.b1, 1e-14);
  ASSERT_NEAR((tn - ampl) / (tn + ampl), f->coeffs.a1, 1e-15);
  g_biquad_vtable.free(f);

  biquad_config_t hs = {
      .type = BIQUAD_TYPE_HIGHSHELF_FO, .freq = freq, .gain = 4.5};
  f = audit_make_biquad(hs, fs);
  ASSERT_TRUE(f != NULL);
  ASSERT_NEAR((ampl * tn + ampl * ampl) / (ampl * tn + 1.0), f->coeffs.b0,
              1e-14);
  ASSERT_NEAR((ampl * tn - 1.0) / (ampl * tn + 1.0), f->coeffs.a1, 1e-15);
  g_biquad_vtable.free(f);
}

// 1.1: AllpassFO at fs/4 (tn == 1) is well defined (upstream's
// (tn+1)/(tn-1) form divides by zero there).
TEST(AuditBiquad_AllpassFOQuarterRate) {
  biquad_config_t p = {.type = BIQUAD_TYPE_ALLPASS_FO, .freq = 12000.0};
  biquad_filter_t *f = audit_make_biquad(p, 48000);
  ASSERT_TRUE(f != NULL);
  ASSERT_TRUE(isfinite(f->coeffs.b0) && isfinite(f->coeffs.b1) &&
              isfinite(f->coeffs.a1));
  ASSERT_NEAR(1.0, f->coeffs.b1, 1e-15);
  ASSERT_NEAR(0.0, f->coeffs.b0, 1e-15);
  g_biquad_vtable.free(f);
}

// 1.3: Free coefficients are checked for stability even without a sample
// rate, and validate/create agree.
TEST(AuditBiquad_FreeStabilityWithoutSampleRate) {
  biquad_config_t unstable = {
      .type = BIQUAD_TYPE_FREE, .b0 = 1.0, .a1 = 0.0, .a2 = 1.5};
  filter_config_t cfg = {.type = FILTER_TYPE_BIQUAD,
                         .parameters.biquad = unstable};
  ASSERT_NE(0, g_biquad_vtable.validate(&cfg, 0, NULL));
  ASSERT_TRUE(g_biquad_vtable.create("x", &cfg, 0, 0, NULL, NULL) == NULL);

  biquad_config_t stable = {
      .type = BIQUAD_TYPE_FREE, .b0 = 1.0, .a1 = -0.5, .a2 = 0.25};
  cfg.parameters.biquad = stable;
  ASSERT_EQ(0, g_biquad_vtable.validate(&cfg, 0, NULL));
  void *f = g_biquad_vtable.create("x", &cfg, 0, 0, NULL, NULL);
  ASSERT_TRUE(f != NULL);
  g_biquad_vtable.free(f);

  // Designed types still need a sample rate.
  biquad_config_t lp = {.type = BIQUAD_TYPE_LOWPASS, .freq = 100.0, .q = 0.7};
  cfg.parameters.biquad = lp;
  ASSERT_NE(0, g_biquad_vtable.validate(&cfg, 0, NULL));
}

// 1.4: non-finite parameters are rejected by both validate and create.
TEST(AuditBiquad_NonFiniteRejected) {
  filter_config_t cfg = {.type = FILTER_TYPE_BIQUAD};
  biquad_config_t free_nan = {
      .type = BIQUAD_TYPE_FREE, .b0 = NAN, .a1 = -0.5, .a2 = 0.25};
  cfg.parameters.biquad = free_nan;
  ASSERT_NE(0, g_biquad_vtable.validate(&cfg, 48000, NULL));
  ASSERT_TRUE(g_biquad_vtable.create("x", &cfg, 48000, 0, NULL, NULL) == NULL);

  biquad_config_t free_inf = {
      .type = BIQUAD_TYPE_FREE, .b0 = 1.0, .b2 = INFINITY, .a1 = -0.5};
  cfg.parameters.biquad = free_inf;
  ASSERT_NE(0, g_biquad_vtable.validate(&cfg, 48000, NULL));

  biquad_config_t peq_nan_gain = {.type = BIQUAD_TYPE_PEAKING,
                                  .freq = 1000.0,
                                  .q = 1.0,
                                  .gain = NAN,
                                  .steepness_type = STEEPNESS_TYPE_Q};
  cfg.parameters.biquad = peq_nan_gain;
  ASSERT_NE(0, g_biquad_vtable.validate(&cfg, 48000, NULL));

  biquad_config_t lp_nan_freq = {
      .type = BIQUAD_TYPE_LOWPASS, .freq = NAN, .q = 0.7};
  cfg.parameters.biquad = lp_nan_freq;
  ASSERT_NE(0, g_biquad_vtable.validate(&cfg, 48000, NULL));
}

// 1.2: biquad_filter_update_parameters applies the validation rules and keeps
// the previous coefficients when they are violated.
TEST(AuditBiquad_UpdateParametersValidates) {
  biquad_config_t lp = {.type = BIQUAD_TYPE_LOWPASS, .freq = 1000.0, .q = 0.7};
  biquad_filter_t *f = audit_make_biquad(lp, 48000);
  ASSERT_TRUE(f != NULL);
  biquad_coefficients_t before = f->coeffs;
  filter_config_t cfg = {.type = FILTER_TYPE_BIQUAD};

  // freq >= nyquist
  biquad_config_t above = {
      .type = BIQUAD_TYPE_LOWPASS, .freq = 30000.0, .q = 0.7};
  cfg.parameters.biquad = above;
  ASSERT_FALSE(biquad_filter_update_parameters(f, &cfg, 48000));

  // slope > 12
  biquad_config_t steep = {.type = BIQUAD_TYPE_LOWSHELF,
                           .freq = 100.0,
                           .gain = 3.0,
                           .slope = 20.0,
                           .steepness_type = STEEPNESS_TYPE_SLOPE};
  cfg.parameters.biquad = steep;
  ASSERT_FALSE(biquad_filter_update_parameters(f, &cfg, 48000));

  // unsupported steepness type
  biquad_config_t bw_lp = {.type = BIQUAD_TYPE_LOWPASS,
                           .freq = 1000.0,
                           .bandwidth = 1.0,
                           .steepness_type = STEEPNESS_TYPE_BANDWIDTH};
  cfg.parameters.biquad = bw_lp;
  ASSERT_FALSE(biquad_filter_update_parameters(f, &cfg, 48000));

  ASSERT_TRUE(memcmp(&before, &f->coeffs, sizeof(before)) == 0);

  // A valid update still goes through.
  biquad_config_t ok = {.type = BIQUAD_TYPE_LOWSHELF,
                        .freq = 100.0,
                        .gain = 3.0,
                        .q = 0.7,
                        .steepness_type = STEEPNESS_TYPE_Q};
  cfg.parameters.biquad = ok;
  ASSERT_TRUE(biquad_filter_update_parameters(f, &cfg, 48000));
  g_biquad_vtable.free(f);
}

// 1.6: Q-steepness shelves follow upstream's beta = sn * sqrt(A) / q.
TEST(AuditBiquad_ShelfQBetaMatchesUpstream) {
  const int fs = 48000;
  const double freq = 250.0, q = 0.9, gain = -7.5;
  biquad_config_t p = {.type = BIQUAD_TYPE_LOWSHELF,
                       .freq = freq,
                       .q = q,
                       .gain = gain,
                       .steepness_type = STEEPNESS_TYPE_Q};
  biquad_filter_t *f = audit_make_biquad(p, fs);
  ASSERT_TRUE(f != NULL);
  double omega = 2.0 * M_PI * freq / fs;
  double sn = sin(omega), cs = cos(omega);
  double ampl = pow(10.0, gain / 40.0);
  double beta = sn * sqrt(ampl) / q;
  double a0 = (ampl + 1.0) + (ampl - 1.0) * cs + beta;
  double b0 = ampl * ((ampl + 1.0) - (ampl - 1.0) * cs + beta);
  double a2 = (ampl + 1.0) + (ampl - 1.0) * cs - beta;
  ASSERT_NEAR(b0 / a0, f->coeffs.b0, 1e-15);
  ASSERT_NEAR(a2 / a0, f->coeffs.a2, 1e-15);
  g_biquad_vtable.free(f);
}

TEST_MAIN()
