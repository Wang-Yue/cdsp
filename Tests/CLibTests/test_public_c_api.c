#include <stdbool.h>
#include <string.h>

#include "cdsp/cdsp_pub_types.h"
#include "cdsp/config.h"
#include "cdsp/fader.h"
#include "cdsp/general.h"
#include "test_support.h"

TEST(PublicSetConfigValuePointerSafety) {
  dsp_engine_t* engine = cdsp_engine_create();
  ASSERT_TRUE(engine != NULL);

  const char* base_json =
      "{\n"
      "    \"title\": \"Initial\",\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\n"
      "            \"type\": \"RawFile\",\n"
      "            \"filename\": \"/dev/null\",\n"
      "            \"format\": \"S16_LE\",\n"
      "            \"channels\": 2\n"
      "        },\n"
      "        \"playback\": {\n"
      "            \"type\": \"File\",\n"
      "            \"filename\": \"/dev/null\",\n"
      "            \"format\": \"S16_LE\",\n"
      "            \"channels\": 2\n"
      "        }\n"
      "    }\n"
      "}\n";

  cdsp_backend_error_t err;
  memset(&err, 0, sizeof(err));
  bool ok = cdsp_set_config_json(engine, base_json, &err);
  ASSERT_TRUE(ok);

  // 1. Valid pointer update
  ok = cdsp_set_config_value(engine, "/title", "\"Updated\"", &err);
  ASSERT_TRUE(ok);
  char* title = cdsp_get_config_title(engine);
  ASSERT_TRUE(title != NULL);
  ASSERT_STR_EQ("Updated", title);
  free(title);

  // 2. Invalid pointer with non-existent intermediate path after object
  ok = cdsp_set_config_value(engine, "/devices/capture/nonexistent/field",
                             "\"val\"", &err);
  ASSERT_FALSE(ok);

  // Verify that capture object was not corrupted
  char* cap_type = cdsp_get_config_value(engine, "/devices/capture/type");
  ASSERT_TRUE(cap_type != NULL);
  ASSERT_STR_EQ("\"RawFile\"", cap_type);
  free(cap_type);

  cdsp_stop(engine);
  cdsp_engine_free(engine);
}

TEST(PublicGeneralAndDeviceTypes) {
  const char* ver = cdsp_get_version();
  ASSERT_TRUE(ver != NULL);
  ASSERT_STR_EQ("5.0.0", ver);

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
