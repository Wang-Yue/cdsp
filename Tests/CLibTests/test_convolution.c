#if defined(__linux__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif
#include <math.h>
#include <string.h>

#include "Config/filter_config_types.h"
#include "Filters/convolution.h"
#include "Filters/filter.h"
#include "test_support.h"

TEST(MovingAverage) {
  double coeffs[] = {0.5, 0.5};
  convolution_config_t params = {
      .type = CONV_TYPE_VALUES, .values = coeffs, .values_count = 2};
  filter_config_t cfg = {.type = FILTER_TYPE_CONV, .parameters.conv = params};
  void* filter = g_convolution_vtable.create("conv", &cfg, 0, 8, NULL, NULL);
  ASSERT_TRUE(filter != NULL);

  double wave[] = {1.0, 1.0, 1.0, 0.0, 0.0, -1.0, 0.0, 0.0};
  double expected[] = {0.5, 1.0, 1.0, 0.5, 0.0, -0.5, -0.5, 0.0};

  g_convolution_vtable.process(filter, wave, 8);
  for (size_t i = 0; i < 8; i++) {
    ASSERT_NEAR(expected[i], wave[i], 1e-7);
  }
  g_convolution_vtable.free(filter);
}

TEST(SegmentedConvolution) {
  double ir[32];
  for (int i = 0; i < 32; i++) ir[i] = (double)i;
  convolution_config_t params = {
      .type = CONV_TYPE_VALUES, .values = ir, .values_count = 32};
  filter_config_t cfg = {.type = FILTER_TYPE_CONV, .parameters.conv = params};
  void* filter = g_convolution_vtable.create("conv", &cfg, 0, 8, NULL, NULL);
  ASSERT_TRUE(filter != NULL);

  double impulse[8] = {1.0, 0, 0, 0, 0, 0, 0, 0};
  g_convolution_vtable.process(filter, impulse, 8);
  for (int i = 0; i < 8; i++) ASSERT_NEAR((double)i, impulse[i], 1e-5);

  double zeros[8] = {0};
  g_convolution_vtable.process(filter, zeros, 8);
  for (int i = 0; i < 8; i++) ASSERT_NEAR((double)(i + 8), zeros[i], 1e-5);

  memset(zeros, 0, sizeof(zeros));
  g_convolution_vtable.process(filter, zeros, 8);
  for (int i = 0; i < 8; i++) ASSERT_NEAR((double)(i + 16), zeros[i], 1e-5);

  memset(zeros, 0, sizeof(zeros));
  g_convolution_vtable.process(filter, zeros, 8);
  for (int i = 0; i < 8; i++) ASSERT_NEAR((double)(i + 24), zeros[i], 1e-5);

  memset(zeros, 0, sizeof(zeros));
  g_convolution_vtable.process(filter, zeros, 8);
  for (int i = 0; i < 8; i++) ASSERT_NEAR(0.0, zeros[i], 1e-5);

  g_convolution_vtable.free(filter);
}

TEST(IdentityConvolution) {
  double coeffs[] = {1.0};
  convolution_config_t params = {
      .type = CONV_TYPE_VALUES, .values = coeffs, .values_count = 1};
  filter_config_t cfg = {.type = FILTER_TYPE_CONV, .parameters.conv = params};
  void* filter = g_convolution_vtable.create("conv", &cfg, 0, 8, NULL, NULL);
  ASSERT_TRUE(filter != NULL);

  double wave[] = {1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  g_convolution_vtable.process(filter, wave, 8);
  ASSERT_NEAR(1.0, wave[0], 1e-7);
  for (int i = 1; i < 8; i++) ASSERT_NEAR(0.0, wave[i], 1e-7);
  g_convolution_vtable.free(filter);
}

TEST(DelayConvolution) {
  double coeffs[] = {0.0, 0.0, 0.0, 1.0};
  convolution_config_t params = {
      .type = CONV_TYPE_VALUES, .values = coeffs, .values_count = 4};
  filter_config_t cfg = {.type = FILTER_TYPE_CONV, .parameters.conv = params};
  void* filter = g_convolution_vtable.create("conv", &cfg, 0, 8, NULL, NULL);
  ASSERT_TRUE(filter != NULL);

  double wave[] = {1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  g_convolution_vtable.process(filter, wave, 8);
  ASSERT_NEAR(0.0, wave[0], 1e-7);
  ASSERT_NEAR(0.0, wave[1], 1e-7);
  ASSERT_NEAR(0.0, wave[2], 1e-7);
  ASSERT_NEAR(1.0, wave[3], 1e-7);
  for (int i = 4; i < 8; i++) ASSERT_NEAR(0.0, wave[i], 1e-7);
  g_convolution_vtable.free(filter);
}

TEST(ConvolutionWithSineWave) {
  double coeffs[] = {0.5, 0.5};
  convolution_config_t params = {
      .type = CONV_TYPE_VALUES, .values = coeffs, .values_count = 2};
  filter_config_t cfg = {.type = FILTER_TYPE_CONV, .parameters.conv = params};
  void* filter = g_convolution_vtable.create("conv", &cfg, 0, 64, NULL, NULL);
  ASSERT_TRUE(filter != NULL);

  double sample_rate = 48000.0;
  double freq = 100.0;
  double theta = 2.0 * M_PI * freq / sample_rate;
  double expected_gain = 0.5 * (1.0 + cos(theta));

  double wave[64];
  for (int chunk = 0; chunk < 8; chunk++) {
    int offset = chunk * 64;
    for (int i = 0; i < 64; i++) {
      wave[i] = cos(2.0 * M_PI * freq * (double)(offset + i) / sample_rate);
    }
    g_convolution_vtable.process(filter, wave, 64);
  }

  double peak = 0.0;
  for (int i = 0; i < 64; i++) {
    if (fabs(wave[i]) > peak) peak = fabs(wave[i]);
  }
  ASSERT_TRUE(fabs(peak - expected_gain) < expected_gain * 0.10);
  g_convolution_vtable.free(filter);
}

TEST(EmptyIRThrows) {
  convolution_config_t params = {
      .type = CONV_TYPE_VALUES, .values = NULL, .values_count = 0};
  filter_config_t cfg = {.type = FILTER_TYPE_CONV, .parameters.conv = params};
  void* filter = g_convolution_vtable.create("conv", &cfg, 0, 8, NULL, NULL);
  ASSERT_TRUE(filter == NULL);
}

TEST(DummyIsIdentity) {
  convolution_config_t params = {.type = CONV_TYPE_DUMMY, .length = 4};
  filter_config_t cfg = {.type = FILTER_TYPE_CONV, .parameters.conv = params};
  void* filter = g_convolution_vtable.create("conv", &cfg, 0, 8, NULL, NULL);
  ASSERT_TRUE(filter != NULL);

  double wave[] = {0.3, -0.2, 0.7, -0.1, 0.0, 0.5, -0.4, 0.9};
  double original[] = {0.3, -0.2, 0.7, -0.1, 0.0, 0.5, -0.4, 0.9};
  g_convolution_vtable.process(filter, wave, 8);

  for (int i = 0; i < 8; i++) {
    ASSERT_NEAR(original[i], wave[i], 1e-7);
  }
  g_convolution_vtable.free(filter);
}

TEST(CachedBuildSharesCoeffsButNotState) {
  double ir[] = {0.1, 0.2, 0.3, 0.4};
  convolution_config_t params = {
      .type = CONV_TYPE_VALUES, .values = ir, .values_count = 4};
  filter_config_t cfg = {.type = FILTER_TYPE_CONV, .parameters.conv = params};

  void* f1 = g_convolution_vtable.create("shared_conv", &cfg, 0, 8, NULL, NULL);
  void* f2 = g_convolution_vtable.create("shared_conv", &cfg, 0, 8, NULL, NULL);
  ASSERT_TRUE(f1 != NULL);
  ASSERT_TRUE(f2 != NULL);

  double wave1[8] = {1.0, 0, 0, 0, 0, 0, 0, 0};
  double wave2[8] = {0, 1.0, 0, 0, 0, 0, 0, 0};

  g_convolution_vtable.process(f1, wave1, 8);
  g_convolution_vtable.process(f2, wave2, 8);

  // wave1 should have impulse response at [0..3]
  ASSERT_NEAR(0.1, wave1[0], 1e-7);
  ASSERT_NEAR(0.2, wave1[1], 1e-7);
  ASSERT_NEAR(0.3, wave1[2], 1e-7);
  ASSERT_NEAR(0.4, wave1[3], 1e-7);

  // wave2 should have impulse response at [1..4]
  ASSERT_NEAR(0.0, wave2[0], 1e-7);
  ASSERT_NEAR(0.1, wave2[1], 1e-7);
  ASSERT_NEAR(0.2, wave2[2], 1e-7);
  ASSERT_NEAR(0.3, wave2[3], 1e-7);
  ASSERT_NEAR(0.4, wave2[4], 1e-7);

  g_convolution_vtable.free(f1);
  g_convolution_vtable.free(f2);
}

TEST(CacheDoesNotShareAcrossLengths) {
  double ir[] = {0.1, 0.2, 0.3, 0.4};
  convolution_config_t params = {
      .type = CONV_TYPE_VALUES, .values = ir, .values_count = 4};
  filter_config_t cfg = {.type = FILTER_TYPE_CONV, .parameters.conv = params};

  void* short_f = g_convolution_vtable.create("size_conv", &cfg, 0, 8, NULL, NULL);
  void* long_f = g_convolution_vtable.create("size_conv", &cfg, 0, 16, NULL, NULL);
  ASSERT_TRUE(short_f != NULL);
  ASSERT_TRUE(long_f != NULL);

  double short_wave[8] = {1.0, 0, 0, 0, 0, 0, 0, 0};
  double long_wave[16] = {1.0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

  g_convolution_vtable.process(short_f, short_wave, 8);
  g_convolution_vtable.process(long_f, long_wave, 16);

  for (size_t i = 0; i < 4; i++) {
    ASSERT_NEAR(ir[i], short_wave[i], 1e-7);
    ASSERT_NEAR(ir[i], long_wave[i], 1e-7);
  }

  g_convolution_vtable.free(short_f);
  g_convolution_vtable.free(long_f);
}

TEST_MAIN()
