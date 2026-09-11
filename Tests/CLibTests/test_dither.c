#include <math.h>
#include <stdbool.h>
#include <stddef.h>

#include "Config/filter_config_types.h"
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

TEST(test_channel_correlation) {
  double waveform1[100] = {0.0};
  double waveform2[100] = {0.0};
  dither_config_t params = {.type = DITHER_TYPE_FLAT,
                            .bits = 16,
                            .amplitude = 2.0,
                            .has_amplitude = true};
  filter_config_t cfg = {.type = FILTER_TYPE_DITHER,
                         .parameters.dither = params};
  void* filter1 = g_dither_vtable.create("dither", &cfg, 0, 0, NULL, NULL);
  void* filter2 = g_dither_vtable.create("dither", &cfg, 0, 0, NULL, NULL);
  ASSERT_TRUE(filter1 != NULL);
  ASSERT_TRUE(filter2 != NULL);

  g_dither_vtable.process(filter1, waveform1, 100);
  g_dither_vtable.process(filter2, waveform2, 100);

  bool identical = true;
  for (size_t i = 0; i < 100; i++) {
    if (waveform1[i] != waveform2[i]) {
      identical = false;
      break;
    }
  }
  // The two dither sequences should be uncorrelated / not identical!
  ASSERT_FALSE(identical);

  g_dither_vtable.free(filter1);
  g_dither_vtable.free(filter2);
}

// An identical reload must be inaudible: the RNG position and the error
// feedback history both carry, so the reloaded filter continues the exact
// sequence the old one would have produced.
TEST(dither_transfer_state_carries_shaper_for_identical_config) {
  dither_config_t params = {.type = DITHER_TYPE_SHIBATA_441, .bits = 16};
  filter_config_t cfg = {.type = FILTER_TYPE_DITHER,
                         .parameters.dither = params};
  void* src = g_dither_vtable.create("dither_src", &cfg, 0, 0, NULL, NULL);
  void* dest = g_dither_vtable.create("dither_dest", &cfg, 0, 0, NULL, NULL);
  ASSERT_TRUE(src != NULL);
  ASSERT_TRUE(dest != NULL);

  double primed[64];
  for (size_t i = 0; i < 64; i++) {
    primed[i] = 0.5 * sin(0.11 * (double)i);
  }
  g_dither_vtable.process(src, primed, 64);

  g_dither_vtable.transfer_state(dest, src);

  double continued[32];
  double reloaded[32];
  for (size_t i = 0; i < 32; i++) {
    continued[i] = 0.25 * sin(0.07 * (double)i);
    reloaded[i] = continued[i];
  }
  g_dither_vtable.process(src, continued, 32);
  g_dither_vtable.process(dest, reloaded, 32);

  for (size_t i = 0; i < 32; i++) {
    ASSERT_DOUBLE_EQ(continued[i], reloaded[i]);
  }

  g_dither_vtable.free(src);
  g_dither_vtable.free(dest);
}

// Resize invariant: the shaper buffer holds quantization errors measured in LSB
// units of one bit depth, so a reload that changes the depth must drop them.
// The RNG position still crosses, which is what makes this observable: the
// reference below is given the same RNG position but a shaper that was never
// charged, by routing it through a flat dither that has no shaper at all.
TEST(dither_transfer_state_drops_shaper_when_scale_changes) {
  dither_config_t params16 = {.type = DITHER_TYPE_SHIBATA_441, .bits = 16};
  dither_config_t params24 = {.type = DITHER_TYPE_SHIBATA_441, .bits = 24};
  dither_config_t params_flat = {.type = DITHER_TYPE_FLAT,
                                 .bits = 16,
                                 .amplitude = 2.0,
                                 .has_amplitude = true};
  filter_config_t cfg16 = {.type = FILTER_TYPE_DITHER,
                           .parameters.dither = params16};
  filter_config_t cfg24 = {.type = FILTER_TYPE_DITHER,
                           .parameters.dither = params24};
  filter_config_t cfg_flat = {.type = FILTER_TYPE_DITHER,
                              .parameters.dither = params_flat};

  void* src = g_dither_vtable.create("dither_src", &cfg16, 0, 0, NULL, NULL);
  ASSERT_TRUE(src != NULL);
  double primed[64];
  for (size_t i = 0; i < 64; i++) {
    primed[i] = 0.5 * sin(0.11 * (double)i);
  }
  g_dither_vtable.process(src, primed, 64);

  // Crosses a bit-depth change, so only the RNG position may be carried.
  void* dest = g_dither_vtable.create("dither_dest", &cfg24, 0, 0, NULL, NULL);
  ASSERT_TRUE(dest != NULL);
  g_dither_vtable.transfer_state(dest, src);

  // Same RNG position, reached without ever touching a charged shaper: the
  // relay has no shaper, so there is nothing for any policy to copy.
  void* relay =
      g_dither_vtable.create("dither_relay", &cfg_flat, 0, 0, NULL, NULL);
  void* reference =
      g_dither_vtable.create("dither_ref", &cfg24, 0, 0, NULL, NULL);
  ASSERT_TRUE(relay != NULL);
  ASSERT_TRUE(reference != NULL);
  g_dither_vtable.transfer_state(relay, src);
  g_dither_vtable.transfer_state(reference, relay);

  double from_charged[32];
  double from_fresh[32];
  for (size_t i = 0; i < 32; i++) {
    from_charged[i] = 0.25 * sin(0.07 * (double)i);
    from_fresh[i] = from_charged[i];
  }
  g_dither_vtable.process(dest, from_charged, 32);
  g_dither_vtable.process(reference, from_fresh, 32);

  for (size_t i = 0; i < 32; i++) {
    ASSERT_DOUBLE_EQ(from_fresh[i], from_charged[i]);
  }

  g_dither_vtable.free(src);
  g_dither_vtable.free(dest);
  g_dither_vtable.free(relay);
  g_dither_vtable.free(reference);
}

TEST_MAIN()
