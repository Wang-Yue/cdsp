#include <stdbool.h>
#include <string.h>

#include "audio/sample_format.h"
#include "backend/audio_backend_registry.h"
#include "backend/backend_error.h"
#include "config/config_error.h"
#include "config/configuration.h"
#include "test_support.h"

#if defined(ENABLE_COREAUDIO)
#include "backend/core_audio_capabilities.h"
#include "backend/core_audio_tap_bridge.h"

TEST(CoreAudioLoopbackCapabilitiesDiscovery) {
  char targets[32][256];
  memset(targets, 0, sizeof(targets));
  int count =
      core_audio_capabilities_available_device_names(false, targets, 32);
  ASSERT_TRUE(count >= 1);

  // Playback devices should offer a "Unified" capability set
  const char *playback_target = targets[0];
  device_error_t err;
  device_error_clear(&err);
  audio_device_descriptor_t *desc =
      core_audio_capabilities_describe(playback_target, false, &err);
  ASSERT_TRUE(desc != NULL);

  ASSERT_TRUE(desc->capability_sets_count >= 1);
  const device_capability_set_t *unified_set = NULL;
  for (size_t i = 0; i < desc->capability_sets_count; i++) {
    if (strcmp(desc->capability_sets[i].mode, "Unified") == 0) {
      unified_set = &desc->capability_sets[i];
      break;
    }
  }
  ASSERT_TRUE(unified_set != NULL);
  ASSERT_TRUE(unified_set->capabilities_count >= 1);
  ASSERT_TRUE(unified_set->capabilities[0].channels > 0);
  ASSERT_TRUE(unified_set->capabilities[0].samplerates_count > 0);
  ASSERT_TRUE(unified_set->capabilities[0].samplerates[0].formats_count > 0);
  ASSERT_TRUE(strlen(unified_set->capabilities[0].samplerates[0].formats[0]) >
              0);

  free_audio_device_descriptor(desc);

  // A named device that does not exist must not silently resolve
  device_error_clear(&err);
  audio_device_descriptor_t *missing =
      core_audio_capabilities_describe("cdsp nonexistent device", false, &err);
  ASSERT_TRUE(missing == NULL);
}

TEST(CoreAudioLoopbackJsonConfigParsing) {
  const char *json_str = "{\n"
                         "  \"devices\": {\n"
                         "    \"samplerate\": 48000,\n"
                         "    \"chunksize\": 64,\n"
                         "    \"capture\": {\n"
                         "      \"type\": \"CoreAudio\",\n"
                         "      \"device\": \"DX3 Pro+\",\n"
                         "      \"channels\": 2,\n"
                         "      \"loopback\": true\n"
                         "    },\n"
                         "    \"playback\": {\n"
                         "      \"type\": \"CoreAudio\",\n"
                         "      \"device\": \"DX3 Pro+\",\n"
                         "      \"channels\": 2\n"
                         "    }\n"
                         "  }\n"
                         "}";

  dsp_config_t *config = NULL;
  config_error_t err;
  config_error_init(&err);
  int res = dsp_config_parse_json(json_str, &config, &err);
  ASSERT_EQ(0, res);
  ASSERT_TRUE(config != NULL);
  ASSERT_EQ(48000, config->devices.samplerate);
  ASSERT_EQ(64, config->devices.chunksize);
  ASSERT_EQ(config->devices.capture.type, AUDIO_BACKEND_TYPE_CORE_AUDIO);
  ASSERT_STR_EQ(config->devices.capture.cfg.coreaudio.device, "DX3 Pro+");
  ASSERT_EQ(config->devices.capture.cfg.coreaudio.channels, 2);
  ASSERT_TRUE(config->devices.capture.cfg.coreaudio.loopback);
  ASSERT_TRUE(config->devices.capture.cfg.coreaudio.has_loopback);
  dsp_config_free(config);
}

/// Build a devices config capturing @p capture_dev with optional @p loopback,
/// playing to @p playback_dev, at @p samplerate with an optional differing
/// capture_samplerate and optional resampler.
static int try_parse_loopback_config(const char *capture_dev, bool loopback,
                                     const char *playback_dev, int samplerate,
                                     int capture_samplerate,
                                     bool include_resampler,
                                     dsp_config_t **out_config,
                                     config_error_t *out_err) {
  char json_str[1024];
  char capture_rate_field[128] = "";
  if (capture_samplerate > 0 && include_resampler) {
    snprintf(capture_rate_field, sizeof(capture_rate_field),
             "    \"capture_samplerate\": %d,\n"
             "    \"resampler\": { \"type\": \"Synchronous\" },\n",
             capture_samplerate);
  } else if (capture_samplerate > 0) {
    snprintf(capture_rate_field, sizeof(capture_rate_field),
             "    \"capture_samplerate\": %d,\n", capture_samplerate);
  } else if (include_resampler) {
    snprintf(capture_rate_field, sizeof(capture_rate_field),
             "    \"resampler\": { \"type\": \"Synchronous\" },\n");
  }
  snprintf(json_str, sizeof(json_str),
           "{\n"
           "  \"devices\": {\n"
           "    \"samplerate\": %d,\n"
           "    \"chunksize\": 1024,\n"
           "%s"
           "    \"capture\": {\n"
           "      \"type\": \"CoreAudio\",\n"
           "      \"device\": \"%s\",\n"
           "      \"channels\": 2,\n"
           "      \"loopback\": %s\n"
           "    },\n"
           "    \"playback\": {\n"
           "      \"type\": \"CoreAudio\",\n"
           "      \"device\": \"%s\",\n"
           "      \"channels\": 2\n"
           "    }\n"
           "  }\n"
           "}",
           samplerate, capture_rate_field, capture_dev,
           loopback ? "true" : "false", playback_dev);

  config_error_init(out_err);
  return dsp_config_parse_json(json_str, out_config, out_err);
}

TEST(CoreAudioLoopbackOfPlaybackDeviceRejectsResampling) {
  // Loopback of the playback device is one piece of hardware with one clock, so
  // resampling is not supported when capture and playback are the same device.
  dsp_config_t *config = NULL;
  config_error_t err;
  int res = try_parse_loopback_config("DX3 Pro+", true, "DX3 Pro+", 88200,
                                      48000, true, &config, &err);
  ASSERT_TRUE(res != 0);
  ASSERT_TRUE(config == NULL);
  // Assert on the reason, so a malformed test fixture cannot pass this by
  // failing for some unrelated parse error.
  ASSERT_EQ(CONFIG_ERR_INVALID_DEVICE, err.type);
  ASSERT_TRUE(strstr(err.message, "Resampling is not supported") != NULL);

  // Even if sample rates are equal, having a resampler configured must be
  // rejected.
  config = NULL;
  res = try_parse_loopback_config("DX3 Pro+", true, "DX3 Pro+", 48000, 48000,
                                  true, &config, &err);
  ASSERT_TRUE(res != 0);
  ASSERT_TRUE(config == NULL);
  ASSERT_EQ(CONFIG_ERR_INVALID_DEVICE, err.type);
  ASSERT_TRUE(strstr(err.message, "Resampling is not supported") != NULL);

  // Device names are matched case-insensitively, as CoreAudio lookups are.
  config = NULL;
  res = try_parse_loopback_config("dx3 PRO+", true, "DX3 Pro+", 88200, 48000,
                                  true, &config, &err);
  ASSERT_TRUE(res != 0);
  ASSERT_TRUE(config == NULL);
  ASSERT_TRUE(strstr(err.message, "Resampling is not supported") != NULL);
}

TEST(CoreAudioLoopbackOfPlaybackDeviceAcceptsWithoutResampler) {
  // Turning the resampler off is valid.
  dsp_config_t *config = NULL;
  config_error_t err;
  int res = try_parse_loopback_config("DX3 Pro+", true, "DX3 Pro+", 48000, 0,
                                      false, &config, &err);
  ASSERT_EQ(0, res);
  ASSERT_TRUE(config != NULL);
  dsp_config_free(config);

  // When resampler is absent, capture_samplerate is ignored with a warning and
  // accepted.
  config = NULL;
  res = try_parse_loopback_config("DX3 Pro+", true, "DX3 Pro+", 48000, 48000,
                                  false, &config, &err);
  ASSERT_EQ(0, res);
  ASSERT_TRUE(config != NULL);
  dsp_config_free(config);
}

TEST(CoreAudioLoopbackOfOtherDeviceAllowsResampling) {
  // Loopback of a device other than the playback device uses two independent
  // clocks, so resampling between them is legitimate and must not be rejected.
  dsp_config_t *config = NULL;
  config_error_t err;
  int res = try_parse_loopback_config("BlackHole 2ch", true, "DX3 Pro+", 88200,
                                      48000, true, &config, &err);
  ASSERT_EQ(0, res);
  ASSERT_TRUE(config != NULL);
  dsp_config_free(config);

  // A plain (non-loopback) capture device that happens to share the playback
  // name is a separate input stream on that device, not a tap, so it is
  // allowed.
  config = NULL;
  res = try_parse_loopback_config("DX3 Pro+", false, "DX3 Pro+", 88200, 48000,
                                  true, &config, &err);
  ASSERT_EQ(0, res);
  ASSERT_TRUE(config != NULL);
  dsp_config_free(config);
}
#endif

TEST_MAIN()
