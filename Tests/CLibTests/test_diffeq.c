#include <math.h>

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

TEST(check_result) {
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

TEST_MAIN()
