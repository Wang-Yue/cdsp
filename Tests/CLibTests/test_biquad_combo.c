#include <math.h>
#include <stdbool.h>
#include <stddef.h>

#include "Config/filter_config_types.h"
#include "Filters/biquad.h"
#include "Filters/biquad_combo.h"
#include "Filters/filter.h"
#include "test_support.h"

// Declare internal helper functions to test them without public header
// pollution
extern size_t biquad_combo_butterworth_q(int order, double* out_q,
                                         size_t max_q);
extern size_t biquad_combo_linkwitz_riley_q(int order, double* out_q,
                                            size_t max_q);

static bool is_close(double left, double right, double maxdiff) {
  return fabs(left - right) < maxdiff;
}

static bool compare_vecs(const double* left, const double* right, size_t count,
                         double maxdiff) {
  for (size_t i = 0; i < count; i++) {
    if (!is_close(left[i], right[i], maxdiff)) return false;
  }
  return true;
}

TEST(make_butterworth_2) {
  double q[16];
  size_t count = biquad_combo_butterworth_q(2, q, 16);
  double expect[] = {0.707};
  ASSERT_EQ(1, count);
  ASSERT_TRUE(compare_vecs(q, expect, 1, 0.01));
}

TEST(make_butterworth_5) {
  double q[16];
  size_t count = biquad_combo_butterworth_q(5, q, 16);
  double expect[] = {1.62, 0.62, -1.0};
  ASSERT_EQ(3, count);
  ASSERT_TRUE(compare_vecs(q, expect, 3, 0.01));
}

TEST(make_butterworth_8) {
  double q[16];
  size_t count = biquad_combo_butterworth_q(8, q, 16);
  double expect[] = {2.56, 0.9, 0.6, 0.51};
  ASSERT_EQ(4, count);
  ASSERT_TRUE(compare_vecs(q, expect, 4, 0.01));
}

TEST(make_lr4) {
  double q[16];
  size_t count = biquad_combo_linkwitz_riley_q(4, q, 16);
  double expect[] = {0.707, 0.707};
  ASSERT_EQ(2, count);
  ASSERT_TRUE(compare_vecs(q, expect, 2, 0.01));
}

TEST(make_lr6) {
  double q[16];
  size_t count = biquad_combo_linkwitz_riley_q(10, q, 16);
  double expect[] = {1.62, 0.62, 1.62, 0.62, 0.5};
  ASSERT_EQ(5, count);
  ASSERT_TRUE(compare_vecs(q, expect, 5, 0.01));
}
TEST(check_lr) {
  int fs = 48000;
  biquad_combo_config_t okconf = {
      .type = BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_HIGHPASS,
      .freq = 1000.0,
      .has_freq = true,
      .order = 6,
      .has_order = true};
  filter_config_t cfg_ok = {.type = FILTER_TYPE_BIQUAD_COMBO,
                            .parameters.biquad_combo = okconf};
  ASSERT_EQ(0, g_biquad_combo_vtable.validate(&cfg_ok, fs, NULL));

  biquad_combo_config_t bad1 = {
      .type = BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_HIGHPASS,
      .freq = 1000.0,
      .has_freq = true,
      .order = 5,
      .has_order = true};
  filter_config_t cfg_bad1 = {.type = FILTER_TYPE_BIQUAD_COMBO,
                              .parameters.biquad_combo = bad1};
  ASSERT_NE(0, g_biquad_combo_vtable.validate(&cfg_bad1, fs, NULL));

  biquad_combo_config_t bad2 = {
      .type = BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_HIGHPASS,
      .freq = 1000.0,
      .has_freq = true,
      .order = 0,
      .has_order = true};
  filter_config_t cfg_bad2 = {.type = FILTER_TYPE_BIQUAD_COMBO,
                              .parameters.biquad_combo = bad2};
  ASSERT_NE(0, g_biquad_combo_vtable.validate(&cfg_bad2, fs, NULL));

  biquad_combo_config_t bad3 = {
      .type = BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_HIGHPASS,
      .freq = 0.0,
      .has_freq = true,
      .order = 2,
      .has_order = true};
  filter_config_t cfg_bad3 = {.type = FILTER_TYPE_BIQUAD_COMBO,
                              .parameters.biquad_combo = bad3};
  ASSERT_NE(0, g_biquad_combo_vtable.validate(&cfg_bad3, fs, NULL));

  biquad_combo_config_t bad4 = {
      .type = BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_HIGHPASS,
      .freq = 25000.0,
      .has_freq = true,
      .order = 2,
      .has_order = true};
  filter_config_t cfg_bad4 = {.type = FILTER_TYPE_BIQUAD_COMBO,
                              .parameters.biquad_combo = bad4};
  ASSERT_NE(0, g_biquad_combo_vtable.validate(&cfg_bad4, fs, NULL));
}

TEST(check_butterworth) {
  int fs = 48000;
  biquad_combo_config_t ok1 = {.type = BIQUAD_COMBO_TYPE_BUTTERWORTH_HIGHPASS,
                               .freq = 1000.0,
                               .has_freq = true,
                               .order = 6,
                               .has_order = true};
  filter_config_t cfg_ok1 = {.type = FILTER_TYPE_BIQUAD_COMBO,
                             .parameters.biquad_combo = ok1};
  ASSERT_EQ(0, g_biquad_combo_vtable.validate(&cfg_ok1, fs, NULL));

  biquad_combo_config_t ok2 = {.type = BIQUAD_COMBO_TYPE_BUTTERWORTH_HIGHPASS,
                               .freq = 1000.0,
                               .has_freq = true,
                               .order = 5,
                               .has_order = true};
  filter_config_t cfg_ok2 = {.type = FILTER_TYPE_BIQUAD_COMBO,
                             .parameters.biquad_combo = ok2};
  ASSERT_EQ(0, g_biquad_combo_vtable.validate(&cfg_ok2, fs, NULL));

  biquad_combo_config_t bad1 = {.type = BIQUAD_COMBO_TYPE_BUTTERWORTH_HIGHPASS,
                                .freq = 1000.0,
                                .has_freq = true,
                                .order = 0,
                                .has_order = true};
  filter_config_t cfg_bad1 = {.type = FILTER_TYPE_BIQUAD_COMBO,
                              .parameters.biquad_combo = bad1};
  ASSERT_NE(0, g_biquad_combo_vtable.validate(&cfg_bad1, fs, NULL));

  biquad_combo_config_t bad2 = {.type = BIQUAD_COMBO_TYPE_BUTTERWORTH_HIGHPASS,
                                .freq = 0.0,
                                .has_freq = true,
                                .order = 2,
                                .has_order = true};
  filter_config_t cfg_bad2 = {.type = FILTER_TYPE_BIQUAD_COMBO,
                              .parameters.biquad_combo = bad2};
  ASSERT_NE(0, g_biquad_combo_vtable.validate(&cfg_bad2, fs, NULL));

  biquad_combo_config_t bad3 = {.type = BIQUAD_COMBO_TYPE_BUTTERWORTH_HIGHPASS,
                                .freq = 25000.0,
                                .has_freq = true,
                                .order = 2,
                                .has_order = true};
  filter_config_t cfg_bad3 = {.type = FILTER_TYPE_BIQUAD_COMBO,
                              .parameters.biquad_combo = bad3};
  ASSERT_NE(0, g_biquad_combo_vtable.validate(&cfg_bad3, fs, NULL));
}

TEST(ComboCanonMatchesStagesRunOneAtATime) {
  int fs = 44100;
  biquad_combo_config_t lr_conf = {
      .type = BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_LOWPASS,
      .freq = 2000.0,
      .has_freq = true,
      .order = 8,
      .has_order = true};
  filter_config_t cfg = {.type = FILTER_TYPE_BIQUAD_COMBO,
                         .parameters.biquad_combo = lr_conf};

  void* combo_canon =
      g_biquad_combo_vtable.create("combo_canon", &cfg, fs, 0, NULL, NULL);
  ASSERT_TRUE(combo_canon != NULL);

  size_t len = 500;
  double wave_canon[500];
  double wave_split[500];
  double signal[500];
  for (size_t i = 0; i < len; i++) {
    signal[i] = 0.4 * sin((double)i * 0.013);
    wave_canon[i] = signal[i];
    wave_split[i] = signal[i];
  }

  // 1. Process with combo canon
  g_biquad_combo_vtable.process(combo_canon, wave_canon, len);

  // 2. Process sequentially through the 4 independent Linkwitz-Riley biquad
  // stages
  double qvals[4] = {
      1.0 / (2.0 * sin(M_PI / 4.0 * 0.5)),
      1.0 / (2.0 * sin(M_PI / 4.0 * 1.5)),
      1.0 / (2.0 * sin(M_PI / 4.0 * 0.5)),
      1.0 / (2.0 * sin(M_PI / 4.0 * 1.5)),
  };
  for (size_t s = 0; s < 4; s++) {
    biquad_config_t bp = {
        .type = BIQUAD_TYPE_LOWPASS,
        .freq = 2000.0,
        .q = qvals[s],
        .steepness_type = STEEPNESS_TYPE_Q,
    };
    filter_config_t bcfg = {.type = FILTER_TYPE_BIQUAD,
                            .parameters.biquad = bp};
    void* bq = g_biquad_vtable.create("seq_bq", &bcfg, fs, 0, NULL, NULL);
    ASSERT_TRUE(bq != NULL);
    g_biquad_vtable.process(bq, wave_split, len);
    g_biquad_vtable.free(bq);
  }

  // Verify match
  for (size_t i = 0; i < len; i++) {
    ASSERT_NEAR(wave_split[i], wave_canon[i], 1e-12);
  }

  // Verify that filter actually changed the signal
  bool changed = false;
  for (size_t i = 0; i < len; i++) {
    if (fabs(wave_canon[i] - signal[i]) > 1e-4) {
      changed = true;
      break;
    }
  }
  ASSERT_TRUE(changed);

  g_biquad_combo_vtable.free(combo_canon);
}

TEST(check_npeq_band_count) {
  int fs = 44100;
  filter_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.type = FILTER_TYPE_BIQUAD_COMBO;
  cfg.parameters.biquad_combo.type = BIQUAD_COMBO_TYPE_N_POINT_PEQ;

  // Empty bands
  cfg.parameters.biquad_combo.bands = NULL;
  cfg.parameters.biquad_combo.bands_count = 0;
  ASSERT_NE(0, g_biquad_combo_vtable.validate(&cfg, fs, NULL));

  // 1 band
  peq_band_t one_band = {.freq = 1000.0, .q = 0.7, .gain = 1.0};
  cfg.parameters.biquad_combo.bands = &one_band;
  cfg.parameters.biquad_combo.bands_count = 1;
  ASSERT_NE(0, g_biquad_combo_vtable.validate(&cfg, fs, NULL));

  // 2 bands
  peq_band_t two_bands[2] = {
      {.freq = 100.0, .q = 0.7, .gain = 1.0},
      {.freq = 8000.0, .q = 0.7, .gain = 1.0},
  };
  cfg.parameters.biquad_combo.bands = two_bands;
  cfg.parameters.biquad_combo.bands_count = 2;
  ASSERT_EQ(0, g_biquad_combo_vtable.validate(&cfg, fs, NULL));
}

TEST(check_npeq_frequency_order) {
  int fs = 44100;
  filter_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.type = FILTER_TYPE_BIQUAD_COMBO;
  cfg.parameters.biquad_combo.type = BIQUAD_COMBO_TYPE_N_POINT_PEQ;

  peq_band_t rising[4] = {
      {.freq = 100.0, .q = 0.7, .gain = 1.0},
      {.freq = 400.0, .q = 0.7, .gain = 1.0},
      {.freq = 1000.0, .q = 0.7, .gain = 1.0},
      {.freq = 8000.0, .q = 0.7, .gain = 1.0},
  };
  cfg.parameters.biquad_combo.bands = rising;
  cfg.parameters.biquad_combo.bands_count = 4;
  ASSERT_EQ(0, g_biquad_combo_vtable.validate(&cfg, fs, NULL));

  // Equal frequencies are allowed
  peq_band_t equal[3] = {
      {.freq = 400.0, .q = 0.7, .gain = 1.0},
      {.freq = 400.0, .q = 0.7, .gain = 1.0},
      {.freq = 400.0, .q = 0.7, .gain = 1.0},
  };
  cfg.parameters.biquad_combo.bands = equal;
  cfg.parameters.biquad_combo.bands_count = 3;
  ASSERT_EQ(0, g_biquad_combo_vtable.validate(&cfg, fs, NULL));

  // Falling frequencies
  peq_band_t falling[3] = {
      {.freq = 100.0, .q = 0.7, .gain = 1.0},
      {.freq = 2000.0, .q = 0.7, .gain = 1.0},
      {.freq = 500.0, .q = 0.7, .gain = 1.0},
  };
  cfg.parameters.biquad_combo.bands = falling;
  cfg.parameters.biquad_combo.bands_count = 3;
  ASSERT_NE(0, g_biquad_combo_vtable.validate(&cfg, fs, NULL));

  // Shelves swapped
  peq_band_t shelves_swapped[2] = {
      {.freq = 8000.0, .q = 0.7, .gain = 1.0},
      {.freq = 100.0, .q = 0.7, .gain = 1.0},
  };
  cfg.parameters.biquad_combo.bands = shelves_swapped;
  cfg.parameters.biquad_combo.bands_count = 2;
  ASSERT_NE(0, g_biquad_combo_vtable.validate(&cfg, fs, NULL));
}

TEST(check_npeq_bands) {
  int fs = 44100;
  filter_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.type = FILTER_TYPE_BIQUAD_COMBO;
  cfg.parameters.biquad_combo.type = BIQUAD_COMBO_TYPE_N_POINT_PEQ;

  peq_band_t bad_freq[2] = {
      {.freq = -5.0, .q = 0.7, .gain = 0.0},
      {.freq = 8000.0, .q = 0.7, .gain = 1.0},
  };
  cfg.parameters.biquad_combo.bands = bad_freq;
  cfg.parameters.biquad_combo.bands_count = 2;
  ASSERT_NE(0, g_biquad_combo_vtable.validate(&cfg, fs, NULL));

  peq_band_t bad_q[3] = {
      {.freq = 100.0, .q = 0.7, .gain = 1.0},
      {.freq = 1000.0, .q = 0.0, .gain = 1.0},
      {.freq = 8000.0, .q = 0.7, .gain = 1.0},
  };
  cfg.parameters.biquad_combo.bands = bad_q;
  cfg.parameters.biquad_combo.bands_count = 3;
  ASSERT_NE(0, g_biquad_combo_vtable.validate(&cfg, fs, NULL));

  peq_band_t above_nyquist[2] = {
      {.freq = 100.0, .q = 0.7, .gain = 1.0},
      {.freq = 30000.0, .q = 0.7, .gain = 1.0},
  };
  cfg.parameters.biquad_combo.bands = above_nyquist;
  cfg.parameters.biquad_combo.bands_count = 2;
  ASSERT_NE(0, g_biquad_combo_vtable.validate(&cfg, fs, NULL));
}

TEST(npeq_all_zero_gain_is_passthrough) {
  int fs = 44100;
  peq_band_t bands[3] = {
      {.freq = 100.0, .q = 0.7, .gain = 0.0},
      {.freq = 1000.0, .q = 0.7, .gain = -0.0},
      {.freq = 8000.0, .q = 0.7, .gain = 0.0},
  };
  filter_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.type = FILTER_TYPE_BIQUAD_COMBO;
  cfg.parameters.biquad_combo.type = BIQUAD_COMBO_TYPE_N_POINT_PEQ;
  cfg.parameters.biquad_combo.bands = bands;
  cfg.parameters.biquad_combo.bands_count = 3;

  void* combo =
      g_biquad_combo_vtable.create("test_npeq_zero", &cfg, fs, 0, NULL, NULL);
  ASSERT_TRUE(combo != NULL);

  double wave[4] = {1.0, 0.5, -0.25, 0.0};
  double expected[4] = {1.0, 0.5, -0.25, 0.0};
  g_biquad_combo_vtable.process(combo, wave, 4);
  for (size_t i = 0; i < 4; i++) {
    ASSERT_NEAR(wave[i], expected[i], 1e-12);
  }

  g_biquad_combo_vtable.free(combo);
}

TEST(npeq_matches_separate_biquads) {
  int fs = 44100;
  peq_band_t bands[5] = {
      {.freq = 125.0, .q = 0.7, .gain = 1.0},
      {.freq = 400.0, .q = 0.7, .gain = -0.5},
      {.freq = 1000.0, .q = 0.7, .gain = 1.5},
      {.freq = 2500.0, .q = 0.7, .gain = -0.25},
      {.freq = 8000.0, .q = 0.7, .gain = 0.5},
  };
  filter_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.type = FILTER_TYPE_BIQUAD_COMBO;
  cfg.parameters.biquad_combo.type = BIQUAD_COMBO_TYPE_N_POINT_PEQ;
  cfg.parameters.biquad_combo.bands = bands;
  cfg.parameters.biquad_combo.bands_count = 5;

  void* combo =
      g_biquad_combo_vtable.create("test_npeq", &cfg, fs, 0, NULL, NULL);
  ASSERT_TRUE(combo != NULL);

  // Impulse
  double wave_combo[1024];
  double wave_separate[1024];
  memset(wave_combo, 0, sizeof(wave_combo));
  memset(wave_separate, 0, sizeof(wave_separate));
  wave_combo[0] = 1.0;
  wave_separate[0] = 1.0;

  g_biquad_combo_vtable.process(combo, wave_combo, 1024);

  // Process 5 separate biquads sequentially
  biquad_type_t types[5] = {
      BIQUAD_TYPE_LOWSHELF, BIQUAD_TYPE_PEAKING,   BIQUAD_TYPE_PEAKING,
      BIQUAD_TYPE_PEAKING,  BIQUAD_TYPE_HIGHSHELF,
  };
  for (size_t s = 0; s < 5; s++) {
    biquad_config_t bp = {
        .type = types[s],
        .freq = bands[s].freq,
        .q = bands[s].q,
        .gain = bands[s].gain,
        .steepness_type = STEEPNESS_TYPE_Q,
    };
    filter_config_t bcfg = {.type = FILTER_TYPE_BIQUAD,
                            .parameters.biquad = bp};
    void* bq = g_biquad_vtable.create("sep_bq", &bcfg, fs, 0, NULL, NULL);
    ASSERT_TRUE(bq != NULL);
    g_biquad_vtable.process(bq, wave_separate, 1024);
    g_biquad_vtable.free(bq);
  }

  for (size_t i = 0; i < 1024; i++) {
    ASSERT_NEAR(wave_combo[i], wave_separate[i], 1e-12);
  }

  g_biquad_combo_vtable.free(combo);
}

TEST_MAIN()
