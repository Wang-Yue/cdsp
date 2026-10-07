/**
 * @file test_audit_config_parse.c
 * @brief Regression tests for audit report 06 (config parsing / serialization
 * / validation divergences vs upstream CamillaDSP).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>

#include "config/config_diff.h"
#include "config/config_error.h"
#include "config/config_gen.h"
#include "config/configuration.h"
#include "test_support.h"

#define CAPTURE_RAW                                                            \
  "{\"type\":\"RawFile\",\"filename\":\"/dev/null\",\"format\":\"S16_LE\","    \
  "\"channels\":2}"
#define PLAYBACK_FILE                                                          \
  "{\"type\":\"File\",\"filename\":\"/dev/null\",\"format\":\"S16_LE\","       \
  "\"channels\":2}"

/* Builds a full config JSON. Any NULL part is replaced by a sensible default.
 */
static char *build_json(const char *devices_extra, const char *capture,
                        const char *playback, const char *filters,
                        const char *pipeline) {
  size_t cap = 16384;
  char *buf = (char *)malloc(cap);
  if (!buf)
    return NULL;
  snprintf(buf, cap,
           "{\"devices\":{\"samplerate\":44100,\"chunksize\":1024%s%s,"
           "\"capture\":%s,\"playback\":%s},"
           "\"filters\":%s,\"pipeline\":%s}",
           devices_extra ? "," : "", devices_extra ? devices_extra : "",
           capture ? capture : CAPTURE_RAW, playback ? playback : PLAYBACK_FILE,
           filters ? filters : "{}", pipeline ? pipeline : "[]");
  return buf;
}

static int parse_parts(const char *devices_extra, const char *capture,
                       const char *playback, const char *filters,
                       const char *pipeline, dsp_config_t **out,
                       config_error_t *err) {
  char *json = build_json(devices_extra, capture, playback, filters, pipeline);
  config_error_init(err);
  *out = NULL;
  int rc = dsp_config_parse_json(json, out, err);
  free(json);
  return rc;
}

/* ---- 1.1 PlaybackDevice::Stdout ---------------------------------------- */

TEST(AuditConfigPlaybackStdoutParsesAndSerializes) {
  dsp_config_t *cfg = NULL;
  config_error_t err;
  int rc = parse_parts(NULL, NULL,
                       "{\"type\":\"Stdout\",\"format\":\"S32_LE\","
                       "\"channels\":2}",
                       NULL, NULL, &cfg, &err);
  if (rc != 0)
    fprintf(stderr, "err: %s\n", err.message);
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(cfg->devices.playback.type, AUDIO_BACKEND_TYPE_STDIN_OUT);
  ASSERT_EQ(cfg->devices.playback.cfg.stdout_out.channels, 2);

  cJSON *j = serialize_playback_device_config(&cfg->devices.playback);
  ASSERT_TRUE(j != NULL);
  cJSON *t = cJSON_GetObjectItemCaseSensitive(j, "type");
  ASSERT_TRUE(cJSON_IsString(t));
  ASSERT_STR_EQ(t->valuestring, "Stdout");
  cJSON_Delete(j);
  dsp_config_free(cfg);

  /* "Stdin" stays rejected for playback. */
  rc = parse_parts(NULL, NULL,
                   "{\"type\":\"Stdin\",\"format\":\"S32_LE\",\"channels\":2}",
                   NULL, NULL, &cfg, &err);
  ASSERT_NE(rc, 0);
  dsp_config_free(cfg);
}

/* ---- 1.2 devices.resampler omitted when absent -------------------------- */

TEST(AuditConfigSerializeOmitsAbsentResampler) {
  dsp_config_t *cfg = NULL;
  config_error_t err;
  ASSERT_EQ(parse_parts(NULL, NULL, NULL, NULL, NULL, &cfg, &err), 0);
  ASSERT_FALSE(cfg->devices.has_resampler);
  cJSON *j = serialize_devices_config(&cfg->devices);
  ASSERT_TRUE(j != NULL);
  ASSERT_TRUE(cJSON_GetObjectItemCaseSensitive(j, "resampler") == NULL);
  cJSON_Delete(j);
  dsp_config_free(cfg);
}

/* ---- 1.4 non-string description on filter/processor rejected ------------ */

TEST(AuditConfigRejectsNonStringFilterDescription) {
  dsp_config_t *cfg = NULL;
  config_error_t err;
  int rc = parse_parts(NULL, NULL, NULL,
                       "{\"g\":{\"type\":\"Gain\",\"description\":123,"
                       "\"parameters\":{\"gain\":-3.0}}}",
                       NULL, &cfg, &err);
  ASSERT_NE(rc, 0);
  dsp_config_free(cfg);

  rc = parse_parts(NULL, NULL, NULL,
                   "{\"g\":{\"type\":\"Gain\",\"description\":\"ok\","
                   "\"parameters\":{\"gain\":-3.0}}}",
                   NULL, &cfg, &err);
  ASSERT_EQ(rc, 0);
  ASSERT_STR_EQ(cfg->filters[0].description, "ok");
  dsp_config_free(cfg);
}

/* ---- 2.x explicit null / non-array handling ----------------------------- */

#define GAIN_FILTER "{\"g\":{\"type\":\"Gain\",\"parameters\":{\"gain\":-3.0}}}"

TEST(AuditConfigNullOptionalEnumIsNone) {
  dsp_config_t *cfg = NULL;
  config_error_t err;
  int rc = parse_parts(NULL, NULL, NULL,
                       "{\"g\":{\"type\":\"Gain\",\"parameters\":{\"gain\":-3."
                       "0,\"scale\":null}}}",
                       NULL, &cfg, &err);
  if (rc != 0)
    fprintf(stderr, "err: %s\n", err.message);
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(cfg->filters[0].filter.parameters.gain.scale, GAIN_SCALE_DB);
  dsp_config_free(cfg);
}

TEST(AuditConfigNullChannelsMeansAllChannels) {
  dsp_config_t *cfg = NULL;
  config_error_t err;
  int rc = parse_parts(NULL, NULL, NULL, GAIN_FILTER,
                       "[{\"type\":\"Filter\",\"channels\":null,"
                       "\"names\":[\"g\"]}]",
                       &cfg, &err);
  if (rc != 0)
    fprintf(stderr, "err: %s\n", err.message);
  ASSERT_EQ(rc, 0);
  ASSERT_FALSE(cfg->pipeline[0].has_channels);
  dsp_config_free(cfg);
}

TEST(AuditConfigScalarForArrayIsRejected) {
  dsp_config_t *cfg = NULL;
  config_error_t err;
  int rc = parse_parts(NULL, NULL, NULL, GAIN_FILTER,
                       "[{\"type\":\"Filter\",\"channels\":0,"
                       "\"names\":[\"g\"]}]",
                       &cfg, &err);
  ASSERT_NE(rc, 0);
  dsp_config_free(cfg);

  rc = parse_parts(NULL, NULL, NULL,
                   "{\"d\":{\"type\":\"DiffEq\",\"parameters\":"
                   "{\"a\":1.0,\"b\":[1.0]}}}",
                   NULL, &cfg, &err);
  ASSERT_NE(rc, 0);
  dsp_config_free(cfg);
}

TEST(AuditConfigNullLabelsAccepted) {
  dsp_config_t *cfg = NULL;
  config_error_t err;
  int rc = parse_parts(NULL,
                       "{\"type\":\"RawFile\",\"filename\":\"/dev/null\","
                       "\"format\":\"S16_LE\",\"channels\":2,\"labels\":null}",
                       NULL, NULL, NULL, &cfg, &err);
  if (rc != 0)
    fprintf(stderr, "err: %s\n", err.message);
  ASSERT_EQ(rc, 0);
  ASSERT_FALSE(cfg->devices.capture.has_labels);
  dsp_config_free(cfg);
}

TEST(AuditConfigNullDoesNotShadowAlternative) {
  dsp_config_t *cfg = NULL;
  config_error_t err;
  /* q: null must not hide bandwidth. */
  int rc = parse_parts(NULL, NULL, NULL,
                       "{\"p\":{\"type\":\"Biquad\",\"parameters\":{\"type\":"
                       "\"Peaking\",\"freq\":1000,\"gain\":3,\"q\":null,"
                       "\"bandwidth\":1.0}}}",
                       NULL, &cfg, &err);
  if (rc != 0)
    fprintf(stderr, "err: %s\n", err.message);
  ASSERT_EQ(rc, 0);
  ASSERT_TRUE(cfg->filters[0].filter.parameters.biquad.has_bandwidth);
  ASSERT_FALSE(cfg->filters[0].filter.parameters.biquad.has_q);
  dsp_config_free(cfg);
}

TEST(AuditConfigNullRequiredFieldRejected) {
  dsp_config_t *cfg = NULL;
  config_error_t err;
  int rc = parse_parts(NULL, NULL, NULL,
                       "{\"g\":{\"type\":\"Gain\",\"parameters\":"
                       "{\"gain\":null}}}",
                       NULL, &cfg, &err);
  ASSERT_NE(rc, 0);
  dsp_config_free(cfg);
}

TEST(AuditConfigNullOptionalResamplerIsNone) {
  dsp_config_t *cfg = NULL;
  config_error_t err;
  int rc =
      parse_parts("\"resampler\":null", NULL, NULL, NULL, NULL, &cfg, &err);
  if (rc != 0)
    fprintf(stderr, "err: %s\n", err.message);
  ASSERT_EQ(rc, 0);
  ASSERT_FALSE(cfg->devices.has_resampler);
  dsp_config_free(cfg);
}

/* ---- 3.2 Loudness reference_level required at parse time ---------------- */

TEST(AuditConfigLoudnessReferenceLevelRequiredEvenIfUnused) {
  dsp_config_t *cfg = NULL;
  config_error_t err;
  /* Defined but not referenced by the pipeline: upstream serde still fails. */
  int rc = parse_parts(NULL, NULL, NULL,
                       "{\"l\":{\"type\":\"Loudness\",\"parameters\":{}}}",
                       NULL, &cfg, &err);
  ASSERT_NE(rc, 0);
  dsp_config_free(cfg);
}

/* ---- 3.6 AsyncSinc profile cannot be mixed with free parameters ---------- */

TEST(AuditConfigAsyncSincProfileExclusiveAtParse) {
  char *json = build_json(
      "\"resampler\":{\"type\":\"AsyncSinc\",\"profile\":\"Balanced\","
      "\"sinc_len\":128},\"capture_samplerate\":48000",
      NULL, NULL, NULL, NULL);
  dsp_config_t *cfg = NULL;
  config_error_t err;
  config_error_init(&err);
  int rc = dsp_config_parse_json_no_validate(json, &cfg, &err);
  free(json);
  ASSERT_NE(rc, 0);
  dsp_config_free(cfg);
}

/* ---- 3.7 no stray "type"/"description" inside parameters / devices ------ */

TEST(AuditConfigRejectsStrayKeysInParameters) {
  dsp_config_t *cfg = NULL;
  config_error_t err;
  int rc = parse_parts(NULL, NULL, NULL,
                       "{\"g\":{\"type\":\"Gain\",\"parameters\":"
                       "{\"type\":\"Gain\",\"gain\":-3.0}}}",
                       NULL, &cfg, &err);
  ASSERT_NE(rc, 0);
  dsp_config_free(cfg);

  rc = parse_parts(NULL, NULL, NULL,
                   "{\"g\":{\"type\":\"Gain\",\"parameters\":"
                   "{\"description\":\"x\",\"gain\":-3.0}}}",
                   NULL, &cfg, &err);
  ASSERT_NE(rc, 0);
  dsp_config_free(cfg);

  rc = parse_parts(NULL, NULL,
                   "{\"type\":\"File\",\"filename\":\"/dev/null\",\"format\":"
                   "\"S16_LE\",\"channels\":2,\"description\":\"x\"}",
                   NULL, NULL, &cfg, &err);
  ASSERT_NE(rc, 0);
  dsp_config_free(cfg);

  /* Tagged parameter enums keep their inner "type". */
  rc = parse_parts(NULL, NULL, NULL,
                   "{\"b\":{\"type\":\"Biquad\",\"parameters\":"
                   "{\"type\":\"Lowpass\",\"freq\":1000,\"q\":0.7}}}",
                   NULL, &cfg, &err);
  if (rc != 0)
    fprintf(stderr, "err: %s\n", err.message);
  ASSERT_EQ(rc, 0);
  dsp_config_free(cfg);
}

/* ---- 3.8 devices.target_level reaches the playback backend --------------- */

#if defined(ENABLE_COREAUDIO)
TEST(AuditConfigTargetLevelPropagatesToPlayback) {
  char *json =
      build_json("\"target_level\":2048", NULL,
                 "{\"type\":\"CoreAudio\",\"channels\":2}", NULL, NULL);
  dsp_config_t *cfg = NULL;
  config_error_t err;
  config_error_init(&err);
  int rc = dsp_config_parse_json_no_validate(json, &cfg, &err);
  free(json);
  ASSERT_EQ(rc, 0);
  ASSERT_TRUE(cfg->devices.playback.cfg.coreaudio.has_target_level);
  ASSERT_EQ(cfg->devices.playback.cfg.coreaudio.target_level, 2048);
  dsp_config_free(cfg);

  /* An explicit per-device value wins. */
  json = build_json("\"target_level\":2048", NULL,
                    "{\"type\":\"CoreAudio\",\"channels\":2,"
                    "\"target_level\":512}",
                    NULL, NULL);
  rc = dsp_config_parse_json_no_validate(json, &cfg, &err);
  free(json);
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(cfg->devices.playback.cfg.coreaudio.target_level, 512);
  dsp_config_free(cfg);
}
#endif

/* ---- 3.9 upstream usize fields reject negative values at parse ---------- */

static int parse_no_validate(const char *devices_extra, const char *filters,
                             config_error_t *err) {
  char *json = build_json(devices_extra, NULL, NULL, filters, NULL);
  dsp_config_t *cfg = NULL;
  config_error_init(err);
  int rc = dsp_config_parse_json_no_validate(json, &cfg, err);
  free(json);
  dsp_config_free(cfg);
  return rc;
}

TEST(AuditConfigNegativeUsizeFieldsRejected) {
  config_error_t err;
  ASSERT_NE(parse_no_validate("\"resampler\":{\"type\":\"AsyncSinc\","
                              "\"sinc_len\":-1,\"interpolation\":\"Cubic\","
                              "\"window\":\"Hann2\",\"oversampling_factor\":"
                              "256},\"capture_samplerate\":48000",
                              NULL, &err),
            0);
  ASSERT_TRUE(strstr(err.message, "sinc_len") != NULL);
  ASSERT_NE(parse_no_validate("\"queuelimit\":-10", NULL, &err), 0);
  ASSERT_NE(parse_no_validate(NULL,
                              "{\"c\":{\"type\":\"Conv\",\"parameters\":"
                              "{\"type\":\"Wav\",\"filename\":\"x.wav\","
                              "\"channel\":-1}}}",
                              &err),
            0);
  ASSERT_NE(parse_no_validate(NULL,
                              "{\"c\":{\"type\":\"Conv\",\"parameters\":"
                              "{\"type\":\"Dummy\",\"length\":0}}}",
                              &err),
            0);
  ASSERT_NE(parse_no_validate(NULL,
                              "{\"b\":{\"type\":\"BiquadCombo\",\"parameters\":"
                              "{\"type\":\"ButterworthLowpass\",\"freq\":1000,"
                              "\"order\":-2}}}",
                              &err),
            0);
  ASSERT_NE(parse_no_validate(NULL,
                              "{\"d\":{\"type\":\"Dither\",\"parameters\":"
                              "{\"type\":\"Highpass\",\"bits\":-16}}}",
                              &err),
            0);
  /* Zero stays parseable where upstream usize allows it. */
  ASSERT_EQ(parse_no_validate("\"queuelimit\":0", NULL, &err), 0);
}

/* ---- 3.10 optional empty arrays round-trip as [] ------------------------- */

TEST(AuditConfigEmptyOptionalArrayRoundTrips) {
  dsp_config_t *cfg = NULL;
  config_error_t err;
  int rc = parse_parts(NULL, NULL, NULL, GAIN_FILTER,
                       "[{\"type\":\"Filter\",\"channels\":[],"
                       "\"names\":[\"g\"]}]",
                       &cfg, &err);
  if (rc != 0)
    fprintf(stderr, "err: %s\n", err.message);
  ASSERT_EQ(rc, 0);
  ASSERT_TRUE(cfg->pipeline[0].has_channels);
  ASSERT_EQ(cfg->pipeline[0].channels_count, 0);
  cJSON *j = serialize_pipeline_step_config(&cfg->pipeline[0]);
  ASSERT_TRUE(j != NULL);
  cJSON *ch = cJSON_GetObjectItemCaseSensitive(j, "channels");
  ASSERT_TRUE(cJSON_IsArray(ch));
  ASSERT_EQ(cJSON_GetArraySize(ch), 0);
  cJSON_Delete(j);
  dsp_config_free(cfg);
}

TEST_MAIN()
