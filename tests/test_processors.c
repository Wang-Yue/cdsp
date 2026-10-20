#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "audio/audio_chunk.h"
#include "config/config_error.h"
#include "config/config_gen.h"
#include "filters/filter.h"
#include "filters/lookahead_limiter.h"
#include "processors/processor.h"
#include "test_support.h"

static bool is_close(double left, double right, double maxdiff) {
  return fabs(left - right) < maxdiff;
}

TEST(compressor_basic_compression) {
  size_t mon_ch[] = {0};
  size_t proc_ch[] = {0, 1};
  compressor_config_t params = {0};
  params.channels = 2;
  params.monitor_channels = mon_ch;
  params.monitor_channels_count = 1;
  params.process_channels = proc_ch;
  params.process_channels_count = 2;
  params.attack = 0.002;
  params.release = 0.1;
  params.threshold = -6.02;
  params.factor = 2.0;
  params.makeup_gain = 0.0;
  params.has_makeup_gain = true;
  params.soft_clip = false;
  params.has_clip_limit = false;

  processor_config_t config = {.type = PROCESSOR_TYPE_COMPRESSOR,
                               .parameters.compressor = params};

  dsp_processor_t *comp =
      dsp_processor_create("compressor", &config, 48000, 1000, NULL);
  ASSERT_TRUE(comp != NULL);

  audio_chunk_t *chunk = audio_chunk_create(1000, 2);
  ASSERT_TRUE(chunk != NULL);
  double *ch0 = audio_chunk_get_channel(chunk, 0);
  double *ch1 = audio_chunk_get_channel(chunk, 1);
  for (size_t i = 0; i < 1000; i++) {
    ch0[i] = 1.0;
    ch1[i] = 0.5;
  }
  audio_chunk_set_valid_frames(chunk, 1000);

  dsp_processor_process(comp, chunk);

  ASSERT_TRUE(ch0[999] < 0.8);
  ASSERT_TRUE(ch1[999] < 0.4);

  audio_chunk_free(chunk);
  dsp_processor_free(comp);
}

// Upstream puts no constraint on `factor`, and a value below 1.0 makes the
// curve -(excess * (factor - 1) / factor) positive above the threshold, i.e.
// upward expansion. The port used to reject the config outright.
TEST(compressor_upward_expansion_factor_below_one) {
  size_t mon_ch[] = {0};
  size_t proc_ch[] = {0};
  compressor_config_t params = {0};
  params.channels = 1;
  params.monitor_channels = mon_ch;
  params.monitor_channels_count = 1;
  params.process_channels = proc_ch;
  params.process_channels_count = 1;
  params.attack = 0.0001;
  params.release = 0.0001;
  params.threshold = -20.0;
  params.factor = 0.5;
  params.makeup_gain = 0.0;
  params.has_makeup_gain = true;
  params.soft_clip = false;
  params.has_clip_limit = false;

  processor_config_t config = {.type = PROCESSOR_TYPE_COMPRESSOR,
                               .parameters.compressor = params};

  config_error_t err;
  config_error_init(&err);
  dsp_processor_t *comp =
      dsp_processor_create("expander", &config, 48000, 1000, &err);
  ASSERT_TRUE(comp != NULL);
  ASSERT_EQ(CONFIG_ERR_NONE, err.type);

  audio_chunk_t *chunk = audio_chunk_create(1000, 1);
  ASSERT_TRUE(chunk != NULL);
  double *ch0 = audio_chunk_get_channel(chunk, 0);
  // -6 dB, well above the -20 dB threshold, so the expander should boost it.
  for (size_t i = 0; i < 1000; i++)
    ch0[i] = 0.5;
  audio_chunk_set_valid_frames(chunk, 1000);

  dsp_processor_process(comp, chunk);

  ASSERT_TRUE(ch0[999] > 0.5);

  audio_chunk_free(chunk);
  dsp_processor_free(comp);
}

// factor == 0 divides by zero and pins everything above the threshold to full
// scale. Upstream would do the same; the port rejects it as a deliberate
// one-value safety divergence.
TEST(compressor_rejects_zero_factor) {
  size_t mon_ch[] = {0};
  size_t proc_ch[] = {0};
  compressor_config_t params = {0};
  params.channels = 1;
  params.monitor_channels = mon_ch;
  params.monitor_channels_count = 1;
  params.process_channels = proc_ch;
  params.process_channels_count = 1;
  params.attack = 0.0001;
  params.release = 0.0001;
  params.threshold = -20.0;
  params.factor = 0.0;
  params.has_makeup_gain = true;

  processor_config_t config = {.type = PROCESSOR_TYPE_COMPRESSOR,
                               .parameters.compressor = params};

  config_error_t err;
  config_error_init(&err);
  ASSERT_TRUE(dsp_processor_create("bad", &config, 48000, 1000, &err) == NULL);
  ASSERT_EQ(CONFIG_ERR_INVALID_PROCESSOR, err.type);
}

TEST(noisegate_basic_gate) {
  size_t mon_ch[] = {0};
  size_t proc_ch[] = {0};
  noise_gate_config_t params = {0};
  params.channels = 1;
  params.monitor_channels = mon_ch;
  params.monitor_channels_count = 1;
  params.process_channels = proc_ch;
  params.process_channels_count = 1;
  params.attack = 0.0001;
  params.release = 0.0001;
  params.threshold = -20.0;
  params.attenuation = 40.0;

  processor_config_t config = {.type = PROCESSOR_TYPE_NOISE_GATE,
                               .parameters.noise_gate = params};

  dsp_processor_t *gate =
      dsp_processor_create("noisegate", &config, 48000, 100, NULL);
  ASSERT_TRUE(gate != NULL);

  audio_chunk_t *chunk = audio_chunk_create(100, 1);
  ASSERT_TRUE(chunk != NULL);
  double *ch0 = audio_chunk_get_channel(chunk, 0);
  for (size_t i = 0; i < 100; i++) {
    if (i >= 20 && i < 40) {
      ch0[i] = 0.5;
    } else {
      ch0[i] = 0.001;
    }
  }
  audio_chunk_set_valid_frames(chunk, 100);

  dsp_processor_process(gate, chunk);

  ASSERT_TRUE(ch0[35] > 0.4);
  ASSERT_TRUE(ch0[60] < 0.00005);

  audio_chunk_free(chunk);
  dsp_processor_free(gate);
}

TEST(race_basic) {
  race_config_t params = {0};
  params.channels = 2;
  params.channel_a = 0;
  params.channel_b = 1;
  params.delay = 5.0;
  params.subsample_delay = false;
  params.has_subsample_delay = true;
  params.delay_unit = DELAY_UNIT_SAMPLES;
  params.attenuation = 6.02;

  processor_config_t config = {.type = PROCESSOR_TYPE_RACE,
                               .parameters.race = params};

  dsp_processor_t *race = dsp_processor_create("race", &config, 48000, 0, NULL);
  ASSERT_TRUE(race != NULL);

  audio_chunk_t *chunk = audio_chunk_create(10, 2);
  ASSERT_TRUE(chunk != NULL);
  double *ch0 = audio_chunk_get_channel(chunk, 0);
  double *ch1 = audio_chunk_get_channel(chunk, 1);
  ch0[0] = 1.0;
  for (size_t i = 1; i < 10; i++) {
    ch0[i] = 0.0;
    ch1[i] = 0.0;
  }
  audio_chunk_set_valid_frames(chunk, 10);

  dsp_processor_process(race, chunk);

  ASSERT_DOUBLE_EQ(1.0, ch0[0]);
  ASSERT_TRUE(is_close(ch1[5], -0.5, 1e-4));

  audio_chunk_free(chunk);
  dsp_processor_free(race);
}

TEST(race_transfer_state) {
  race_config_t params = {0};
  params.channels = 2;
  params.channel_a = 0;
  params.channel_b = 1;
  params.delay = 5.0;
  params.subsample_delay = false;
  params.has_subsample_delay = true;
  params.delay_unit = DELAY_UNIT_SAMPLES;
  params.attenuation = 6.02;

  processor_config_t config = {.type = PROCESSOR_TYPE_RACE,
                               .parameters.race = params};

  dsp_processor_t *race1 =
      dsp_processor_create("race1", &config, 48000, 0, NULL);
  dsp_processor_t *race2 =
      dsp_processor_create("race2", &config, 48000, 0, NULL);
  ASSERT_TRUE(race1 != NULL);
  ASSERT_TRUE(race2 != NULL);

  // Send an impulse to race1 to populate state
  audio_chunk_t *chunk_init = audio_chunk_create(10, 2);
  double *ch0_init = audio_chunk_get_channel(chunk_init, 0);
  double *ch1_init = audio_chunk_get_channel(chunk_init, 1);
  ch0_init[0] = 1.0;
  for (size_t i = 1; i < 10; i++) {
    ch0_init[i] = 0.0;
    ch1_init[i] = 0.0;
  }
  audio_chunk_set_valid_frames(chunk_init, 10);
  dsp_processor_process(race1, chunk_init);
  audio_chunk_free(chunk_init);

  // Transfer state from race1 to race2
  dsp_processor_transfer_state(race2, race1);

  // Now process a second block with both and make sure they produce exactly the
  // same results
  audio_chunk_t *chunk1 = audio_chunk_create(10, 2);
  audio_chunk_t *chunk2 = audio_chunk_create(10, 2);

  double *ch0_1 = audio_chunk_get_channel(chunk1, 0);
  double *ch1_1 = audio_chunk_get_channel(chunk1, 1);
  double *ch0_2 = audio_chunk_get_channel(chunk2, 0);
  double *ch1_2 = audio_chunk_get_channel(chunk2, 1);

  for (size_t i = 0; i < 10; i++) {
    ch0_1[i] = 0.25 * (i + 1);
    ch1_1[i] = -0.1 * (i + 1);
    ch0_2[i] = 0.25 * (i + 1);
    ch1_2[i] = -0.1 * (i + 1);
  }
  audio_chunk_set_valid_frames(chunk1, 10);
  audio_chunk_set_valid_frames(chunk2, 10);

  dsp_processor_process(race1, chunk1);
  dsp_processor_process(race2, chunk2);

  for (size_t i = 0; i < 10; i++) {
    ASSERT_DOUBLE_EQ(ch0_1[i], ch0_2[i]);
    ASSERT_DOUBLE_EQ(ch1_1[i], ch1_2[i]);
  }

  audio_chunk_free(chunk1);
  audio_chunk_free(chunk2);
  dsp_processor_free(race1);
  dsp_processor_free(race2);
}

TEST(lookahead_limiter_processor_basic) {
  size_t mon_ch[] = {0};
  size_t proc_ch[] = {0, 1};
  lookahead_limiter_processor_config_t params = {0};
  params.channels = 2;
  params.monitor_channels = mon_ch;
  params.monitor_channels_count = 1;
  params.process_channels = proc_ch;
  params.process_channels_count = 2;
  params.limit =
      -6.020599913279624; // -6.02... dB corresponds to ~0.5 linear limit
  params.attack = 2.0;    // 2 ms
  params.attack_unit = TIME_UNIT_MS;
  params.release = 10.0; // 10 ms
  params.release_unit = TIME_UNIT_MS;
  params.delay_processed_only = false;

  processor_config_t config = {.type = PROCESSOR_TYPE_LOOKAHEAD_LIMITER,
                               .parameters.lookahead_limiter = params};

  // 48000 Hz, 2 ms attack = 96 samples delay
  dsp_processor_t *limiter =
      dsp_processor_create("limiter", &config, 48000, 1000, NULL);
  ASSERT_TRUE(limiter != NULL);

  audio_chunk_t *chunk = audio_chunk_create(1000, 2);
  ASSERT_TRUE(chunk != NULL);
  double *ch0 = audio_chunk_get_channel(chunk, 0);
  double *ch1 = audio_chunk_get_channel(chunk, 1);

  // Initialize with signal
  for (size_t i = 0; i < 1000; i++) {
    ch0[i] = 0.2;
    ch1[i] = 0.2;
  }
  // Peak at index 200 on ch0 (monitored)
  ch0[200] = 2.0;

  audio_chunk_set_valid_frames(chunk, 1000);
  dsp_processor_process(limiter, chunk);

  // The peak at 200 should be delayed by 96 samples (to index 296) and limited
  // to <= 0.5
  ASSERT_TRUE(ch0[296] <= 0.501);
  ASSERT_TRUE(ch1[296] <= 0.501);

  // The original peak index (200) should be delayed and not contain the peak
  ASSERT_TRUE(ch0[200] < 0.3);

  audio_chunk_free(chunk);
  dsp_processor_free(limiter);
}

TEST(lookahead_limiter_processor_transfer_state) {
  size_t mon_ch[] = {0};
  size_t proc_ch[] = {0, 1};
  lookahead_limiter_processor_config_t params = {0};
  params.channels = 2;
  params.monitor_channels = mon_ch;
  params.monitor_channels_count = 1;
  params.process_channels = proc_ch;
  params.process_channels_count = 2;
  params.limit = -6.020599913279624;
  params.attack = 2.0;
  params.attack_unit = TIME_UNIT_MS;
  params.release = 10.0;
  params.release_unit = TIME_UNIT_MS;
  params.delay_processed_only = false;

  processor_config_t config = {.type = PROCESSOR_TYPE_LOOKAHEAD_LIMITER,
                               .parameters.lookahead_limiter = params};

  dsp_processor_t *limiter1 =
      dsp_processor_create("limiter1", &config, 48000, 1000, NULL);
  dsp_processor_t *limiter2 =
      dsp_processor_create("limiter2", &config, 48000, 1000, NULL);
  ASSERT_TRUE(limiter1 != NULL);
  ASSERT_TRUE(limiter2 != NULL);

  // Send peak to limiter1 to populate lookahead history
  audio_chunk_t *chunk_init = audio_chunk_create(1000, 2);
  double *ch0_init = audio_chunk_get_channel(chunk_init, 0);
  double *ch1_init = audio_chunk_get_channel(chunk_init, 1);
  for (size_t i = 0; i < 1000; i++) {
    ch0_init[i] = 0.1;
    ch1_init[i] = 0.1;
  }
  ch0_init[900] = 2.0; // Peak close to the end
  audio_chunk_set_valid_frames(chunk_init, 1000);
  dsp_processor_process(limiter1, chunk_init);
  audio_chunk_free(chunk_init);

  // Transfer state
  dsp_processor_transfer_state(limiter2, limiter1);

  // Verify that both processors produce identical outputs on the next chunk
  audio_chunk_t *chunk1 = audio_chunk_create(500, 2);
  audio_chunk_t *chunk2 = audio_chunk_create(500, 2);
  double *ch0_1 = audio_chunk_get_channel(chunk1, 0);
  double *ch1_1 = audio_chunk_get_channel(chunk1, 1);
  double *ch0_2 = audio_chunk_get_channel(chunk2, 0);
  double *ch1_2 = audio_chunk_get_channel(chunk2, 1);

  for (size_t i = 0; i < 500; i++) {
    ch0_1[i] = 0.3;
    ch1_1[i] = 0.3;
    ch0_2[i] = 0.3;
    ch1_2[i] = 0.3;
  }
  audio_chunk_set_valid_frames(chunk1, 500);
  audio_chunk_set_valid_frames(chunk2, 500);

  dsp_processor_process(limiter1, chunk1);
  dsp_processor_process(limiter2, chunk2);

  for (size_t i = 0; i < 500; i++) {
    ASSERT_DOUBLE_EQ(ch0_1[i], ch0_2[i]);
    ASSERT_DOUBLE_EQ(ch1_1[i], ch1_2[i]);
  }

  audio_chunk_free(chunk1);
  audio_chunk_free(chunk2);
  dsp_processor_free(limiter1);
  dsp_processor_free(limiter2);
}

TEST(test_lookahead_limiter_processor_matches_filter) {
  int samplerate = 48000;
  double input[] = {1.0, 1.0, 1.0, 1.0, 1.0, 2.0, -2.0, 1.0, 1.0, 2.0,
                    1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0,  1.0, 1.0};
  size_t chunksize = 19;

  // Filter
  lookahead_limiter_filter_config_t params_filter = {
      .limit = 0.0,
      .attack = 4.0,
      .attack_unit = TIME_UNIT_SAMPLES,
      .release = 1.0 / log(2.0),
      .release_unit = TIME_UNIT_SAMPLES};
  filter_config_t cfg_filter = {.type = FILTER_TYPE_LOOKAHEAD_LIMITER,
                                .parameters.lookahead_limiter = params_filter};
  void *filter = g_lookahead_limiter_vtable.create(
      "filter", &cfg_filter, samplerate, chunksize, NULL, NULL);
  ASSERT_TRUE(filter != NULL);

  // Processor
  lookahead_limiter_processor_config_t params_proc = {
      .channels = 1,
      .monitor_channels = NULL,
      .monitor_channels_count = 0,
      .process_channels = NULL,
      .process_channels_count = 0,
      .limit = 0.0,
      .attack = 4.0,
      .attack_unit = TIME_UNIT_SAMPLES,
      .release = 1.0 / log(2.0),
      .release_unit = TIME_UNIT_SAMPLES,
      .delay_processed_only = false};
  processor_config_t cfg_proc = {.type = PROCESSOR_TYPE_LOOKAHEAD_LIMITER,
                                 .parameters.lookahead_limiter = params_proc};
  dsp_processor_t *processor =
      dsp_processor_create("processor", &cfg_proc, samplerate, chunksize, NULL);
  ASSERT_TRUE(processor != NULL);

  audio_chunk_t *processor_chunk = audio_chunk_create(chunksize, 1);
  ASSERT_TRUE(processor_chunk != NULL);
  double *proc_buf = audio_chunk_get_channel(processor_chunk, 0);
  memcpy(proc_buf, input, chunksize * sizeof(double));
  audio_chunk_set_valid_frames(processor_chunk, chunksize);

  g_lookahead_limiter_vtable.process(filter, input, chunksize);
  dsp_processor_process(processor, processor_chunk);

  for (size_t i = 0; i < chunksize; i++) {
    ASSERT_TRUE(is_close(proc_buf[i], input[i], 1e-12));
  }

  audio_chunk_free(processor_chunk);
  dsp_processor_free(processor);
  g_lookahead_limiter_vtable.free(filter);
}

TEST(test_lookahead_limiter_processor_uses_loudest_channel) {
  lookahead_limiter_processor_config_t params = {
      .channels = 2,
      .monitor_channels = NULL,
      .monitor_channels_count = 0,
      .process_channels = NULL,
      .process_channels_count = 0,
      .limit = 0.0,
      .attack = 0.0,
      .attack_unit = TIME_UNIT_SAMPLES,
      .release = 0.0,
      .release_unit = TIME_UNIT_SAMPLES,
      .delay_processed_only = false};
  processor_config_t cfg = {.type = PROCESSOR_TYPE_LOOKAHEAD_LIMITER,
                            .parameters.lookahead_limiter = params};
  dsp_processor_t *limiter =
      dsp_processor_create("limiter", &cfg, 48000, 4, NULL);
  ASSERT_TRUE(limiter != NULL);

  audio_chunk_t *chunk = audio_chunk_create(4, 2);
  ASSERT_TRUE(chunk != NULL);
  double *ch0 = audio_chunk_get_channel(chunk, 0);
  double *ch1 = audio_chunk_get_channel(chunk, 1);

  ch0[0] = 0.25;
  ch0[1] = 0.25;
  ch0[2] = 0.25;
  ch0[3] = 0.25;
  ch1[0] = 0.5;
  ch1[1] = 1.0;
  ch1[2] = 2.0;
  ch1[3] = 4.0;
  audio_chunk_set_valid_frames(chunk, 4);

  dsp_processor_process(limiter, chunk);

  double expected_ch0[] = {0.25, 0.25, 0.125, 0.0625};
  double expected_ch1[] = {0.5, 1.0, 1.0, 1.0};

  for (size_t i = 0; i < 4; i++) {
    ASSERT_TRUE(is_close(ch0[i], expected_ch0[i], 1e-12));
    ASSERT_TRUE(is_close(ch1[i], expected_ch1[i], 1e-12));
  }

  audio_chunk_free(chunk);
  dsp_processor_free(limiter);
}

TEST(test_lookahead_limiter_processor_monitor_subset) {
  size_t mon_ch[] = {0};
  lookahead_limiter_processor_config_t params = {
      .channels = 2,
      .monitor_channels = mon_ch,
      .monitor_channels_count = 1,
      .process_channels = NULL,
      .process_channels_count = 0,
      .limit = 0.0,
      .attack = 0.0,
      .attack_unit = TIME_UNIT_SAMPLES,
      .release = 0.0,
      .release_unit = TIME_UNIT_SAMPLES,
      .delay_processed_only = false};
  processor_config_t cfg = {.type = PROCESSOR_TYPE_LOOKAHEAD_LIMITER,
                            .parameters.lookahead_limiter = params};
  dsp_processor_t *limiter =
      dsp_processor_create("limiter", &cfg, 48000, 2, NULL);
  ASSERT_TRUE(limiter != NULL);

  audio_chunk_t *chunk = audio_chunk_create(2, 2);
  ASSERT_TRUE(chunk != NULL);
  double *ch0 = audio_chunk_get_channel(chunk, 0);
  double *ch1 = audio_chunk_get_channel(chunk, 1);

  ch0[0] = 1.0;
  ch0[1] = 2.0;
  ch1[0] = 4.0;
  ch1[1] = 1.0;
  audio_chunk_set_valid_frames(chunk, 2);

  dsp_processor_process(limiter, chunk);

  double expected_ch0[] = {1.0, 1.0};
  double expected_ch1[] = {4.0, 0.5};

  for (size_t i = 0; i < 2; i++) {
    ASSERT_TRUE(is_close(ch0[i], expected_ch0[i], 1e-12));
    ASSERT_TRUE(is_close(ch1[i], expected_ch1[i], 1e-12));
  }

  audio_chunk_free(chunk);
  dsp_processor_free(limiter);
}

TEST(test_lookahead_limiter_processor_unprocessed_channel_is_delayed_only) {
  size_t mon_ch[] = {0};
  size_t proc_ch[] = {0};
  lookahead_limiter_processor_config_t params = {
      .channels = 2,
      .monitor_channels = mon_ch,
      .monitor_channels_count = 1,
      .process_channels = proc_ch,
      .process_channels_count = 1,
      .limit = 0.0,
      .attack = 2.0,
      .attack_unit = TIME_UNIT_SAMPLES,
      .release = 0.0,
      .release_unit = TIME_UNIT_SAMPLES,
      .delay_processed_only = false};
  processor_config_t cfg = {.type = PROCESSOR_TYPE_LOOKAHEAD_LIMITER,
                            .parameters.lookahead_limiter = params};
  dsp_processor_t *limiter =
      dsp_processor_create("limiter", &cfg, 48000, 4, NULL);
  ASSERT_TRUE(limiter != NULL);

  // Chunk 1
  audio_chunk_t *chunk1 = audio_chunk_create(4, 2);
  ASSERT_TRUE(chunk1 != NULL);
  double *ch0_1 = audio_chunk_get_channel(chunk1, 0);
  double *ch1_1 = audio_chunk_get_channel(chunk1, 1);
  ch0_1[0] = 0.0;
  ch0_1[1] = 0.0;
  ch0_1[2] = 2.0;
  ch0_1[3] = 0.0;
  ch1_1[0] = 1.0;
  ch1_1[1] = 2.0;
  ch1_1[2] = 3.0;
  ch1_1[3] = 4.0;
  audio_chunk_set_valid_frames(chunk1, 4);

  dsp_processor_process(limiter, chunk1);

  double expected_ch0_1[] = {0.0, 0.0, 0.0, 0.0};
  double expected_ch1_1[] = {0.0, 0.0, 1.0, 2.0};

  for (size_t i = 0; i < 4; i++) {
    ASSERT_TRUE(is_close(ch0_1[i], expected_ch0_1[i], 1e-12));
    ASSERT_TRUE(is_close(ch1_1[i], expected_ch1_1[i], 1e-12));
  }

  // Chunk 2
  audio_chunk_t *chunk2 = audio_chunk_create(4, 2);
  ASSERT_TRUE(chunk2 != NULL);
  double *ch0_2 = audio_chunk_get_channel(chunk2, 0);
  double *ch1_2 = audio_chunk_get_channel(chunk2, 1);
  ch0_2[0] = 0.0;
  ch0_2[1] = 0.0;
  ch0_2[2] = 0.0;
  ch0_2[3] = 0.0;
  ch1_2[0] = 5.0;
  ch1_2[1] = 6.0;
  ch1_2[2] = 7.0;
  ch1_2[3] = 8.0;
  audio_chunk_set_valid_frames(chunk2, 4);

  dsp_processor_process(limiter, chunk2);

  double expected_ch0_2[] = {1.0, 0.0, 0.0, 0.0};
  double expected_ch1_2[] = {3.0, 4.0, 5.0, 6.0};

  for (size_t i = 0; i < 4; i++) {
    ASSERT_TRUE(is_close(ch0_2[i], expected_ch0_2[i], 1e-12));
    ASSERT_TRUE(is_close(ch1_2[i], expected_ch1_2[i], 1e-12));
  }

  audio_chunk_free(chunk1);
  audio_chunk_free(chunk2);
  dsp_processor_free(limiter);
}

TEST(test_lookahead_limiter_processor_delay_processed_only) {
  size_t mon_ch[] = {0};
  size_t proc_ch[] = {0};
  lookahead_limiter_processor_config_t params = {
      .channels = 2,
      .monitor_channels = mon_ch,
      .monitor_channels_count = 1,
      .process_channels = proc_ch,
      .process_channels_count = 1,
      .limit = 0.0,
      .attack = 2.0,
      .attack_unit = TIME_UNIT_SAMPLES,
      .release = 0.0,
      .release_unit = TIME_UNIT_SAMPLES,
      .delay_processed_only = true};
  processor_config_t cfg = {.type = PROCESSOR_TYPE_LOOKAHEAD_LIMITER,
                            .parameters.lookahead_limiter = params};
  dsp_processor_t *limiter =
      dsp_processor_create("limiter", &cfg, 48000, 4, NULL);
  ASSERT_TRUE(limiter != NULL);

  audio_chunk_t *chunk = audio_chunk_create(4, 2);
  ASSERT_TRUE(chunk != NULL);
  double *ch0 = audio_chunk_get_channel(chunk, 0);
  double *ch1 = audio_chunk_get_channel(chunk, 1);

  ch0[0] = 0.0;
  ch0[1] = 0.0;
  ch0[2] = 2.0;
  ch0[3] = 0.0;
  ch1[0] = 1.0;
  ch1[1] = 2.0;
  ch1[2] = 3.0;
  ch1[3] = 4.0;
  audio_chunk_set_valid_frames(chunk, 4);

  dsp_processor_process(limiter, chunk);

  double expected_ch0[] = {0.0, 0.0, 0.0, 0.0};
  double expected_ch1[] = {1.0, 2.0, 3.0, 4.0}; // No delay applied

  for (size_t i = 0; i < 4; i++) {
    ASSERT_TRUE(is_close(ch0[i], expected_ch0[i], 1e-12));
    ASSERT_TRUE(is_close(ch1[i], expected_ch1[i], 1e-12));
  }

  audio_chunk_free(chunk);
  dsp_processor_free(limiter);
}

TEST(test_lookahead_limiter_processor_validate) {
  size_t mon_ch_valid[] = {0, 1};
  size_t mon_ch_invalid[] = {0, 2};
  size_t proc_ch_invalid[] = {5};

  lookahead_limiter_processor_config_t params_valid = {
      .channels = 2,
      .monitor_channels = mon_ch_valid,
      .monitor_channels_count = 2,
      .process_channels = NULL,
      .process_channels_count = 0,
      .limit = 0.0,
      .attack = 4.0,
      .attack_unit = TIME_UNIT_SAMPLES,
      .release = 4.0,
      .release_unit = TIME_UNIT_SAMPLES,
      .delay_processed_only = false};
  processor_config_t cfg_valid = {.type = PROCESSOR_TYPE_LOOKAHEAD_LIMITER,
                                  .parameters.lookahead_limiter = params_valid};
  ASSERT_EQ(0, processor_config_validate(&cfg_valid, 48000, NULL));

  // Monitor channel index out of range (>= channels)
  lookahead_limiter_processor_config_t params_invalid_mon = params_valid;
  params_invalid_mon.monitor_channels = mon_ch_invalid;
  params_invalid_mon.monitor_channels_count = 2;
  processor_config_t cfg_invalid_mon = {
      .type = PROCESSOR_TYPE_LOOKAHEAD_LIMITER,
      .parameters.lookahead_limiter = params_invalid_mon};
  ASSERT_NE(0, processor_config_validate(&cfg_invalid_mon, 48000, NULL));

  // Process channel index out of range (>= channels)
  lookahead_limiter_processor_config_t params_invalid_proc = params_valid;
  params_invalid_proc.process_channels = proc_ch_invalid;
  params_invalid_proc.process_channels_count = 1;
  processor_config_t cfg_invalid_proc = {
      .type = PROCESSOR_TYPE_LOOKAHEAD_LIMITER,
      .parameters.lookahead_limiter = params_invalid_proc};
  ASSERT_NE(0, processor_config_validate(&cfg_invalid_proc, 48000, NULL));

  // Attack > 1s (48001 samples at 48000Hz)
  lookahead_limiter_processor_config_t params_invalid_attack = params_valid;
  params_invalid_attack.attack = 48001.0;
  processor_config_t cfg_invalid_attack = {
      .type = PROCESSOR_TYPE_LOOKAHEAD_LIMITER,
      .parameters.lookahead_limiter = params_invalid_attack};
  ASSERT_NE(0, processor_config_validate(&cfg_invalid_attack, 48000, NULL));

  // Attack < 0
  lookahead_limiter_processor_config_t params_invalid_attack_neg = params_valid;
  params_invalid_attack_neg.attack = -1.0;
  processor_config_t cfg_invalid_attack_neg = {
      .type = PROCESSOR_TYPE_LOOKAHEAD_LIMITER,
      .parameters.lookahead_limiter = params_invalid_attack_neg};
  ASSERT_NE(0, processor_config_validate(&cfg_invalid_attack_neg, 48000, NULL));
}

TEST(test_compressor_config_validation) {
  compressor_config_t params_valid = {
      .channels = 2,
      .attack = 0.01,
      .attack_unit = TIME_UNIT_S,
      .release = 0.1,
      .release_unit = TIME_UNIT_S,
      .threshold = -10.0,
      .factor = 2.0,
  };
  processor_config_t cfg = {
      .type = PROCESSOR_TYPE_COMPRESSOR,
      .parameters.compressor = params_valid,
  };
  ASSERT_EQ(0, processor_config_validate(&cfg, 48000, NULL));

  // Attack == 0.0 must be rejected (04-M1)
  cfg.parameters.compressor.attack = 0.0;
  ASSERT_NE(0, processor_config_validate(&cfg, 48000, NULL));

  // Attack < 0.0 must be rejected
  cfg.parameters.compressor.attack = -0.01;
  ASSERT_NE(0, processor_config_validate(&cfg, 48000, NULL));

  // Release <= 0.0 must be rejected
  cfg.parameters.compressor.attack = 0.01;
  cfg.parameters.compressor.release = 0.0;
  ASSERT_NE(0, processor_config_validate(&cfg, 48000, NULL));
  cfg.parameters.compressor.release = -0.1;
  ASSERT_NE(0, processor_config_validate(&cfg, 48000, NULL));
}

TEST(test_race_large_attenuation) {
  race_config_t params = {
      .channels = 2,
      .channel_a = 0,
      .channel_b = 1,
      .attenuation = 160.0,
      .delay = 0.0001,
      .delay_unit = DELAY_UNIT_S,
  };
  processor_config_t cfg = {
      .type = PROCESSOR_TYPE_RACE,
      .parameters.race = params,
  };
  ASSERT_EQ(0, processor_config_validate(&cfg, 48000, NULL));
  dsp_processor_t *race = dsp_processor_create("race", &cfg, 48000, 512, NULL);
  ASSERT_TRUE(race != NULL);
  dsp_processor_free(race);
}

/// Upstream `test_resumed_channel_does_not_replay_old_samples` (01fce11): a
/// channel that is delayed again after a pause in its delay must start from
/// silence, not replay what was left in the delay line.
TEST(test_lookahead_limiter_processor_resumed_channel_starts_silent) {
  lookahead_limiter_processor_config_t params = {
      .channels = 2,
      .monitor_channels = NULL,
      .monitor_channels_count = 0,
      .process_channels = NULL,
      .process_channels_count = 0,
      .limit = 0.0,
      .attack = 2.0,
      .attack_unit = TIME_UNIT_SAMPLES,
      .release = 0.0,
      .release_unit = TIME_UNIT_SAMPLES,
      .delay_processed_only = true};
  processor_config_t cfg = {.type = PROCESSOR_TYPE_LOOKAHEAD_LIMITER,
                            .parameters.lookahead_limiter = params};

  // All channels delayed: channel 1 leaves 0.5 in its delay line.
  dsp_processor_t *all = dsp_processor_create("limiter", &cfg, 48000, 4, NULL);
  ASSERT_TRUE(all != NULL);
  audio_chunk_t *chunk = audio_chunk_create(4, 2);
  ASSERT_TRUE(chunk != NULL);
  double *ch1 = audio_chunk_get_channel(chunk, 1);
  for (size_t i = 0; i < 4; i++)
    ch1[i] = 0.5;
  audio_chunk_set_valid_frames(chunk, 4);
  dsp_processor_process(all, chunk);

  // Only channel 0 processed, so channel 1 is not delayed any more.
  size_t only0[] = {0};
  cfg.parameters.lookahead_limiter.process_channels = only0;
  cfg.parameters.lookahead_limiter.process_channels_count = 1;
  dsp_processor_t *ch0_only =
      dsp_processor_create("limiter", &cfg, 48000, 4, NULL);
  ASSERT_TRUE(ch0_only != NULL);
  dsp_processor_transfer_state(ch0_only, all);
  dsp_processor_free(all);
  audio_chunk_zero(chunk);
  audio_chunk_set_valid_frames(chunk, 4);
  dsp_processor_process(ch0_only, chunk);

  // Channel 1 delayed again: its delay must not replay the old 0.5 samples.
  cfg.parameters.lookahead_limiter.process_channels = NULL;
  cfg.parameters.lookahead_limiter.process_channels_count = 0;
  dsp_processor_t *resumed =
      dsp_processor_create("limiter", &cfg, 48000, 4, NULL);
  ASSERT_TRUE(resumed != NULL);
  dsp_processor_transfer_state(resumed, ch0_only);
  dsp_processor_free(ch0_only);
  audio_chunk_zero(chunk);
  audio_chunk_set_valid_frames(chunk, 4);
  dsp_processor_process(resumed, chunk);

  ch1 = audio_chunk_get_channel(chunk, 1);
  for (size_t i = 0; i < 4; i++) {
    ASSERT_TRUE(is_close(ch1[i], 0.0, 1e-12));
  }

  audio_chunk_free(chunk);
  dsp_processor_free(resumed);
}

/* ------------------------------------------------------------------------- */
/* Processor regression tests (from audit)                                   */
/* ------------------------------------------------------------------------- */

static processor_config_t audit_limiter_cfg(size_t channels, double attack_samples) {
  processor_config_t cfg = {.type = PROCESSOR_TYPE_LOOKAHEAD_LIMITER};
  lookahead_limiter_processor_config_t *p = &cfg.parameters.lookahead_limiter;
  p->channels = channels;
  p->has_limit = true;
  p->limit = -6.0;
  p->attack = attack_samples;
  p->attack_unit = TIME_UNIT_SAMPLES;
  p->release = 100.0;
  p->release_unit = TIME_UNIT_SAMPLES;
  return cfg;
}

TEST(audit_limiter_rejects_non_finite_params) {
  processor_config_t cfg = audit_limiter_cfg(2, 4.0);
  ASSERT_EQ(0, processor_config_validate(&cfg, 48000, NULL));

  cfg.parameters.lookahead_limiter.limit = NAN;
  ASSERT_NE(0, processor_config_validate(&cfg, 48000, NULL));
  cfg.parameters.lookahead_limiter.limit = INFINITY;
  ASSERT_NE(0, processor_config_validate(&cfg, 48000, NULL));
  cfg.parameters.lookahead_limiter.limit = -INFINITY;
  ASSERT_NE(0, processor_config_validate(&cfg, 48000, NULL));
  cfg.parameters.lookahead_limiter.limit = -6.0;

  cfg.parameters.lookahead_limiter.attack = NAN;
  ASSERT_NE(0, processor_config_validate(&cfg, 48000, NULL));
  cfg.parameters.lookahead_limiter.attack = 4.0;

  cfg.parameters.lookahead_limiter.release = NAN;
  ASSERT_NE(0, processor_config_validate(&cfg, 48000, NULL));
  cfg.parameters.lookahead_limiter.release = INFINITY;
  ASSERT_NE(0, processor_config_validate(&cfg, 48000, NULL));
  cfg.parameters.lookahead_limiter.release = 100.0;

  cfg.parameters.lookahead_limiter.attack_unit = TIME_UNIT_INVALID;
  ASSERT_NE(0, processor_config_validate(&cfg, 48000, NULL));
  cfg.parameters.lookahead_limiter.attack_unit = TIME_UNIT_SAMPLES;
  cfg.parameters.lookahead_limiter.release_unit = TIME_UNIT_INVALID;
  ASSERT_NE(0, processor_config_validate(&cfg, 48000, NULL));
  cfg.parameters.lookahead_limiter.release_unit = TIME_UNIT_SAMPLES;

  // create() must report the failure through err.
  cfg.parameters.lookahead_limiter.release = NAN;
  config_error_t err;
  config_error_init(&err);
  ASSERT_TRUE(dsp_processor_create("lim", &cfg, 48000, 16, &err) == NULL);
  ASSERT_NE(CONFIG_ERR_NONE, err.type);
  ASSERT_TRUE(err.message[0] != '\0');
}

// Upstream rebuilds all lookahead delays when `channels` changes. A hot reload
// that changes the channel count must not leak old delayed audio.
TEST(audit_limiter_transfer_channel_change_resets_delays) {
  const size_t frames = 8;
  processor_config_t cfg2 = audit_limiter_cfg(2, 4.0);
  processor_config_t cfg3 = audit_limiter_cfg(3, 4.0);
  dsp_processor_t *old_lim =
      dsp_processor_create("lim", &cfg2, 48000, frames, NULL);
  dsp_processor_t *new_lim =
      dsp_processor_create("lim", &cfg3, 48000, frames, NULL);
  ASSERT_TRUE(old_lim != NULL);
  ASSERT_TRUE(new_lim != NULL);

  audio_chunk_t *c2 = audio_chunk_create(frames, 2);
  for (size_t ch = 0; ch < 2; ch++) {
    double *buf = audio_chunk_get_channel(c2, ch);
    for (size_t i = 0; i < frames; i++)
      buf[i] = 0.25;
  }
  audio_chunk_set_valid_frames(c2, frames);
  dsp_processor_process(old_lim, c2);

  dsp_processor_transfer_state(new_lim, old_lim);

  audio_chunk_t *c3 = audio_chunk_create(frames, 3);
  for (size_t ch = 0; ch < 3; ch++)
    memset(audio_chunk_get_channel(c3, ch), 0, frames * sizeof(double));
  audio_chunk_set_valid_frames(c3, frames);
  dsp_processor_process(new_lim, c3);
  for (size_t ch = 0; ch < 3; ch++) {
    const double *buf = audio_chunk_get_channel(c3, ch);
    for (size_t i = 0; i < frames; i++)
      ASSERT_TRUE(buf[i] == 0.0);
  }

  audio_chunk_free(c2);
  audio_chunk_free(c3);
  dsp_processor_free(old_lim);
  dsp_processor_free(new_lim);
}

// Same channel count: delay contents are still preserved (cdsp keeps state
// across unchanged reloads).
TEST(audit_limiter_transfer_same_channels_keeps_delays) {
  const size_t frames = 8;
  processor_config_t cfg = audit_limiter_cfg(2, 4.0);
  dsp_processor_t *old_lim =
      dsp_processor_create("lim", &cfg, 48000, frames, NULL);
  dsp_processor_t *new_lim =
      dsp_processor_create("lim", &cfg, 48000, frames, NULL);
  ASSERT_TRUE(old_lim != NULL && new_lim != NULL);

  audio_chunk_t *c = audio_chunk_create(frames, 2);
  for (size_t ch = 0; ch < 2; ch++) {
    double *buf = audio_chunk_get_channel(c, ch);
    for (size_t i = 0; i < frames; i++)
      buf[i] = 0.25;
  }
  audio_chunk_set_valid_frames(c, frames);
  dsp_processor_process(old_lim, c);
  dsp_processor_transfer_state(new_lim, old_lim);

  for (size_t ch = 0; ch < 2; ch++)
    memset(audio_chunk_get_channel(c, ch), 0, frames * sizeof(double));
  dsp_processor_process(new_lim, c);
  // 0.25 is below the -6 dB limit, so the 4 delayed samples pass unchanged.
  const double *buf = audio_chunk_get_channel(c, 0);
  ASSERT_TRUE(fabs(buf[0] - 0.25) < 1e-12);
  ASSERT_TRUE(buf[frames - 1] == 0.0);

  audio_chunk_free(c);
  dsp_processor_free(old_lim);
  dsp_processor_free(new_lim);
}

static processor_config_t audit_compressor_cfg(void) {
  processor_config_t cfg = {.type = PROCESSOR_TYPE_COMPRESSOR};
  compressor_config_t *p = &cfg.parameters.compressor;
  p->channels = 2;
  p->attack = 0.01;
  p->attack_unit = TIME_UNIT_S;
  p->release = 0.1;
  p->release_unit = TIME_UNIT_S;
  p->threshold = -20.0;
  p->factor = 2.0;
  return cfg;
}

TEST(audit_compressor_validation) {
  processor_config_t cfg = audit_compressor_cfg();
  ASSERT_EQ(0, processor_config_validate(&cfg, 48000, NULL));

  cfg.parameters.compressor.attack = NAN;
  ASSERT_NE(0, processor_config_validate(&cfg, 48000, NULL));
  cfg.parameters.compressor.attack = INFINITY;
  ASSERT_NE(0, processor_config_validate(&cfg, 48000, NULL));
  cfg.parameters.compressor.attack = 0.01;

  cfg.parameters.compressor.release = NAN;
  ASSERT_NE(0, processor_config_validate(&cfg, 48000, NULL));
  cfg.parameters.compressor.release = 0.1;

  cfg.parameters.compressor.threshold = NAN;
  ASSERT_NE(0, processor_config_validate(&cfg, 48000, NULL));
  cfg.parameters.compressor.threshold = -20.0;

  cfg.parameters.compressor.has_makeup_gain = true;
  cfg.parameters.compressor.makeup_gain = INFINITY;
  ASSERT_NE(0, processor_config_validate(&cfg, 48000, NULL));
  cfg.parameters.compressor.makeup_gain = 3.0;
  ASSERT_EQ(0, processor_config_validate(&cfg, 48000, NULL));

  cfg.parameters.compressor.attack_unit = TIME_UNIT_INVALID;
  ASSERT_NE(0, processor_config_validate(&cfg, 48000, NULL));
  cfg.parameters.compressor.attack_unit = TIME_UNIT_S;

  // clip_limit is validated up front, consistent with create().
  cfg.parameters.compressor.has_clip_limit = true;
  cfg.parameters.compressor.clip_limit = -1.0;
  ASSERT_EQ(0, processor_config_validate(&cfg, 48000, NULL));
  cfg.parameters.compressor.clip_limit = NAN;
  ASSERT_NE(0, processor_config_validate(&cfg, 48000, NULL));
  cfg.parameters.compressor.clip_limit = -7000.0; // underflows to 0 linear
  ASSERT_NE(0, processor_config_validate(&cfg, 48000, NULL));
  ASSERT_TRUE(dsp_processor_create("c", &cfg, 48000, 16, NULL) == NULL);
}

TEST(audit_noise_gate_validation) {
  processor_config_t cfg = {.type = PROCESSOR_TYPE_NOISE_GATE};
  noise_gate_config_t *p = &cfg.parameters.noise_gate;
  p->channels = 2;
  p->attack = 0.01;
  p->attack_unit = TIME_UNIT_S;
  p->release = 0.1;
  p->release_unit = TIME_UNIT_S;
  p->threshold = -40.0;
  p->attenuation = 20.0;
  ASSERT_EQ(0, processor_config_validate(&cfg, 48000, NULL));

  p->attack = NAN;
  ASSERT_NE(0, processor_config_validate(&cfg, 48000, NULL));
  p->attack = 0.01;
  p->release = INFINITY;
  ASSERT_NE(0, processor_config_validate(&cfg, 48000, NULL));
  p->release = 0.1;
  p->threshold = NAN;
  ASSERT_NE(0, processor_config_validate(&cfg, 48000, NULL));
  p->threshold = -40.0;
  p->attenuation = INFINITY;
  ASSERT_NE(0, processor_config_validate(&cfg, 48000, NULL));
  p->attenuation = 20.0;
  p->release_unit = TIME_UNIT_INVALID;
  ASSERT_NE(0, processor_config_validate(&cfg, 48000, NULL));
}

static processor_config_t audit_race_cfg(size_t channels, size_t a, size_t b) {
  processor_config_t cfg = {.type = PROCESSOR_TYPE_RACE};
  race_config_t *p = &cfg.parameters.race;
  p->channels = channels;
  p->channel_a = a;
  p->channel_b = b;
  p->delay = 4.0;
  p->delay_unit = DELAY_UNIT_SAMPLES;
  p->attenuation = 3.0;
  return cfg;
}

TEST(audit_race_validate_rejects_oversized_delay) {
  processor_config_t cfg = audit_race_cfg(2, 0, 1);
  ASSERT_EQ(0, processor_config_validate(&cfg, 48000, NULL));
  cfg.parameters.race.delay = 2.0e8; // > 100M samples delay filter limit
  ASSERT_NE(0, processor_config_validate(&cfg, 48000, NULL));
  ASSERT_TRUE(dsp_processor_create("r", &cfg, 48000, 16, NULL) == NULL);
}

// Upstream race.rs returns early if either channel is an (empty) unused capture
// channel; cdsp marks those via used_channels.
TEST(audit_race_skips_unused_channel) {
  processor_config_t cfg = audit_race_cfg(2, 0, 1);
  dsp_processor_t *race = dsp_processor_create("r", &cfg, 48000, 16, NULL);
  ASSERT_TRUE(race != NULL);

  const size_t frames = 16;
  audio_chunk_t *c = audio_chunk_create(frames, 2);
  double *a = audio_chunk_get_channel(c, 0);
  double *b = audio_chunk_get_channel(c, 1);
  for (size_t i = 0; i < frames; i++) {
    a[i] = (i == 0) ? 1.0 : 0.0;
    b[i] = 0.0;
  }
  audio_chunk_set_valid_frames(c, frames);
  bool used[2] = {true, false};
  audio_chunk_set_used_channels(c, used);

  dsp_processor_process(race, c);
  ASSERT_TRUE(a[0] == 1.0);
  for (size_t i = 1; i < frames; i++)
    ASSERT_TRUE(a[i] == 0.0);
  for (size_t i = 0; i < frames; i++)
    ASSERT_TRUE(b[i] == 0.0);

  // With both channels used, the cross-talk loop does run.
  bool all_used[2] = {true, true};
  audio_chunk_set_used_channels(c, all_used);
  dsp_processor_process(race, c);
  bool any_nonzero = false;
  for (size_t i = 0; i < frames; i++)
    if (b[i] != 0.0)
      any_nonzero = true;
  ASSERT_TRUE(any_nonzero);

  audio_chunk_free(c);
  dsp_processor_free(race);
}

// Report 05 §3.4: feedback/delay state must not migrate to a different
// channel pair on hot reload.
TEST(audit_race_transfer_channel_change_resets_state) {
  const size_t frames = 16;
  processor_config_t cfg_old = audit_race_cfg(3, 0, 1);
  processor_config_t cfg_new = audit_race_cfg(3, 0, 2);
  dsp_processor_t *old_r = dsp_processor_create("r", &cfg_old, 48000, 16, NULL);
  dsp_processor_t *new_r = dsp_processor_create("r", &cfg_new, 48000, 16, NULL);
  ASSERT_TRUE(old_r != NULL && new_r != NULL);

  audio_chunk_t *c = audio_chunk_create(frames, 3);
  for (size_t ch = 0; ch < 3; ch++) {
    double *buf = audio_chunk_get_channel(c, ch);
    for (size_t i = 0; i < frames; i++)
      buf[i] = 0.5;
  }
  audio_chunk_set_valid_frames(c, frames);
  dsp_processor_process(old_r, c);

  dsp_processor_transfer_state(new_r, old_r);
  for (size_t ch = 0; ch < 3; ch++)
    memset(audio_chunk_get_channel(c, ch), 0, frames * sizeof(double));
  dsp_processor_process(new_r, c);
  for (size_t ch = 0; ch < 3; ch++) {
    const double *buf = audio_chunk_get_channel(c, ch);
    for (size_t i = 0; i < frames; i++)
      ASSERT_TRUE(buf[i] == 0.0);
  }

  audio_chunk_free(c);
  dsp_processor_free(old_r);
  dsp_processor_free(new_r);
}

TEST_MAIN()
