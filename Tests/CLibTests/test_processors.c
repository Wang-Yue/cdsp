#include <math.h>

#include "Audio/audio_chunk.h"
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

TEST_MAIN()
