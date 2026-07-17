#include <math.h>

#include "Filters/filter.h"
#include "Filters/gain.h"
#include "Filters/volume.h"
#include "test_support.h"

TEST(GainInvert) {
  gain_config_t params = {
      .gain = 0.0, .has_gain = true, .scale = GAIN_SCALE_DB, .inverted = true};
  filter_config_t cfg = {.type = FILTER_TYPE_GAIN, .parameters.gain = params};
  void* filter = g_gain_vtable.create("gain", &cfg, 0, 0, NULL, NULL);
  ASSERT_TRUE(filter != NULL);
  double waveform[] = {-0.5, 0.0, 0.5};
  g_gain_vtable.process(filter, waveform, 3);
  ASSERT_NEAR(0.5, waveform[0], 1e-10);
  ASSERT_NEAR(0.0, waveform[1], 1e-10);
  ASSERT_NEAR(-0.5, waveform[2], 1e-10);
  g_gain_vtable.free(filter);
}

TEST(GainAmplify) {
  gain_config_t params = {
      .gain = 20.0, .has_gain = true, .scale = GAIN_SCALE_DB};
  filter_config_t cfg = {.type = FILTER_TYPE_GAIN, .parameters.gain = params};
  void* filter = g_gain_vtable.create("gain", &cfg, 0, 0, NULL, NULL);
  ASSERT_TRUE(filter != NULL);
  double waveform[] = {-0.5, 0.0, 0.5};
  g_gain_vtable.process(filter, waveform, 3);
  ASSERT_NEAR(-5.0, waveform[0], 1e-6);
  ASSERT_NEAR(0.0, waveform[1], 1e-10);
  ASSERT_NEAR(5.0, waveform[2], 1e-6);
  g_gain_vtable.free(filter);
}

TEST(GainMute) {
  gain_config_t params = {.gain = 0.0, .has_gain = true, .mute = true};
  filter_config_t cfg = {.type = FILTER_TYPE_GAIN, .parameters.gain = params};
  void* filter = g_gain_vtable.create("gain", &cfg, 0, 0, NULL, NULL);
  ASSERT_TRUE(filter != NULL);
  double waveform[] = {-0.5, 0.0, 0.5, 1.0, -1.0};
  g_gain_vtable.process(filter, waveform, 5);
  for (size_t i = 0; i < 5; i++) {
    ASSERT_DOUBLE_EQ(0.0, waveform[i]);
  }
  g_gain_vtable.free(filter);
}

TEST(GainLinearScale) {
  gain_config_t params = {
      .gain = 0.5, .has_gain = true, .scale = GAIN_SCALE_LINEAR};
  filter_config_t cfg = {.type = FILTER_TYPE_GAIN, .parameters.gain = params};
  void* filter = g_gain_vtable.create("gain", &cfg, 0, 0, NULL, NULL);
  ASSERT_TRUE(filter != NULL);
  double waveform[] = {1.0};
  g_gain_vtable.process(filter, waveform, 1);
  ASSERT_NEAR(0.5, waveform[0], 1e-10);
  g_gain_vtable.free(filter);
}

static void process_vol(volume_filter_t* filter, double* waveform,
                        size_t count) {
  volume_filter_prepare_chunk(filter);
  g_volume_vtable.process(filter, waveform, count);
  volume_filter_advance_ramp(filter);
}

TEST(VolumeUnityGain) {
  processing_parameters_t* proc_params = processing_parameters_create(2, 2);
  processing_parameters_set_target_volume_for_fader(proc_params, 0.0,
                                                    FADER_MAIN);
  volume_config_t params = {.ramp_time_ms = 0.0,
                            .has_ramp_time_ms = true,
                            .limit = 50.0,
                            .has_limit = true,
                            .fader = FADER_MAIN};
  filter_config_t cfg = {.type = FILTER_TYPE_VOLUME,
                         .parameters.volume = params};
  volume_filter_t* filter = (volume_filter_t*)g_volume_vtable.create(
      "volume", &cfg, 44100, 4, proc_params, NULL);
  ASSERT_TRUE(filter != NULL);

  double waveform[] = {1.0, -0.5, 0.25, 0.0};
  double original[] = {1.0, -0.5, 0.25, 0.0};
  process_vol(filter, waveform, 4);
  for (size_t i = 0; i < 4; i++) {
    ASSERT_NEAR(original[i], waveform[i], 1e-10);
  }
  g_volume_vtable.free(filter);
  processing_parameters_free(proc_params);
}

TEST(VolumeAttenuation) {
  processing_parameters_t* proc_params = processing_parameters_create(2, 2);
  processing_parameters_set_target_volume_for_fader(proc_params, -20.0,
                                                    FADER_MAIN);
  volume_config_t params = {.ramp_time_ms = 0.0,
                            .has_ramp_time_ms = true,
                            .limit = 50.0,
                            .has_limit = true,
                            .fader = FADER_MAIN};
  filter_config_t cfg = {.type = FILTER_TYPE_VOLUME,
                         .parameters.volume = params};
  volume_filter_t* filter = (volume_filter_t*)g_volume_vtable.create(
      "volume", &cfg, 44100, 4, proc_params, NULL);
  ASSERT_TRUE(filter != NULL);

  double waveform[] = {1.0, -1.0, 0.5, -0.5};
  process_vol(filter, waveform, 4);
  double gain = double_from_db(-20.0);
  ASSERT_NEAR(1.0 * gain, waveform[0], 1e-10);
  ASSERT_NEAR(-1.0 * gain, waveform[1], 1e-10);
  ASSERT_NEAR(0.5 * gain, waveform[2], 1e-10);
  ASSERT_NEAR(-0.5 * gain, waveform[3], 1e-10);
  g_volume_vtable.free(filter);
  processing_parameters_free(proc_params);
}

TEST(VolumeMuteRampsToZero) {
  processing_parameters_t* proc_params = processing_parameters_create(2, 2);
  processing_parameters_set_target_volume_for_fader(proc_params, 0.0,
                                                    FADER_MAIN);
  processing_parameters_set_muted_for_fader(proc_params, true, FADER_MAIN);
  volume_config_t params = {.ramp_time_ms = 0.0,
                            .has_ramp_time_ms = true,
                            .limit = 50.0,
                            .has_limit = true,
                            .fader = FADER_MAIN};
  filter_config_t cfg = {.type = FILTER_TYPE_VOLUME,
                         .parameters.volume = params};
  volume_filter_t* filter = (volume_filter_t*)g_volume_vtable.create(
      "volume", &cfg, 44100, 4, proc_params, NULL);
  ASSERT_TRUE(filter != NULL);

  double waveform[] = {1.0, 0.5, -0.5, -1.0};
  process_vol(filter, waveform, 4);
  for (size_t i = 0; i < 4; i++) {
    ASSERT_NEAR(0.0, waveform[i], 1e-10);
  }
  g_volume_vtable.free(filter);
  processing_parameters_free(proc_params);
}

TEST(VolumeRamp) {
  size_t chunk_size = 4;
  int sample_rate = 44100;
  double ramp_time_ms = 1000.0 * (double)chunk_size / (double)sample_rate * 2.0;

  processing_parameters_t* proc_params = processing_parameters_create(2, 2);
  processing_parameters_set_target_volume_for_fader(proc_params, 0.0,
                                                    FADER_MAIN);
  volume_config_t params = {.ramp_time_ms = ramp_time_ms,
                            .has_ramp_time_ms = true,
                            .limit = 50.0,
                            .has_limit = true,
                            .fader = FADER_MAIN};
  filter_config_t cfg = {.type = FILTER_TYPE_VOLUME,
                         .parameters.volume = params};
  volume_filter_t* filter = (volume_filter_t*)g_volume_vtable.create(
      "volume", &cfg, sample_rate, chunk_size, proc_params, NULL);
  ASSERT_TRUE(filter != NULL);

  double chunk0[] = {1.0, 1.0, 1.0, 1.0};
  process_vol(filter, chunk0, 4);
  for (size_t i = 0; i < chunk_size; i++) {
    ASSERT_NEAR(1.0, chunk0[i], 1e-10);
  }

  processing_parameters_set_target_volume_for_fader(proc_params, -20.0,
                                                    FADER_MAIN);

  double chunk1[] = {1.0, 1.0, 1.0, 1.0};
  process_vol(filter, chunk1, 4);

  double gain0db = double_from_db(0.0);
  double gain_m20db = double_from_db(-20.0);
  for (size_t i = 0; i < chunk_size; i++) {
    ASSERT_TRUE(chunk1[i] <= gain0db + 1e-6);
    ASSERT_TRUE(chunk1[i] >= gain_m20db - 1e-6);
  }
  ASSERT_TRUE(chunk1[0] > chunk1[chunk_size - 1]);

  double chunk2[] = {1.0, 1.0, 1.0, 1.0};
  process_vol(filter, chunk2, 4);
  ASSERT_TRUE(chunk2[chunk_size - 1] < chunk1[chunk_size - 1]);
  ASSERT_TRUE(chunk2[chunk_size - 1] >= gain_m20db - 1e-6);

  double chunk3[] = {1.0, 1.0, 1.0, 1.0};
  process_vol(filter, chunk3, 4);
  for (size_t i = 0; i < chunk_size; i++) {
    ASSERT_NEAR(gain_m20db, chunk3[i], 1e-6);
  }
  g_volume_vtable.free(filter);
  processing_parameters_free(proc_params);
}

TEST(VolumeChangeThreshold) {
  processing_parameters_t* proc_params = processing_parameters_create(2, 2);
  processing_parameters_set_target_volume_for_fader(proc_params, 0.0,
                                                    FADER_MAIN);
  volume_config_t params = {.ramp_time_ms = 0.0,
                            .has_ramp_time_ms = true,
                            .limit = 50.0,
                            .has_limit = true,
                            .fader = FADER_MAIN};
  filter_config_t cfg = {.type = FILTER_TYPE_VOLUME,
                         .parameters.volume = params};
  volume_filter_t* filter = (volume_filter_t*)g_volume_vtable.create(
      "volume", &cfg, 44100, 4, proc_params, NULL);
  ASSERT_TRUE(filter != NULL);

  double wave1[] = {1.0, 1.0, 1.0, 1.0};
  process_vol(filter, wave1, 4);

  processing_parameters_set_target_volume_for_fader(proc_params, 0.005,
                                                    FADER_MAIN);
  double wave2[] = {1.0, 1.0, 1.0, 1.0};
  process_vol(filter, wave2, 4);
  for (size_t i = 0; i < 4; i++) {
    ASSERT_NEAR(1.0, wave2[i], 1e-10);
  }

  processing_parameters_set_target_volume_for_fader(proc_params, 0.02,
                                                    FADER_MAIN);
  double wave3[] = {1.0, 1.0, 1.0, 1.0};
  process_vol(filter, wave3, 4);
  double expected_gain = double_from_db(0.02);
  for (size_t i = 0; i < 4; i++) {
    ASSERT_NEAR(expected_gain, wave3[i], 1e-6);
  }
  g_volume_vtable.free(filter);
  processing_parameters_free(proc_params);
}

TEST(VolumeLimit) {
  processing_parameters_t* proc_params = processing_parameters_create(2, 2);
  processing_parameters_set_target_volume_for_fader(proc_params, 0.0,
                                                    FADER_MAIN);
  volume_config_t params = {.ramp_time_ms = 0.0,
                            .has_ramp_time_ms = true,
                            .limit = 10.0,
                            .has_limit = true,
                            .fader = FADER_MAIN};
  filter_config_t cfg = {.type = FILTER_TYPE_VOLUME,
                         .parameters.volume = params};
  volume_filter_t* filter = (volume_filter_t*)g_volume_vtable.create(
      "volume", &cfg, 44100, 4, proc_params, NULL);
  ASSERT_TRUE(filter != NULL);

  processing_parameters_set_target_volume_for_fader(proc_params, 20.0,
                                                    FADER_MAIN);
  double waveform[] = {1.0, 1.0, 1.0, 1.0};
  process_vol(filter, waveform, 4);

  double expected_gain = double_from_db(10.0);
  for (size_t i = 0; i < 4; i++) {
    ASSERT_NEAR(expected_gain, waveform[i], 1e-6);
  }
  g_volume_vtable.free(filter);
  processing_parameters_free(proc_params);
}

TEST_MAIN()
