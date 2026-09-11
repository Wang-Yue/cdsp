#include <math.h>
#include <stdbool.h>
#include <stddef.h>

#include "Config/filter_config_types.h"
#include "Filters/delay.h"
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

TEST(delay_small) {
  double waveform[] = {0.0, -0.5, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  double waveform_delayed[] = {0.0, 0.0, 0.0, 0.0, -0.5, 1.0, 0.0, 0.0};
  delay_config_t params = {
      .delay = 3.0, .delay_unit = DELAY_UNIT_SAMPLES, .subsample = false};
  filter_config_t cfg = {.type = FILTER_TYPE_DELAY, .parameters.delay = params};
  void* filter = g_delay_vtable.create("delay", &cfg, 44100, 0, NULL, NULL);
  ASSERT_TRUE(filter != NULL);
  g_delay_vtable.process(filter, waveform, 8);
  for (size_t i = 0; i < 8; i++) {
    ASSERT_DOUBLE_EQ(waveform_delayed[i], waveform[i]);
  }
  g_delay_vtable.free(filter);
}

TEST(delay_supersmall) {
  double waveform[] = {0.0, -0.5, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  double waveform_delayed[] = {0.0, -0.5, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  delay_config_t params = {
      .delay = 0.1, .delay_unit = DELAY_UNIT_SAMPLES, .subsample = false};
  filter_config_t cfg = {.type = FILTER_TYPE_DELAY, .parameters.delay = params};
  void* filter = g_delay_vtable.create("delay", &cfg, 44100, 0, NULL, NULL);
  ASSERT_TRUE(filter != NULL);
  g_delay_vtable.process(filter, waveform, 8);
  for (size_t i = 0; i < 8; i++) {
    ASSERT_DOUBLE_EQ(waveform_delayed[i], waveform[i]);
  }
  g_delay_vtable.free(filter);
}

TEST(delay_large) {
  double waveform1[] = {0.0, -0.5, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  double waveform2[] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  double waveform_delayed[] = {0.0, 0.0, -0.5, 1.0, 0.0, 0.0, 0.0, 0.0};
  delay_config_t params = {
      .delay = 9.0, .delay_unit = DELAY_UNIT_SAMPLES, .subsample = false};
  filter_config_t cfg = {.type = FILTER_TYPE_DELAY, .parameters.delay = params};
  void* filter = g_delay_vtable.create("delay", &cfg, 44100, 0, NULL, NULL);
  ASSERT_TRUE(filter != NULL);
  g_delay_vtable.process(filter, waveform1, 8);
  g_delay_vtable.process(filter, waveform2, 8);
  for (size_t i = 0; i < 8; i++) {
    ASSERT_DOUBLE_EQ(0.0, waveform1[i]);
    ASSERT_DOUBLE_EQ(waveform_delayed[i], waveform2[i]);
  }
  g_delay_vtable.free(filter);
}

TEST(delay_fraction) {
  double waveform[] = {0.0, -0.5, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  double expected_waveform[] = {
      0.0,
      0.01051051051051051,
      -0.13446780113446782,
      -0.2476751025299573,
      1.0522122611990257,
      -0.23903133046978262,
      0.07523664949897024,
      -0.021743938066703532,
      0.006413537427714274,
      -0.001882310318672015,
  };
  delay_config_t params = {
      .delay = 1.7, .delay_unit = DELAY_UNIT_SAMPLES, .subsample = true};
  filter_config_t cfg = {.type = FILTER_TYPE_DELAY, .parameters.delay = params};
  void* filter = g_delay_vtable.create("delay", &cfg, 44100, 0, NULL, NULL);
  ASSERT_TRUE(filter != NULL);
  g_delay_vtable.process(filter, waveform, 10);
  ASSERT_TRUE(compare_waveforms(waveform, expected_waveform, 10, 1.0e-6));
  g_delay_vtable.free(filter);
}

// A reload that leaves the delay length alone must replay the queued samples
// exactly, otherwise every config change would punch a hole in the audio.
TEST(delay_transfer_state_carries_queue_for_equal_length) {
  delay_config_t params = {
      .delay = 3.0, .delay_unit = DELAY_UNIT_SAMPLES, .subsample = false};
  filter_config_t cfg = {.type = FILTER_TYPE_DELAY, .parameters.delay = params};
  void* src = g_delay_vtable.create("delay_src", &cfg, 44100, 0, NULL, NULL);
  void* dest = g_delay_vtable.create("delay_dest", &cfg, 44100, 0, NULL, NULL);
  ASSERT_TRUE(src != NULL);
  ASSERT_TRUE(dest != NULL);

  double primed[] = {1.0, 2.0, 3.0};
  g_delay_vtable.process(src, primed, 3);

  g_delay_vtable.transfer_state(dest, src);

  double silence[] = {0.0, 0.0, 0.0};
  g_delay_vtable.process(dest, silence, 3);
  ASSERT_DOUBLE_EQ(1.0, silence[0]);
  ASSERT_DOUBLE_EQ(2.0, silence[1]);
  ASSERT_DOUBLE_EQ(3.0, silence[2]);

  g_delay_vtable.free(src);
  g_delay_vtable.free(dest);
}

// Resize invariant: the queue is indexed purely by age, so a different delay
// length would replay every retained sample at the wrong time. Carry nothing.
TEST(delay_transfer_state_drops_queue_when_length_changes) {
  delay_config_t short_params = {
      .delay = 3.0, .delay_unit = DELAY_UNIT_SAMPLES, .subsample = false};
  delay_config_t long_params = {
      .delay = 5.0, .delay_unit = DELAY_UNIT_SAMPLES, .subsample = false};
  filter_config_t short_cfg = {.type = FILTER_TYPE_DELAY,
                               .parameters.delay = short_params};
  filter_config_t long_cfg = {.type = FILTER_TYPE_DELAY,
                              .parameters.delay = long_params};
  void* src =
      g_delay_vtable.create("delay_src", &short_cfg, 44100, 0, NULL, NULL);
  void* dest =
      g_delay_vtable.create("delay_dest", &long_cfg, 44100, 0, NULL, NULL);
  ASSERT_TRUE(src != NULL);
  ASSERT_TRUE(dest != NULL);

  double primed[] = {1.0, 2.0, 3.0};
  g_delay_vtable.process(src, primed, 3);

  g_delay_vtable.transfer_state(dest, src);

  double silence[] = {0.0, 0.0, 0.0, 0.0, 0.0};
  g_delay_vtable.process(dest, silence, 5);
  for (size_t i = 0; i < 5; i++) {
    ASSERT_DOUBLE_EQ(0.0, silence[i]);
  }

  g_delay_vtable.free(src);
  g_delay_vtable.free(dest);
}

TEST_MAIN()
