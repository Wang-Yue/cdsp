#if defined(__linux__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif
#include <stdio.h>
#include <stdlib.h>

#include "Config/config_diff.h"
#include "Config/config_error.h"
#include "Config/configuration.h"
#include "test_support.h"

static const char* base_json =
    "{\n"
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
    "            \"channels\": [0],\n"
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

  config_change_type_t res = config_diff(c1, c2);
  ASSERT_EQ(CONFIG_CHANGE_NONE, res);

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
      "}";

  dsp_config_t* c1 = NULL;
  dsp_config_t* c2 = NULL;
  config_error_t err;
  config_error_init(&err);

  int r1 = dsp_config_parse_json(base_json, &c1, &err);
  int r2 = dsp_config_parse_json(json_diff, &c2, &err);
  ASSERT_EQ(0, r1);
  ASSERT_EQ(0, r2);

  config_change_type_t res = config_diff(c1, c2);
  ASSERT_EQ(CONFIG_CHANGE_DEVICES, res);

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
      "            \"channels\": [0],\n"
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

  config_change_type_t res = config_diff(c1, c2);
  ASSERT_EQ(CONFIG_CHANGE_FILTER_PARAMETERS, res);

  dsp_config_free(c1);
  dsp_config_free(c2);
}

TEST(ConfigDiffDictionaryOrderIndependent) {
  const char* json_order1 =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\"type\": \"RawFile\", \"filename\": "
      "\"/dev/null\", \"format\": \"S16_LE\", \"channels\": 2},\n"
      "        \"playback\": {\"type\": \"File\", \"filename\": \"/dev/null\", "
      "\"format\": \"S16_LE\", \"channels\": 2}\n"
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
      "        {\"type\": \"Filter\", \"channels\": [0], \"names\": "
      "[\"first_filter\"]},\n"
      "        {\"type\": \"Filter\", \"channels\": [1], \"names\": "
      "[\"second_filter\"]}\n"
      "    ]\n"
      "}";

  const char* json_order2 =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\"type\": \"RawFile\", \"filename\": "
      "\"/dev/null\", \"format\": \"S16_LE\", \"channels\": 2},\n"
      "        \"playback\": {\"type\": \"File\", \"filename\": \"/dev/null\", "
      "\"format\": \"S16_LE\", \"channels\": 2}\n"
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
      "        {\"type\": \"Filter\", \"channels\": [0], \"names\": "
      "[\"first_filter\"]},\n"
      "        {\"type\": \"Filter\", \"channels\": [1], \"names\": "
      "[\"second_filter\"]}\n"
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

  config_change_type_t res = config_diff(c1, c2);
  ASSERT_EQ(CONFIG_CHANGE_NONE, res);

  dsp_config_free(c1);
  dsp_config_free(c2);
}

TEST(ConfigDiffMixerDescriptionAndLabels) {
  const char* base_json =
      "{\n"
      "    \"devices\": {\"samplerate\": 48000, \"chunksize\": 1024,\n"
      "        \"capture\": {\"type\": \"RawFile\", \"filename\": "
      "\"/dev/null\", \"format\": \"S16_LE\", \"channels\": 2},\n"
      "        \"playback\": {\"type\": \"File\", \"filename\": \"/dev/null\", "
      "\"format\": \"S16_LE\", \"channels\": 2}\n"
      "    },\n"
      "    \"mixers\": {\n"
      "        \"mymixer\": {\n"
      "            \"channels\": {\"in\": 2, \"out\": 2},\n"
      "            \"mapping\": [\n"
      "                {\"dest\": 0, \"sources\": [{\"channel\": 0, \"gain\": "
      "0.0, \"inverted\": false, \"mute\": false}]},\n"
      "                {\"dest\": 1, \"sources\": [{\"channel\": 1, \"gain\": "
      "0.0, \"inverted\": false, \"mute\": false}]}\n"
      "            ],\n"
      "            \"description\": \"Original mixer\",\n"
      "            \"labels\": [\"Left\", \"Right\"]\n"
      "        }\n"
      "    },\n"
      "    \"pipeline\": [\n"
      "        {\"type\": \"Mixer\", \"name\": \"mymixer\"}\n"
      "    ]\n"
      "}";

  const char* diff_desc_json =
      "{\n"
      "    \"devices\": {\"samplerate\": 48000, \"chunksize\": 1024,\n"
      "        \"capture\": {\"type\": \"RawFile\", \"filename\": "
      "\"/dev/null\", \"format\": \"S16_LE\", \"channels\": 2},\n"
      "        \"playback\": {\"type\": \"File\", \"filename\": \"/dev/null\", "
      "\"format\": \"S16_LE\", \"channels\": 2}\n"
      "    },\n"
      "    \"mixers\": {\n"
      "        \"mymixer\": {\n"
      "            \"channels\": {\"in\": 2, \"out\": 2},\n"
      "            \"mapping\": [\n"
      "                {\"dest\": 0, \"sources\": [{\"channel\": 0, \"gain\": "
      "0.0, \"inverted\": false, \"mute\": false}]},\n"
      "                {\"dest\": 1, \"sources\": [{\"channel\": 1, \"gain\": "
      "0.0, \"inverted\": false, \"mute\": false}]}\n"
      "            ],\n"
      "            \"description\": \"Modified description\",\n"
      "            \"labels\": [\"Left\", \"Right\"]\n"
      "        }\n"
      "    },\n"
      "    \"pipeline\": [\n"
      "        {\"type\": \"Mixer\", \"name\": \"mymixer\"}\n"
      "    ]\n"
      "}";

  const char* diff_labels_json =
      "{\n"
      "    \"devices\": {\"samplerate\": 48000, \"chunksize\": 1024,\n"
      "        \"capture\": {\"type\": \"RawFile\", \"filename\": "
      "\"/dev/null\", \"format\": \"S16_LE\", \"channels\": 2},\n"
      "        \"playback\": {\"type\": \"File\", \"filename\": \"/dev/null\", "
      "\"format\": \"S16_LE\", \"channels\": 2}\n"
      "    },\n"
      "    \"mixers\": {\n"
      "        \"mymixer\": {\n"
      "            \"channels\": {\"in\": 2, \"out\": 2},\n"
      "            \"mapping\": [\n"
      "                {\"dest\": 0, \"sources\": [{\"channel\": 0, \"gain\": "
      "0.0, \"inverted\": false, \"mute\": false}]},\n"
      "                {\"dest\": 1, \"sources\": [{\"channel\": 1, \"gain\": "
      "0.0, \"inverted\": false, \"mute\": false}]}\n"
      "            ],\n"
      "            \"description\": \"Original mixer\",\n"
      "            \"labels\": [\"L\", \"R\"]\n"
      "        }\n"
      "    },\n"
      "    \"pipeline\": [\n"
      "        {\"type\": \"Mixer\", \"name\": \"mymixer\"}\n"
      "    ]\n"
      "}";

  dsp_config_t *c_base = NULL, *c_desc = NULL, *c_labels = NULL;
  config_error_t err;
  config_error_init(&err);

  ASSERT_EQ(0, dsp_config_parse_json(base_json, &c_base, &err));
  ASSERT_EQ(0, dsp_config_parse_json(diff_desc_json, &c_desc, &err));
  ASSERT_EQ(0, dsp_config_parse_json(diff_labels_json, &c_labels, &err));

  // Description change must detect mixer change
  config_change_type_t res1 = config_diff(c_base, c_desc);
  ASSERT_EQ(CONFIG_CHANGE_MIXER_PARAMETERS, res1);

  // Labels change must detect mixer change
  config_change_type_t res2 = config_diff(c_base, c_labels);
  ASSERT_EQ(CONFIG_CHANGE_MIXER_PARAMETERS, res2);

  dsp_config_free(c_base);
  dsp_config_free(c_desc);
  dsp_config_free(c_labels);
}

#if defined(ENABLE_PIPEWIRE)
TEST(ConfigDiffPipeWireAutoconnectTo) {
  const char* cfg_empty_str =
      "{\n"
      "    \"devices\": {\"samplerate\": 48000, \"chunksize\": 1024,\n"
      "        \"capture\": {\"type\": \"PipeWire\", \"channels\": 2, "
      "\"autoconnect_to\": \"\"},\n"
      "        \"playback\": {\"type\": \"PipeWire\", \"channels\": 2, "
      "\"autoconnect_to\": \"\"}\n"
      "    }\n"
      "}";

  const char* cfg_null_val =
      "{\n"
      "    \"devices\": {\"samplerate\": 48000, \"chunksize\": 1024,\n"
      "        \"capture\": {\"type\": \"PipeWire\", \"channels\": 2, "
      "\"autoconnect_to\": null},\n"
      "        \"playback\": {\"type\": \"PipeWire\", \"channels\": 2, "
      "\"autoconnect_to\": null}\n"
      "    }\n"
      "}";

  const char* cfg_target =
      "{\n"
      "    \"devices\": {\"samplerate\": 48000, \"chunksize\": 1024,\n"
      "        \"capture\": {\"type\": \"PipeWire\", \"channels\": 2, "
      "\"autoconnect_to\": \"alsa_input\"},\n"
      "        \"playback\": {\"type\": \"PipeWire\", \"channels\": 2, "
      "\"autoconnect_to\": \"alsa_output\"}\n"
      "    }\n"
      "}";

  dsp_config_t *c_empty = NULL, *c_null = NULL, *c_target = NULL;
  config_error_t err;
  config_error_init(&err);

  ASSERT_EQ(0, dsp_config_parse_json(cfg_empty_str, &c_empty, &err));
  ASSERT_EQ(0, dsp_config_parse_json(cfg_null_val, &c_null, &err));
  ASSERT_EQ(0, dsp_config_parse_json(cfg_target, &c_target, &err));

  // Changing autoconnect_to from "" to null must trigger full reload
  config_change_type_t res1 = config_diff(c_empty, c_null);
  ASSERT_EQ(CONFIG_CHANGE_DEVICES, res1);

  // Changing autoconnect_to from null to "" must trigger full reload
  config_change_type_t res2 = config_diff(c_null, c_empty);
  ASSERT_EQ(CONFIG_CHANGE_DEVICES, res2);

  // Changing autoconnect_to from "" to target node must trigger full reload
  config_change_type_t res3 = config_diff(c_empty, c_target);
  ASSERT_EQ(CONFIG_CHANGE_DEVICES, res3);

  dsp_config_free(c_empty);
  dsp_config_free(c_null);
  dsp_config_free(c_target);
}
#endif

TEST_MAIN()
