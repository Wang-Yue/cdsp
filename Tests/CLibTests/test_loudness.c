#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Audio/processing_parameters.h"
#include "Config/filter_config_types.h"
#include "Filters/filter.h"
#include "Filters/loudness.h"
#include "test_support.h"

TEST(LoudnessFilterBasic) {
  filter_config_t config;
  memset(&config, 0, sizeof(config));
  config.type = FILTER_TYPE_LOUDNESS;
  config.parameters.loudness.reference_level = -20.0;
  config.parameters.loudness.has_reference_level = true;
  config.parameters.loudness.low_boost = 6.0;
  config.parameters.loudness.has_low_boost = true;
  config.parameters.loudness.high_boost = 6.0;
  config.parameters.loudness.has_high_boost = true;

  config_error_t err;
  config_error_init(&err);

  processing_parameters_t* proc_params = processing_parameters_create(2, 2);
  processing_parameters_set_current_volume_for_fader(proc_params, -30.0,
                                                     FADER_MAIN);

  filter_t* filter =
      filter_create("loudness1", &config, 48000, 256, proc_params, &err);
  ASSERT_TRUE(filter != NULL);

  double buffer[256];
  for (size_t i = 0; i < 256; i++) {
    buffer[i] = sin(2.0 * M_PI * 50.0 * (double)i / 48000.0);
  }

  filter_process(filter, buffer, 256);

  // High-level volume: no boost active
  processing_parameters_set_current_volume_for_fader(proc_params, 0.0,
                                                     FADER_MAIN);
  filter_process(filter, buffer, 256);

  filter_free(filter);
  processing_parameters_free(proc_params);
}

TEST(LoudnessFilterMidbandAttenuation) {
  filter_config_t config;
  memset(&config, 0, sizeof(config));
  config.type = FILTER_TYPE_LOUDNESS;
  config.parameters.loudness.reference_level = 0.0;
  config.parameters.loudness.has_reference_level = true;
  config.parameters.loudness.low_boost = 10.0;
  config.parameters.loudness.has_low_boost = true;
  config.parameters.loudness.attenuate_mid = true;

  config_error_t err;
  config_error_init(&err);

  processing_parameters_t* proc_params = processing_parameters_create(1, 1);
  processing_parameters_set_current_volume_for_fader(proc_params, -20.0,
                                                     FADER_MAIN);

  filter_t* filter =
      filter_create("loudness2", &config, 44100, 128, proc_params, &err);
  ASSERT_TRUE(filter != NULL);

  double buffer[128];
  memset(buffer, 0, sizeof(buffer));

  filter_process(filter, buffer, 128);

  filter_free(filter);
  processing_parameters_free(proc_params);
}

TEST_MAIN()
