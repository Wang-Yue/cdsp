// CoreAudio Tap Objective-C Descriptor Helper for macOS 14.2+
// Encapsulates CATapDescription and AudioHardwareCreateProcessTap under ARC.

#include "backend/core_audio_tap_desc.h"

#if defined(ENABLE_COREAUDIO)

#import <CoreAudio/CoreAudio.h>
#import <CoreFoundation/CoreFoundation.h>
#import <Availability.h>

#if defined(__MAC_14_2) && (__MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_14_2)
#define HAVE_COREAUDIO_TAP 1
#import <CoreAudio/AudioHardwareTapping.h>
#import <CoreAudio/CATapDescription.h>
#import <Foundation/NSUUID.h>
#else
#define HAVE_COREAUDIO_TAP 0
#endif

bool cdsp_tap_desc_is_supported(void) {
#if HAVE_COREAUDIO_TAP
    if (@available(macOS 14.2, *)) {
        return true;
    }
#endif
    return false;
}

OSStatus cdsp_tap_desc_create(CFArrayRef proc_ids,
                              bool is_exclusive,
                              CFStringRef target_device_uid,
                              AudioObjectID *out_tap_id,
                              CFStringRef *out_tap_uuid_str) {
    if (!out_tap_id || !out_tap_uuid_str || !proc_ids || !target_device_uid) {
        return kAudioHardwareIllegalOperationError;
    }
    *out_tap_id = kAudioObjectUnknown;
    *out_tap_uuid_str = NULL;

#if HAVE_COREAUDIO_TAP
    if (!cdsp_tap_desc_is_supported()) {
        return kAudioHardwareIllegalOperationError;
    }

    @autoreleasepool {
        NSArray* procArray = (__bridge NSArray*)proc_ids;
        NSString* uidString = (__bridge NSString*)target_device_uid;

        CATapDescription* desc = is_exclusive
            ? [[CATapDescription alloc] initExcludingProcesses:procArray andDeviceUID:uidString withStream:0]
            : [[CATapDescription alloc] initWithProcesses:procArray andDeviceUID:uidString withStream:0];
        if (!desc) {
            return kAudioHardwareBadDeviceError;
        }

        desc.UUID = [[NSUUID alloc] init];
        desc.muteBehavior = CATapMuted;
        desc.privateTap = YES;

        AudioObjectID tapID = kAudioObjectUnknown;
        OSStatus status = AudioHardwareCreateProcessTap(desc, &tapID);
        if (status != noErr || tapID == kAudioObjectUnknown) {
            return (status != noErr) ? status : kAudioHardwareUnspecifiedError;
        }

        *out_tap_id = tapID;
        *out_tap_uuid_str = (__bridge_retained CFStringRef)desc.UUID.UUIDString;
        return noErr;
    }
#else
    (void)proc_ids;
    (void)is_exclusive;
    (void)target_device_uid;
    return kAudioHardwareIllegalOperationError;
#endif
}

OSStatus cdsp_tap_desc_destroy(AudioObjectID tap_id) {
#if HAVE_COREAUDIO_TAP
    if (tap_id != kAudioObjectUnknown) {
        if (@available(macOS 14.2, *)) {
            return AudioHardwareDestroyProcessTap(tap_id);
        }
    }
#else
    (void)tap_id;
#endif
    return noErr;
}

#endif // ENABLE_COREAUDIO
