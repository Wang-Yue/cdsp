#include <math.h>

#include "Filters/dither.h"
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

TEST(test_quantize) {
  double waveform[] = {-1.0, -0.5, -1.0 / 3.0, 0.0, 1.0 / 3.0, 0.5, 1.0};
  double waveform2[] = {-1.0, -0.5, -1.0 / 3.0, 0.0, 1.0 / 3.0, 0.5, 1.0};
  dither_config_t params = {.type = DITHER_TYPE_NONE, .bits = 8};
  filter_config_t cfg = {.type = FILTER_TYPE_DITHER,
                         .parameters.dither = params};
  void* filter = g_dither_vtable.create("dither", &cfg, 0, 0, NULL, NULL);
  ASSERT_TRUE(filter != NULL);
  g_dither_vtable.process(filter, waveform, 7);

  ASSERT_TRUE(compare_waveforms(waveform, waveform2, 7, 1.0 / 128.0));
  ASSERT_TRUE(is_close(round(128.0 * waveform[2]), 128.0 * waveform[2], 1e-9));
  g_dither_vtable.free(filter);
}

TEST(test_flat) {
  double waveform[] = {-1.0, -0.5, -1.0 / 3.0, 0.0, 1.0 / 3.0, 0.5, 1.0};
  double waveform2[] = {-1.0, -0.5, -1.0 / 3.0, 0.0, 1.0 / 3.0, 0.5, 1.0};
  dither_config_t params = {.type = DITHER_TYPE_FLAT,
                            .bits = 8,
                            .amplitude = 2.0,
                            .has_amplitude = true};
  filter_config_t cfg = {.type = FILTER_TYPE_DITHER,
                         .parameters.dither = params};
  void* filter = g_dither_vtable.create("dither", &cfg, 0, 0, NULL, NULL);
  ASSERT_TRUE(filter != NULL);
  g_dither_vtable.process(filter, waveform, 7);

  ASSERT_TRUE(compare_waveforms(waveform, waveform2, 7, 1.0 / 64.0));
  ASSERT_TRUE(is_close(round(128.0 * waveform[2]), 128.0 * waveform[2], 1e-9));
  g_dither_vtable.free(filter);
}

TEST(test_high_pass) {
  double waveform[] = {-1.0, -0.5, -1.0 / 3.0, 0.0, 1.0 / 3.0, 0.5, 1.0};
  double waveform2[] = {-1.0, -0.5, -1.0 / 3.0, 0.0, 1.0 / 3.0, 0.5, 1.0};
  dither_config_t params = {.type = DITHER_TYPE_HIGHPASS, .bits = 8};
  filter_config_t cfg = {.type = FILTER_TYPE_DITHER,
                         .parameters.dither = params};
  void* filter = g_dither_vtable.create("dither", &cfg, 0, 0, NULL, NULL);
  ASSERT_TRUE(filter != NULL);
  g_dither_vtable.process(filter, waveform, 7);

  ASSERT_TRUE(compare_waveforms(waveform, waveform2, 7, 1.0 / 32.0));
  ASSERT_TRUE(is_close(round(128.0 * waveform[2]), 128.0 * waveform[2], 1e-9));
  g_dither_vtable.free(filter);
}

TEST(test_lip) {
  double waveform[] = {-1.0, -0.5, -1.0 / 3.0, 0.0, 1.0 / 3.0, 0.5, 1.0};
  double waveform2[] = {-1.0, -0.5, -1.0 / 3.0, 0.0, 1.0 / 3.0, 0.5, 1.0};
  dither_config_t params = {.type = DITHER_TYPE_LIPSHITZ_441, .bits = 8};
  filter_config_t cfg = {.type = FILTER_TYPE_DITHER,
                         .parameters.dither = params};
  void* filter = g_dither_vtable.create("dither", &cfg, 0, 0, NULL, NULL);
  ASSERT_TRUE(filter != NULL);
  g_dither_vtable.process(filter, waveform, 7);

  ASSERT_TRUE(compare_waveforms(waveform, waveform2, 7, 1.0 / 16.0));
  ASSERT_TRUE(is_close(round(128.0 * waveform[2]), 128.0 * waveform[2], 1e-9));
  g_dither_vtable.free(filter);
}

TEST(test_zero_amplitude) {
  double waveform[] = {0.5, -0.5, 0.25};
  dither_config_t params = {.type = DITHER_TYPE_FLAT,
                            .bits = 16,
                            .amplitude = 0.0,
                            .has_amplitude = true};
  filter_config_t cfg = {.type = FILTER_TYPE_DITHER,
                         .parameters.dither = params};
  void* filter = g_dither_vtable.create("dither", &cfg, 0, 0, NULL, NULL);
  ASSERT_TRUE(filter != NULL);
  g_dither_vtable.process(filter, waveform, 3);
  ASSERT_FALSE(isnan(waveform[0]));
  ASSERT_FALSE(isnan(waveform[1]));
  ASSERT_FALSE(isnan(waveform[2]));
  g_dither_vtable.free(filter);
}

TEST(test_noise_shaping_has_noise) {
  double waveform[100] = {0.0};
  dither_config_t params = {.type = DITHER_TYPE_FWEIGHTED_441, .bits = 8};
  filter_config_t cfg = {.type = FILTER_TYPE_DITHER,
                         .parameters.dither = params};
  void* filter =
      g_dither_vtable.create("dither_ns_test", &cfg, 0, 0, NULL, NULL);
  ASSERT_TRUE(filter != NULL);
  g_dither_vtable.process(filter, waveform, 100);

  bool found_nonzero = false;
  for (size_t i = 0; i < 100; i++) {
    if (waveform[i] != 0.0) {
      found_nonzero = true;
      break;
    }
  }
  ASSERT_TRUE(found_nonzero);
  g_dither_vtable.free(filter);
}

TEST_MAIN()
