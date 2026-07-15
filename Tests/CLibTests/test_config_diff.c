#if defined(__linux__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif
#include <stdlib.h>
#include <string.h>

#include "Config/config_diff.h"
#include "Config/configuration.h"
#include "test_support.h"

static const char* base_json =
    "{\n"
    "    \"devices\": {\n"
    "        \"samplerate\": 44100,\n"
    "        \"chunksize\": 1024,\n"
    "        \"capture\": {\n"
    "            \"type\": \"File\",\n"
    "            \"channels\": 2\n"
    "        },\n"
    "        \"playback\": {\n"
    "            \"type\": \"File\",\n"
    "            \"channels\": 2\n"
    "        }\n"
    "    },\n"
    "    \"filters\": {\n"
    "        \"my_gain\": {\n"
    "            \"type\": \"Gain\",\n"
    "            \"parameters\": {\n"
    "                \"gain\": -6.0\n"
    "            }\n"
    "        }\n"
    "    },\n"
    "    \"pipeline\": [\n"
    "        {\n"
    "            \"type\": \"Filter\",\n"
    "            \"channel\": 0,\n"
    "            \"names\": [\"my_gain\"]\n"
    "        }\n"
    "    ]\n"
    "}";

TEST(ConfigDiffEqual) {
  dsp_config_t* c1 = NULL;
  dsp_config_t* c2 = NULL;
  config_error_t err;
  config_error_init(&err);

  int r1 = dsp_config_parse_json(base_json, &c1, &err);
  if (r1 != 0) printf("PARSER FAIL MSG: %s\n", err.message);
  int r2 = dsp_config_parse_json(base_json, &c2, &err);
  ASSERT_EQ(0, r1);
  ASSERT_EQ(0, r2);

  config_change_t* change = config_change_create();
  ASSERT_TRUE(change != NULL);
  config_change_type_t res = config_diff(c1, c2, change);
  ASSERT_EQ(CONFIG_CHANGE_NONE, res);

  size_t filters_count = 0;
  char** filters = config_change_take_filters(change, &filters_count);
  ASSERT_EQ(0, filters_count);
  ASSERT_TRUE(filters == NULL);

  config_change_free(change);
  dsp_config_free(c1);
  dsp_config_free(c2);
}

TEST(ConfigDiffDevices) {
  const char* json_diff =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 48000,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\n"
      "            \"type\": \"File\",\n"
      "            \"channels\": 2\n"
      "        },\n"
      "        \"playback\": {\n"
      "            \"type\": \"File\",\n"
      "            \"channels\": 2\n"
      "        }\n"
      "    }\n"
      "}";

  dsp_config_t* c1 = NULL;
  dsp_config_t* c2 = NULL;
  config_error_t err;
  config_error_init(&err);

  int r1 = dsp_config_parse_json(base_json, &c1, &err);
  int r2 = dsp_config_parse_json(json_diff, &c2, &err);
  ASSERT_EQ(0, r1);
  ASSERT_EQ(0, r2);

  config_change_t* change = config_change_create();
  ASSERT_TRUE(change != NULL);
  config_change_type_t res = config_diff(c1, c2, change);
  ASSERT_EQ(CONFIG_CHANGE_DEVICES, res);

  config_change_free(change);
  dsp_config_free(c1);
  dsp_config_free(c2);
}

TEST(ConfigDiffFilterParams) {
  const char* json_diff =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\n"
      "            \"type\": \"File\",\n"
      "            \"channels\": 2\n"
      "        },\n"
      "        \"playback\": {\n"
      "            \"type\": \"File\",\n"
      "            \"channels\": 2\n"
      "        }\n"
      "    },\n"
      "    \"filters\": {\n"
      "        \"my_gain\": {\n"
      "            \"type\": \"Gain\",\n"
      "            \"parameters\": {\n"
      "                \"gain\": -3.0\n"
      "            }\n"
      "        }\n"
      "    },\n"
      "    \"pipeline\": [\n"
      "        {\n"
      "            \"type\": \"Filter\",\n"
      "            \"channel\": 0,\n"
      "            \"names\": [\"my_gain\"]\n"
      "        }\n"
      "    ]\n"
      "}";

  dsp_config_t* c1 = NULL;
  dsp_config_t* c2 = NULL;
  config_error_t err;
  config_error_init(&err);

  int r1 = dsp_config_parse_json(base_json, &c1, &err);
  int r2 = dsp_config_parse_json(json_diff, &c2, &err);
  ASSERT_EQ(0, r1);
  ASSERT_EQ(0, r2);

  config_change_t* change = config_change_create();
  ASSERT_TRUE(change != NULL);
  config_change_type_t res = config_diff(c1, c2, change);
  ASSERT_EQ(CONFIG_CHANGE_FILTER_PARAMETERS, res);

  size_t filters_count = 0;
  char** filters = config_change_take_filters(change, &filters_count);
  ASSERT_EQ(1, filters_count);
  ASSERT_STR_EQ("my_gain", filters[0]);

  // Clean up returned name list since ownership was transferred to us
  for (size_t i = 0; i < filters_count; i++) {
    free(filters[i]);
  }
  free(filters);

  config_change_free(change);
  dsp_config_free(c1);
  dsp_config_free(c2);
}

TEST(ConfigDiffDictionaryOrderIndependent) {
  const char* json_order1 =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\"type\": \"File\", \"channels\": 2},\n"
      "        \"playback\": {\"type\": \"File\", \"channels\": 2}\n"
      "    },\n"
      "    \"filters\": {\n"
      "        \"first_filter\": {\n"
      "            \"type\": \"Gain\",\n"
      "            \"parameters\": {\"gain\": -6.0}\n"
      "        },\n"
      "        \"second_filter\": {\n"
      "            \"type\": \"Gain\",\n"
      "            \"parameters\": {\"gain\": -3.0}\n"
      "        }\n"
      "    },\n"
      "    \"pipeline\": [\n"
      "        {\"type\": \"Filter\", \"channel\": 0, \"names\": [\"first_filter\"]},\n"
      "        {\"type\": \"Filter\", \"channel\": 1, \"names\": [\"second_filter\"]}\n"
      "    ]\n"
      "}";

  const char* json_order2 =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\"type\": \"File\", \"channels\": 2},\n"
      "        \"playback\": {\"type\": \"File\", \"channels\": 2}\n"
      "    },\n"
      "    \"filters\": {\n"
      "        \"second_filter\": {\n"
      "            \"type\": \"Gain\",\n"
      "            \"parameters\": {\"gain\": -3.0}\n"
      "        },\n"
      "        \"first_filter\": {\n"
      "            \"type\": \"Gain\",\n"
      "            \"parameters\": {\"gain\": -6.0}\n"
      "        }\n"
      "    },\n"
      "    \"pipeline\": [\n"
      "        {\"type\": \"Filter\", \"channel\": 0, \"names\": [\"first_filter\"]},\n"
      "        {\"type\": \"Filter\", \"channel\": 1, \"names\": [\"second_filter\"]}\n"
      "    ]\n"
      "}";

  dsp_config_t* c1 = NULL;
  dsp_config_t* c2 = NULL;
  config_error_t err;
  config_error_init(&err);

  int r1 = dsp_config_parse_json(json_order1, &c1, &err);
  int r2 = dsp_config_parse_json(json_order2, &c2, &err);
  ASSERT_EQ(0, r1);
  ASSERT_EQ(0, r2);

  config_change_t* change = config_change_create();
  ASSERT_TRUE(change != NULL);
  config_change_type_t res = config_diff(c1, c2, change);
  ASSERT_EQ(CONFIG_CHANGE_NONE, res);

  config_change_free(change);
  dsp_config_free(c1);
  dsp_config_free(c2);
}

TEST_MAIN()
