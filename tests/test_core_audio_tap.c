#include <stdbool.h>
#include <string.h>

#include "audio/sample_format.h"
#include "backend/audio_backend_registry.h"
#include "backend/backend_error.h"
#include "config/config_error.h"
#include "config/configuration.h"
#include "test_support.h"

#if defined(ENABLE_COREAUDIO)
#include <CoreAudio/CoreAudio.h>
#include "backend/audio_backend.h"
#include "backend/core_audio_capabilities.h"
#include "backend/core_audio_device.h"
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

TEST(CoreAudioLoopbackOfPlaybackDeviceRejectsRateAdjust) {
  // Loopback of the playback device shares the same hardware clock, so
  // rate adjust cannot be used and must be rejected.
  const char *json_str =
      "{\n"
      "  \"devices\": {\n"
      "    \"samplerate\": 48000,\n"
      "    \"chunksize\": 1024,\n"
      "    \"enable_rate_adjust\": true,\n"
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
  ASSERT_TRUE(res != 0);
  ASSERT_TRUE(config == NULL);
  ASSERT_EQ(CONFIG_ERR_INVALID_DEVICE, err.type);
  ASSERT_TRUE(strstr(err.message, "Rate adjust is not supported") != NULL);
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
static bool read_clock_source(AudioDeviceID dev, uint32_t *out) {
  AudioObjectPropertyAddress addr = {
      .mSelector = kAudioDevicePropertyClockSource,
      .mScope = kAudioObjectPropertyScopeGlobal,
      .mElement = kAudioObjectPropertyElementMain};
  UInt32 size = sizeof(uint32_t);
  return AudioObjectGetPropertyData(dev, &addr, 0, NULL, &size, out) == noErr;
}

static int parse_blackhole_capture_config(dsp_config_t **out) {
  const char *json = "{\n"
                     "  \"devices\": {\n"
                     "    \"samplerate\": 48000,\n"
                     "    \"chunksize\": 1024,\n"
                     "    \"capture\": {\n"
                     "      \"type\": \"CoreAudio\",\n"
                     "      \"device\": \"BlackHole 2ch\",\n"
                     "      \"channels\": 2\n"
                     "    },\n"
                     "    \"playback\": {\n"
                     "      \"type\": \"File\",\n"
                     "      \"filename\": \"" TEST_NULL_DEVICE "\",\n"
                     "      \"channels\": 2,\n"
                     "      \"format\": \"S16_LE\"\n"
                     "    }\n"
                     "  }\n"
                     "}";
  config_error_t err;
  config_error_init(&err);
  return dsp_config_parse_json(json, out, &err);
}

// CA-02: probing for pitch support must never switch the device clock source.
TEST(AuditCoreAudioClockSourceProbeIsReadOnly) {
  AudioDeviceID dev =
      core_audio_device_id_for_name("BlackHole 2ch", CORE_AUDIO_SCOPE_INPUT);
  if (dev == kAudioObjectUnknown)
    return; // BlackHole not installed: nothing to verify.
  uint32_t before = 0;
  if (!read_clock_source(dev, &before))
    return; // Device has no clock source control.
  uint32_t found_id = 0;
  (void)core_audio_device_find_adjustable_clock_source(dev, &found_id);
  uint32_t after = 0;
  ASSERT_TRUE(read_clock_source(dev, &after));
  ASSERT_EQ(before, after);
}

// CA-01 + CA-02: pitch support must be known right after create() (the engine
// queries it before the capture thread opens the device), and neither create()
// nor open() may switch the clock source; only the first set_pitch() may.
TEST(AuditCoreAudioPitchSupportKnownBeforeOpen) {
  AudioDeviceID dev =
      core_audio_device_id_for_name("BlackHole 2ch", CORE_AUDIO_SCOPE_INPUT);
  if (dev == kAudioObjectUnknown)
    return;
  uint32_t adjustable_id = 0;
  bool expected =
      core_audio_device_find_adjustable_clock_source(dev, &adjustable_id);
  uint32_t before = 0;
  bool has_clock = read_clock_source(dev, &before);

  dsp_config_t *config = NULL;
  ASSERT_EQ(0, parse_blackhole_capture_config(&config));
  ASSERT_TRUE(config != NULL);
  backend_error_t berr;
  backend_error_init(&berr, BACKEND_ERROR_NONE, "");
  capture_backend_t *cap = create_capture_backend(
      &config->devices.capture, 48000, 1024, false, NULL, &berr);
  ASSERT_TRUE(cap != NULL);
  ASSERT_EQ(expected, capture_backend_pitch_control_supported(cap));
  if (has_clock) {
    uint32_t after_create = 0;
    ASSERT_TRUE(read_clock_source(dev, &after_create));
    ASSERT_EQ(before, after_create);
  }
  capture_backend_free(cap);
  dsp_config_free(config);
}

// CA-05: a freshly created rate watcher must report a mismatch between the
// expected rate and the device's current nominal rate, without waiting for a
// HAL notification.
TEST(AuditCoreAudioRateWatcherSeededWithCurrentRate) {
  AudioDeviceID dev = core_audio_device_default_id(CORE_AUDIO_SCOPE_OUTPUT);
  if (dev == kAudioObjectUnknown ||
      !core_audio_device_has_nominal_sample_rate_property(dev))
    return;
  double current = 0.0;
  ASSERT_TRUE(core_audio_device_get_nominal_sample_rate(dev, &current));
  ASSERT_TRUE(current > 0.0);

  rate_change_watcher_t *same = rate_change_watcher_create(dev, current);
  ASSERT_TRUE(same != NULL);
  double reported = 0.0;
  ASSERT_FALSE(rate_change_watcher_get_pending_change(same, &reported));
  rate_change_watcher_free(same);

  rate_change_watcher_t *other =
      rate_change_watcher_create(dev, current + 1000.0);
  ASSERT_TRUE(other != NULL);
  reported = 0.0;
  ASSERT_TRUE(rate_change_watcher_get_pending_change(other, &reported));
  ASSERT_NEAR(current, reported, 0.5);
  rate_change_watcher_free(other);
}
#endif

TEST_MAIN()
