#include <math.h>
#include <stdbool.h>
#include <stdlib.h>

#include "Config/filter_config_types.h"
#include "Filters/biquad.h"
#include "Filters/filter.h"
#include "Utils/double_helpers.h"
#include "test_support.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void gain_and_phase(const biquad_config_t* params, double f, double fs,
                           double* gain_db, double* phase_deg) {
  filter_config_t cfg = {.type = FILTER_TYPE_BIQUAD,
                         .parameters.biquad = *params};
  biquad_filter_t* filter = (biquad_filter_t*)g_biquad_vtable.create(
      "test", &cfg, (int)fs, 0, NULL, NULL);
  if (!filter) {
    *gain_db = 0.0;
    *phase_deg = 0.0;
    return;
  }
  size_t N = 8192;
  double* wave = (double*)calloc(N, sizeof(double));
  wave[0] = 1.0;
  g_biquad_vtable.process(filter, wave, N);
  g_biquad_vtable.free(filter);

  double w = 2.0 * M_PI * f / fs;
  double re = 0.0, im = 0.0;
  for (size_t n = 0; n < N; n++) {
    re += wave[n] * cos(w * (double)n);
    im -= wave[n] * sin(w * (double)n);
  }
  free(wave);
  double mag = sqrt(re * re + im * im);
  *gain_db = double_to_db(mag);
  *phase_deg = atan2(im, re) * 180.0 / M_PI;
}

static bool is_close(double left, double right, double maxdiff) {
  return fabs(left - right) < maxdiff;
}

TEST(ImpulseResponse) {
  biquad_config_t params = {
      .type = BIQUAD_TYPE_LOWPASS, .freq = 10000.0, .q = 0.5};
  filter_config_t cfg = {.type = FILTER_TYPE_BIQUAD,
                         .parameters.biquad = params};
  biquad_filter_t* filter = (biquad_filter_t*)g_biquad_vtable.create(
      "biquad", &cfg, 44100, 0, NULL, NULL);
  ASSERT_TRUE(filter != NULL);

  double wave[] = {1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  double expected[] = {0.215, 0.461, 0.281, 0.039, 0.004, 0.0, 0.0, 0.0};

  g_biquad_vtable.process(filter, wave, 8);

  for (size_t i = 0; i < 8; i++) {
    ASSERT_TRUE(is_close(wave[i], expected[i], 1e-3));
  }
  g_biquad_vtable.free(filter);
}

TEST(Lowpass) {
  biquad_config_t params = {
      .type = BIQUAD_TYPE_LOWPASS, .freq = 100.0, .q = 1.0 / sqrt(2.0)};
  double gf0, pf0, ghf, phf, glf, plf;
  gain_and_phase(&params, 100.0, 44100.0, &gf0, &pf0);
  gain_and_phase(&params, 400.0, 44100.0, &ghf, &phf);
  gain_and_phase(&params, 10.0, 44100.0, &glf, &plf);
  ASSERT_TRUE(is_close(gf0, -3.0, 0.1));
  ASSERT_TRUE(is_close(glf, 0.0, 0.1));
  ASSERT_TRUE(is_close(ghf, -24.0, 0.2));
}

TEST(Highpass) {
  biquad_config_t params = {
      .type = BIQUAD_TYPE_HIGHPASS, .freq = 100.0, .q = 1.0 / sqrt(2.0)};
  double gf0, pf0, ghf, phf, glf, plf;
  gain_and_phase(&params, 100.0, 44100.0, &gf0, &pf0);
  gain_and_phase(&params, 400.0, 44100.0, &ghf, &phf);
  gain_and_phase(&params, 25.0, 44100.0, &glf, &plf);
  ASSERT_TRUE(is_close(gf0, -3.0, 0.1));
  ASSERT_TRUE(is_close(glf, -24.0, 0.2));
  ASSERT_TRUE(is_close(ghf, 0.0, 0.1));
}

TEST(LowpassFO) {
  biquad_config_t params = {.type = BIQUAD_TYPE_LOWPASS_FO, .freq = 100.0};
  double gf0, pf0, ghf, phf, glf, plf;
  gain_and_phase(&params, 100.0, 44100.0, &gf0, &pf0);
  gain_and_phase(&params, 400.0, 44100.0, &ghf, &phf);
  gain_and_phase(&params, 10.0, 44100.0, &glf, &plf);
  ASSERT_TRUE(is_close(gf0, -3.0, 0.1));
  ASSERT_TRUE(is_close(glf, 0.0, 0.1));
  ASSERT_TRUE(is_close(ghf, -12.3, 0.1));
}

TEST(HighpassFO) {
  biquad_config_t params = {.type = BIQUAD_TYPE_HIGHPASS_FO, .freq = 100.0};
  double gf0, pf0, ghf, phf, glf, plf;
  gain_and_phase(&params, 100.0, 44100.0, &gf0, &pf0);
  gain_and_phase(&params, 800.0, 44100.0, &ghf, &phf);
  gain_and_phase(&params, 25.0, 44100.0, &glf, &plf);
  ASSERT_TRUE(is_close(gf0, -3.0, 0.1));
  ASSERT_TRUE(is_close(glf, -12.3, 0.1));
  ASSERT_TRUE(is_close(ghf, 0.0, 0.1));
}

TEST(Peaking) {
  biquad_config_t params = {
      .type = BIQUAD_TYPE_PEAKING, .freq = 100.0, .q = 3.0, .gain = 7.0};
  double gf0, pf0, ghf, phf, glf, plf;
  gain_and_phase(&params, 100.0, 44100.0, &gf0, &pf0);
  gain_and_phase(&params, 400.0, 44100.0, &ghf, &phf);
  gain_and_phase(&params, 25.0, 44100.0, &glf, &plf);
  ASSERT_TRUE(is_close(gf0, 7.0, 0.1));
  ASSERT_TRUE(is_close(glf, 0.0, 0.1));
  ASSERT_TRUE(is_close(ghf, 0.0, 0.1));
}

TEST(Bandpass) {
  biquad_config_t params = {
      .type = BIQUAD_TYPE_BANDPASS, .freq = 100.0, .q = 1.0};
  double gf0, pf0, ghf, phf, glf, plf;
  gain_and_phase(&params, 100.0, 44100.0, &gf0, &pf0);
  gain_and_phase(&params, 400.0, 44100.0, &ghf, &phf);
  gain_and_phase(&params, 25.0, 44100.0, &glf, &plf);
  ASSERT_TRUE(is_close(gf0, 0.0, 0.1));
  ASSERT_TRUE(is_close(glf, -12.0, 0.3));
  ASSERT_TRUE(is_close(ghf, -12.0, 0.3));
}

TEST(Notch) {
  biquad_config_t params = {.type = BIQUAD_TYPE_NOTCH, .freq = 100.0, .q = 3.0};
  double gf0, pf0, ghf, phf, glf, plf;
  gain_and_phase(&params, 100.0, 44100.0, &gf0, &pf0);
  gain_and_phase(&params, 400.0, 44100.0, &ghf, &phf);
  gain_and_phase(&params, 25.0, 44100.0, &glf, &plf);
  ASSERT_TRUE(gf0 < -40.0);
  ASSERT_TRUE(is_close(glf, 0.0, 0.1));
  ASSERT_TRUE(is_close(ghf, 0.0, 0.1));
}

TEST(Allpass) {
  biquad_config_t params = {
      .type = BIQUAD_TYPE_ALLPASS, .freq = 100.0, .q = 3.0};
  double gf0, pf0, ghf, phf, glf, plf;
  gain_and_phase(&params, 100.0, 44100.0, &gf0, &pf0);
  gain_and_phase(&params, 10000.0, 44100.0, &ghf, &phf);
  gain_and_phase(&params, 1.0, 44100.0, &glf, &plf);
  ASSERT_TRUE(is_close(gf0, 0.0, 0.1));
  ASSERT_TRUE(is_close(glf, 0.0, 0.1));
  ASSERT_TRUE(is_close(ghf, 0.0, 0.1));
  ASSERT_TRUE(is_close(fabs(pf0), 180.0, 0.5));
  ASSERT_TRUE(is_close(plf, 0.0, 0.5));
  ASSERT_TRUE(is_close(phf, 0.0, 0.5));
}

TEST(AllpassFO) {
  biquad_config_t params = {.type = BIQUAD_TYPE_ALLPASS_FO, .freq = 100.0};
  double gf0, pf0, ghf, phf, glf, plf;
  gain_and_phase(&params, 100.0, 44100.0, &gf0, &pf0);
  gain_and_phase(&params, 10000.0, 44100.0, &ghf, &phf);
  gain_and_phase(&params, 1.0, 44100.0, &glf, &plf);
  ASSERT_TRUE(is_close(gf0, 0.0, 0.1));
  ASSERT_TRUE(is_close(glf, 0.0, 0.1));
  ASSERT_TRUE(is_close(ghf, 0.0, 0.1));
  ASSERT_TRUE(is_close(fabs(pf0), 90.0, 0.5));
  ASSERT_TRUE(is_close(plf, 0.0, 2.0));
  ASSERT_TRUE(is_close(fabs(phf), 180.0, 2.0));
}

TEST(Highshelf) {
  biquad_config_t params = {.type = BIQUAD_TYPE_HIGHSHELF,
                            .freq = 100.0,
                            .gain = -24.0,
                            .slope = 6.0,
                            .steepness_type = STEEPNESS_TYPE_SLOPE};
  double gf0, pf0, gf0h, pf0h, gf0l, pf0l, ghf, phf, glf, plf;
  gain_and_phase(&params, 100.0, 44100.0, &gf0, &pf0);
  gain_and_phase(&params, 200.0, 44100.0, &gf0h, &pf0h);
  gain_and_phase(&params, 50.0, 44100.0, &gf0l, &pf0l);
  gain_and_phase(&params, 10000.0, 44100.0, &ghf, &phf);
  gain_and_phase(&params, 1.0, 44100.0, &glf, &plf);
  ASSERT_TRUE(is_close(gf0, -12.0, 0.1));
  ASSERT_TRUE(is_close(gf0h, -18.0, 1.0));
  ASSERT_TRUE(is_close(gf0l, -6.0, 1.0));
  ASSERT_TRUE(is_close(glf, 0.0, 0.1));
  ASSERT_TRUE(is_close(ghf, -24.0, 0.1));
}

TEST(Lowshelf) {
  biquad_config_t params = {.type = BIQUAD_TYPE_LOWSHELF,
                            .freq = 100.0,
                            .gain = -24.0,
                            .slope = 6.0,
                            .steepness_type = STEEPNESS_TYPE_SLOPE};
  double gf0, pf0, gf0h, pf0h, gf0l, pf0l, ghf, phf, glf, plf;
  gain_and_phase(&params, 100.0, 44100.0, &gf0, &pf0);
  gain_and_phase(&params, 200.0, 44100.0, &gf0h, &pf0h);
  gain_and_phase(&params, 50.0, 44100.0, &gf0l, &pf0l);
  gain_and_phase(&params, 10000.0, 44100.0, &ghf, &phf);
  gain_and_phase(&params, 1.0, 44100.0, &glf, &plf);
  ASSERT_TRUE(is_close(gf0, -12.0, 0.1));
  ASSERT_TRUE(is_close(gf0h, -6.0, 1.0));
  ASSERT_TRUE(is_close(gf0l, -18.0, 1.0));
  ASSERT_TRUE(is_close(glf, -24.0, 0.1));
  ASSERT_TRUE(is_close(ghf, 0.0, 0.1));
}

TEST(LowshelfSlopeVsQ) {
  biquad_config_t pS = {.type = BIQUAD_TYPE_LOWSHELF,
                        .freq = 100.0,
                        .gain = -24.0,
                        .slope = 12.0,
                        .steepness_type = STEEPNESS_TYPE_SLOPE};
  biquad_config_t pQ = {.type = BIQUAD_TYPE_LOWSHELF,
                        .freq = 100.0,
                        .q = 1.0 / sqrt(2.0),
                        .gain = -24.0};
  double gS, pS_deg, gQ, pQ_deg;
  gain_and_phase(&pS, 100.0, 44100.0, &gS, &pS_deg);
  gain_and_phase(&pQ, 100.0, 44100.0, &gQ, &pQ_deg);
  ASSERT_TRUE(is_close(gS, gQ, 0.001));
  ASSERT_TRUE(is_close(pS_deg, pQ_deg, 0.001));
}

TEST(HighshelfSlopeVsQ) {
  biquad_config_t pS = {.type = BIQUAD_TYPE_HIGHSHELF,
                        .freq = 100.0,
                        .gain = -24.0,
                        .slope = 12.0,
                        .steepness_type = STEEPNESS_TYPE_SLOPE};
  biquad_config_t pQ = {.type = BIQUAD_TYPE_HIGHSHELF,
                        .freq = 100.0,
                        .q = 1.0 / sqrt(2.0),
                        .gain = -24.0};
  double gS, pS_deg, gQ, pQ_deg;
  gain_and_phase(&pS, 100.0, 44100.0, &gS, &pS_deg);
  gain_and_phase(&pQ, 100.0, 44100.0, &gQ, &pQ_deg);
  ASSERT_TRUE(is_close(gS, gQ, 0.001));
  ASSERT_TRUE(is_close(pS_deg, pQ_deg, 0.001));
}

TEST(BandpassBWvsQ) {
  biquad_config_t pBW = {.type = BIQUAD_TYPE_BANDPASS,
                         .freq = 100.0,
                         .bandwidth = 1.0,
                         .steepness_type = STEEPNESS_TYPE_BANDWIDTH};
  biquad_config_t pQ = {
      .type = BIQUAD_TYPE_BANDPASS, .freq = 100.0, .q = sqrt(2.0)};
  double gBW, pBW_deg, gQ, pQ_deg;
  gain_and_phase(&pBW, 100.0, 44100.0, &gBW, &pBW_deg);
  gain_and_phase(&pQ, 100.0, 44100.0, &gQ, &pQ_deg);
  ASSERT_TRUE(is_close(gBW, gQ, 0.001));
  ASSERT_TRUE(is_close(pBW_deg, pQ_deg, 0.001));
}

TEST(NotchBWvsQ) {
  biquad_config_t pBW = {.type = BIQUAD_TYPE_NOTCH,
                         .freq = 100.0,
                         .bandwidth = 1.0,
                         .steepness_type = STEEPNESS_TYPE_BANDWIDTH};
  biquad_config_t pQ = {
      .type = BIQUAD_TYPE_NOTCH, .freq = 100.0, .q = sqrt(2.0)};
  double gBW, pBW_deg, gQ, pQ_deg;
  gain_and_phase(&pBW, 200.0, 44100.0, &gBW, &pBW_deg);
  gain_and_phase(&pQ, 200.0, 44100.0, &gQ, &pQ_deg);
  ASSERT_TRUE(is_close(gBW, gQ, 0.001));
  ASSERT_TRUE(is_close(pBW_deg, pQ_deg, 0.001));
}

TEST(AllpassBWvsQ) {
  biquad_config_t pBW = {.type = BIQUAD_TYPE_ALLPASS,
                         .freq = 100.0,
                         .bandwidth = 1.0,
                         .steepness_type = STEEPNESS_TYPE_BANDWIDTH};
  biquad_config_t pQ = {
      .type = BIQUAD_TYPE_ALLPASS, .freq = 100.0, .q = sqrt(2.0)};
  double gBW, pBW_deg, gQ, pQ_deg;
  gain_and_phase(&pBW, 100.0, 44100.0, &gBW, &pBW_deg);
  gain_and_phase(&pQ, 100.0, 44100.0, &gQ, &pQ_deg);
  ASSERT_TRUE(is_close(gBW, gQ, 0.001));
  ASSERT_TRUE(is_close(pBW_deg, pQ_deg, 0.001));
}

TEST(HighshelfFO) {
  biquad_config_t params = {
      .type = BIQUAD_TYPE_HIGHSHELF_FO, .freq = 100.0, .gain = -12.0};
  double gf0, pf0, ghf, phf, glf, plf;
  gain_and_phase(&params, 100.0, 44100.0, &gf0, &pf0);
  gain_and_phase(&params, 10000.0, 44100.0, &ghf, &phf);
  gain_and_phase(&params, 1.0, 44100.0, &glf, &plf);
  ASSERT_TRUE(is_close(gf0, -6.0, 0.1));
  ASSERT_TRUE(is_close(glf, 0.0, 0.1));
  ASSERT_TRUE(is_close(ghf, -12.0, 0.1));
}

TEST(LowshelfFO) {
  biquad_config_t params = {
      .type = BIQUAD_TYPE_LOWSHELF_FO, .freq = 100.0, .gain = -12.0};
  double gf0, pf0, ghf, phf, glf, plf;
  gain_and_phase(&params, 100.0, 44100.0, &gf0, &pf0);
  gain_and_phase(&params, 10000.0, 44100.0, &ghf, &phf);
  gain_and_phase(&params, 1.0, 44100.0, &glf, &plf);
  ASSERT_TRUE(is_close(gf0, -6.0, 0.1));
  ASSERT_TRUE(is_close(glf, -12.0, 0.1));
  ASSERT_TRUE(is_close(ghf, 0.0, 0.1));
}

TEST(FreeBiquad) {
  biquad_config_t params = {.type = BIQUAD_TYPE_FREE,
                            .a1 = -0.5,
                            .a2 = 0.1,
                            .b0 = 0.25,
                            .b1 = 0.5,
                            .b2 = 0.25};
  ASSERT_DOUBLE_EQ(0.25, params.b0);
  ASSERT_DOUBLE_EQ(0.5, params.b1);
  ASSERT_DOUBLE_EQ(0.25, params.b2);
  ASSERT_DOUBLE_EQ(-0.5, params.a1);
  ASSERT_DOUBLE_EQ(0.1, params.a2);
}

TEST(GeneralNotchHP) {
  biquad_config_t params = {.type = BIQUAD_TYPE_GENERAL_NOTCH,
                            .q_p = 1.0,
                            .freq_notch = 1000.0,
                            .freq_pole = 2000.0,
                            .normalize_at_dc = false};
  double gain_fp, p1, gain_hf, p2, gain_lf, p3;
  gain_and_phase(&params, 1000.0, 44100.0, &gain_fp, &p1);
  gain_and_phase(&params, 20000.0, 44100.0, &gain_hf, &p2);
  gain_and_phase(&params, 1.0, 44100.0, &gain_lf, &p3);
  ASSERT_TRUE(gain_fp < -40.0);
  ASSERT_TRUE(is_close(gain_lf, -12.1, 0.1));
  ASSERT_TRUE(is_close(gain_hf, 0.0, 0.1));
}

TEST(GeneralNotchLP) {
  biquad_config_t params = {.type = BIQUAD_TYPE_GENERAL_NOTCH,
                            .q_p = 1.0,
                            .freq_notch = 1000.0,
                            .freq_pole = 500.0,
                            .normalize_at_dc = true};
  double gain_fp, p1, gain_hf, p2, gain_lf, p3;
  gain_and_phase(&params, 1000.0, 44100.0, &gain_fp, &p1);
  gain_and_phase(&params, 20000.0, 44100.0, &gain_hf, &p2);
  gain_and_phase(&params, 1.0, 44100.0, &gain_lf, &p3);
  ASSERT_TRUE(gain_fp < -40.0);
  ASSERT_TRUE(is_close(gain_lf, 0.0, 0.1));
  ASSERT_TRUE(is_close(gain_hf, -12.1, 0.1));
}

TEST(LinkwitzTransform) {
  biquad_config_t params = {.type = BIQUAD_TYPE_LINKWITZ_TRANSFORM,
                            .freq_act = 100.0,
                            .q_act = 1.2,
                            .freq_target = 25.0,
                            .q_target = 0.7};
  double gain10, p1, gain87, p2, gain123, p3, gain_hf, p4;
  gain_and_phase(&params, 10.0, 44100.0, &gain10, &p1);
  gain_and_phase(&params, 87.0, 44100.0, &gain87, &p2);
  gain_and_phase(&params, 123.0, 44100.0, &gain123, &p3);
  gain_and_phase(&params, 10000.0, 44100.0, &gain_hf, &p4);
  ASSERT_TRUE(is_close(gain10, 23.9, 0.1));
  ASSERT_TRUE(is_close(gain87, 0.0, 0.1));
  ASSERT_TRUE(is_close(gain123, -2.4, 0.1));
  ASSERT_TRUE(is_close(gain_hf, 0.0, 0.1));
}

TEST(ValidateFreqQ) {
  int fs48 = 48000;
  biquad_config_t p1 = {
      .type = BIQUAD_TYPE_PEAKING, .freq = 1000.0, .q = 2.0, .gain = 1.23};
  filter_config_t w1 = {.type = FILTER_TYPE_BIQUAD, .parameters.biquad = p1};
  ASSERT_EQ(0, g_biquad_vtable.validate(&w1, fs48, NULL));
  biquad_config_t p2 = {
      .type = BIQUAD_TYPE_PEAKING, .freq = 1000.0, .q = 0.0, .gain = 1.23};
  filter_config_t w2 = {.type = FILTER_TYPE_BIQUAD, .parameters.biquad = p2};
  ASSERT_NE(0, g_biquad_vtable.validate(&w2, fs48, NULL));
  biquad_config_t p3 = {
      .type = BIQUAD_TYPE_PEAKING, .freq = 25000.0, .q = 1.0, .gain = 1.23};
  filter_config_t w3 = {.type = FILTER_TYPE_BIQUAD, .parameters.biquad = p3};
  ASSERT_NE(0, g_biquad_vtable.validate(&w3, fs48, NULL));
  biquad_config_t p4 = {
      .type = BIQUAD_TYPE_PEAKING, .freq = 0.0, .q = 1.0, .gain = 1.23};
  filter_config_t w4 = {.type = FILTER_TYPE_BIQUAD, .parameters.biquad = p4};
  ASSERT_NE(0, g_biquad_vtable.validate(&w4, fs48, NULL));
}

TEST(ValidateSlope) {
  int fs48 = 48000;
  biquad_config_t p1 = {.type = BIQUAD_TYPE_HIGHSHELF,
                        .freq = 1000.0,
                        .gain = 1.23,
                        .slope = 5.0,
                        .steepness_type = STEEPNESS_TYPE_SLOPE};
  filter_config_t w1 = {.type = FILTER_TYPE_BIQUAD, .parameters.biquad = p1};
  ASSERT_EQ(0, g_biquad_vtable.validate(&w1, fs48, NULL));
  biquad_config_t p2 = {.type = BIQUAD_TYPE_HIGHSHELF,
                        .freq = 1000.0,
                        .gain = 1.23,
                        .slope = 0.0,
                        .steepness_type = STEEPNESS_TYPE_SLOPE};
  filter_config_t w2 = {.type = FILTER_TYPE_BIQUAD, .parameters.biquad = p2};
  ASSERT_NE(0, g_biquad_vtable.validate(&w2, fs48, NULL));
  biquad_config_t p3 = {.type = BIQUAD_TYPE_HIGHSHELF,
                        .freq = 1000.0,
                        .gain = 1.23,
                        .slope = 15.0,
                        .steepness_type = STEEPNESS_TYPE_SLOPE};
  filter_config_t w3 = {.type = FILTER_TYPE_BIQUAD, .parameters.biquad = p3};
  ASSERT_NE(0, g_biquad_vtable.validate(&w3, fs48, NULL));
}

TEST(BiquadCanonMatchesSequential) {
  int sample_rate = 48000;
  for (size_t num_stages = 1; num_stages <= 20; num_stages++) {
    biquad_filter_t* seq_filters[20];
    biquad_filter_t* can_filters[20];

    for (size_t k = 0; k < num_stages; k++) {
      biquad_config_t p = {.type = BIQUAD_TYPE_PEAKING,
                           .freq = 200.0 * (double)(k + 1),
                           .q = 0.707 + 0.1 * (double)k,
                           .gain = 2.0 - 0.2 * (double)k,
                           .steepness_type = STEEPNESS_TYPE_Q};
      filter_config_t cfg = {.type = FILTER_TYPE_BIQUAD,
                             .parameters.biquad = p};
      seq_filters[k] = (biquad_filter_t*)g_biquad_vtable.create(
          "seq", &cfg, sample_rate, 0, NULL, NULL);
      can_filters[k] = (biquad_filter_t*)g_biquad_vtable.create(
          "can", &cfg, sample_rate, 0, NULL, NULL);
      ASSERT_TRUE(seq_filters[k] != NULL);
      ASSERT_TRUE(can_filters[k] != NULL);
    }

    size_t count = 512;
    double seq_wave[512];
    double can_wave[512];
    for (size_t i = 0; i < count; i++) {
      double s = sin((double)i * 0.05);
      seq_wave[i] = s;
      can_wave[i] = s;
    }

    for (size_t k = 0; k < num_stages; k++) {
      g_biquad_vtable.process(seq_filters[k], seq_wave, count);
    }
    biquad_process_mono_cascade(can_filters, num_stages, can_wave, count);

    for (size_t i = 0; i < count; i++) {
      ASSERT_TRUE(fabs(seq_wave[i] - can_wave[i]) < 1e-12);
    }

    for (size_t k = 0; k < num_stages; k++) {
      g_biquad_vtable.free(seq_filters[k]);
      g_biquad_vtable.free(can_filters[k]);
    }
  }
}

TEST(BiquadCanonShortWaveforms) {
  int sample_rate = 48000;
  for (size_t len = 0; len <= 10; len++) {
    for (size_t num_stages = 1; num_stages <= 10; num_stages++) {
      biquad_filter_t* seq_filters[10];
      biquad_filter_t* can_filters[10];

      for (size_t k = 0; k < num_stages; k++) {
        biquad_config_t p = {.type = BIQUAD_TYPE_LOWPASS,
                             .freq = 1000.0 + 100.0 * (double)k,
                             .q = 0.5 + 0.1 * (double)k};
        filter_config_t cfg = {.type = FILTER_TYPE_BIQUAD,
                               .parameters.biquad = p};
        seq_filters[k] = (biquad_filter_t*)g_biquad_vtable.create(
            "seq", &cfg, sample_rate, 0, NULL, NULL);
        can_filters[k] = (biquad_filter_t*)g_biquad_vtable.create(
            "can", &cfg, sample_rate, 0, NULL, NULL);
      }

      double seq_wave[16] = {0};
      double can_wave[16] = {0};
      for (size_t i = 0; i < len; i++) {
        double s = cos((double)i * 0.1);
        seq_wave[i] = s;
        can_wave[i] = s;
      }

      if (len > 0) {
        for (size_t k = 0; k < num_stages; k++) {
          g_biquad_vtable.process(seq_filters[k], seq_wave, len);
        }
        biquad_process_mono_cascade(can_filters, num_stages, can_wave, len);

        for (size_t i = 0; i < len; i++) {
          ASSERT_TRUE(fabs(seq_wave[i] - can_wave[i]) < 1e-12);
        }
      }

      for (size_t k = 0; k < num_stages; k++) {
        g_biquad_vtable.free(seq_filters[k]);
        g_biquad_vtable.free(can_filters[k]);
      }
    }
  }
}

TEST(BiquadCanonHandlesEmptyAndTinyWaveforms) {
  int sample_rate = 48000;
  for (size_t num_stages = 1; num_stages <= 16; num_stages++) {
    for (size_t len = 0; len <= 1; len++) {
      biquad_filter_t* filters[16];
      for (size_t k = 0; k < num_stages; k++) {
        biquad_config_t p = {.type = BIQUAD_TYPE_LOWPASS,
                             .freq = 1000.0 + 50.0 * (double)k,
                             .q = 0.707};
        filter_config_t cfg = {.type = FILTER_TYPE_BIQUAD,
                               .parameters.biquad = p};
        filters[k] = (biquad_filter_t*)g_biquad_vtable.create(
            "bq", &cfg, sample_rate, 0, NULL, NULL);
      }
      double wave[2] = {1.0, 0.0};
      biquad_process_mono_cascade(filters, num_stages, wave, len);
      for (size_t k = 0; k < num_stages; k++) {
        g_biquad_vtable.free(filters[k]);
      }
    }
  }
}

TEST(BiquadCanonMatchesSequentialAcrossChunks) {
  int sample_rate = 48000;
  size_t stages_arr[] = {1, 7, 8, 9, 16, 19};
  size_t chunk_sizes[] = {4, 64};

  for (size_t s_idx = 0; s_idx < 6; s_idx++) {
    size_t num_stages = stages_arr[s_idx];
    for (size_t c_idx = 0; c_idx < 2; c_idx++) {
      size_t chunk_size = chunk_sizes[c_idx];
      size_t num_chunks = 8;
      size_t total_samples = num_chunks * chunk_size;

      biquad_filter_t* seq_filters[20];
      biquad_filter_t* can_filters[20];
      for (size_t k = 0; k < num_stages; k++) {
        biquad_config_t p = {.type = BIQUAD_TYPE_PEAKING,
                             .freq = 500.0 + 100.0 * (double)k,
                             .q = 1.0 + 0.1 * (double)k,
                             .gain = (k % 2 == 0) ? 3.0 : -3.0};
        filter_config_t cfg = {.type = FILTER_TYPE_BIQUAD,
                               .parameters.biquad = p};
        seq_filters[k] = (biquad_filter_t*)g_biquad_vtable.create(
            "seq", &cfg, sample_rate, 0, NULL, NULL);
        can_filters[k] = (biquad_filter_t*)g_biquad_vtable.create(
            "can", &cfg, sample_rate, 0, NULL, NULL);
      }

      double full_signal[512];
      for (size_t i = 0; i < total_samples; i++) {
        full_signal[i] = sin(0.013 * (double)i) * 0.5;
      }

      for (size_t chunk = 0; chunk < num_chunks; chunk++) {
        double seq_chunk[64];
        double can_chunk[64];
        for (size_t i = 0; i < chunk_size; i++) {
          seq_chunk[i] = full_signal[chunk * chunk_size + i];
          can_chunk[i] = full_signal[chunk * chunk_size + i];
        }

        for (size_t k = 0; k < num_stages; k++) {
          g_biquad_vtable.process(seq_filters[k], seq_chunk, chunk_size);
        }
        biquad_process_mono_cascade(can_filters, num_stages, can_chunk,
                                    chunk_size);

        for (size_t i = 0; i < chunk_size; i++) {
          ASSERT_TRUE(fabs(seq_chunk[i] - can_chunk[i]) < 1e-12);
        }
      }

      for (size_t k = 0; k < num_stages; k++) {
        g_biquad_vtable.free(seq_filters[k]);
        g_biquad_vtable.free(can_filters[k]);
      }
    }
  }
}

TEST(BiquadCascadesMultiChannel) {
  int sample_rate = 48000;
  size_t count = 256;
  size_t channels = 4;
  size_t stages = 3;

  biquad_filter_t* ch0[3];
  biquad_filter_t* ch1[3];
  biquad_filter_t* ch2[3];
  biquad_filter_t* ch3[3];
  biquad_filter_t** cascades[4] = {ch0, ch1, ch2, ch3};
  filter_t* ref_filters[4][3];

  for (size_t c = 0; c < channels; c++) {
    for (size_t s = 0; s < stages; s++) {
      char name[32];
      snprintf(name, sizeof(name), "bq_%zu_%zu", c, s);
      filter_config_t cfg = {
          .type = FILTER_TYPE_BIQUAD,
          .parameters.biquad = {.type = BIQUAD_TYPE_PEAKING,
                                .freq = 400.0 + 50.0 * (double)(s + c),
                                .q = 1.0,
                                .gain = (s % 2 == 0) ? 2.5 : -2.5}};
      cascades[c][s] = (biquad_filter_t*)g_biquad_vtable.create(
          name, &cfg, sample_rate, 0, NULL, NULL);
      ref_filters[c][s] =
          filter_create(name, &cfg, sample_rate, count, NULL, NULL);
      ASSERT_TRUE(cascades[c][s] != NULL);
      ASSERT_TRUE(ref_filters[c][s] != NULL);
    }
  }

  double wave_canon[4][256];
  double wave_ref[4][256];
  double* waveforms[4];
  size_t channel_of[4] = {0, 1, 2, 3};
  size_t live[4] = {0, 1, 2, 3};

  for (size_t c = 0; c < channels; c++) {
    waveforms[c] = wave_canon[c];
    for (size_t i = 0; i < count; i++) {
      double v = 0.3 * sin(0.015 * (double)(i + c * 10));
      wave_canon[c][i] = v;
      wave_ref[c][i] = v;
    }
  }

  // Process via 2D systolic biquad canon
  biquad_process_cascades(cascades, waveforms, channel_of, live, channels,
                          stages, count);

  // Process sequentially via reference filters
  for (size_t c = 0; c < channels; c++) {
    for (size_t s = 0; s < stages; s++) {
      filter_process(ref_filters[c][s], wave_ref[c], count);
      filter_free(ref_filters[c][s]);
    }
  }

  for (size_t c = 0; c < channels; c++) {
    for (size_t i = 0; i < count; i++) {
      ASSERT_NEAR(wave_ref[c][i], wave_canon[c][i], 1e-12);
    }
    for (size_t s = 0; s < stages; s++) {
      g_biquad_vtable.free(cascades[c][s]);
    }
  }
}

TEST_MAIN()
