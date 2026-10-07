// Regression tests for the CoreAudio backend audit (audit_reports/backends/
// 03_coreaudio.md). Hardware-dependent cases skip silently when the required
// device (BlackHole) is not installed.

#include <stdbool.h>
#include <string.h>

#include "test_support.h"

#if defined(ENABLE_COREAUDIO)
#include <CoreAudio/CoreAudio.h>

#include "backend/audio_backend.h"
#include "backend/backend_error.h"
#include "backend/core_audio_device.h"
#include "config/configuration.h"

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

#endif // ENABLE_COREAUDIO
