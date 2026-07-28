#include <math.h>

#include "Audio/audio_chunk.h"
#include "Filters/filter.h"
#include "Filters/lookahead_limiter.h"
#include "Processors/compressor_processor.h"
#include "Processors/noise_gate_processor.h"
#include "Processors/processor.h"
#include "Processors/race_processor.h"
#include "test_support.h"

static bool is_close(double left, double right, double maxdiff) {
  return fabs(left - right) < maxdiff;
}

TEST(compressor_basic_compression) {
  int mon_ch[] = {0};
  int proc_ch[] = {0, 1};
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

  dsp_processor_t* comp =
      dsp_processor_create("compressor", &config, 48000, 1000, NULL);
  ASSERT_TRUE(comp != NULL);

  audio_chunk_t* chunk = audio_chunk_create(1000, 2);
  ASSERT_TRUE(chunk != NULL);
  double* ch0 = audio_chunk_get_channel(chunk, 0);
  double* ch1 = audio_chunk_get_channel(chunk, 1);
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

TEST(noisegate_basic_gate) {
  int mon_ch[] = {0};
  int proc_ch[] = {0};
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

  dsp_processor_t* gate =
      dsp_processor_create("noisegate", &config, 48000, 100, NULL);
  ASSERT_TRUE(gate != NULL);

  audio_chunk_t* chunk = audio_chunk_create(100, 1);
  ASSERT_TRUE(chunk != NULL);
  double* ch0 = audio_chunk_get_channel(chunk, 0);
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
  params.has_delay_unit = true;
  params.attenuation = 6.02;

  processor_config_t config = {.type = PROCESSOR_TYPE_RACE,
                               .parameters.race = params};

  dsp_processor_t* race = dsp_processor_create("race", &config, 48000, 0, NULL);
  ASSERT_TRUE(race != NULL);

  audio_chunk_t* chunk = audio_chunk_create(10, 2);
  ASSERT_TRUE(chunk != NULL);
  double* ch0 = audio_chunk_get_channel(chunk, 0);
  double* ch1 = audio_chunk_get_channel(chunk, 1);
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
  params.has_delay_unit = true;
  params.attenuation = 6.02;

  processor_config_t config = {.type = PROCESSOR_TYPE_RACE,
                               .parameters.race = params};

  dsp_processor_t* race1 =
      dsp_processor_create("race1", &config, 48000, 0, NULL);
  dsp_processor_t* race2 =
      dsp_processor_create("race2", &config, 48000, 0, NULL);
  ASSERT_TRUE(race1 != NULL);
  ASSERT_TRUE(race2 != NULL);

  // Send an impulse to race1 to populate state
  audio_chunk_t* chunk_init = audio_chunk_create(10, 2);
  double* ch0_init = audio_chunk_get_channel(chunk_init, 0);
  double* ch1_init = audio_chunk_get_channel(chunk_init, 1);
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
  audio_chunk_t* chunk1 = audio_chunk_create(10, 2);
  audio_chunk_t* chunk2 = audio_chunk_create(10, 2);

  double* ch0_1 = audio_chunk_get_channel(chunk1, 0);
  double* ch1_1 = audio_chunk_get_channel(chunk1, 1);
  double* ch0_2 = audio_chunk_get_channel(chunk2, 0);
  double* ch1_2 = audio_chunk_get_channel(chunk2, 1);

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
  int mon_ch[] = {0};
  int proc_ch[] = {0, 1};
  lookahead_limiter_processor_config_t params = {0};
  params.channels = 2;
  params.monitor_channels = mon_ch;
  params.monitor_channels_count = 1;
  params.process_channels = proc_ch;
  params.process_channels_count = 2;
  params.limit =
      -6.020599913279624;  // -6.02... dB corresponds to ~0.5 linear limit
  params.attack = 2.0;     // 2 ms
  params.attack_unit = TIME_UNIT_MS;
  params.release = 10.0;  // 10 ms
  params.release_unit = TIME_UNIT_MS;
  params.delay_processed_only = false;

  processor_config_t config = {.type = PROCESSOR_TYPE_LOOKAHEAD_LIMITER,
                               .parameters.lookahead_limiter = params};

  // 48000 Hz, 2 ms attack = 96 samples delay
  dsp_processor_t* limiter =
      dsp_processor_create("limiter", &config, 48000, 1000, NULL);
  ASSERT_TRUE(limiter != NULL);

  audio_chunk_t* chunk = audio_chunk_create(1000, 2);
  ASSERT_TRUE(chunk != NULL);
  double* ch0 = audio_chunk_get_channel(chunk, 0);
  double* ch1 = audio_chunk_get_channel(chunk, 1);

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
  int mon_ch[] = {0};
  int proc_ch[] = {0, 1};
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

  dsp_processor_t* limiter1 =
      dsp_processor_create("limiter1", &config, 48000, 1000, NULL);
  dsp_processor_t* limiter2 =
      dsp_processor_create("limiter2", &config, 48000, 1000, NULL);
  ASSERT_TRUE(limiter1 != NULL);
  ASSERT_TRUE(limiter2 != NULL);

  // Send peak to limiter1 to populate lookahead history
  audio_chunk_t* chunk_init = audio_chunk_create(1000, 2);
  double* ch0_init = audio_chunk_get_channel(chunk_init, 0);
  double* ch1_init = audio_chunk_get_channel(chunk_init, 1);
  for (size_t i = 0; i < 1000; i++) {
    ch0_init[i] = 0.1;
    ch1_init[i] = 0.1;
  }
  ch0_init[900] = 2.0;  // Peak close to the end
  audio_chunk_set_valid_frames(chunk_init, 1000);
  dsp_processor_process(limiter1, chunk_init);
  audio_chunk_free(chunk_init);

  // Transfer state
  dsp_processor_transfer_state(limiter2, limiter1);

  // Verify that both processors produce identical outputs on the next chunk
  audio_chunk_t* chunk1 = audio_chunk_create(500, 2);
  audio_chunk_t* chunk2 = audio_chunk_create(500, 2);
  double* ch0_1 = audio_chunk_get_channel(chunk1, 0);
  double* ch1_1 = audio_chunk_get_channel(chunk1, 1);
  double* ch0_2 = audio_chunk_get_channel(chunk2, 0);
  double* ch1_2 = audio_chunk_get_channel(chunk2, 1);

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
  lookahead_limiter_config_t params_filter = {
      .limit = 0.0,
      .attack = 4.0,
      .attack_unit = TIME_UNIT_SAMPLES,
      .release = 1.0 / log(2.0),
      .release_unit = TIME_UNIT_SAMPLES};
  filter_config_t cfg_filter = {.type = FILTER_TYPE_LOOKAHEAD_LIMITER,
                                .parameters.lookahead_limiter = params_filter};
  void* filter = g_lookahead_limiter_vtable.create(
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
  dsp_processor_t* processor =
      dsp_processor_create("processor", &cfg_proc, samplerate, chunksize, NULL);
  ASSERT_TRUE(processor != NULL);

  audio_chunk_t* processor_chunk = audio_chunk_create(chunksize, 1);
  ASSERT_TRUE(processor_chunk != NULL);
  double* proc_buf = audio_chunk_get_channel(processor_chunk, 0);
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
  dsp_processor_t* limiter =
      dsp_processor_create("limiter", &cfg, 48000, 4, NULL);
  ASSERT_TRUE(limiter != NULL);

  audio_chunk_t* chunk = audio_chunk_create(4, 2);
  ASSERT_TRUE(chunk != NULL);
  double* ch0 = audio_chunk_get_channel(chunk, 0);
  double* ch1 = audio_chunk_get_channel(chunk, 1);

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
  int mon_ch[] = {0};
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
  dsp_processor_t* limiter =
      dsp_processor_create("limiter", &cfg, 48000, 2, NULL);
  ASSERT_TRUE(limiter != NULL);

  audio_chunk_t* chunk = audio_chunk_create(2, 2);
  ASSERT_TRUE(chunk != NULL);
  double* ch0 = audio_chunk_get_channel(chunk, 0);
  double* ch1 = audio_chunk_get_channel(chunk, 1);

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
  int mon_ch[] = {0};
  int proc_ch[] = {0};
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
  dsp_processor_t* limiter =
      dsp_processor_create("limiter", &cfg, 48000, 4, NULL);
  ASSERT_TRUE(limiter != NULL);

  // Chunk 1
  audio_chunk_t* chunk1 = audio_chunk_create(4, 2);
  ASSERT_TRUE(chunk1 != NULL);
  double* ch0_1 = audio_chunk_get_channel(chunk1, 0);
  double* ch1_1 = audio_chunk_get_channel(chunk1, 1);
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
  audio_chunk_t* chunk2 = audio_chunk_create(4, 2);
  ASSERT_TRUE(chunk2 != NULL);
  double* ch0_2 = audio_chunk_get_channel(chunk2, 0);
  double* ch1_2 = audio_chunk_get_channel(chunk2, 1);
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
  int mon_ch[] = {0};
  int proc_ch[] = {0};
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
  dsp_processor_t* limiter =
      dsp_processor_create("limiter", &cfg, 48000, 4, NULL);
  ASSERT_TRUE(limiter != NULL);

  audio_chunk_t* chunk = audio_chunk_create(4, 2);
  ASSERT_TRUE(chunk != NULL);
  double* ch0 = audio_chunk_get_channel(chunk, 0);
  double* ch1 = audio_chunk_get_channel(chunk, 1);

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
  double expected_ch1[] = {1.0, 2.0, 3.0, 4.0};  // No delay applied

  for (size_t i = 0; i < 4; i++) {
    ASSERT_TRUE(is_close(ch0[i], expected_ch0[i], 1e-12));
    ASSERT_TRUE(is_close(ch1[i], expected_ch1[i], 1e-12));
  }

  audio_chunk_free(chunk);
  dsp_processor_free(limiter);
}

TEST(test_lookahead_limiter_processor_validate) {
  int mon_ch_valid[] = {0, 1};
  int mon_ch_invalid[] = {0, 2};
  int proc_ch_invalid[] = {5};

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

TEST_MAIN()
