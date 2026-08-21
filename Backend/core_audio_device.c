// Shared CoreAudio HAL helpers used by both `CoreAudioBackend` (the
// capture/playback runtime) and `CoreAudioCapabilities` (the device
// description discovery). Keeps the boilerplate around
// `AudioObjectGetPropertyData` and friends in one place so the two
// backends don't carry near-identical copies of every enumeration helper.

#include "Backend/core_audio_device.h"

#if defined(ENABLE_COREAUDIO)
#include <CoreFoundation/CoreFoundation.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "Logging/app_logger.h"
#include "Utils/cdsp_time.h"

static const logger_t g_coreaudio_dev_logger = {"dsp.backend.coreaudio"};

// MARK: - Enumeration

/// Every HAL device on the system, regardless of stream direction.
static int core_audio_device_all_ids(AudioDeviceID* out_ids, int max_ids) {
  AudioObjectPropertyAddress addr = {
      .mSelector = kAudioHardwarePropertyDevices,
      .mScope = kAudioObjectPropertyScopeGlobal,
      .mElement = kAudioObjectPropertyElementMain};
  uint32_t size = 0;
  if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, NULL,
                                     &size) != noErr ||
      size == 0) {
    return 0;
  }
  int count = (int)(size / sizeof(AudioDeviceID));
  if (count > max_ids && out_ids) count = max_ids;
  if (out_ids && count > 0) {
    size = (uint32_t)(count * sizeof(AudioDeviceID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL,
                                   &size, out_ids) != noErr) {
      return 0;
    }
  }
  return count;
}

/// User-facing name of a device, or `nil` if the lookup fails.
bool core_audio_device_name(AudioDeviceID device_id, char* out_name,
                            size_t max_len) {
  if (!out_name || max_len == 0) return false;
  out_name[0] = '\0';
  AudioObjectPropertyAddress addr = {
      .mSelector = kAudioObjectPropertyName,
      .mScope = kAudioObjectPropertyScopeGlobal,
      .mElement = kAudioObjectPropertyElementMain};
  CFStringRef cf_name = NULL;
  uint32_t size = sizeof(CFStringRef);
  if (AudioObjectGetPropertyData(device_id, &addr, 0, NULL, &size, &cf_name) ==
          noErr &&
      cf_name) {
    CFStringGetCString(cf_name, out_name, (CFIndex)max_len,
                       kCFStringEncodingUTF8);
    CFRelease(cf_name);
    return true;
  }
  return false;
}

/// True if the device exposes any streams in the given direction.
static bool core_audio_device_has_stream(AudioDeviceID device_id,
                                         core_audio_scope_t scope) {
  AudioObjectPropertyAddress addr = {
      .mSelector = kAudioDevicePropertyStreams,
      .mScope = (scope == CORE_AUDIO_SCOPE_INPUT)
                    ? kAudioDevicePropertyScopeInput
                    : kAudioDevicePropertyScopeOutput,
      .mElement = kAudioObjectPropertyElementMain};
  uint32_t size = 0;
  OSStatus status =
      AudioObjectGetPropertyDataSize(device_id, &addr, 0, NULL, &size);
  return (status == noErr && size >= sizeof(AudioStreamID));
}

/// HAL stream IDs for the given device + direction.
int core_audio_device_streams(AudioDeviceID device_id, core_audio_scope_t scope,
                              AudioStreamID* out_streams, int max_streams) {
  AudioObjectPropertyAddress addr = {
      .mSelector = kAudioDevicePropertyStreams,
      .mScope = (scope == CORE_AUDIO_SCOPE_INPUT)
                    ? kAudioDevicePropertyScopeInput
                    : kAudioDevicePropertyScopeOutput,
      .mElement = kAudioObjectPropertyElementMain};
  uint32_t size = 0;
  if (AudioObjectGetPropertyDataSize(device_id, &addr, 0, NULL, &size) !=
          noErr ||
      size == 0) {
    return 0;
  }
  int count = (int)(size / sizeof(AudioStreamID));
  if (count > max_streams && out_streams) count = max_streams;
  if (out_streams && count > 0) {
    size = (uint32_t)(count * sizeof(AudioStreamID));
    if (AudioObjectGetPropertyData(device_id, &addr, 0, NULL, &size,
                                   out_streams) != noErr) {
      return 0;
    }
  }
  return count;
}

/// Devices that have at least one stream in the requested direction,
/// each paired with its user-facing name. Devices that fail the
/// stream-config check (e.g. an output-only device queried in
/// `.input` scope) are filtered out.
int core_audio_device_list_devices(core_audio_scope_t scope,
                                   core_audio_device_info_t* out_devices,
                                   int max_devices) {
  AudioDeviceID all_ids[128];
  int total = core_audio_device_all_ids(all_ids, 128);
  int count = 0;
  for (int i = 0; i < total && count < max_devices; i++) {
    if (core_audio_device_has_stream(all_ids[i], scope)) {
      if (out_devices) {
        out_devices[count].id = all_ids[i];
        if (!core_audio_device_name(all_ids[i], out_devices[count].name,
                                    sizeof(out_devices[count].name))) {
          out_devices[count].name[0] = '\0';
        }
      }
      count++;
    }
  }
  return count;
}

// MARK: - Lookup

/// HAL ID of the system-default device for the given direction.
AudioDeviceID core_audio_device_default_id(core_audio_scope_t scope) {
  AudioObjectPropertyAddress addr = {
      .mSelector = (scope == CORE_AUDIO_SCOPE_INPUT)
                       ? kAudioHardwarePropertyDefaultInputDevice
                       : kAudioHardwarePropertyDefaultOutputDevice,
      .mScope = kAudioObjectPropertyScopeGlobal,
      .mElement = kAudioObjectPropertyElementMain};
  AudioDeviceID id = kAudioObjectUnknown;
  uint32_t size = sizeof(AudioDeviceID);
  if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL,
                                 &size, &id) == noErr) {
    return id;
  }
  return kAudioObjectUnknown;
}

/// HAL ID of a named device, or the system default when `name` is
/// `nil`. Returns `nil` if the named device can't be found.
AudioDeviceID core_audio_device_id_for_name(const char* name,
                                            core_audio_scope_t scope) {
  if (!name || name[0] == '\0' || strcmp(name, "default") == 0) {
    return core_audio_device_default_id(scope);
  }
  core_audio_device_info_t devices[128];
  int count = core_audio_device_list_devices(scope, devices, 128);
  for (int i = 0; i < count; i++) {
    if (strcmp(devices[i].name, name) == 0) {
      return devices[i].id;
    }
  }
  return kAudioObjectUnknown;
}

// MARK: - Sample-rate control

/// Read the device's current nominal sample rate. Used to verify
/// that `setNominalSampleRate` actually took effect — CoreAudio
/// applies the change asynchronously, so callers should poll this
/// for a short window before falling back.
bool core_audio_device_get_nominal_sample_rate(AudioDeviceID device_id,
                                               double* out_rate) {
  AudioObjectPropertyAddress addr = {
      .mSelector = kAudioDevicePropertyNominalSampleRate,
      .mScope = kAudioObjectPropertyScopeGlobal,
      .mElement = kAudioObjectPropertyElementMain};
  Float64 rate = 0;
  uint32_t size = sizeof(Float64);
  if (AudioObjectGetPropertyData(device_id, &addr, 0, NULL, &size, &rate) ==
      noErr) {
    if (out_rate) *out_rate = rate;
    return true;
  }
  return false;
}

/// Push the device's nominal sample rate, then poll until the
/// change has been committed. CoreAudio applies the change
/// asynchronously on a HAL thread; if we proceed straight to
/// `AudioUnitInitialize` the AudioUnit can latch the *old* rate
/// and silently sample-rate-convert from then on. Returns `true`
/// only when both the set call succeeded *and* the device's
/// reported rate matches `rate` within `~0.5 Hz` after the poll.
///
/// Devices that don't support the requested rate return a
/// non-zero status from the set call; we surface that as `false`
/// without polling.
bool core_audio_device_set_nominal_sample_rate(AudioDeviceID device_id,
                                               double rate) {
  AudioObjectPropertyAddress addr = {
      .mSelector = kAudioDevicePropertyNominalSampleRate,
      .mScope = kAudioObjectPropertyScopeGlobal,
      .mElement = kAudioObjectPropertyElementMain};
  Float64 value = rate;
  // Submit the nominal rate change request. This is processed asynchronously by
  // the HAL.
  OSStatus status = AudioObjectSetPropertyData(device_id, &addr, 0, NULL,
                                               sizeof(Float64), &value);
  if (status != noErr) return false;
  // Poll for up to 250ms (50 iterations * 5ms) until the device reports a
  // nominal rate that matches the target rate. This ensures the change is
  // finalized before we attempt to initialize AudioUnits, which could otherwise
  // lock onto the old rate.
  for (int i = 0; i < 50; i++) {
    double current = 0.0;
    if (core_audio_device_get_nominal_sample_rate(device_id, &current)) {
      if (fabs(current - rate) < 0.5) return true;
    }
    cdsp_sleep_us(5000);
  }
  return false;
}

// MARK: - Buffer frame size control

/// Set the device's buffer frame size for a given scope. Returns `true` on
/// success.
bool core_audio_device_set_buffer_frame_size(AudioDeviceID device_id,
                                             uint32_t frames,
                                             core_audio_scope_t scope) {
  AudioObjectPropertyAddress addr = {
      .mSelector = kAudioDevicePropertyBufferFrameSize,
      .mScope = (scope == CORE_AUDIO_SCOPE_INPUT)
                    ? kAudioDevicePropertyScopeInput
                    : kAudioDevicePropertyScopeOutput,
      .mElement = kAudioObjectPropertyElementMain};
  uint32_t value = frames;
  return (AudioObjectSetPropertyData(device_id, &addr, 0, NULL,
                                     sizeof(uint32_t), &value) == noErr);
}

bool core_audio_device_get_buffer_frame_size(AudioDeviceID device_id,
                                             core_audio_scope_t scope,
                                             uint32_t* out_frames) {
  AudioObjectPropertyAddress addr = {
      .mSelector = kAudioDevicePropertyBufferFrameSize,
      .mScope = (scope == CORE_AUDIO_SCOPE_INPUT)
                    ? kAudioDevicePropertyScopeInput
                    : kAudioDevicePropertyScopeOutput,
      .mElement = kAudioObjectPropertyElementMain};
  uint32_t frames = 0;
  uint32_t size = sizeof(uint32_t);
  if (AudioObjectGetPropertyData(device_id, &addr, 0, NULL, &size, &frames) ==
      noErr) {
    if (out_frames) *out_frames = frames;
    return true;
  }
  return false;
}

// MARK: - Clock-source / pitch control (BlackHole 0.5.0+)

/// Set the device's active clock source by ID. Returns `true` on success.
static bool core_audio_device_set_clock_source_id(AudioDeviceID device_id,
                                                  uint32_t source_id) {
  AudioObjectPropertyAddress addr = {
      .mSelector = kAudioDevicePropertyClockSource,
      .mScope = kAudioObjectPropertyScopeGlobal,
      .mElement = kAudioObjectPropertyElementMain};
  uint32_t value = source_id;
  return (AudioObjectSetPropertyData(device_id, &addr, 0, NULL,
                                     sizeof(uint32_t), &value) == noErr);
}

/// If `deviceID` advertises an "Internal Adjustable" clock source
/// (BlackHole 0.5.0+), select it as the active source and return
/// `true`. Returns `false` for devices that don't support pitch
/// tuning.
/// Enumerate clock sources for `deviceID`. Returns the parallel
/// `(name, id)` arrays in declaration order. Used by
/// `selectAdjustableClockSource` to find the magic
/// `"Internal Adjustable"` source that BlackHole 0.5.0+ exposes
/// for fine-grained pitch control.
bool core_audio_device_select_adjustable_clock_source(AudioDeviceID device_id) {
  AudioObjectPropertyAddress addr = {
      .mSelector = kAudioDevicePropertyClockSources,
      .mScope = kAudioObjectPropertyScopeGlobal,
      .mElement = kAudioObjectPropertyElementMain};
  uint32_t size = 0;
  // Fetch the size of the clock sources array.
  OSStatus sz_status =
      AudioObjectGetPropertyDataSize(device_id, &addr, 0, NULL, &size);
  if (sz_status != noErr) {
    logger_warn(&g_coreaudio_dev_logger,
                "Unable to read number of clock sources, error code: %d.",
                (int)sz_status);
    return false;
  }
  if (size == 0) {
    logger_info(&g_coreaudio_dev_logger,
                "The capture device has no clock source control.");
    return false;
  }
  int count = (int)(size / sizeof(uint32_t));
  if (count > 32) count = 32;
  uint32_t ids[32];
  size = (uint32_t)(count * sizeof(uint32_t));
  // Retrieve the clock source IDs.
  OSStatus get_status =
      AudioObjectGetPropertyData(device_id, &addr, 0, NULL, &size, ids);
  if (get_status != noErr) {
    logger_warn(&g_coreaudio_dev_logger,
                "Unable to list clock sources, error code: %d.",
                (int)get_status);
    return false;
  }
  logger_debug(&g_coreaudio_dev_logger, "Capture device has %d clock sources.",
               count);
  // Iterate through the clock source IDs and fetch their CFString names using
  // AudioValueTranslation.
  for (int i = 0; i < count; i++) {
    AudioObjectPropertyAddress name_addr = {
        .mSelector = kAudioDevicePropertyClockSourceNameForIDCFString,
        .mScope = kAudioObjectPropertyScopeGlobal,
        .mElement = kAudioObjectPropertyElementMain};
    uint32_t source_id = ids[i];
    CFStringRef cf_name = NULL;
    AudioValueTranslation trans = {.mInputData = &source_id,
                                   .mInputDataSize = sizeof(uint32_t),
                                   .mOutputData = &cf_name,
                                   .mOutputDataSize = sizeof(CFStringRef)};
    uint32_t trans_size = sizeof(AudioValueTranslation);
    if (AudioObjectGetPropertyData(device_id, &name_addr, 0, NULL, &trans_size,
                                   &trans) == noErr &&
        cf_name) {
      char name_buf[256];
      if (CFStringGetCString(cf_name, name_buf, sizeof(name_buf),
                             kCFStringEncodingUTF8)) {
        // Look for the magic "Internal Adjustable" clock source (provided by
        // virtual drivers like BlackHole 0.5.0+ to allow pitch-shifting).
        if (strcmp(name_buf, "Internal Adjustable") == 0) {
          CFRelease(cf_name);
          logger_debug(
              &g_coreaudio_dev_logger,
              "Changing capture device clock source to item with index %d.", i);
          bool ok =
              core_audio_device_set_clock_source_id(device_id, source_id);
          if (ok) {
            logger_info(&g_coreaudio_dev_logger,
                        "The capture device supports pitch control.");
          } else {
            logger_warn(&g_coreaudio_dev_logger,
                        "Unable to set clock source, error code: -1.");
          }
          return ok;
        }
      }
      CFRelease(cf_name);
    }
  }
  logger_info(&g_coreaudio_dev_logger,
              "The capture device does not support pitch control.");
  return false;
}

/// Apply a clock-pitch correction to the capture device by
/// writing `kAudioDevicePropertyStereoPan`. Upstream maps
/// `pitch ∈ [0.99, 1.01]` to `pan ∈ [0, 1]` with the formula
/// `pan = (pitch - 1.0) * 50.0 + 0.5`, clamped to the valid
/// range.
void core_audio_device_set_pitch(AudioDeviceID device_id, double pitch) {
  AudioObjectPropertyAddress addr = {
      .mSelector = kAudioDevicePropertyStereoPan,
      .mScope = kAudioObjectPropertyScopeOutput,
      .mElement = kAudioObjectPropertyElementMain};
  // CoreAudio does not expose a generic API to adjust physical clock rates.
  // However, virtual devices like BlackHole hijack the Stereo Pan control on
  // their output scope as a proxy for pitch tuning. Map pitch multiplier range
  // [0.99, 1.01] to pan range [0.0, 1.0].
  float pan = (float)((pitch - 1.0) * 50.0 + 0.5);
  if (pan < 0.0f) pan = 0.0f;
  if (pan > 1.0f) pan = 1.0f;
  logger_debug(
      &g_coreaudio_dev_logger,
      "Setting capture pitch to: %f, corresponding pan value: %f.",
      pitch, (double)pan);
  OSStatus status =
      AudioObjectSetPropertyData(device_id, &addr, 0, NULL, sizeof(float), &pan);
  if (status != noErr) {
    logger_warn(&g_coreaudio_dev_logger,
                "Unable to set pitch, error code: %d.", (int)status);
  }
}

/// Returns true if the device exposes the nominal-sample-rate
/// property — needed before installing a `RateChangeWatcher` so we
/// don't churn HAL listener registrations on devices that can't
/// publish rate changes anyway.
bool core_audio_device_has_nominal_sample_rate_property(
    AudioDeviceID device_id) {
  AudioObjectPropertyAddress addr = {
      .mSelector = kAudioDevicePropertyNominalSampleRate,
      .mScope = kAudioObjectPropertyScopeGlobal,
      .mElement = kAudioObjectPropertyElementMain};
  return AudioObjectHasProperty(device_id, &addr);
}

// RateChangeWatcher implementation
/// Watches a CoreAudio device's `kAudioDevicePropertyNominalSampleRate`
/// and reports any change away from the rate the engine asked for.
///
/// The capture thread polls
/// `pendingRateChange` once per chunk; on a real change it stops the
/// engine with `.captureFormatChange(rate)` / `.playbackFormatChange(rate)`
/// so the host can rebuild at the new rate.
///
/// Lifetime: created by `CoreAudioCapture.open()` /
/// `CoreAudioPlayback.open()` *after* `setNominalSampleRate` has been
/// applied (so the watcher's expected rate is the one we just pushed).
/// `dispose()` removes the HAL listener and must run before the owner
/// is deallocated — `deinit` calls it as a backstop.
struct rate_change_watcher {
  AudioDeviceID device_id;
  double expected_rate;
  _Atomic uint64_t latest_rate_bits;
  _Atomic bool registered;
};

/// HAL listener callback. Runs on a CoreAudio dispatch thread, never on
/// the render thread, so the `AudioObjectGetPropertyData` query here
/// is safe — but we still keep it to a single read + atomic store so
/// the listener returns promptly.
/**
 * @brief HAL nominal sample rate change listener callback.
 *
 * Runs on a system dispatch thread (never render thread). Resolves the new
 * nominal rate and updates the watcher atomically.
 */
static OSStatus rate_change_listener_callback(
    AudioObjectID inObjectID, UInt32 inNumberAddresses,
    const AudioObjectPropertyAddress* inAddresses, void* inClientData) {
  (void)inNumberAddresses;
  (void)inAddresses;
  rate_change_watcher_t* watcher = (rate_change_watcher_t*)inClientData;
  if (!watcher) return noErr;
  double rate = 0.0;
  if (core_audio_device_get_nominal_sample_rate(inObjectID, &rate)) {
    if (rate > 0.0) {
      uint64_t bits;
      memcpy(&bits, &rate, sizeof(uint64_t));
      atomic_store_explicit(&watcher->latest_rate_bits, bits,
                            memory_order_release);
    }
  }
  return noErr;
}

rate_change_watcher_t* rate_change_watcher_create(AudioDeviceID device_id,
                                                  double expected_rate) {
  rate_change_watcher_t* watcher =
      (rate_change_watcher_t*)calloc(1, sizeof(rate_change_watcher_t));
  if (!watcher) return NULL;
  watcher->device_id = device_id;
  watcher->expected_rate = expected_rate;
  atomic_init(&watcher->latest_rate_bits, 0);

  AudioObjectPropertyAddress addr = {
      .mSelector = kAudioDevicePropertyNominalSampleRate,
      .mScope = kAudioObjectPropertyScopeGlobal,
      .mElement = kAudioObjectPropertyElementMain};
  OSStatus status = AudioObjectAddPropertyListener(
      device_id, &addr, rate_change_listener_callback, watcher);
  if (status != noErr) {
    free(watcher);
    return NULL;
  }
  atomic_init(&watcher->registered, true);
  return watcher;
}

bool rate_change_watcher_get_pending_change(rate_change_watcher_t* watcher,
                                            double* out_rate) {
  if (!watcher) return false;
  uint64_t bits =
      atomic_load_explicit(&watcher->latest_rate_bits, memory_order_acquire);
  if (bits == 0) return false;
  double rate;
  memcpy(&rate, &bits, sizeof(double));
  if (fabs(rate - watcher->expected_rate) < 0.5) return false;
  if (out_rate) *out_rate = rate;
  return true;
}

void rate_change_watcher_dispose(rate_change_watcher_t* watcher) {
  if (!watcher) return;
  if (atomic_load_explicit(&watcher->registered, memory_order_acquire)) {
    AudioObjectPropertyAddress addr = {
        .mSelector = kAudioDevicePropertyNominalSampleRate,
        .mScope = kAudioObjectPropertyScopeGlobal,
        .mElement = kAudioObjectPropertyElementMain};
    AudioObjectRemovePropertyListener(watcher->device_id, &addr,
                                      rate_change_listener_callback, watcher);
    atomic_store_explicit(&watcher->registered, false, memory_order_release);
  }
}

void rate_change_watcher_free(rate_change_watcher_t* watcher) {
  if (!watcher) return;
  rate_change_watcher_dispose(watcher);
  free(watcher);
}

/**
 * @brief Internal helper to map an AudioStreamBasicDescription to a format
 * string token.
 *
 * Filters and maps supported formats to "S16", "S24", "S32", or "F32".
 *
 * @param asbd Pointer to the AudioStreamBasicDescription.
 * @return A string literal representing the format, or empty if unsupported.
 */
static const char* format_string_for_asbd_local(
    const AudioStreamBasicDescription* asbd) {
  AudioFormatFlags flags = asbd->mFormatFlags;
  bool is_float = (flags & kAudioFormatFlagIsFloat) != 0;
  bool is_signed_int = (flags & kAudioFormatFlagIsSignedInteger) != 0;
  uint32_t bits = asbd->mBitsPerChannel;

  if (is_float && bits == 32) return "F32";
  if (is_signed_int) {
    switch (bits) {
      case 16:
        return "S16";
      case 24:
        return "S24";
      case 32:
        return "S32";
      default:
        return "";
    }
  }
  return "";
}

AudioStreamBasicDescription core_audio_device_float32_stream_format(
    double sample_rate, int channels) {
  uint32_t bytes_per_frame = (uint32_t)(4 * channels);
  AudioFormatFlags flags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
  AudioStreamBasicDescription asbd = {.mSampleRate = sample_rate,
                                      .mFormatID = kAudioFormatLinearPCM,
                                      .mFormatFlags = flags,
                                      .mBytesPerPacket = bytes_per_frame,
                                      .mFramesPerPacket = 1,
                                      .mBytesPerFrame = bytes_per_frame,
                                      .mChannelsPerFrame = (uint32_t)channels,
                                      .mBitsPerChannel = 32,
                                      .mReserved = 0};
  return asbd;
}

AudioStreamBasicDescription core_audio_device_asbd_for_format(
    double sample_rate, size_t channels, const char* format_str) {
  AudioStreamBasicDescription asbd = {0};
  asbd.mSampleRate = sample_rate;
  asbd.mFormatID = kAudioFormatLinearPCM;
  asbd.mFramesPerPacket = 1;
  asbd.mChannelsPerFrame = (uint32_t)channels;

  if (format_str && strcmp(format_str, "S16") == 0) {
    asbd.mFormatFlags =
        kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    asbd.mBitsPerChannel = 16;
    asbd.mBytesPerFrame = (uint32_t)(channels * 2);
    asbd.mBytesPerPacket = asbd.mBytesPerFrame;
  } else if (format_str && strcmp(format_str, "S24") == 0) {
    asbd.mFormatFlags =
        kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsAlignedHigh;
    asbd.mBitsPerChannel = 24;
    asbd.mBytesPerFrame = (uint32_t)(channels * 4);
    asbd.mBytesPerPacket = asbd.mBytesPerFrame;
  } else if (format_str && strcmp(format_str, "S32") == 0) {
    asbd.mFormatFlags =
        kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    asbd.mBitsPerChannel = 32;
    asbd.mBytesPerFrame = (uint32_t)(channels * 4);
    asbd.mBytesPerPacket = asbd.mBytesPerFrame;
  } else {
    // Default to Float32
    asbd.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    asbd.mBitsPerChannel = 32;
    asbd.mBytesPerFrame = (uint32_t)(channels * 4);
    asbd.mBytesPerPacket = asbd.mBytesPerFrame;
  }
  return asbd;
}

binary_sample_format_t core_audio_device_asbd_to_binary_format(
    const AudioStreamBasicDescription* asbd) {
  if (!asbd || asbd->mFormatID != kAudioFormatLinearPCM) {
    return BINARY_SAMPLE_FORMAT_INVALID;
  }
  bool is_float = (asbd->mFormatFlags & kAudioFormatFlagIsFloat) != 0;
  bool is_signed = (asbd->mFormatFlags & kAudioFormatFlagIsSignedInteger) != 0;
  bool is_be = (asbd->mFormatFlags & kAudioFormatFlagIsBigEndian) != 0;
  uint32_t bits = asbd->mBitsPerChannel;
  uint32_t bytes_per_channel =
      (asbd->mChannelsPerFrame > 0)
          ? (asbd->mBytesPerFrame / asbd->mChannelsPerFrame)
          : (bits / 8);

  if (is_float) {
    if (bits == 32)
      return is_be ? BINARY_SAMPLE_FORMAT_F32_BE : BINARY_SAMPLE_FORMAT_F32_LE;
    if (bits == 64)
      return is_be ? BINARY_SAMPLE_FORMAT_F64_BE : BINARY_SAMPLE_FORMAT_F64_LE;
  } else if (is_signed) {
    if (bits == 16) {
      return is_be ? BINARY_SAMPLE_FORMAT_S16_BE : BINARY_SAMPLE_FORMAT_S16_LE;
    } else if (bits == 24) {
      if (bytes_per_channel == 3) {
        return is_be ? BINARY_SAMPLE_FORMAT_S24_3_BE
                     : BINARY_SAMPLE_FORMAT_S24_3_LE;
      } else if (bytes_per_channel == 4) {
        bool is_aligned_high =
            (asbd->mFormatFlags & kAudioFormatFlagIsAlignedHigh) != 0;
        if (is_aligned_high) {
          return is_be ? BINARY_SAMPLE_FORMAT_S24_4_LJ_BE
                       : BINARY_SAMPLE_FORMAT_S24_4_LJ_LE;
        } else {
          return is_be ? BINARY_SAMPLE_FORMAT_S24_4_RJ_BE
                       : BINARY_SAMPLE_FORMAT_S24_4_RJ_LE;
        }
      }
    } else if (bits == 32) {
      return is_be ? BINARY_SAMPLE_FORMAT_S32_BE : BINARY_SAMPLE_FORMAT_S32_LE;
    }
  }
  return BINARY_SAMPLE_FORMAT_INVALID;
}

bool core_audio_device_get_virtual_format(
    AudioDeviceID device_id, core_audio_scope_t scope,
    AudioStreamBasicDescription* out_asbd) {
  if (!out_asbd) return false;
  AudioStreamID streams[32];
  int stream_count = core_audio_device_streams(device_id, scope, streams, 32);
  if (stream_count > 0) {
    AudioObjectPropertyAddress addr = {
        .mSelector = kAudioStreamPropertyVirtualFormat,
        .mScope = kAudioObjectPropertyScopeGlobal,
        .mElement = kAudioObjectPropertyElementMain};
    uint32_t size = sizeof(AudioStreamBasicDescription);
    if (AudioObjectGetPropertyData(streams[0], &addr, 0, NULL, &size,
                                   out_asbd) == noErr) {
      return true;
    }
  }
  AudioObjectPropertyAddress dev_addr = {
      .mSelector = kAudioDevicePropertyStreamFormat,
      .mScope = (scope == CORE_AUDIO_SCOPE_INPUT)
                    ? kAudioDevicePropertyScopeInput
                    : kAudioDevicePropertyScopeOutput,
      .mElement = kAudioObjectPropertyElementMain};
  uint32_t size = sizeof(AudioStreamBasicDescription);
  if (AudioObjectGetPropertyData(device_id, &dev_addr, 0, NULL, &size,
                                 out_asbd) == noErr) {
    return true;
  }
  return false;
}

bool core_audio_device_set_matching_virtual_format(
    AudioDeviceID device_id, core_audio_scope_t scope, double sample_rate,
    const char* format_str, int requested_channels,
    AudioStreamBasicDescription* out_actual_asbd) {
  AudioStreamID streams[32];
  int stream_count = core_audio_device_streams(device_id, scope, streams, 32);

  AudioStreamBasicDescription best_asbd;
  bool found_best = false;
  AudioStreamID best_stream_id = 0;

  for (int s = 0; s < stream_count; s++) {
    AudioObjectPropertyAddress addr = {
        .mSelector = kAudioStreamPropertyAvailableVirtualFormats,
        .mScope = kAudioObjectPropertyScopeGlobal,
        .mElement = kAudioObjectPropertyElementMain};
    uint32_t size = 0;
    if (AudioObjectGetPropertyDataSize(streams[s], &addr, 0, NULL, &size) !=
            noErr ||
        size == 0) {
      continue;
    }
    int count = (int)(size / sizeof(AudioStreamRangedDescription));
    AudioStreamRangedDescription* ranged =
        (AudioStreamRangedDescription*)calloc(
            count, sizeof(AudioStreamRangedDescription));
    if (!ranged) continue;

    if (AudioObjectGetPropertyData(streams[s], &addr, 0, NULL, &size, ranged) ==
        noErr) {
      for (int i = 0; i < count; i++) {
        AudioStreamBasicDescription asbd = ranged[i].mFormat;
        if (asbd.mFormatID != kAudioFormatLinearPCM) continue;

        double lo = ranged[i].mSampleRateRange.mMinimum;
        double hi = ranged[i].mSampleRateRange.mMaximum;
        if (sample_rate < lo || sample_rate > hi) {
          continue;
        }

        const char* stream_fmt_str = format_string_for_asbd_local(&asbd);
        if (format_str && strcmp(stream_fmt_str, format_str) != 0) {
          continue;
        }

        int virt_channels = (int)asbd.mChannelsPerFrame;
        if (virt_channels < requested_channels) {
          continue;
        }

        if (!found_best || virt_channels < (int)best_asbd.mChannelsPerFrame) {
          best_asbd = asbd;
          best_asbd.mSampleRate = sample_rate;
          best_stream_id = streams[s];
          found_best = true;
        }
      }
    }
    free(ranged);
  }

  if (found_best) {
    AudioObjectPropertyAddress addr = {
        .mSelector = kAudioStreamPropertyVirtualFormat,
        .mScope = kAudioObjectPropertyScopeGlobal,
        .mElement = kAudioObjectPropertyElementMain};
    OSStatus status = AudioObjectSetPropertyData(
        best_stream_id, &addr, 0, NULL, sizeof(AudioStreamBasicDescription),
        &best_asbd);
    if (status == noErr) {
      if (out_actual_asbd) *out_actual_asbd = best_asbd;
      return true;
    }
  }

  // Fall back to direct ASBD assignment
  AudioStreamBasicDescription direct_asbd = core_audio_device_asbd_for_format(
      sample_rate, requested_channels, format_str);
  if (direct_asbd.mFormatID == kAudioFormatLinearPCM) {
    for (int s = 0; s < stream_count; s++) {
      AudioObjectPropertyAddress addr = {
          .mSelector = kAudioStreamPropertyVirtualFormat,
          .mScope = kAudioObjectPropertyScopeGlobal,
          .mElement = kAudioObjectPropertyElementMain};
      if (AudioObjectSetPropertyData(streams[s], &addr, 0, NULL,
                                     sizeof(AudioStreamBasicDescription),
                                     &direct_asbd) == noErr) {
        if (out_actual_asbd) *out_actual_asbd = direct_asbd;
        return true;
      }
    }
    AudioObjectPropertyAddress dev_addr = {
        .mSelector = kAudioDevicePropertyStreamFormat,
        .mScope = (scope == CORE_AUDIO_SCOPE_INPUT)
                      ? kAudioDevicePropertyScopeInput
                      : kAudioDevicePropertyScopeOutput,
        .mElement = kAudioObjectPropertyElementMain};
    if (AudioObjectSetPropertyData(device_id, &dev_addr, 0, NULL,
                                   sizeof(AudioStreamBasicDescription),
                                   &direct_asbd) == noErr) {
      if (out_actual_asbd) *out_actual_asbd = direct_asbd;
      return true;
    }
  }

  return false;
}

bool core_audio_device_set_matching_physical_format(AudioDeviceID device_id,
                                                    core_audio_scope_t scope,
                                                    double sample_rate,
                                                    const char* format_str,
                                                    int requested_channels) {
  // Query all stream IDs associated with this scope on the device.
  AudioStreamID streams[32];
  int stream_count = core_audio_device_streams(device_id, scope, streams, 32);

  AudioStreamBasicDescription best_asbd;
  bool found_best = false;
  AudioStreamID best_stream_id = 0;

  // Loop through the streams to find a matching physical format from their
  // available formats list.
  for (int s = 0; s < stream_count; s++) {
    AudioObjectPropertyAddress addr = {
        .mSelector = kAudioStreamPropertyAvailablePhysicalFormats,
        .mScope = kAudioObjectPropertyScopeGlobal,
        .mElement = kAudioObjectPropertyElementMain};
    uint32_t size = 0;
    if (AudioObjectGetPropertyDataSize(streams[s], &addr, 0, NULL, &size) !=
            noErr ||
        size == 0) {
      continue;
    }
    int count = (int)(size / sizeof(AudioStreamRangedDescription));
    AudioStreamRangedDescription* ranged =
        (AudioStreamRangedDescription*)calloc(
            count, sizeof(AudioStreamRangedDescription));
    if (!ranged) continue;

    if (AudioObjectGetPropertyData(streams[s], &addr, 0, NULL, &size, ranged) ==
        noErr) {
      for (int i = 0; i < count; i++) {
        AudioStreamBasicDescription asbd = ranged[i].mFormat;
        if (asbd.mFormatID != kAudioFormatLinearPCM) continue;

        // Match sample rate (checking if requested rate falls within physical
        // stream limits).
        double lo = ranged[i].mSampleRateRange.mMinimum;
        double hi = ranged[i].mSampleRateRange.mMaximum;
        if (sample_rate < lo || sample_rate > hi) {
          continue;
        }

        // Match format type (S16, S24, S32, F32)
        const char* stream_fmt_str = format_string_for_asbd_local(&asbd);
        if (strcmp(stream_fmt_str, format_str) != 0) {
          continue;
        }

        // Match channels (must satisfy minimum channel request).
        int phys_channels = (int)asbd.mChannelsPerFrame;
        if (phys_channels < requested_channels) {
          continue;
        }

        // We want the smallest channel count that fits.
        if (!found_best || phys_channels < (int)best_asbd.mChannelsPerFrame) {
          best_asbd = asbd;
          best_asbd.mSampleRate = sample_rate;
          best_stream_id = streams[s];
          found_best = true;
        }
      }
    }
    free(ranged);
  }

  // Set the physical format property on the matching stream if found.
  if (found_best) {
    AudioObjectPropertyAddress addr = {
        .mSelector = kAudioStreamPropertyPhysicalFormat,
        .mScope = kAudioObjectPropertyScopeGlobal,
        .mElement = kAudioObjectPropertyElementMain};
    OSStatus status = AudioObjectSetPropertyData(
        best_stream_id, &addr, 0, NULL, sizeof(AudioStreamBasicDescription),
        &best_asbd);
    return (status == noErr);
  }

  return false;
}

// MARK: - Hog Mode Control

bool core_audio_device_acquire_hog_mode(AudioDeviceID device_id) {
  if (device_id == 0) return false;
  pid_t camilla_pid = getpid();
  pid_t current_hog_pid = -1;
  uint32_t hog_size = sizeof(pid_t);
  AudioObjectPropertyAddress hog_addr = {
      .mSelector = kAudioDevicePropertyHogMode,
      .mScope = kAudioObjectPropertyScopeGlobal,
      .mElement = kAudioObjectPropertyElementMain};
  if (AudioObjectGetPropertyData(device_id, &hog_addr, 0, NULL, &hog_size,
                                 &current_hog_pid) == noErr) {
    if (current_hog_pid == camilla_pid) {
      logger_debug(&g_coreaudio_dev_logger, "We already have exclusive access.");
      return true;
    } else if (current_hog_pid != -1) {
      logger_warn(&g_coreaudio_dev_logger,
                  "Device is owned by another process with pid %d!",
                  (int)current_hog_pid);
    } else {
      logger_debug(&g_coreaudio_dev_logger,
                   "Device is free, trying to get exclusive access.");
    }
  }
  pid_t hog_pid = camilla_pid;
  if (AudioObjectSetPropertyData(device_id, &hog_addr, 0, NULL, sizeof(pid_t),
                                 &hog_pid) == noErr) {
    pid_t new_device_pid = -1;
    if (AudioObjectGetPropertyData(device_id, &hog_addr, 0, NULL, &hog_size,
                                   &new_device_pid) == noErr &&
        new_device_pid == camilla_pid) {
      logger_debug(&g_coreaudio_dev_logger, "We have exclusive access.");
      return true;
    } else {
      logger_warn(
          &g_coreaudio_dev_logger,
          "Could not get exclusive access. CamillaDSP pid: %d, device owner "
          "pid: %d.",
          (int)camilla_pid, (int)current_hog_pid);
    }
  }
  return false;
}

void core_audio_device_release_hog_mode(AudioDeviceID device_id) {
  if (device_id == 0) return;
  pid_t camilla_pid = getpid();
  logger_trace(&g_coreaudio_dev_logger,
               "Releasing any device ownership for device id %u.",
               (unsigned int)device_id);
  pid_t current_hog_pid = -1;
  uint32_t hog_size = sizeof(pid_t);
  AudioObjectPropertyAddress addr = {
      .mSelector = kAudioDevicePropertyHogMode,
      .mScope = kAudioObjectPropertyScopeGlobal,
      .mElement = kAudioObjectPropertyElementMain};
  if (AudioObjectGetPropertyData(device_id, &addr, 0, NULL, &hog_size,
                                 &current_hog_pid) == noErr) {
    if (current_hog_pid == camilla_pid) {
      logger_debug(&g_coreaudio_dev_logger, "Releasing exclusive access.");
      pid_t pid = -1;
      AudioObjectSetPropertyData(device_id, &addr, 0, NULL, sizeof(pid_t), &pid);
      pid_t new_device_pid = -1;
      if (AudioObjectGetPropertyData(device_id, &addr, 0, NULL, &hog_size,
                                     &new_device_pid) == noErr &&
          new_device_pid == -1) {
        logger_debug(&g_coreaudio_dev_logger, "Exclusive access released.");
      } else {
        logger_warn(&g_coreaudio_dev_logger,
                    "Could not release exclusive access. CamillaDSP pid: %d, "
                    "device owner pid: %d.",
                    (int)camilla_pid, (int)new_device_pid);
      }
    }
  }
}

// MARK: - Device Liveness Watcher

static OSStatus core_audio_alive_listener_proc(
    AudioObjectID inObjectID, UInt32 inNumberAddresses,
    const AudioObjectPropertyAddress* inAddresses, void* inClientData) {
  (void)inNumberAddresses;
  (void)inAddresses;
  _Atomic bool* is_alive = (_Atomic bool*)inClientData;
  if (!is_alive) return noErr;
  uint32_t alive = 0;
  uint32_t size = sizeof(uint32_t);
  AudioObjectPropertyAddress addr = {
      .mSelector = kAudioDevicePropertyDeviceIsAlive,
      .mScope = kAudioObjectPropertyScopeGlobal,
      .mElement = kAudioObjectPropertyElementMain};
  if (AudioObjectGetPropertyData(inObjectID, &addr, 0, NULL, &size, &alive) ==
      noErr) {
    atomic_store_explicit(is_alive, (alive != 0), memory_order_release);
  } else {
    atomic_store_explicit(is_alive, false, memory_order_release);
  }
  return noErr;
}

bool core_audio_device_add_alive_watcher(AudioDeviceID device_id,
                                         _Atomic bool* is_alive_flag) {
  if (device_id == 0 || !is_alive_flag) return false;
  atomic_store_explicit(is_alive_flag, true, memory_order_release);
  AudioObjectPropertyAddress alive_addr = {
      .mSelector = kAudioDevicePropertyDeviceIsAlive,
      .mScope = kAudioObjectPropertyScopeGlobal,
      .mElement = kAudioObjectPropertyElementMain};
  return (AudioObjectAddPropertyListener(device_id, &alive_addr,
                                         core_audio_alive_listener_proc,
                                         is_alive_flag) == noErr);
}

void core_audio_device_remove_alive_watcher(AudioDeviceID device_id,
                                            _Atomic bool* is_alive_flag) {
  if (device_id == 0 || !is_alive_flag) return;
  AudioObjectPropertyAddress alive_addr = {
      .mSelector = kAudioDevicePropertyDeviceIsAlive,
      .mScope = kAudioObjectPropertyScopeGlobal,
      .mElement = kAudioObjectPropertyElementMain};
  AudioObjectRemovePropertyListener(
      device_id, &alive_addr, core_audio_alive_listener_proc, is_alive_flag);
}

// MARK: - Stream Configuration & Lifecycle Helpers

bool core_audio_device_configure_stream(
    AudioDeviceID device_id, core_audio_scope_t scope, double sample_rate,
    const char* format_str, bool has_format, size_t channels, size_t chunk_size,
    binary_sample_format_t* out_binary_format, size_t* out_bytes_per_sample,
    size_t* out_blockalign) {
  if (device_id == 0 || channels == 0) return false;

  if (has_format && format_str && format_str[0]) {
    core_audio_device_set_matching_physical_format(
        device_id, scope, sample_rate, format_str, channels);
    AudioStreamBasicDescription actual_asbd;
    core_audio_device_set_matching_virtual_format(
        device_id, scope, sample_rate, format_str, channels, &actual_asbd);
  }
  core_audio_device_set_nominal_sample_rate(device_id, sample_rate);
  core_audio_device_set_buffer_frame_size(device_id, (uint32_t)chunk_size,
                                          scope);

  binary_sample_format_t bin_fmt = BINARY_SAMPLE_FORMAT_F32_LE;
  AudioStreamBasicDescription current_asbd;
  if (core_audio_device_get_virtual_format(device_id, scope, &current_asbd)) {
    binary_sample_format_t parsed_fmt =
        core_audio_device_asbd_to_binary_format(&current_asbd);
    if (parsed_fmt != BINARY_SAMPLE_FORMAT_INVALID) {
      bin_fmt = parsed_fmt;
    }
  }

  size_t bytes_per_samp = sample_format_bytes_per_sample(bin_fmt);
  if (bytes_per_samp == 0) {
    bytes_per_samp = sizeof(float);
    bin_fmt = BINARY_SAMPLE_FORMAT_F32_LE;
  }
  size_t blkalign = (size_t)channels * bytes_per_samp;

  if (out_binary_format) *out_binary_format = bin_fmt;
  if (out_bytes_per_sample) *out_bytes_per_sample = bytes_per_samp;
  if (out_blockalign) *out_blockalign = blkalign;
  return true;
}

void core_audio_device_stop_and_destroy_ioproc(
    AudioDeviceID device_id, AudioDeviceIOProcID io_proc_id,
    const _Atomic int* active_callbacks) {
  if (device_id == 0 || io_proc_id == NULL) return;
  AudioDeviceStop(device_id, io_proc_id);
  if (active_callbacks) {
    int timeout_count = 1000;  // 500ms max (1000 * 500us)
    while (atomic_load_explicit(active_callbacks, memory_order_acquire) > 0 &&
           timeout_count-- > 0) {
      usleep(500);
    }
  }
  AudioDeviceDestroyIOProcID(device_id, io_proc_id);
}

bool core_audio_device_check_rate_change(AudioDeviceID device_id,
                                         rate_change_watcher_t* watcher,
                                         double expected_rate,
                                         double* out_rate) {
  if (watcher) {
    return rate_change_watcher_get_pending_change(watcher, out_rate);
  }
  if (device_id != 0) {
    double current = 0.0;
    if (core_audio_device_get_nominal_sample_rate(device_id, &current)) {
      if (current > 0.0 && fabs(current - expected_rate) >= 0.5) {
        if (out_rate) *out_rate = current;
        return true;
      }
    }
  }
  return false;
}

#endif  // ENABLE_COREAUDIO
