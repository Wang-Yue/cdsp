#include <cjson/cJSON.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cdsp/cdsp_pub_types.h"
#include "cdsp/config.h"
#include "cdsp/general.h"
#include "config/cdsp_yaml.h"
#include "test_support.h"

TEST(YamlConverter_JsonToYaml) {
  const char *json_raw =
      "{\n"
      "  \"devices\": {\n"
      "    \"samplerate\": 44100,\n"
      "    \"chunksize\": 1024,\n"
      "    \"capture\": {\"type\": \"File\", \"channels\": 2},\n"
      "    \"playback\": {\"type\": \"File\", \"channels\": 2}\n"
      "  },\n"
      "  \"enable_volume\": true\n"
      "}";

  cJSON *json = cJSON_Parse(json_raw);
  ASSERT_TRUE(json != NULL);

  char *yaml = cdsp_json_to_yaml(json);
  ASSERT_TRUE(yaml != NULL);
  ASSERT_TRUE(strstr(yaml, "samplerate: 44100") != NULL);
  ASSERT_TRUE(strstr(yaml, "chunksize: 1024") != NULL);
  ASSERT_TRUE(strstr(yaml, "enable_volume: true") != NULL);

  cJSON_Delete(json);
  free(yaml);
}

TEST(YamlConverter_YamlToJson) {
  const char *yaml_raw = "devices:\n"
                         "  samplerate: 48000\n"
                         "  chunksize: 512\n"
                         "  capture:\n"
                         "    type: File\n"
                         "    channels: 2\n"
                         "  playback:\n"
                         "    type: File\n"
                         "    channels: 2\n"
                         "enable_volume: false\n";

  char *err = NULL;
  cJSON *json = cdsp_yaml_to_json(yaml_raw, &err);
  ASSERT_TRUE(json != NULL);
  ASSERT_TRUE(err == NULL);

  cJSON *devices = cJSON_GetObjectItem(json, "devices");
  ASSERT_TRUE(devices != NULL);

  cJSON *sr = cJSON_GetObjectItem(devices, "samplerate");
  ASSERT_TRUE(sr != NULL);
  ASSERT_EQ(48000, sr->valueint);

  cJSON *vol = cJSON_GetObjectItem(json, "enable_volume");
  ASSERT_TRUE(vol != NULL);
  ASSERT_FALSE(cJSON_IsTrue(vol));

  cJSON_Delete(json);
}

TEST(YamlConverter_RoundTrip) {
  const char *yaml_raw = "devices:\n"
                         "  samplerate: 96000\n"
                         "  chunksize: 2048\n"
                         "  capture:\n"
                         "    type: File\n"
                         "    channels: 4\n"
                         "  playback:\n"
                         "    type: File\n"
                         "    channels: 4\n";

  char *err = NULL;
  cJSON *parsed = cdsp_yaml_to_json(yaml_raw, &err);
  ASSERT_TRUE(parsed != NULL);

  char *emitted_yaml = cdsp_json_to_yaml(parsed);
  ASSERT_TRUE(emitted_yaml != NULL);

  cJSON *re_parsed = cdsp_yaml_to_json(emitted_yaml, &err);
  ASSERT_TRUE(re_parsed != NULL);

  cJSON *devices1 = cJSON_GetObjectItem(parsed, "devices");
  cJSON *devices2 = cJSON_GetObjectItem(re_parsed, "devices");
  ASSERT_EQ(cJSON_GetObjectItem(devices1, "samplerate")->valueint,
            cJSON_GetObjectItem(devices2, "samplerate")->valueint);

  cJSON_Delete(parsed);
  cJSON_Delete(re_parsed);
  free(emitted_yaml);
}

TEST(YamlConverter_EnginePublicAPI) {
  dsp_engine_t *engine = cdsp_engine_create();
  ASSERT_TRUE(engine != NULL);

  const char *yaml_config = "devices:\n"
                            "  samplerate: 44100\n"
                            "  chunksize: 1024\n"
                            "  capture:\n"
                            "    type: RawFile\n"
                            "    channels: 2\n"
                            "    filename: \"/dev/null\"\n"
                            "    format: S16_LE\n"
                            "  playback:\n"
                            "    type: File\n"
                            "    channels: 2\n"
                            "    filename: \"/dev/null\"\n"
                            "    format: S16_LE\n";

  cdsp_backend_error_t berr;
  bool set_ok = cdsp_set_config_yaml(engine, yaml_config, &berr);
  ASSERT_TRUE(set_ok);

  char *active_yaml = NULL;
  bool get_ok = cdsp_get_active_config_yaml(engine, &active_yaml);
  ASSERT_TRUE(get_ok);
  ASSERT_TRUE(active_yaml != NULL);
  ASSERT_TRUE(strstr(active_yaml, "samplerate:") != NULL);

  free(active_yaml);
  cdsp_engine_free(engine);
}

TEST(YamlConverter_Validation) {
  const char *yaml_config = "devices:\n"
                            "  samplerate: 44100\n"
                            "  chunksize: 1024\n"
                            "  capture:\n"
                            "    type: RawFile\n"
                            "    channels: 2\n"
                            "    filename: \"/dev/null\"\n"
                            "    format: S16_LE\n"
                            "  playback:\n"
                            "    type: File\n"
                            "    channels: 2\n"
                            "    filename: \"/dev/null\"\n"
                            "    format: S16_LE\n";

  char *result_yaml = NULL;
  cdsp_config_error_type_t err_type = CDSP_CONFIG_ERR_PARSE;
  bool valid = cdsp_validate_config_yaml(yaml_config, &result_yaml, &err_type);
  ASSERT_TRUE(valid);
  ASSERT_EQ(CDSP_CONFIG_ERR_NONE, err_type);
  ASSERT_TRUE(result_yaml != NULL);
  ASSERT_TRUE(strstr(result_yaml, "samplerate:") != NULL);

  free(result_yaml);
}

TEST(YamlConverter_KeyArrayParsing) {
  const char *yaml_raw = "pipeline:\n"
                         "  - type: Filter\n"
                         "    channel: 0\n"
                         "    names:\n"
                         "      - filter1\n"
                         "      - filter2\n";

  char *err = NULL;
  cJSON *json = cdsp_yaml_to_json(yaml_raw, &err);
  ASSERT_TRUE(json != NULL);
  ASSERT_TRUE(err == NULL);

  cJSON *pipe = cJSON_GetObjectItem(json, "pipeline");
  ASSERT_TRUE(pipe != NULL && cJSON_IsArray(pipe));

  cJSON *step = cJSON_GetArrayItem(pipe, 0);
  ASSERT_TRUE(step != NULL && cJSON_IsObject(step));

  cJSON *names = cJSON_GetObjectItem(step, "names");
  ASSERT_TRUE(names != NULL);
  ASSERT_TRUE(cJSON_IsArray(names));
  ASSERT_EQ(2, cJSON_GetArraySize(names));

  cJSON_Delete(json);
}

TEST(YamlConverter_QuotedKeyListItems) {
  const char *yaml_raw = "pipeline:\n"
                         "  - \"type\": Filter\n"
                         "    \"channel\": 0\n"
                         "    \"names\":\n"
                         "      - \"filter1:with_colon\"\n"
                         "      - 'filter2'\n";

  char *err = NULL;
  cJSON *json = cdsp_yaml_to_json(yaml_raw, &err);
  ASSERT_TRUE(json != NULL);
  ASSERT_TRUE(err == NULL);

  cJSON *pipe = cJSON_GetObjectItem(json, "pipeline");
  ASSERT_TRUE(pipe != NULL && cJSON_IsArray(pipe));

  cJSON *step = cJSON_GetArrayItem(pipe, 0);
  ASSERT_TRUE(step != NULL && cJSON_IsObject(step));

  cJSON *type_node = cJSON_GetObjectItem(step, "type");
  ASSERT_TRUE(type_node != NULL && cJSON_IsString(type_node));
  ASSERT_STR_EQ("Filter", type_node->valuestring);

  cJSON *names = cJSON_GetObjectItem(step, "names");
  ASSERT_TRUE(names != NULL && cJSON_IsArray(names));
  ASSERT_EQ(2, cJSON_GetArraySize(names));
  ASSERT_STR_EQ("filter1:with_colon",
                cJSON_GetArrayItem(names, 0)->valuestring);
  ASSERT_STR_EQ("filter2", cJSON_GetArrayItem(names, 1)->valuestring);

  cJSON_Delete(json);
}

TEST(YamlConverter_FlowSequence) {
  char *err = NULL;
  cJSON *json = cdsp_yaml_to_json("names: [Bass, Treble]", &err);
  ASSERT_TRUE(json != NULL);
  cJSON *names = cJSON_GetObjectItem(json, "names");
  ASSERT_TRUE(names != NULL && cJSON_IsArray(names));
  ASSERT_EQ(2, cJSON_GetArraySize(names));
  ASSERT_STR_EQ("Bass", cJSON_GetArrayItem(names, 0)->valuestring);
  ASSERT_STR_EQ("Treble", cJSON_GetArrayItem(names, 1)->valuestring);
  cJSON_Delete(json);
}

TEST(YamlConverter_FlowMapping) {
  char *err = NULL;
  cJSON *json = cdsp_yaml_to_json("p: {type: Gain, gain: -3}", &err);
  ASSERT_TRUE(json != NULL);
  cJSON *p = cJSON_GetObjectItem(json, "p");
  ASSERT_TRUE(p != NULL && cJSON_IsObject(p));
  ASSERT_STR_EQ("Gain", cJSON_GetObjectItem(p, "type")->valuestring);
  ASSERT_EQ(-3, cJSON_GetObjectItem(p, "gain")->valueint);
  cJSON_Delete(json);
}

TEST(YamlConverter_NestedFlowSequenceInList) {
  char *err = NULL;
  cJSON *json = cdsp_yaml_to_json("- [b, c]", &err);
  ASSERT_TRUE(json != NULL && cJSON_IsArray(json));
  cJSON *inner = cJSON_GetArrayItem(json, 0);
  ASSERT_TRUE(inner != NULL && cJSON_IsArray(inner));
  ASSERT_EQ(2, cJSON_GetArraySize(inner));
  ASSERT_STR_EQ("b", cJSON_GetArrayItem(inner, 0)->valuestring);
  ASSERT_STR_EQ("c", cJSON_GetArrayItem(inner, 1)->valuestring);
  cJSON_Delete(json);
}

TEST(YamlConverter_HashInUnquotedScalar) {
  char *err = NULL;
  cJSON *json = cdsp_yaml_to_json("desc: a#b", &err);
  ASSERT_TRUE(json != NULL);
  cJSON *desc = cJSON_GetObjectItem(json, "desc");
  ASSERT_TRUE(desc != NULL && cJSON_IsString(desc));
  ASSERT_STR_EQ("a#b", desc->valuestring);
  cJSON_Delete(json);
}

TEST(YamlConverter_AnchorsAndAliases) {
  char *err = NULL;
  cJSON *json = cdsp_yaml_to_json("a: &g\n  q: 1\nb: *g", &err);
  ASSERT_TRUE(json != NULL);
  cJSON *a_node = cJSON_GetObjectItem(json, "a");
  cJSON *b_node = cJSON_GetObjectItem(json, "b");
  ASSERT_TRUE(a_node != NULL && cJSON_IsObject(a_node));
  ASSERT_TRUE(b_node != NULL && cJSON_IsObject(b_node));
  ASSERT_EQ(1, cJSON_GetObjectItem(a_node, "q")->valueint);
  ASSERT_EQ(1, cJSON_GetObjectItem(b_node, "q")->valueint);
  cJSON_Delete(json);
}

TEST(YamlConverter_EscapedSingleQuote) {
  char *err = NULL;
  cJSON *json = cdsp_yaml_to_json("key: 'it''s'", &err);
  ASSERT_TRUE(json != NULL);
  cJSON *k = cJSON_GetObjectItem(json, "key");
  ASSERT_TRUE(k != NULL && cJSON_IsString(k));
  ASSERT_STR_EQ("it's", k->valuestring);
  cJSON_Delete(json);
}

TEST(YamlConverter_HexEscapeInDoubleQuote) {
  char *err = NULL;
  cJSON *json = cdsp_yaml_to_json("key: \"a\\x41\"", &err);
  ASSERT_TRUE(json != NULL);
  cJSON *k = cJSON_GetObjectItem(json, "key");
  ASSERT_TRUE(k != NULL && cJSON_IsString(k));
  ASSERT_STR_EQ("aA", k->valuestring);
  cJSON_Delete(json);
}

TEST(YamlConverter_InfinityFloat) {
  char *err = NULL;
  cJSON *json = cdsp_yaml_to_json("val: .inf", &err);
  ASSERT_TRUE(json != NULL);
  cJSON *k = cJSON_GetObjectItem(json, "val");
  ASSERT_TRUE(k != NULL && cJSON_IsNumber(k));
  ASSERT_TRUE(isinf(k->valuedouble));
  cJSON_Delete(json);
}

TEST(YamlConverter_RejectTabIndentation) {
  char *err = NULL;
  cJSON *json = cdsp_yaml_to_json("a:\n\tb: 1", &err);
  ASSERT_TRUE(json == NULL);
  ASSERT_TRUE(err != NULL);
  free(err);
}

TEST(YamlConverter_RejectBadIndentation) {
  char *err = NULL;
  cJSON *json = cdsp_yaml_to_json("a:\n  b: 1\n c: 2", &err);
  ASSERT_TRUE(json == NULL);
  ASSERT_TRUE(err != NULL);
  free(err);
}

TEST(YamlConverter_ValidateConfigFileYaml) {
  char yaml_path[256];
  snprintf(yaml_path, sizeof(yaml_path), "/tmp/test_val_config_%d.yml",
           getpid());
  const char *yaml_config = "devices:\n"
                            "  samplerate: 44100\n"
                            "  chunksize: 1024\n"
                            "  capture:\n"
                            "    type: RawFile\n"
                            "    channels: 2\n"
                            "    filename: \"/dev/null\"\n"
                            "    format: S16_LE\n"
                            "  playback:\n"
                            "    type: File\n"
                            "    channels: 2\n"
                            "    filename: \"/dev/null\"\n"
                            "    format: S16_LE\n";

  FILE *f = fopen(yaml_path, "w");
  ASSERT_TRUE(f != NULL);
  fputs(yaml_config, f);
  fclose(f);

  char *result_str = NULL;
  cdsp_config_error_type_t err_type = CDSP_CONFIG_ERR_PARSE;
  bool valid = cdsp_validate_config_file_with_overrides(
      yaml_path, 48000, 2, NULL, -1, &result_str, &err_type);
  ASSERT_TRUE(valid);
  ASSERT_EQ(CDSP_CONFIG_ERR_NONE, err_type);
  ASSERT_TRUE(result_str != NULL);
  ASSERT_TRUE(strstr(result_str, "samplerate: 48000") != NULL);

  free(result_str);
  remove(yaml_path);
}

TEST(YamlConverter_ValidateConfigFileJson) {
  char json_path[256];
  snprintf(json_path, sizeof(json_path), "/tmp/test_val_config_%d.json",
           getpid());
  const char *json_config =
      "{\n"
      "  \"devices\": {\n"
      "    \"samplerate\": 44100,\n"
      "    \"chunksize\": 1024,\n"
      "    \"capture\": {\"type\": \"RawFile\", \"channels\": 2, \"filename\": "
      "\"/dev/null\", \"format\": \"S16_LE\"},\n"
      "    \"playback\": {\"type\": \"File\", \"channels\": 2, \"filename\": "
      "\"/dev/null\", \"format\": \"S16_LE\"}\n"
      "  }\n"
      "}";

  FILE *f = fopen(json_path, "w");
  ASSERT_TRUE(f != NULL);
  fputs(json_config, f);
  fclose(f);

  char *result_str = NULL;
  cdsp_config_error_type_t err_type = CDSP_CONFIG_ERR_PARSE;
  bool valid = cdsp_validate_config_file_with_overrides(
      json_path, 96000, 2, NULL, -1, &result_str, &err_type);
  ASSERT_TRUE(valid);
  ASSERT_EQ(CDSP_CONFIG_ERR_NONE, err_type);
  ASSERT_TRUE(result_str != NULL);
  ASSERT_TRUE(strstr(result_str, "\"samplerate\":96000") != NULL ||
              strstr(result_str, "\"samplerate\": 96000") != NULL);

  free(result_str);
  remove(json_path);
}

TEST(YamlConverter_EngineSetConfigYamlAndJsonFiles) {
  dsp_engine_t *engine = cdsp_engine_create();
  ASSERT_TRUE(engine != NULL);

  // 1. Test with YAML file
  char yaml_path[256];
  snprintf(yaml_path, sizeof(yaml_path), "/tmp/test_engine_cfg_%d.yml",
           getpid());
  const char *yaml_config = "devices:\n"
                            "  samplerate: 44100\n"
                            "  chunksize: 1024\n"
                            "  capture:\n"
                            "    type: RawFile\n"
                            "    channels: 2\n"
                            "    filename: \"/dev/null\"\n"
                            "    format: S16_LE\n"
                            "  playback:\n"
                            "    type: File\n"
                            "    channels: 2\n"
                            "    filename: \"/dev/null\"\n"
                            "    format: S16_LE\n";

  FILE *f = fopen(yaml_path, "w");
  ASSERT_TRUE(f != NULL);
  fputs(yaml_config, f);
  fclose(f);

  cdsp_backend_error_t berr = {0};
  bool ok_yaml =
      cdsp_engine_set_config_file(engine, yaml_path, 48000, 2, NULL, -1, &berr);
  ASSERT_TRUE(ok_yaml);

  char *path_stored = cdsp_get_config_file_path(engine);
  ASSERT_TRUE(path_stored != NULL);
  ASSERT_STR_EQ(yaml_path, path_stored);
  free(path_stored);

  char *active_yaml = NULL;
  ASSERT_TRUE(cdsp_get_active_config_yaml(engine, &active_yaml));
  ASSERT_TRUE(strstr(active_yaml, "samplerate: 48000") != NULL);
  free(active_yaml);
  remove(yaml_path);

  // 2. Test with JSON file
  char json_path[256];
  snprintf(json_path, sizeof(json_path), "/tmp/test_engine_cfg_%d.json",
           getpid());
  const char *json_config =
      "{\n"
      "  \"devices\": {\n"
      "    \"samplerate\": 44100,\n"
      "    \"chunksize\": 1024,\n"
      "    \"capture\": {\"type\": \"RawFile\", \"channels\": 2, \"filename\": "
      "\"/dev/null\", \"format\": \"S16_LE\"},\n"
      "    \"playback\": {\"type\": \"File\", \"channels\": 2, \"filename\": "
      "\"/dev/null\", \"format\": \"S16_LE\"}\n"
      "  }\n"
      "}";

  f = fopen(json_path, "w");
  ASSERT_TRUE(f != NULL);
  fputs(json_config, f);
  fclose(f);

  bool ok_json =
      cdsp_engine_set_config_file(engine, json_path, 96000, 2, NULL, -1, &berr);
  ASSERT_TRUE(ok_json);

  char *active_json = NULL;
  ASSERT_TRUE(cdsp_get_active_config_json(engine, &active_json));
  ASSERT_TRUE(strstr(active_json, "\"samplerate\":96000") != NULL ||
              strstr(active_json, "\"samplerate\": 96000") != NULL);
  free(active_json);
  remove(json_path);

  cdsp_engine_free(engine);
}

// Audit 07-§5.2: explicit file-load overrides must take precedence over the
// persistent CLI overrides; they were previously re-applied afterwards and
// silently replaced the explicit values.
TEST(Audit_ConfigFileExplicitOverridesBeatCliOverrides) {
  char json_path[256];
  snprintf(json_path, sizeof(json_path), "/tmp/test_audit_ovr_%d.json",
           getpid());
  const char *json_config =
      "{\"devices\": {\"samplerate\": 48000, \"chunksize\": 1024,"
      " \"capture\": {\"type\": \"RawFile\", \"channels\": 2, \"filename\":"
      " \"/dev/null\", \"format\": \"S16_LE\"},"
      " \"playback\": {\"type\": \"File\", \"channels\": 2, \"filename\":"
      " \"/dev/null\", \"format\": \"S16_LE\"}}}";
  FILE *f = fopen(json_path, "w");
  ASSERT_TRUE(f != NULL);
  fputs(json_config, f);
  fclose(f);

  cdsp_set_cli_overrides(44100, -1, NULL, -1);
  char *result_str = NULL;
  cdsp_config_error_type_t err_type = CDSP_CONFIG_ERR_PARSE;
  bool valid = cdsp_validate_config_file_with_overrides(
      json_path, 96000, -1, NULL, -1, &result_str, &err_type);
  cdsp_set_cli_overrides(-1, -1, NULL, -1);
  remove(json_path);
  ASSERT_TRUE(valid);
  ASSERT_TRUE(result_str != NULL);
  cJSON *root = cJSON_Parse(result_str);
  free(result_str);
  ASSERT_TRUE(root != NULL);
  cJSON *dev = cJSON_GetObjectItemCaseSensitive(root, "devices");
  ASSERT_EQ(96000,
            cJSON_GetObjectItemCaseSensitive(dev, "samplerate")->valueint);
  ASSERT_EQ(2048, cJSON_GetObjectItemCaseSensitive(dev, "chunksize")->valueint);
  cJSON_Delete(root);
}

// Audit 07-§5.2: a CLI override that cannot be applied must fail validation
// (as cdsp_set_config_json does) instead of validating the un-overridden
// config.
TEST(Audit_ValidateConfigJsonRejectsUnapplicableCliOverride) {
  const char *json_config =
      "{\"devices\": {\"samplerate\": 48000, \"chunksize\": 1024,"
      " \"capture\": {\"type\": \"CoreAudio\", \"channels\": 2,"
      " \"format\": \"F32\"},"
      " \"playback\": {\"type\": \"File\", \"channels\": 2, \"filename\":"
      " \"/dev/null\", \"format\": \"S16_LE\"}}}";
  cdsp_set_cli_overrides(-1, -1, "F64_LE", -1);
  char *result_str = NULL;
  cdsp_config_error_type_t err_type = CDSP_CONFIG_ERR_NONE;
  bool valid = cdsp_validate_config_json(json_config, &result_str, &err_type);
  cdsp_set_cli_overrides(-1, -1, NULL, -1);
  ASSERT_FALSE(valid);
  ASSERT_EQ(CDSP_CONFIG_ERR_PARSE, err_type);
  ASSERT_TRUE(result_str != NULL);
  ASSERT_TRUE(strstr(result_str, "CoreAudio") != NULL);
  free(result_str);
}

TEST_MAIN()

// Report 06 §6.1: plain scalars in string-typed fields stay strings verbatim
// (upstream yaml_serde accepts any plain scalar for a String field); other
// fields still infer numbers/booleans, and nulls stay null.
TEST(YamlConverter_StringFieldsKeepPlainScalarText) {
  const char *yaml_raw = "title: 12345\n"
                         "description: 1.50\n"
                         "pipeline:\n"
                         "  - type: Filter\n"
                         "    channels: [0]\n"
                         "    names: [1, 007, true]\n"
                         "    description: ~\n"
                         "filters:\n"
                         "  1:\n"
                         "    type: Gain\n"
                         "    parameters:\n"
                         "      gain: 3\n";
  char *err = NULL;
  cJSON *json = cdsp_yaml_to_json(yaml_raw, &err);
  ASSERT_TRUE(json != NULL);
  cJSON *title = cJSON_GetObjectItemCaseSensitive(json, "title");
  ASSERT_TRUE(cJSON_IsString(title));
  ASSERT_STR_EQ("12345", title->valuestring);
  cJSON *desc = cJSON_GetObjectItemCaseSensitive(json, "description");
  ASSERT_TRUE(cJSON_IsString(desc));
  ASSERT_STR_EQ("1.50", desc->valuestring);
  cJSON *step =
      cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(json, "pipeline"), 0);
  cJSON *names = cJSON_GetObjectItemCaseSensitive(step, "names");
  ASSERT_STR_EQ("1", cJSON_GetArrayItem(names, 0)->valuestring);
  ASSERT_STR_EQ("007", cJSON_GetArrayItem(names, 1)->valuestring);
  ASSERT_STR_EQ("true", cJSON_GetArrayItem(names, 2)->valuestring);
  ASSERT_TRUE(
      cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(step, "description")));
  cJSON *channels = cJSON_GetObjectItemCaseSensitive(step, "channels");
  ASSERT_TRUE(cJSON_IsNumber(cJSON_GetArrayItem(channels, 0)));
  cJSON *gain = cJSON_GetObjectItemCaseSensitive(
      cJSON_GetObjectItemCaseSensitive(
          cJSON_GetObjectItemCaseSensitive(
              cJSON_GetObjectItemCaseSensitive(json, "filters"), "1"),
          "parameters"),
      "gain");
  ASSERT_TRUE(cJSON_IsNumber(gain));
  cJSON_Delete(json);
  free(err);
}
