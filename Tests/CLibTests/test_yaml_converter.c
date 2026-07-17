#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Config/cJSON.h"
#include "Config/cdsp_yaml.h"
#include "Public/config.h"
#include "Public/general.h"
#include "test_support.h"

TEST(YamlConverter_JsonToYaml) {
  const char* json_raw =
      "{\n"
      "  \"devices\": {\n"
      "    \"samplerate\": 44100,\n"
      "    \"chunksize\": 1024,\n"
      "    \"capture\": {\"type\": \"File\", \"channels\": 2},\n"
      "    \"playback\": {\"type\": \"File\", \"channels\": 2}\n"
      "  },\n"
      "  \"enable_volume\": true\n"
      "}";

  cJSON* json = cJSON_Parse(json_raw);
  ASSERT_TRUE(json != NULL);

  char* yaml = cdsp_json_to_yaml(json);
  ASSERT_TRUE(yaml != NULL);
  ASSERT_TRUE(strstr(yaml, "samplerate: 44100") != NULL);
  ASSERT_TRUE(strstr(yaml, "chunksize: 1024") != NULL);
  ASSERT_TRUE(strstr(yaml, "enable_volume: true") != NULL);

  cJSON_Delete(json);
  free(yaml);
}

TEST(YamlConverter_YamlToJson) {
  const char* yaml_raw =
      "devices:\n"
      "  samplerate: 48000\n"
      "  chunksize: 512\n"
      "  capture:\n"
      "    type: File\n"
      "    channels: 2\n"
      "  playback:\n"
      "    type: File\n"
      "    channels: 2\n"
      "enable_volume: false\n";

  char* err = NULL;
  cJSON* json = cdsp_yaml_to_json(yaml_raw, &err);
  ASSERT_TRUE(json != NULL);
  ASSERT_TRUE(err == NULL);

  cJSON* devices = cJSON_GetObjectItem(json, "devices");
  ASSERT_TRUE(devices != NULL);

  cJSON* sr = cJSON_GetObjectItem(devices, "samplerate");
  ASSERT_TRUE(sr != NULL);
  ASSERT_EQ(48000, sr->valueint);

  cJSON* vol = cJSON_GetObjectItem(json, "enable_volume");
  ASSERT_TRUE(vol != NULL);
  ASSERT_FALSE(cJSON_IsTrue(vol));

  cJSON_Delete(json);
}

TEST(YamlConverter_RoundTrip) {
  const char* yaml_raw =
      "devices:\n"
      "  samplerate: 96000\n"
      "  chunksize: 2048\n"
      "  capture:\n"
      "    type: File\n"
      "    channels: 4\n"
      "  playback:\n"
      "    type: File\n"
      "    channels: 4\n";

  char* err = NULL;
  cJSON* parsed = cdsp_yaml_to_json(yaml_raw, &err);
  ASSERT_TRUE(parsed != NULL);

  char* emitted_yaml = cdsp_json_to_yaml(parsed);
  ASSERT_TRUE(emitted_yaml != NULL);

  cJSON* re_parsed = cdsp_yaml_to_json(emitted_yaml, &err);
  ASSERT_TRUE(re_parsed != NULL);

  cJSON* devices1 = cJSON_GetObjectItem(parsed, "devices");
  cJSON* devices2 = cJSON_GetObjectItem(re_parsed, "devices");
  ASSERT_EQ(cJSON_GetObjectItem(devices1, "samplerate")->valueint,
            cJSON_GetObjectItem(devices2, "samplerate")->valueint);

  cJSON_Delete(parsed);
  cJSON_Delete(re_parsed);
  free(emitted_yaml);
}

TEST(YamlConverter_EnginePublicAPI) {
  dsp_engine_t* engine = cdsp_engine_create();
  ASSERT_TRUE(engine != NULL);

  const char* yaml_config =
      "devices:\n"
      "  samplerate: 44100\n"
      "  chunksize: 1024\n"
      "  capture:\n"
      "    type: File\n"
      "    channels: 2\n"
      "    filename: \"/dev/null\"\n"
      "    format: S16LE\n"
      "  playback:\n"
      "    type: File\n"
      "    channels: 2\n"
      "    filename: \"/dev/null\"\n"
      "    format: S16LE\n";

  cdsp_backend_error_t berr;
  bool set_ok = cdsp_set_config_yaml(engine, yaml_config, &berr);
  ASSERT_TRUE(set_ok);

  char* active_yaml = NULL;
  bool get_ok = cdsp_get_active_config_yaml(engine, &active_yaml);
  ASSERT_TRUE(get_ok);
  ASSERT_TRUE(active_yaml != NULL);
  ASSERT_TRUE(strstr(active_yaml, "samplerate:") != NULL);

  free(active_yaml);
  cdsp_engine_free(engine);
}

TEST(YamlConverter_Validation) {
  const char* yaml_config =
      "devices:\n"
      "  samplerate: 44100\n"
      "  chunksize: 1024\n"
      "  capture:\n"
      "    type: File\n"
      "    channels: 2\n"
      "    filename: \"/dev/null\"\n"
      "    format: S16LE\n"
      "  playback:\n"
      "    type: File\n"
      "    channels: 2\n"
      "    filename: \"/dev/null\"\n"
      "    format: S16LE\n";

  char* result_yaml = NULL;
  bool is_error = true;
  bool valid = cdsp_validate_config_yaml(yaml_config, &result_yaml, &is_error);
  ASSERT_TRUE(valid);
  ASSERT_FALSE(is_error);
  ASSERT_TRUE(result_yaml != NULL);
  ASSERT_TRUE(strstr(result_yaml, "samplerate:") != NULL);

  free(result_yaml);
}

TEST(YamlConverter_KeyArrayParsing) {
  const char* yaml_raw =
      "pipeline:\n"
      "  - type: Filter\n"
      "    channel: 0\n"
      "    names:\n"
      "      - filter1\n"
      "      - filter2\n";

  char* err = NULL;
  cJSON* json = cdsp_yaml_to_json(yaml_raw, &err);
  ASSERT_TRUE(json != NULL);
  ASSERT_TRUE(err == NULL);

  cJSON* pipe = cJSON_GetObjectItem(json, "pipeline");
  ASSERT_TRUE(pipe != NULL && cJSON_IsArray(pipe));

  cJSON* step = cJSON_GetArrayItem(pipe, 0);
  ASSERT_TRUE(step != NULL && cJSON_IsObject(step));

  cJSON* names = cJSON_GetObjectItem(step, "names");
  ASSERT_TRUE(names != NULL);
  ASSERT_TRUE(cJSON_IsArray(names));
  ASSERT_EQ(2, cJSON_GetArraySize(names));

  cJSON_Delete(json);
}

TEST_MAIN()
