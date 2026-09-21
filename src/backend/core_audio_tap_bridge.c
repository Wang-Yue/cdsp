// CoreAudio Tap Bridge for macOS 14.2+
// Pure C implementation of Aggregate Tap Device lifecycle.

#include "backend/core_audio_tap_bridge.h"

#if defined(ENABLE_COREAUDIO)

#include <AudioToolbox/AudioToolbox.h>
#include <Availability.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "backend/core_audio_device.h"
#include "backend/core_audio_tap_desc.h"
#include "logging/app_logger.h"
#include "utils/cdsp_time.h"

static const logger_t g_tap_logger = {"dsp.backend.coreaudio.tap"};

bool cdsp_tap_is_supported(void) { return cdsp_tap_desc_is_supported(); }

/**
 * @brief Retrieve the list of active process AudioObjectIDs from CoreAudio HAL.
 *
 * @param[out] out_count Pointer to receive the number of returned process
 * objects.
 * @return Dynamically allocated array of AudioObjectIDs (caller frees), or NULL
 * on failure.
 */
static AudioObjectID *get_all_coreaudio_process_objects(UInt32 *out_count) {
  if (out_count)
    *out_count = 0;
  UInt32 size = 0;
  AudioObjectPropertyAddress addr = {kAudioHardwarePropertyProcessObjectList,
                                     kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain};
  if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, NULL,
                                     &size) != noErr ||
      size == 0) {
    return NULL;
  }
  UInt32 count = size / sizeof(AudioObjectID);
  AudioObjectID *procs = (AudioObjectID *)malloc(size);
  if (!procs)
    return NULL;
  if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL,
                                 &size, procs) != noErr) {
    free(procs);
    return NULL;
  }
  if (out_count)
    *out_count = count;
  return procs;
}

/**
 * @brief Find the CoreAudio AudioObjectID associated with a given Unix PID.
 *
 * @param pid Unix process identifier.
 * @return AudioObjectID of the process, or kAudioObjectUnknown if not
 * registered.
 */
static AudioObjectID get_process_object_id_for_pid(pid_t pid) {
  UInt32 count = 0;
  AudioObjectID *procs = get_all_coreaudio_process_objects(&count);
  if (!procs || count == 0) {
    free(procs);
    return kAudioObjectUnknown;
  }

  AudioObjectID found = kAudioObjectUnknown;
  for (UInt32 i = 0; i < count; i++) {
    pid_t procPID = 0;
    UInt32 pidSize = sizeof(procPID);
    AudioObjectPropertyAddress pidAddr = {kAudioProcessPropertyPID,
                                          kAudioObjectPropertyScopeGlobal,
                                          kAudioObjectPropertyElementMain};
    if (AudioObjectGetPropertyData(procs[i], &pidAddr, 0, NULL, &pidSize,
                                   &procPID) == noErr) {
      if (procPID == pid) {
        found = procs[i];
        break;
      }
    }
  }
  free(procs);
  return found;
}

/**
 * @brief Count the channels a device exposes on the input scope.
 *
 * Used to determine where a tap's channels begin in the aggregate device's flat
 * input channel list: the HAL lays out sub-device channels first, then taps.
 *
 * @param dev Device to inspect, or kAudioObjectUnknown.
 * @return Total input channel count, or 0 if the device has no input streams.
 */
static UInt32 device_input_channel_count(AudioDeviceID dev) {
  if (dev == kAudioObjectUnknown)
    return 0;
  AudioObjectPropertyAddress addr = {kAudioDevicePropertyStreamConfiguration,
                                     kAudioObjectPropertyScopeInput,
                                     kAudioObjectPropertyElementMain};
  UInt32 size = 0;
  if (AudioObjectGetPropertyDataSize(dev, &addr, 0, NULL, &size) != noErr ||
      size == 0) {
    return 0;
  }
  AudioBufferList *abl = (AudioBufferList *)malloc(size);
  if (!abl)
    return 0;
  UInt32 channels = 0;
  if (AudioObjectGetPropertyData(dev, &addr, 0, NULL, &size, abl) == noErr) {
    for (UInt32 i = 0; i < abl->mNumberBuffers; i++) {
      channels += abl->mBuffers[i].mNumberChannels;
    }
  }
  free(abl);
  return channels;
}

/**
 * @brief Retrieve the persistent unique identifier (UID) string for a device by
 * ID.
 *
 * @param dev Device identifier.
 * @return Retained CFStringRef containing the device UID, or NULL if
 * unavailable. Caller releases.
 */
static CFStringRef copy_device_uid_for_id(AudioDeviceID dev) {
  if (dev == kAudioObjectUnknown)
    return NULL;
  CFStringRef uidRef = NULL;
  UInt32 size = sizeof(uidRef);
  AudioObjectPropertyAddress addr = {kAudioDevicePropertyDeviceUID,
                                     kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain};
  if (AudioObjectGetPropertyData(dev, &addr, 0, NULL, &size, &uidRef) !=
          noErr ||
      !uidRef) {
    return NULL;
  }
  return uidRef;
}

/**
 * @brief Build the CoreFoundation dictionary describing a private Aggregate Tap
 * Device.
 *
 * @param clockUID UID of the clock sub-device.
 * @param tapUIDStr UUID string of the process tap.
 * @return Retained CFDictionaryRef. Caller releases.
 */
static CFDictionaryRef create_tap_aggregate_dict(CFStringRef clockUID,
                                                 CFStringRef tapUIDStr) {
  // Sub-device entry: { "uid": clockUID }
  CFStringRef subDevKeys[] = {CFSTR(kAudioSubDeviceUIDKey)};
  CFTypeRef subDevValues[] = {clockUID};
  CFDictionaryRef subDevDict = CFDictionaryCreate(
      kCFAllocatorDefault, (const void **)subDevKeys,
      (const void **)subDevValues, 1, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);

  CFTypeRef subDevArrayValues[] = {subDevDict};
  CFArrayRef subDevList = CFArrayCreate(kCFAllocatorDefault, subDevArrayValues,
                                        1, &kCFTypeArrayCallBacks);
  CFRelease(subDevDict);

  // Tap entry: { "uid": tapUIDStr, "drift": kCFBooleanTrue }
  CFStringRef tapKeys[] = {CFSTR(kAudioSubTapUIDKey),
                           CFSTR(kAudioSubTapDriftCompensationKey)};
  CFTypeRef tapValues[] = {tapUIDStr, kCFBooleanTrue};
  CFDictionaryRef tapDict = CFDictionaryCreate(
      kCFAllocatorDefault, (const void **)tapKeys, (const void **)tapValues, 2,
      &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

  CFTypeRef tapArrayValues[] = {tapDict};
  CFArrayRef tapList = CFArrayCreate(kCFAllocatorDefault, tapArrayValues, 1,
                                     &kCFTypeArrayCallBacks);
  CFRelease(tapDict);

  // Aggregate UID: "cdsp.tap.aggregate.<tapUIDStr>"
  CFStringRef aggUID = CFStringCreateWithFormat(
      kCFAllocatorDefault, NULL, CFSTR("cdsp.tap.aggregate.%@"), tapUIDStr);

  CFStringRef aggKeys[] = {CFSTR(kAudioAggregateDeviceNameKey),
                           CFSTR(kAudioAggregateDeviceUIDKey),
                           CFSTR(kAudioAggregateDeviceMainSubDeviceKey),
                           CFSTR(kAudioAggregateDeviceIsPrivateKey),
                           CFSTR(kAudioAggregateDeviceIsStackedKey),
                           CFSTR(kAudioAggregateDeviceTapAutoStartKey),
                           CFSTR(kAudioAggregateDeviceSubDeviceListKey),
                           CFSTR(kAudioAggregateDeviceTapListKey)};

  int one = 1;
  int zero = 0;
  CFNumberRef numOne =
      CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &one);
  CFNumberRef numZero =
      CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &zero);

  CFTypeRef aggValues[] = {CFSTR("cdsp_tap_aggregate"),
                           aggUID,
                           clockUID,
                           numOne,         // private = 1
                           numZero,        // stacked = 0
                           kCFBooleanTrue, // autostart = true
                           subDevList,
                           tapList};

  CFDictionaryRef aggDesc = CFDictionaryCreate(
      kCFAllocatorDefault, (const void **)aggKeys, (const void **)aggValues, 8,
      &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

  CFRelease(aggUID);
  CFRelease(subDevList);
  CFRelease(tapList);
  CFRelease(numOne);
  CFRelease(numZero);

  return aggDesc;
}

OSStatus cdsp_tap_create(const char *device_name,
                         cdsp_tap_handle_t *out_handle) {
  if (!out_handle) {
    return kAudioHardwareIllegalOperationError;
  }
  memset(out_handle, 0, sizeof(*out_handle));
  out_handle->tap_id = kAudioObjectUnknown;
  out_handle->aggregate_dev_id = kAudioObjectUnknown;

  if (!cdsp_tap_is_supported()) {
    logger_error(&g_tap_logger,
                 "CoreAudio hardware tap requires macOS 14.2 or later.");
    return kAudioHardwareIllegalOperationError;
  }

  AudioDeviceID target_dev_id =
      core_audio_device_id_for_name(device_name, CORE_AUDIO_SCOPE_OUTPUT);
  if (target_dev_id == kAudioObjectUnknown) {
    logger_error(&g_tap_logger, "Could not resolve output device for tap.");
    return kAudioHardwareBadDeviceError;
  }

  AudioObjectID myProcObj = get_process_object_id_for_pid(getpid());
  CFMutableArrayRef excludeProcs =
      CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
  if (myProcObj != kAudioObjectUnknown) {
    uint32_t objVal = (uint32_t)myProcObj;
    CFNumberRef num =
        CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &objVal);
    if (num) {
      CFArrayAppendValue(excludeProcs, num);
      CFRelease(num);
    }
  } else {
    logger_warn(&g_tap_logger,
                "Could not resolve own CoreAudio process object; tap "
                "self-exclusion is not active.");
  }

  CFStringRef targetDeviceUID = copy_device_uid_for_id(target_dev_id);
  if (!targetDeviceUID) {
    CFRelease(excludeProcs);
    logger_error(&g_tap_logger, "Could not resolve device UID for tap.");
    return kAudioHardwareBadDeviceError;
  }

  AudioObjectID tapID = kAudioObjectUnknown;
  CFStringRef tapUIDStr = NULL;
  OSStatus status =
      cdsp_tap_desc_create(excludeProcs, targetDeviceUID, &tapID, &tapUIDStr);
  CFRelease(excludeProcs);
  CFRelease(targetDeviceUID);

  if (status != noErr || tapID == kAudioObjectUnknown || !tapUIDStr) {
    logger_error(&g_tap_logger, "Failed to create tap (status=%d)",
                 (int)status);
    return (status != noErr) ? status : kAudioHardwareUnspecifiedError;
  }

  out_handle->tap_id = tapID;

  // The aggregate needs a real clock source, otherwise its IO cycle is
  // never reliably scheduled. Use the tapped device as the main
  // sub-device, exactly as Apple's reference tap composition does.
  AudioDeviceID clock_dev_id = target_dev_id;
  CFStringRef clockUID = copy_device_uid_for_id(clock_dev_id);
  if (!clockUID) {
    cdsp_tap_desc_destroy(tapID);
    CFRelease(tapUIDStr);
    out_handle->tap_id = kAudioObjectUnknown;
    logger_error(
        &g_tap_logger,
        "Could not resolve a clock sub-device UID for the tap aggregate.");
    return kAudioHardwareBadDeviceError;
  }

  CFDictionaryRef aggDesc = create_tap_aggregate_dict(clockUID, tapUIDStr);
  CFRelease(clockUID);
  CFRelease(tapUIDStr);

  AudioDeviceID aggDeviceID = kAudioObjectUnknown;
  status = AudioHardwareCreateAggregateDevice(aggDesc, &aggDeviceID);
  CFRelease(aggDesc);

  if (status != noErr || aggDeviceID == kAudioObjectUnknown) {
    cdsp_tap_desc_destroy(tapID);
    out_handle->tap_id = kAudioObjectUnknown;
    logger_error(&g_tap_logger, "AudioHardwareCreateAggregateDevice failed: %d",
                 (int)status);
    return (status != noErr) ? status : kAudioHardwareUnspecifiedError;
  }
  out_handle->aggregate_dev_id = aggDeviceID;

  // The HAL composes the aggregate asynchronously. Binding an AudioUnit
  // before the tap has been attached yields a device that delivers
  // nothing, which is the classic "works only sometimes" symptom. Wait
  // for the tap's channels to show up before handing the device back.
  UInt32 sub_channels = device_input_channel_count(clock_dev_id);
  UInt32 agg_channels = 0;
  // Poll for up to 500 ms (100 iterations * 5ms).
  for (int attempt = 0; attempt < 100; attempt++) {
    agg_channels = device_input_channel_count(aggDeviceID);
    if (agg_channels > sub_channels)
      break;
    cdsp_sleep_us(5000);
  }

  if (agg_channels > sub_channels) {
    out_handle->tap_channel_offset = sub_channels;
  } else {
    // The tap's channels never materialised alongside the sub-device's.
    // Warn and fall back to offset 0: capture may end up reading the
    // sub-device's own input instead of the tap.
    out_handle->tap_channel_offset = 0;
    logger_warn(&g_tap_logger,
                "Tap channels did not appear on aggregate device "
                "(sub-device channels=%u, aggregate channels=%u).",
                (unsigned)sub_channels, (unsigned)agg_channels);
  }

  // Wait for the aggregate to adopt its clock sub-device's rate.
  //
  // Adoption is asynchronous, and until it completes the aggregate
  // reports a default rate. Binding an AudioUnit to an unsettled
  // aggregate produces kAudioUnitErr_CannotDoInCurrentContext (-10863):
  // AUHAL latches the rate it sees at bind time, and then the device
  // changes under it.
  double clock_rate = 0.0;
  core_audio_device_get_nominal_sample_rate(clock_dev_id, &clock_rate);
  double agg_rate = 0.0;
  if (clock_rate > 0.0) {
    // Poll for up to 500 ms (100 iterations * 5ms).
    for (int attempt = 0; attempt < 100; attempt++) {
      if (core_audio_device_get_nominal_sample_rate(aggDeviceID, &agg_rate) &&
          fabs(agg_rate - clock_rate) < 1.0) {
        break;
      }
      cdsp_sleep_us(5000);
    }
    if (fabs(agg_rate - clock_rate) >= 1.0) {
      logger_warn(&g_tap_logger,
                  "Tap aggregate settled at %.0f Hz but its clock "
                  "device runs at %.0f Hz; capture may over- or "
                  "under-run.",
                  agg_rate, clock_rate);
    }
  } else {
    core_audio_device_get_nominal_sample_rate(aggDeviceID, &agg_rate);
  }
  logger_info(&g_tap_logger,
              "Tap aggregate composed: agg_id=%u @ %.0f Hz, "
              "clock_dev=%u @ %.0f Hz",
              (unsigned)aggDeviceID, agg_rate, (unsigned)clock_dev_id,
              clock_rate);
  logger_info(&g_tap_logger,
              "Tap channel layout: input_channels=%u, "
              "sub_device_channels=%u, tap_channel_offset=%u",
              (unsigned)agg_channels, (unsigned)sub_channels,
              (unsigned)out_handle->tap_channel_offset);

  return noErr;
}

bool cdsp_tap_open(const char *device_name, cdsp_tap_handle_t *out_handle,
                   backend_error_t *err) {
  if (!out_handle) {
    return false;
  }
  memset(out_handle, 0, sizeof(*out_handle));
  OSStatus status = cdsp_tap_create(device_name, out_handle);
  if (status != noErr || out_handle->aggregate_dev_id == kAudioObjectUnknown) {
    logger_error(&g_tap_logger, "Failed to create CoreAudio Tap: OSStatus %d",
                 (int)status);
    if (err) {
      char msg[128];
      snprintf(msg, sizeof(msg), "Failed to create CoreAudio Tap (status=%d)",
               (int)status);
      backend_error_init(err,
                         status == kAudioHardwareBadDeviceError
                             ? BACKEND_ERROR_DEVICE_NOT_FOUND
                             : BACKEND_ERROR_INITIALIZATION_FAILED,
                         msg);
    }
    return false;
  }
  return true;
}

OSStatus cdsp_tap_destroy(AudioObjectID tap_id,
                          AudioDeviceID aggregate_dev_id) {
  if (aggregate_dev_id != kAudioObjectUnknown) {
    AudioHardwareDestroyAggregateDevice(aggregate_dev_id);
  }
  if (tap_id != kAudioObjectUnknown) {
    cdsp_tap_desc_destroy(tap_id);
  }
  return noErr;
}

OSStatus cdsp_tap_destroy_handle(cdsp_tap_handle_t *handle) {
  if (!handle) {
    return noErr;
  }
  OSStatus status = cdsp_tap_destroy(handle->tap_id, handle->aggregate_dev_id);
  memset(handle, 0, sizeof(*handle));
  return status;
}

bool cdsp_tap_apply_channel_map(AudioUnit audio_unit,
                                const cdsp_tap_handle_t *tap, size_t channels,
                                backend_error_t *err) {
  if (!audio_unit) {
    if (err) {
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "AudioUnit is NULL when applying tap channel map");
    }
    return false;
  }
  // If the tapped device exposes its own input channels (e.g. duplex devices or
  // audio interfaces), those appear first in the aggregate's flat channel list.
  // Install an AUHAL channel map to route from the tap's offset (Apple TN2091).
  if (!tap || tap->tap_channel_offset == 0 || channels == 0) {
    return true;
  }

  uint32_t tap_channel_offset = tap->tap_channel_offset;
  size_t map_count = channels;
  SInt32 *channel_map = (SInt32 *)malloc(map_count * sizeof(SInt32));
  if (!channel_map) {
    logger_error(&g_tap_logger,
                 "Failed to allocate tap channel map buffer (%zu channels)",
                 map_count);
    if (err) {
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to allocate tap channel map buffer");
    }
    return false;
  }

  for (size_t i = 0; i < map_count; i++) {
    channel_map[i] = (SInt32)(tap_channel_offset + i);
  }

  OSStatus status = AudioUnitSetProperty(
      audio_unit, kAudioOutputUnitProperty_ChannelMap, kAudioUnitScope_Output,
      1, channel_map, (UInt32)(map_count * sizeof(SInt32)));

  free(channel_map);

  if (status != noErr) {
    logger_error(&g_tap_logger,
                 "Failed to set tap channel map on AudioUnit: status=%d",
                 (int)status);
    if (err) {
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to set tap channel map on AudioUnit");
    }
    return false;
  }

  logger_info(
      &g_tap_logger,
      "Tap channel map installed: %zu channels starting at device channel %u.",
      map_count, (unsigned)tap_channel_offset);
  return true;
}

#endif // ENABLE_COREAUDIO
