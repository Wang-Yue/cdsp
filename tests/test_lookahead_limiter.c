#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "audio/audio_chunk.h"
#include "config/config_gen.h"
#include "filters/clipper.h"
#include "filters/filter.h"
#include "filters/lookahead_limiter.h"
#include "processors/processor.h"
#include "test_support.h"

static bool is_close(double left, double right, double maxdiff) {
  return fabs(left - right) < maxdiff;
}

static bool compare_waveforms(const double *left, const double *right,
                              size_t count, double maxdiff) {
  for (size_t i = 0; i < count; i++) {
    if (!is_close(left[i], right[i], maxdiff))
      return false;
  }
  return true;
}

TEST(test_lookahead_limiter_basic) {
  lookahead_limiter_filter_config_t params = {.limit = 0.0,
                                              .attack = 4.0,
                                              .attack_unit = TIME_UNIT_SAMPLES,
                                              .release = 1.0 / log(2.0),
                                              .release_unit =
                                                  TIME_UNIT_SAMPLES};
  filter_config_t cfg = {.type = FILTER_TYPE_LOOKAHEAD_LIMITER,
                         .parameters.lookahead_limiter = params};
  void *filter = g_lookahead_limiter_vtable.create("lookahead_limiter", &cfg,
                                                   48000, 1024, NULL, NULL);
  ASSERT_TRUE(filter != NULL);

  double input[] = {1.0, 1.0, 1.0, 1.0, 1.0, 2.0, -2.0, 1.0, 1.0, 2.0,
                    1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0,  1.0, 1.0};
  double expected[] = {0.0,
                       0.0,
                       0.0,
                       0.0,
                       1.0,
                       1.0,
                       0.875,
                       0.75,
                       0.625,
                       1.0,
                       -1.0,
                       pow(0.5, 1.0 / 2.0),
                       0.625,
                       1.0,
                       pow(0.5, 1.0 / 2.0),
                       pow(0.5, 1.0 / 4.0),
                       pow(0.5, 1.0 / 8.0),
                       pow(0.5, 1.0 / 16.0),
                       pow(0.5, 1.0 / 32.0)};

  g_lookahead_limiter_vtable.process(filter, input, 19);
  ASSERT_TRUE(compare_waveforms(input, expected, 19, 1e-6));
  g_lookahead_limiter_vtable.free(filter);
}

TEST(test_lookahead_limiter_same_as_limiter) {
  lookahead_limiter_filter_config_t params_lookahead = {
      .limit = 0.0,
      .attack = 0.0,
      .attack_unit = TIME_UNIT_SAMPLES,
      .release = 0.0,
      .release_unit = TIME_UNIT_SAMPLES};
  filter_config_t cfg_lookahead = {.type = FILTER_TYPE_LOOKAHEAD_LIMITER,
                                   .parameters.lookahead_limiter =
                                       params_lookahead};
  void *filter_lookahead = g_lookahead_limiter_vtable.create(
      "lookahead", &cfg_lookahead, 48000, 1024, NULL, NULL);
  ASSERT_TRUE(filter_lookahead != NULL);

  clipper_config_t params_clipper = {.clip_limit = 0.0, .soft_clip = false};
  filter_config_t cfg_clipper = {.type = FILTER_TYPE_CLIPPER,
                                 .parameters.clipper = params_clipper};
  void *filter_clipper =
      g_clipper_vtable.create("clipper", &cfg_clipper, 0, 0, NULL, NULL);
  ASSERT_TRUE(filter_clipper != NULL);

  double lookahead_input[] = {0.5, 1.0, 2.0, -2.0, -1.0, -0.5, 1.5, -1.5, 0.0};
  double limiter_input[] = {0.5, 1.0, 2.0, -2.0, -1.0, -0.5, 1.5, -1.5, 0.0};

  g_lookahead_limiter_vtable.process(filter_lookahead, lookahead_input, 9);
  g_clipper_vtable.process(filter_clipper, limiter_input, 9);

  for (size_t i = 0; i < 9; i++) {
    ASSERT_DOUBLE_EQ(limiter_input[i], lookahead_input[i]);
  }

  g_lookahead_limiter_vtable.free(filter_lookahead);
  g_clipper_vtable.free(filter_clipper);
}

TEST(test_lookahead_limiter_zero_attack_matches_compressor) {
  double release_samples = 4.0;
  int samplerate = 48000;

  double limiter_input[] = {2.0, 1.0, 1.0, 1.0, 1.0};
  size_t chunksize = 5;

  lookahead_limiter_filter_config_t params_limiter = {
      .limit = 0.0,
      .attack = 0.0,
      .attack_unit = TIME_UNIT_SAMPLES,
      .release = release_samples,
      .release_unit = TIME_UNIT_SAMPLES};
  filter_config_t cfg_limiter = {.type = FILTER_TYPE_LOOKAHEAD_LIMITER,
                                 .parameters.lookahead_limiter =
                                     params_limiter};
  void *limiter = g_lookahead_limiter_vtable.create(
      "limiter", &cfg_limiter, samplerate, chunksize, NULL, NULL);
  ASSERT_TRUE(limiter != NULL);

  compressor_config_t params_compressor = {.channels = 1,
                                           .monitor_channels = NULL,
                                           .monitor_channels_count = 0,
                                           .process_channels = NULL,
                                           .process_channels_count = 0,
                                           .attack = 1e-12,
                                           .attack_unit = TIME_UNIT_S,
                                           .release = release_samples /
                                                      (double)samplerate,
                                           .release_unit = TIME_UNIT_S,
                                           .threshold = 0.0,
                                           .factor = 1e20,
                                           .makeup_gain = 0.0,
                                           .has_makeup_gain = false,
                                           .soft_clip = false,
                                           .has_clip_limit = false};
  processor_config_t cfg_compressor = {.type = PROCESSOR_TYPE_COMPRESSOR,
                                       .parameters.compressor =
                                           params_compressor};
  dsp_processor_t *compressor = dsp_processor_create(
      "compressor", &cfg_compressor, samplerate, chunksize, NULL);
  ASSERT_TRUE(compressor != NULL);

  audio_chunk_t *compressor_chunk = audio_chunk_create(chunksize, 1);
  ASSERT_TRUE(compressor_chunk != NULL);
  double *comp_buf = audio_chunk_get_channel(compressor_chunk, 0);
  memcpy(comp_buf, limiter_input, chunksize * sizeof(double));
  audio_chunk_set_valid_frames(compressor_chunk, chunksize);

  g_lookahead_limiter_vtable.process(limiter, limiter_input, chunksize);
  dsp_processor_process(compressor, compressor_chunk);

  ASSERT_TRUE(compare_waveforms(limiter_input, comp_buf, chunksize, 1e-6));

  audio_chunk_free(compressor_chunk);
  dsp_processor_free(compressor);
  g_lookahead_limiter_vtable.free(limiter);
}

TEST(test_lookahead_limiter_zero_release) {
  lookahead_limiter_filter_config_t params = {.limit = 0.0,
                                              .attack = 2.0,
                                              .attack_unit = TIME_UNIT_SAMPLES,
                                              .release = 0.0,
                                              .release_unit =
                                                  TIME_UNIT_SAMPLES};
  filter_config_t cfg = {.type = FILTER_TYPE_LOOKAHEAD_LIMITER,
                         .parameters.lookahead_limiter = params};
  void *filter = g_lookahead_limiter_vtable.create("lookahead", &cfg, 48000,
                                                   1024, NULL, NULL);
  ASSERT_TRUE(filter != NULL);
  double input[] = {1.0, 1.0, 1.0, 2.0, 2.0, 2.0, 2.0, 2.0, 1.0, 1.0, 1.0};
  g_lookahead_limiter_vtable.process(filter, input, 11);
  for (size_t i = 0; i < 11; i++) {
    ASSERT_TRUE(fabs(input[i]) <= 1.0);
  }
  g_lookahead_limiter_vtable.free(filter);
}

TEST(test_lookahead_limiter_state_persistence) {
  lookahead_limiter_filter_config_t params = {.limit = 0.0,
                                              .attack = 5.0,
                                              .attack_unit = TIME_UNIT_SAMPLES,
                                              .release = 1.0 / log(2.0),
                                              .release_unit =
                                                  TIME_UNIT_SAMPLES};
  filter_config_t cfg = {.type = FILTER_TYPE_LOOKAHEAD_LIMITER,
                         .parameters.lookahead_limiter = params};
  void *filter = g_lookahead_limiter_vtable.create("lookahead", &cfg, 48000,
                                                   1024, NULL, NULL);
  ASSERT_TRUE(filter != NULL);

  double buf1[] = {1.0, 1.0, 1.0, 1.0, 1.0, 2.0, 1.0, 1.0, 1.0, 1.0, 1.0};
  double expected1[] = {0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.9, 0.8, 0.7, 0.6, 1.0};
  g_lookahead_limiter_vtable.process(filter, buf1, 11);
  ASSERT_TRUE(compare_waveforms(buf1, expected1, 11, 1e-6));

  double buf2[] = {1.0, 1.0, 1.0, 1.0};
  double expected2[] = {pow(0.5, 1.0 / 2.0), pow(0.5, 1.0 / 4.0),
                        pow(0.5, 1.0 / 8.0), pow(0.5, 1.0 / 16.0)};
  g_lookahead_limiter_vtable.process(filter, buf2, 4);
  ASSERT_TRUE(compare_waveforms(buf2, expected2, 4, 1e-6));

  g_lookahead_limiter_vtable.free(filter);
}

TEST(test_lookahead_limiter_attack_over_one_second_rejected) {
  lookahead_limiter_filter_config_t params = {.limit = 0.0,
                                              .attack = 48001.0,
                                              .attack_unit = TIME_UNIT_SAMPLES,
                                              .release = 4.0,
                                              .release_unit =
                                                  TIME_UNIT_SAMPLES};
  filter_config_t cfg = {.type = FILTER_TYPE_LOOKAHEAD_LIMITER,
                         .parameters.lookahead_limiter = params};
  ASSERT_NE(0, g_lookahead_limiter_vtable.validate(&cfg, 48000, NULL));
}

TEST(test_lookahead_limiter_chunksize_larger_than_samplerate) {
  lookahead_limiter_filter_config_t params = {.limit = 0.0,
                                              .attack = 4.0,
                                              .attack_unit = TIME_UNIT_SAMPLES,
                                              .release = 1.0,
                                              .release_unit =
                                                  TIME_UNIT_SAMPLES};
  filter_config_t cfg = {.type = FILTER_TYPE_LOOKAHEAD_LIMITER,
                         .parameters.lookahead_limiter = params};
  void *filter =
      g_lookahead_limiter_vtable.create("lookahead", &cfg, 4, 8, NULL, NULL);
  ASSERT_TRUE(filter != NULL);
  double input[] = {1.0, 1.0, 2.0, 1.0, 1.0, -2.0, 1.0, 1.0};
  g_lookahead_limiter_vtable.process(filter, input, 8);
  g_lookahead_limiter_vtable.free(filter);
}

// Upstream pads the lookahead history with `attack_samples` of silence whenever
// the parameters change (LookaheadGain::set_parameters). The samples still in
// the window were captured under the old attack time, so keeping them would
// both replay stale audio and duck the incoming chunk with a peak that no
// longer applies.
TEST(test_lookahead_limiter_transfer_state_flushes_lookahead) {
  lookahead_limiter_filter_config_t params = {.limit = 0.0,
                                              .attack = 4.0,
                                              .attack_unit = TIME_UNIT_SAMPLES,
                                              .release = 1.0,
                                              .release_unit =
                                                  TIME_UNIT_SAMPLES};
  filter_config_t cfg = {.type = FILTER_TYPE_LOOKAHEAD_LIMITER,
                         .parameters.lookahead_limiter = params};
  void *src = g_lookahead_limiter_vtable.create("limiter_src", &cfg, 48000, 32,
                                                NULL, NULL);
  void *dest_same = g_lookahead_limiter_vtable.create("limiter_dest_same", &cfg,
                                                      48000, 32, NULL, NULL);

  lookahead_limiter_filter_config_t params_changed = {
      .limit = -1.0,
      .attack = 4.0,
      .attack_unit = TIME_UNIT_SAMPLES,
      .release = 1.0,
      .release_unit = TIME_UNIT_SAMPLES};
  filter_config_t cfg_changed = {.type = FILTER_TYPE_LOOKAHEAD_LIMITER,
                                 .parameters.lookahead_limiter =
                                     params_changed};
  void *dest_changed = g_lookahead_limiter_vtable.create(
      "limiter_dest_changed", &cfg_changed, 48000, 32, NULL, NULL);
  ASSERT_TRUE(src != NULL);
  ASSERT_TRUE(dest_same != NULL);
  ASSERT_TRUE(dest_changed != NULL);

  // Quiet audio ending in a burst that is still inside the lookahead window
  // when the chunk returns: it has not been emitted or limited yet.
  double primed[19];
  for (size_t i = 0; i < 19; i++) {
    primed[i] = i >= 15 ? 4.0 : 0.5;
  }
  g_lookahead_limiter_vtable.process(src, primed, 19);

  // Transfer state to identical config (preserves history) and changed config
  // (flushes history)
  g_lookahead_limiter_vtable.transfer_state(dest_same, src);
  g_lookahead_limiter_vtable.transfer_state(dest_changed, src);

  double continued[19];
  double reloaded_same[19];
  double reloaded_changed[19];
  for (size_t i = 0; i < 19; i++) {
    continued[i] = 0.5;
    reloaded_same[i] = 0.5;
    reloaded_changed[i] = 0.5;
  }
  g_lookahead_limiter_vtable.process(src, continued, 19);
  g_lookahead_limiter_vtable.process(dest_same, reloaded_same, 19);
  g_lookahead_limiter_vtable.process(dest_changed, reloaded_changed, 19);

  // Without a reload the burst comes out of the window, limited to 0 dB, and
  // holds the gain down behind it.
  ASSERT_TRUE(is_close(continued[0], 1.0, 1e-9));

  // When config is identical, lookahead history is preserved across reloads
  // (M-1)
  for (size_t i = 0; i < 19; i++) {
    ASSERT_DOUBLE_EQ(continued[i], reloaded_same[i]);
  }

  // When parameters changed, the window was flushed with silence
  for (size_t i = 0; i < 4; i++) {
    ASSERT_DOUBLE_EQ(0.0, reloaded_changed[i]);
  }
  ASSERT_TRUE(reloaded_changed[4] > continued[4] + 0.1);

  g_lookahead_limiter_vtable.free(src);
  g_lookahead_limiter_vtable.free(dest_same);
  g_lookahead_limiter_vtable.free(dest_changed);
}

TEST_MAIN()
