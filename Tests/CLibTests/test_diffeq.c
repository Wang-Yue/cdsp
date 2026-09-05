#include <math.h>
#include <stdbool.h>
#include <stddef.h>

#include "Config/config_error.h"
#include "Config/filter_config_types.h"
#include "Filters/diffeq.h"
#include "Filters/filter.h"
#include "test_support.h"

static bool is_close(double left, double right, double maxdiff) {
  return fabs(left - right) < maxdiff;
}

static bool compare_waveforms(const double* left, const double* right,
                              size_t count, double maxdiff) {
  for (size_t i = 0; i < count; i++) {
    if (!is_close(left[i], right[i], maxdiff)) return false;
  }
  return true;
}

TEST(DiffEq_CheckResult) {
  double a_vals[] = {1.0, -0.1462978543780541, 0.005350765548905586};
  double b_vals[] = {0.21476322779271284, 0.4295264555854257,
                     0.21476322779271284};
  diffeq_config_t params = {
      .a = a_vals, .a_count = 3, .b = b_vals, .b_count = 3};
  filter_config_t cfg = {.type = FILTER_TYPE_DIFF_EQ,
                         .parameters.diff_eq = params};
  void* filter = g_diffeq_vtable.create("diffeq", &cfg, 0, 0, NULL, NULL);
  ASSERT_TRUE(filter != NULL);

  double wave[] = {1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  double expected[] = {0.215, 0.461, 0.281, 0.039, 0.004, 0.0, 0.0, 0.0};

  g_diffeq_vtable.process(filter, wave, 8);

  ASSERT_TRUE(compare_waveforms(wave, expected, 8, 1e-3));
  g_diffeq_vtable.free(filter);
}

TEST(DiffEq_InvalidA0) {
  double a_vals[] = {0.0, -0.146, 0.005};
  double b_vals[] = {0.214, 0.429, 0.214};
  diffeq_config_t params = {
      .a = a_vals, .a_count = 3, .b = b_vals, .b_count = 3};
  filter_config_t cfg = {.type = FILTER_TYPE_DIFF_EQ,
                         .parameters.diff_eq = params};
  config_error_t err = {0};
  int res = g_diffeq_vtable.validate(&cfg, 48000, &err);
  ASSERT_NE(0, res);
}

TEST(DiffEq_CheckResultUnscaled) {
  double a_vals[] = {3.0, -0.4388935631341623, 0.016052296646716757};
  double b_vals[] = {0.6442896833781385, 1.288579366756277, 0.6442896833781385};
  diffeq_config_t params = {
      .a = a_vals, .a_count = 3, .b = b_vals, .b_count = 3};
  filter_config_t cfg = {.type = FILTER_TYPE_DIFF_EQ,
                         .parameters.diff_eq = params};
  void* filter =
      g_diffeq_vtable.create("diffeq_unscaled", &cfg, 0, 0, NULL, NULL);
  ASSERT_TRUE(filter != NULL);

  double wave[] = {1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  double expected[] = {0.215, 0.461, 0.281, 0.039, 0.004, 0.0, 0.0, 0.0};

  g_diffeq_vtable.process(filter, wave, 8);
  ASSERT_TRUE(compare_waveforms(wave, expected, 8, 1e-3));
  g_diffeq_vtable.free(filter);
}

TEST(DiffEq_CheckResultHighOrder) {
  double a_vals[] = {1.0,
                     0.05,
                     0.0333333333333333,
                     0.025,
                     0.02,
                     0.0166666666666667,
                     0.0142857142857143,
                     0.0125,
                     0.0111111111111111,
                     0.01,
                     0.0090909090909091,
                     0.0083333333333333,
                     0.0076923076923077};
  double b_vals[] = {0.3,
                     0.15,
                     0.1,
                     0.075,
                     0.06,
                     0.05,
                     0.0428571428571429,
                     0.0375,
                     0.0333333333333333,
                     0.03,
                     0.0272727272727273,
                     0.025,
                     0.05};
  diffeq_config_t params = {
      .a = a_vals, .a_count = 13, .b = b_vals, .b_count = 13};
  filter_config_t cfg = {.type = FILTER_TYPE_DIFF_EQ,
                         .parameters.diff_eq = params};
  void* filter = g_diffeq_vtable.create("diffeq_o12", &cfg, 0, 0, NULL, NULL);
  ASSERT_TRUE(filter != NULL);

  double wave[12] = {1.0, 0.0, 0.0, 0.0, 0.0, 0.0,
                     0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  double expected[12] = {0.3,
                         0.135,
                         0.08325,
                         0.0588375,
                         0.044908125,
                         0.03601209375,
                         0.029887948884,
                         0.025439674877,
                         0.022075875735,
                         0.019451146989,
                         0.017351068867,
                         0.015635919346};
  g_diffeq_vtable.process(filter, wave, 12);
  ASSERT_TRUE(compare_waveforms(wave, expected, 12, 1e-6));
  g_diffeq_vtable.free(filter);
}

TEST(DiffEq_CheckStateBetweenChunks) {
  double a_vals[] = {1.0,
                     0.05,
                     0.0333333333333333,
                     0.025,
                     0.02,
                     0.0166666666666667,
                     0.0142857142857143,
                     0.0125,
                     0.0111111111111111,
                     0.01,
                     0.0090909090909091,
                     0.0083333333333333,
                     0.0076923076923077};
  double b_vals[] = {0.3,
                     0.15,
                     0.1,
                     0.075,
                     0.06,
                     0.05,
                     0.0428571428571429,
                     0.0375,
                     0.0333333333333333,
                     0.03,
                     0.0272727272727273,
                     0.025,
                     0.05};
  diffeq_config_t params = {
      .a = a_vals, .a_count = 13, .b = b_vals, .b_count = 13};
  filter_config_t cfg = {.type = FILTER_TYPE_DIFF_EQ,
                         .parameters.diff_eq = params};
  void* filter1 =
      g_diffeq_vtable.create("diffeq_chunk1", &cfg, 0, 0, NULL, NULL);
  void* filter2 =
      g_diffeq_vtable.create("diffeq_chunk2", &cfg, 0, 0, NULL, NULL);
  ASSERT_TRUE(filter1 != NULL);
  ASSERT_TRUE(filter2 != NULL);

  double wave1[12] = {1.0, 0.0, 0.0, 0.0, 0.0, 0.0,
                      0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  g_diffeq_vtable.process(filter1, wave1, 12);

  double wave2_a[6] = {1.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  double wave2_b[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  g_diffeq_vtable.process(filter2, wave2_a, 6);
  g_diffeq_vtable.process(filter2, wave2_b, 6);

  for (size_t i = 0; i < 6; i++) {
    ASSERT_NEAR(wave1[i], wave2_a[i], 1e-9);
    ASSERT_NEAR(wave1[6 + i], wave2_b[i], 1e-9);
  }

  g_diffeq_vtable.free(filter1);
  g_diffeq_vtable.free(filter2);
}

TEST(DiffEq_CheckResultUnevenLengths) {
  // More b than a
  double a1[] = {1.0, -0.5};
  double b1[] = {0.2, 0.1, 0.05, 0.01};
  diffeq_config_t params1 = {.a = a1, .a_count = 2, .b = b1, .b_count = 4};
  filter_config_t cfg1 = {.type = FILTER_TYPE_DIFF_EQ,
                          .parameters.diff_eq = params1};
  void* f1 = g_diffeq_vtable.create("f1", &cfg1, 0, 0, NULL, NULL);
  ASSERT_TRUE(f1 != NULL);
  double wave1[8] = {1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  double exp1[8] = {0.2,    0.2,     0.15,     0.085,
                    0.0425, 0.02125, 0.010625, 0.0053125};
  g_diffeq_vtable.process(f1, wave1, 8);
  ASSERT_TRUE(compare_waveforms(wave1, exp1, 8, 1e-6));
  g_diffeq_vtable.free(f1);

  // More a than b
  double a2[] = {1.0, -0.5, 0.2, -0.05};
  double b2[] = {0.5};
  diffeq_config_t params2 = {.a = a2, .a_count = 4, .b = b2, .b_count = 1};
  filter_config_t cfg2 = {.type = FILTER_TYPE_DIFF_EQ,
                          .parameters.diff_eq = params2};
  void* f2 = g_diffeq_vtable.create("f2", &cfg2, 0, 0, NULL, NULL);
  ASSERT_TRUE(f2 != NULL);
  double wave2[8] = {1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  double exp2[8] = {0.5,     0.25,     0.025,     -0.0125,
                    0.00125, 0.004375, 0.0013125, -0.00015625};
  g_diffeq_vtable.process(f2, wave2, 8);
  ASSERT_TRUE(compare_waveforms(wave2, exp2, 8, 1e-6));
  g_diffeq_vtable.free(f2);
}

TEST(DiffEq_CheckResultNoState) {
  double a[] = {2.0};
  double b[] = {1.0};
  diffeq_config_t params = {.a = a, .a_count = 1, .b = b, .b_count = 1};
  filter_config_t cfg = {.type = FILTER_TYPE_DIFF_EQ,
                         .parameters.diff_eq = params};
  void* f = g_diffeq_vtable.create("f_gain", &cfg, 0, 0, NULL, NULL);
  ASSERT_TRUE(f != NULL);
  double wave[3] = {1.0, -1.0, 0.5};
  double expected[3] = {0.5, -0.5, 0.25};
  g_diffeq_vtable.process(f, wave, 3);
  ASSERT_TRUE(compare_waveforms(wave, expected, 3, 1e-9));
  g_diffeq_vtable.free(f);
}

TEST(DiffEq_ValidateStable) {
  double b[] = {1.0};

  // Lowpass biquad, poles at 0.073 and 0.073
  double a1[] = {1.0, -0.1462978543780541, 0.005350765548905586};
  diffeq_config_t p1 = {.a = a1, .a_count = 3, .b = b, .b_count = 1};
  filter_config_t c1 = {.type = FILTER_TYPE_DIFF_EQ, .parameters.diff_eq = p1};
  ASSERT_EQ(0, g_diffeq_vtable.validate(&c1, 0, NULL));

  // Single pole at 0.5
  double a2[] = {1.0, -0.5};
  diffeq_config_t p2 = {.a = a2, .a_count = 2, .b = b, .b_count = 1};
  filter_config_t c2 = {.type = FILTER_TYPE_DIFF_EQ, .parameters.diff_eq = p2};
  ASSERT_EQ(0, g_diffeq_vtable.validate(&c2, 0, NULL));

  // Complex pole pair at 0.707, note that a1 is larger than unity
  double a3[] = {1.0, -1.2, 0.5};
  diffeq_config_t p3 = {.a = a3, .a_count = 3, .b = b, .b_count = 1};
  filter_config_t c3 = {.type = FILTER_TYPE_DIFF_EQ, .parameters.diff_eq = p3};
  ASSERT_EQ(0, g_diffeq_vtable.validate(&c3, 0, NULL));

  // Fourth order, poles at 0.9, -0.9, 0.5, -0.5
  double a4[] = {1.0, 0.0, -1.06, 0.0, 0.2025};
  diffeq_config_t p4 = {.a = a4, .a_count = 5, .b = b, .b_count = 1};
  filter_config_t c4 = {.type = FILTER_TYPE_DIFF_EQ, .parameters.diff_eq = p4};
  ASSERT_EQ(0, g_diffeq_vtable.validate(&c4, 0, NULL));

  // Pole at zero, from trailing zero coefficient
  double a5[] = {1.0, 0.5, 0.0};
  diffeq_config_t p5 = {.a = a5, .a_count = 3, .b = b, .b_count = 1};
  filter_config_t c5 = {.type = FILTER_TYPE_DIFF_EQ, .parameters.diff_eq = p5};
  ASSERT_EQ(0, g_diffeq_vtable.validate(&c5, 0, NULL));

  // Unscaled coefficients, poles at 0.073 and 0.073
  double a6[] = {4.0, -0.585, 0.0214};
  diffeq_config_t p6 = {.a = a6, .a_count = 3, .b = b, .b_count = 1};
  filter_config_t c6 = {.type = FILTER_TYPE_DIFF_EQ, .parameters.diff_eq = p6};
  ASSERT_EQ(0, g_diffeq_vtable.validate(&c6, 0, NULL));

  // No feedback, plain FIR filter
  diffeq_config_t p7 = {.a = NULL, .a_count = 0, .b = b, .b_count = 1};
  filter_config_t c7 = {.type = FILTER_TYPE_DIFF_EQ, .parameters.diff_eq = p7};
  ASSERT_EQ(0, g_diffeq_vtable.validate(&c7, 0, NULL));
}

TEST(DiffEq_ValidateUnstable) {
  double b[] = {1.0};

  // Single pole at 1.1
  double a1[] = {1.0, -1.1};
  diffeq_config_t p1 = {.a = a1, .a_count = 2, .b = b, .b_count = 1};
  filter_config_t c1 = {.type = FILTER_TYPE_DIFF_EQ, .parameters.diff_eq = p1};
  ASSERT_NE(0, g_diffeq_vtable.validate(&c1, 0, NULL));

  // Single pole exactly on the unit circle
  double a2[] = {1.0, -1.0};
  diffeq_config_t p2 = {.a = a2, .a_count = 2, .b = b, .b_count = 1};
  filter_config_t c2 = {.type = FILTER_TYPE_DIFF_EQ, .parameters.diff_eq = p2};
  ASSERT_NE(0, g_diffeq_vtable.validate(&c2, 0, NULL));

  // Poles at 1.1 and -1.1
  double a3[] = {1.0, 0.0, -1.21};
  diffeq_config_t p3 = {.a = a3, .a_count = 3, .b = b, .b_count = 1};
  filter_config_t c3 = {.type = FILTER_TYPE_DIFF_EQ, .parameters.diff_eq = p3};
  ASSERT_NE(0, g_diffeq_vtable.validate(&c3, 0, NULL));

  // Poles at 1.5 and -0.6, all coefficients are smaller than unity
  double a4[] = {1.0, -0.9, -0.9};
  diffeq_config_t p4 = {.a = a4, .a_count = 3, .b = b, .b_count = 1};
  filter_config_t c4 = {.type = FILTER_TYPE_DIFF_EQ, .parameters.diff_eq = p4};
  ASSERT_NE(0, g_diffeq_vtable.validate(&c4, 0, NULL));

  // Unscaled coefficients, pole at 1.1
  double a5[] = {2.0, -2.2};
  diffeq_config_t p5 = {.a = a5, .a_count = 2, .b = b, .b_count = 1};
  filter_config_t c5 = {.type = FILTER_TYPE_DIFF_EQ, .parameters.diff_eq = p5};
  ASSERT_NE(0, g_diffeq_vtable.validate(&c5, 0, NULL));
}

TEST(DiffEq_ValidateInvalidCoefficients) {
  double b[] = {1.0};

  // a[0] == 0
  double a1[] = {0.0, 0.5};
  diffeq_config_t p1 = {.a = a1, .a_count = 2, .b = b, .b_count = 1};
  filter_config_t c1 = {.type = FILTER_TYPE_DIFF_EQ, .parameters.diff_eq = p1};
  ASSERT_NE(0, g_diffeq_vtable.validate(&c1, 0, NULL));

  // NaN
  double a2[] = {1.0, NAN};
  diffeq_config_t p2 = {.a = a2, .a_count = 2, .b = b, .b_count = 1};
  filter_config_t c2 = {.type = FILTER_TYPE_DIFF_EQ, .parameters.diff_eq = p2};
  ASSERT_NE(0, g_diffeq_vtable.validate(&c2, 0, NULL));

  // Inf
  double a3[] = {1.0, 0.5};
  double b3[] = {INFINITY};
  diffeq_config_t p3 = {.a = a3, .a_count = 2, .b = b3, .b_count = 1};
  filter_config_t c3 = {.type = FILTER_TYPE_DIFF_EQ, .parameters.diff_eq = p3};
  ASSERT_NE(0, g_diffeq_vtable.validate(&c3, 0, NULL));
}

TEST_MAIN()
