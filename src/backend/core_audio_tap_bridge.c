// CoreAudio Tap Bridge for macOS 14.2+
// Pure C implementation of Aggregate Tap Device lifecycle and process
// discovery.

#include "backend/core_audio_tap_bridge.h"

#if defined(ENABLE_COREAUDIO)

#include <AudioToolbox/AudioToolbox.h>
#include <Availability.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <ctype.h>
#include <libproc.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "backend/core_audio_device.h"
#include "backend/core_audio_tap_desc.h"
#include "logging/app_logger.h"
#include "utils/cdsp_time.h"

static const logger_t g_tap_logger = {"dsp.backend.coreaudio.tap"};

bool cdsp_tap_is_supported(void) { return cdsp_tap_desc_is_supported(); }

bool cdsp_tap_is_app_device(const char *device_name) {
  if (!device_name)
    return false;
  return (strncasecmp(device_name, "app:", 4) == 0);
}

const char *cdsp_tap_parse_app_name(const char *device_name) {
  if (!cdsp_tap_is_app_device(device_name))
    return NULL;
  const char *p = device_name + 4;
  while (*p == ' ' || *p == '\t')
    p++;
  return p;
}

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
 * @brief Resolve process info (name and PID) for a CoreAudio process object.
 *
 * Queries kAudioProcessPropertyPID on the AudioObjectID and resolves the
 * process name via libproc's proc_name().
 *
 * @param procObj CoreAudio Process AudioObjectID.
 * @param[out] out_name Buffer to store process name.
 * @param name_len Length of out_name buffer.
 * @param[out] out_pid Optional pointer to receive Unix PID.
 * @return true if successfully resolved, false otherwise.
 */
static bool get_coreaudio_process_info(AudioObjectID procObj, char *out_name,
                                       size_t name_len, pid_t *out_pid) {
  if (procObj == kAudioObjectUnknown || !out_name || name_len == 0)
    return false;
  out_name[0] = '\0';

  pid_t pid = 0;
  UInt32 pidSize = sizeof(pid);
  AudioObjectPropertyAddress pidAddr = {kAudioProcessPropertyPID,
                                        kAudioObjectPropertyScopeGlobal,
                                        kAudioObjectPropertyElementMain};
  if (AudioObjectGetPropertyData(procObj, &pidAddr, 0, NULL, &pidSize, &pid) !=
          noErr ||
      pid == 0 || pid == getpid()) {
    return false;
  }
  if (out_pid) {
    *out_pid = pid;
  }

  if (proc_name(pid, out_name, (uint32_t)name_len) <= 0 ||
      out_name[0] == '\0') {
    return false;
  }
  return true;
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
 * @brief Create a CoreFoundation array of AudioObjectID NSNumbers matching a
 * process name.
 *
 * @param app_name Process name to match (e.g. "Google Chrome", "Music").
 * @return CFArrayRef containing CFNumberRef objects, or NULL on failure. Caller
 * releases.
 */
static CFArrayRef create_process_object_array_for_app(const char *app_name) {
  if (!app_name || app_name[0] == '\0')
    return NULL;

  UInt32 count = 0;
  AudioObjectID *procs = get_all_coreaudio_process_objects(&count);
  if (!procs || count == 0) {
    free(procs);
    return NULL;
  }

  CFMutableArrayRef matches =
      CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
  for (UInt32 i = 0; i < count; i++) {
    char procName[256] = {0};
    pid_t pid = 0;
    if (get_coreaudio_process_info(procs[i], procName, sizeof(procName),
                                   &pid)) {
      if (strcasecmp(procName, app_name) == 0) {
        uint32_t objVal = (uint32_t)procs[i];
        CFNumberRef num =
            CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &objVal);
        if (num) {
          CFArrayAppendValue(matches, num);
          CFRelease(num);
        }
      }
    }
  }
  free(procs);
  return matches;
}

int cdsp_tap_get_available_app_names(char out_names[][256], int max_names) {
  if (!out_names || max_names <= 0)
    return 0;

  UInt32 count = 0;
  AudioObjectID *procs = get_all_coreaudio_process_objects(&count);
  if (!procs || count == 0) {
    free(procs);
    return 0;
  }

  int res = 0;
  for (UInt32 i = 0; i < count; i++) {
    char procName[256] = {0};
    pid_t pid = 0;
    if (!get_coreaudio_process_info(procs[i], procName, sizeof(procName),
                                    &pid)) {
      continue;
    }

    // Deduplicate case-insensitively
    bool duplicate = false;
    char candidate[256];
    snprintf(candidate, sizeof(candidate), "app:%s", procName);
    for (int j = 0; j < res; j++) {
      if (strcasecmp(out_names[j], candidate) == 0) {
        duplicate = true;
        break;
      }
    }
    if (!duplicate && res < max_names) {
      snprintf(out_names[res], 256, "%s", candidate);
      res++;
    }
  }
  free(procs);

  // Sort alphabetically
  qsort(out_names, res, sizeof(out_names[0]),
        (int (*)(const void *, const void *))strcasecmp);
  return res;
}

bool cdsp_tap_find_app(const char *app_name, char *out_matched_name,
                       size_t max_len) {
  if (!app_name || app_name[0] == '\0')
    return false;

  UInt32 count = 0;
  AudioObjectID *procs = get_all_coreaudio_process_objects(&count);
  if (!procs || count == 0) {
    free(procs);
    return false;
  }

  bool found = false;
  for (UInt32 i = 0; i < count; i++) {
    char procName[256] = {0};
    pid_t pid = 0;
    if (get_coreaudio_process_info(procs[i], procName, sizeof(procName),
                                   &pid)) {
      if (strcasecmp(procName, app_name) == 0) {
        if (out_matched_name && max_len > 0) {
          snprintf(out_matched_name, max_len, "%s", procName);
        }
        found = true;
        break;
      }
    }
  }
  free(procs);
  return found;
}

AudioDeviceID cdsp_tap_get_active_device_for_app(const char *app_name) {
  AudioDeviceID default_id =
      core_audio_device_id_for_name(NULL, CORE_AUDIO_SCOPE_OUTPUT);
  if (!app_name || app_name[0] == '\0') {
    return default_id;
  }

  UInt32 count = 0;
  AudioObjectID *procs = get_all_coreaudio_process_objects(&count);
  if (!procs || count == 0) {
    free(procs);
    return default_id;
  }

  AudioDeviceID matched_dev = kAudioObjectUnknown;
  for (UInt32 i = 0; i < count; i++) {
    char procName[256] = {0};
    pid_t pid = 0;
    if (!get_coreaudio_process_info(procs[i], procName, sizeof(procName),
                                    &pid)) {
      continue;
    }

    if (strcasecmp(procName, app_name) == 0) {
      AudioObjectPropertyAddress devAddr = {kAudioProcessPropertyDevices,
                                            kAudioObjectPropertyScopeOutput,
                                            kAudioObjectPropertyElementMain};
      UInt32 devSize = 0;
      if (AudioObjectGetPropertyDataSize(procs[i], &devAddr, 0, NULL,
                                         &devSize) == noErr &&
          devSize > 0) {
        UInt32 numDevs = devSize / sizeof(AudioObjectID);
        AudioObjectID *devIDs = (AudioObjectID *)malloc(devSize);
        if (devIDs) {
          if (AudioObjectGetPropertyData(procs[i], &devAddr, 0, NULL, &devSize,
                                         devIDs) == noErr &&
              numDevs > 0) {
            for (UInt32 d = 0; d < numDevs; d++) {
              if (devIDs[d] != kAudioObjectUnknown) {
                matched_dev = devIDs[d];
                break;
              }
            }
          }
          free(devIDs);
        }
      }
      if (matched_dev != kAudioObjectUnknown) {
        break;
      }
    }
  }
  free(procs);

  return (matched_dev != kAudioObjectUnknown) ? matched_dev : default_id;
}

bool cdsp_tap_resolve_app_device(const char *device_name, bool is_capture,
                                 AudioDeviceID *out_device_id,
                                 device_error_t *err) {
  if (!is_capture) {
    if (err) {
      device_error_init(err, DEVICE_ERROR_NOT_FOUND,
                        "Application tap is only supported for capture");
    }
    return false;
  }
  if (!cdsp_tap_is_supported()) {
    if (err) {
      device_error_init(err, DEVICE_ERROR_NOT_FOUND,
                        "CoreAudio process tap requires macOS 14.2+");
    }
    return false;
  }
  const char *app_name = cdsp_tap_parse_app_name(device_name);
  if (!app_name || app_name[0] == '\0' ||
      !cdsp_tap_find_app(app_name, NULL, 0)) {
    if (err) {
      device_error_init(err, DEVICE_ERROR_NOT_FOUND,
                        "Application or process not found");
    }
    return false;
  }
  if (out_device_id) {
    *out_device_id = cdsp_tap_get_active_device_for_app(app_name);
  }
  return true;
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

  AudioDeviceID target_dev_id = kAudioObjectUnknown;
  AudioObjectID tapID = kAudioObjectUnknown;
  CFStringRef tapUIDStr = NULL;
  OSStatus status = noErr;

  bool is_app = cdsp_tap_is_app_device(device_name);
  CFArrayRef procIDs = NULL;

  if (is_app) {
    const char *app_name = cdsp_tap_parse_app_name(device_name);
    if (!app_name || app_name[0] == '\0') {
      logger_error(&g_tap_logger, "No application name specified in '%s'",
                   device_name);
      return kAudioHardwareBadDeviceError;
    }

    procIDs = create_process_object_array_for_app(app_name);
    if (!procIDs || CFArrayGetCount(procIDs) == 0) {
      if (procIDs)
        CFRelease(procIDs);
      logger_error(
          &g_tap_logger,
          "Could not find running process with CoreAudio support for app '%s'",
          app_name);
      return kAudioHardwareBadDeviceError;
    }
    target_dev_id = cdsp_tap_get_active_device_for_app(app_name);
  } else {
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
    procIDs = excludeProcs;
    target_dev_id =
        core_audio_device_id_for_name(device_name, CORE_AUDIO_SCOPE_OUTPUT);
  }

  if (target_dev_id == kAudioObjectUnknown) {
    if (procIDs)
      CFRelease(procIDs);
    logger_error(&g_tap_logger, "Could not resolve output device for tap.");
    return kAudioHardwareBadDeviceError;
  }

  CFStringRef targetDeviceUID = copy_device_uid_for_id(target_dev_id);
  if (!targetDeviceUID) {
    if (procIDs)
      CFRelease(procIDs);
    logger_error(&g_tap_logger, "Could not resolve device UID for tap.");
    return kAudioHardwareBadDeviceError;
  }

  status = cdsp_tap_desc_create(procIDs, !is_app, targetDeviceUID, &tapID,
                                &tapUIDStr);
  CFRelease(procIDs);
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
