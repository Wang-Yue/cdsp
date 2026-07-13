#include <math.h>

#include "Filters/biquad.h"
#include "test_support.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void gain_and_phase(const biquad_parameters_t* params, double f,
                           double fs, double* gain_db, double* phase_deg) {
  biquad_filter_t* filter = biquad_filter_create("test", params, (int)fs, NULL);
  if (!filter) {
    *gain_db = 0.0;
    *phase_deg = 0.0;
    return;
  }
  size_t N = 8192;
  double* wave = (double*)calloc(N, sizeof(double));
  wave[0] = 1.0;
  biquad_filter_process(filter, wave, N);
  biquad_filter_free(filter);

  double w = 2.0 * M_PI * f / fs;
  double re = 0.0, im = 0.0;
  for (size_t n = 0; n < N; n++) {
    re += wave[n] * cos(w * (double)n);
    im -= wave[n] * sin(w * (double)n);
  }
  free(wave);
  double mag = sqrt(re * re + im * im);
  *gain_db = 20.0 * log10(mag > 1e-12 ? mag : 1e-12);
  *phase_deg = atan2(im, re) * 180.0 / M_PI;
}

static bool is_close(double left, double right, double maxdiff) {
  return fabs(left - right) < maxdiff;
}

static bool is_close_relative(double left, double right, double maxdiff) {
  return fabs(left / right - 1.0) < maxdiff;
}

TEST(ImpulseResponse) {
  biquad_parameters_t params = {
      .type = BIQUAD_TYPE_LOWPASS, .freq = 10000.0, .q = 0.5};
  biquad_filter_t* filter =
      biquad_filter_create("biquad", &params, 44100, NULL);
  ASSERT_TRUE(filter != NULL);

  double wave[] = {1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  double expected[] = {0.215, 0.461, 0.281, 0.039, 0.004, 0.0, 0.0, 0.0};

  biquad_filter_process(filter, wave, 8);

  for (size_t i = 0; i < 8; i++) {
    ASSERT_TRUE(is_close(wave[i], expected[i], 1e-3));
  }
  biquad_filter_free(filter);
}

TEST(Lowpass) {
  biquad_parameters_t params = {
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
  biquad_parameters_t params = {
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
  biquad_parameters_t params = {.type = BIQUAD_TYPE_LOWPASS_FO, .freq = 100.0};
  double gf0, pf0, ghf, phf, glf, plf;
  gain_and_phase(&params, 100.0, 44100.0, &gf0, &pf0);
  gain_and_phase(&params, 400.0, 44100.0, &ghf, &phf);
  gain_and_phase(&params, 10.0, 44100.0, &glf, &plf);
  ASSERT_TRUE(is_close(gf0, -3.0, 0.1));
  ASSERT_TRUE(is_close(glf, 0.0, 0.1));
  ASSERT_TRUE(is_close(ghf, -12.3, 0.1));
}

TEST(HighpassFO) {
  biquad_parameters_t params = {.type = BIQUAD_TYPE_HIGHPASS_FO, .freq = 100.0};
  double gf0, pf0, ghf, phf, glf, plf;
  gain_and_phase(&params, 100.0, 44100.0, &gf0, &pf0);
  gain_and_phase(&params, 800.0, 44100.0, &ghf, &phf);
  gain_and_phase(&params, 25.0, 44100.0, &glf, &plf);
  ASSERT_TRUE(is_close(gf0, -3.0, 0.1));
  ASSERT_TRUE(is_close(glf, -12.3, 0.1));
  ASSERT_TRUE(is_close(ghf, 0.0, 0.1));
}

TEST(Peaking) {
  biquad_parameters_t params = {
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
  biquad_parameters_t params = {
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
  biquad_parameters_t params = {
      .type = BIQUAD_TYPE_NOTCH, .freq = 100.0, .q = 3.0};
  double gf0, pf0, ghf, phf, glf, plf;
  gain_and_phase(&params, 100.0, 44100.0, &gf0, &pf0);
  gain_and_phase(&params, 400.0, 44100.0, &ghf, &phf);
  gain_and_phase(&params, 25.0, 44100.0, &glf, &plf);
  ASSERT_TRUE(gf0 < -40.0);
  ASSERT_TRUE(is_close(glf, 0.0, 0.1));
  ASSERT_TRUE(is_close(ghf, 0.0, 0.1));
}

TEST(Allpass) {
  biquad_parameters_t params = {
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
  biquad_parameters_t params = {.type = BIQUAD_TYPE_ALLPASS_FO, .freq = 100.0};
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
  biquad_parameters_t params = {.type = BIQUAD_TYPE_HIGHSHELF,
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
  biquad_parameters_t params = {.type = BIQUAD_TYPE_LOWSHELF,
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
  biquad_parameters_t pS = {.type = BIQUAD_TYPE_LOWSHELF,
                            .freq = 100.0,
                            .gain = -24.0,
                            .slope = 12.0,
                            .steepness_type = STEEPNESS_TYPE_SLOPE};
  biquad_parameters_t pQ = {.type = BIQUAD_TYPE_LOWSHELF,
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
  biquad_parameters_t pS = {.type = BIQUAD_TYPE_HIGHSHELF,
                            .freq = 100.0,
                            .gain = -24.0,
                            .slope = 12.0,
                            .steepness_type = STEEPNESS_TYPE_SLOPE};
  biquad_parameters_t pQ = {.type = BIQUAD_TYPE_HIGHSHELF,
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
  biquad_parameters_t pBW = {.type = BIQUAD_TYPE_BANDPASS,
                             .freq = 100.0,
                             .bandwidth = 1.0,
                             .steepness_type = STEEPNESS_TYPE_BANDWIDTH};
  biquad_parameters_t pQ = {
      .type = BIQUAD_TYPE_BANDPASS, .freq = 100.0, .q = sqrt(2.0)};
  double gBW, pBW_deg, gQ, pQ_deg;
  gain_and_phase(&pBW, 100.0, 44100.0, &gBW, &pBW_deg);
  gain_and_phase(&pQ, 100.0, 44100.0, &gQ, &pQ_deg);
  ASSERT_TRUE(is_close(gBW, gQ, 0.001));
  ASSERT_TRUE(is_close(pBW_deg, pQ_deg, 0.001));
}

TEST(NotchBWvsQ) {
  biquad_parameters_t pBW = {.type = BIQUAD_TYPE_NOTCH,
                             .freq = 100.0,
                             .bandwidth = 1.0,
                             .steepness_type = STEEPNESS_TYPE_BANDWIDTH};
  biquad_parameters_t pQ = {
      .type = BIQUAD_TYPE_NOTCH, .freq = 100.0, .q = sqrt(2.0)};
  double gBW, pBW_deg, gQ, pQ_deg;
  gain_and_phase(&pBW, 200.0, 44100.0, &gBW, &pBW_deg);
  gain_and_phase(&pQ, 200.0, 44100.0, &gQ, &pQ_deg);
  ASSERT_TRUE(is_close(gBW, gQ, 0.001));
  ASSERT_TRUE(is_close(pBW_deg, pQ_deg, 0.001));
}

TEST(AllpassBWvsQ) {
  biquad_parameters_t pBW = {.type = BIQUAD_TYPE_ALLPASS,
                             .freq = 100.0,
                             .bandwidth = 1.0,
                             .steepness_type = STEEPNESS_TYPE_BANDWIDTH};
  biquad_parameters_t pQ = {
      .type = BIQUAD_TYPE_ALLPASS, .freq = 100.0, .q = sqrt(2.0)};
  double gBW, pBW_deg, gQ, pQ_deg;
  gain_and_phase(&pBW, 100.0, 44100.0, &gBW, &pBW_deg);
  gain_and_phase(&pQ, 100.0, 44100.0, &gQ, &pQ_deg);
  ASSERT_TRUE(is_close(gBW, gQ, 0.001));
  ASSERT_TRUE(is_close(pBW_deg, pQ_deg, 0.001));
}

TEST(HighshelfFO) {
  biquad_parameters_t params = {
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
  biquad_parameters_t params = {
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
  biquad_parameters_t params = {.type = BIQUAD_TYPE_FREE,
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
  biquad_parameters_t params = {.type = BIQUAD_TYPE_GENERAL_NOTCH,
                                .q = 1.0,
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
  biquad_parameters_t params = {.type = BIQUAD_TYPE_GENERAL_NOTCH,
                                .q = 1.0,
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
  biquad_parameters_t params = {.type = BIQUAD_TYPE_LINKWITZ_TRANSFORM,
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
  biquad_parameters_t p1 = {
      .type = BIQUAD_TYPE_PEAKING, .freq = 1000.0, .q = 2.0, .gain = 1.23};
  ASSERT_EQ(0, biquad_parameters_validate(&p1, fs48, NULL));
  biquad_parameters_t p2 = {
      .type = BIQUAD_TYPE_PEAKING, .freq = 1000.0, .q = 0.0, .gain = 1.23};
  ASSERT_NE(0, biquad_parameters_validate(&p2, fs48, NULL));
  biquad_parameters_t p3 = {
      .type = BIQUAD_TYPE_PEAKING, .freq = 25000.0, .q = 1.0, .gain = 1.23};
  ASSERT_NE(0, biquad_parameters_validate(&p3, fs48, NULL));
  biquad_parameters_t p4 = {
      .type = BIQUAD_TYPE_PEAKING, .freq = 0.0, .q = 1.0, .gain = 1.23};
  ASSERT_NE(0, biquad_parameters_validate(&p4, fs48, NULL));
}

TEST(ValidateSlope) {
  int fs48 = 48000;
  biquad_parameters_t p1 = {.type = BIQUAD_TYPE_HIGHSHELF,
                            .freq = 1000.0,
                            .gain = 1.23,
                            .slope = 5.0,
                            .steepness_type = STEEPNESS_TYPE_SLOPE};
  ASSERT_EQ(0, biquad_parameters_validate(&p1, fs48, NULL));
  biquad_parameters_t p2 = {.type = BIQUAD_TYPE_HIGHSHELF,
                            .freq = 1000.0,
                            .gain = 1.23,
                            .slope = 0.0,
                            .steepness_type = STEEPNESS_TYPE_SLOPE};
  ASSERT_NE(0, biquad_parameters_validate(&p2, fs48, NULL));
  biquad_parameters_t p3 = {.type = BIQUAD_TYPE_HIGHSHELF,
                            .freq = 1000.0,
                            .gain = 1.23,
                            .slope = 15.0,
                            .steepness_type = STEEPNESS_TYPE_SLOPE};
  ASSERT_NE(0, biquad_parameters_validate(&p3, fs48, NULL));
}

TEST_MAIN()
