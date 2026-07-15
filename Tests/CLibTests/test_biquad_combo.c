#include "Filters/biquad_combo.h"
#include "Filters/filter.h"
#include "test_support.h"

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

TEST_MAIN()
