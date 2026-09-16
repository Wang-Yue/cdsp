/**
 * @file core_audio_tap_desc.h
 * @brief Minimal Objective-C helper interface for CATapDescription
 * (macOS 14.2+).
 *
 * Provides a minimal, isolated C interface wrapping Apple's Objective-C
 * CATapDescription class and AudioHardwareCreateProcessTap API under ARC.
 */

#ifndef CLIB_BACKEND_CORE_AUDIO_TAP_DESC_H
#define CLIB_BACKEND_CORE_AUDIO_TAP_DESC_H

#if defined(ENABLE_COREAUDIO)

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdbool.h>

/**
 * @brief Check if CoreAudio process tap API is supported at runtime
 * (macOS 14.2+).
 *
 * @return true if running on macOS 14.2 or later with tap support available.
 */
bool cdsp_tap_desc_is_supported(void);

/**
 * @brief Create a CoreAudio process tap object using CATapDescription.
 *
 * Allocates and configures a CATapDescription instance with the specified
 * process list and target device UID, applies private tap visibility and muting
 * behavior, and calls AudioHardwareCreateProcessTap to register the tap with
 * CoreAudio HAL.
 *
 * @param proc_ids CoreFoundation array of CFNumberRef (AudioObjectID) processes
 * to include or exclude.
 * @param is_exclusive If true, excludes proc_ids (for device taps). If false,
 * includes proc_ids (for process taps).
 * @param target_device_uid CoreFoundation string of the target output device
 * UID.
 * @param[out] out_tap_id Pointer to receive the AudioObjectID of the created
 * tap.
 * @param[out] out_tap_uuid_str Pointer to receive the retained CFStringRef tap
 * UUID string (caller releases).
 * @return OSStatus noErr on success, or an error code on failure.
 */
OSStatus cdsp_tap_desc_create(CFArrayRef proc_ids, bool is_exclusive,
                              CFStringRef target_device_uid,
                              AudioObjectID *out_tap_id,
                              CFStringRef *out_tap_uuid_str);

/**
 * @brief Destroy a CoreAudio process/device tap.
 *
 * @param tap_id The AudioObjectID of the tap to destroy.
 * @return OSStatus noErr on success, or an error code on failure.
 */
OSStatus cdsp_tap_desc_destroy(AudioObjectID tap_id);

#endif // ENABLE_COREAUDIO

#endif // CLIB_BACKEND_CORE_AUDIO_TAP_DESC_H
