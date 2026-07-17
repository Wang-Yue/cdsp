#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Public/general.h"
#include "Public/signal_levels.h"
#include "Public/state.h"
#include "Public/volume.h"
#include "test_support.h"

TEST(PublicGeneralAndDeviceTypes) {
  const char* ver = cdsp_get_version();
  ASSERT_TRUE(ver != NULL);
  ASSERT_TRUE(strlen(ver) > 0);

  char** playback_types = NULL;
  char** capture_types = NULL;
  size_t playback_count = 0;
  size_t capture_count = 0;

  cdsp_get_supported_device_types(&playback_types, &playback_count,
                                  &capture_types, &capture_count);

  ASSERT_TRUE(playback_count > 0);
  ASSERT_TRUE(capture_count > 0);
  ASSERT_TRUE(playback_types != NULL);
  ASSERT_TRUE(capture_types != NULL);

  cdsp_free_device_types(playback_types, playback_count);
  cdsp_free_device_types(capture_types, capture_count);

  cdsp_set_log_level("debug");
  cdsp_set_log_level("info");
}

TEST(PublicEngineLifecycleAndVolume) {
  dsp_engine_t* engine = cdsp_engine_create();
  ASSERT_TRUE(engine != NULL);

  cdsp_engine_poll(engine);

  cdsp_set_volume(engine, -12.5f, true);
  float vol = cdsp_get_volume(engine);
  ASSERT_NEAR(vol, -12.5f, 1e-4);

  cdsp_set_mute(engine, true);
  ASSERT_TRUE(cdsp_get_mute(engine));

  cdsp_set_mute(engine, false);
  ASSERT_FALSE(cdsp_get_mute(engine));

  cdsp_set_fader_volume(engine, CDSP_FADER_AUX1, -6.0f, true);
  float aux1_vol = cdsp_get_fader_volume(engine, CDSP_FADER_AUX1);
  ASSERT_NEAR(aux1_vol, -6.0f, 1e-4);

  cdsp_stop(engine);
  cdsp_engine_free(engine);
}

TEST_MAIN()
