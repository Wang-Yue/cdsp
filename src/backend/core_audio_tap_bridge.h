/**
 * @file core_audio_tap_bridge.h
 * @brief CoreAudio Hardware Device Tap C Bridge for macOS 14.2+.
 *
 * Encapsulates CoreAudio audio tapping APIs and private aggregate device
 * composition for hardware output device loopback capture.
 */

#ifndef CLIB_BACKEND_CORE_AUDIO_TAP_BRIDGE_H
#define CLIB_BACKEND_CORE_AUDIO_TAP_BRIDGE_H

#if defined(ENABLE_COREAUDIO)

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "backend/backend_error.h"

/**
 * @brief Result of a successful tap + aggregate device composition.
 *
 * The aggregate device that carries a tap is built from a *main sub-device*
 * (which supplies the clock) plus the tap itself. The HAL presents the
 * aggregate's input channels as a single flat list with the sub-device's
 * channels first, so a consumer that wants the tap must skip
 * @ref tap_channel_offset channels — typically via an AUHAL channel map.
 */
typedef struct {
  /** @brief AudioObjectID of the created device tap. */
  AudioObjectID tap_id;
  /** @brief AudioDeviceID of the private aggregate device wrapping the tap. */
  AudioDeviceID aggregate_dev_id;
  /** @brief Index of the tap's first channel among the aggregate's inputs. */
  uint32_t tap_channel_offset;
} cdsp_tap_handle_t;

/**
 * @brief Check if the running macOS system supports CoreAudio device
 * taps (macOS 14.2+).
 *
 * @return true if running on macOS 14.2 or later with tap support available.
 */
bool cdsp_tap_is_supported(void);

/**
 * @brief Create a CoreAudio Device Tap and a private Aggregate Device wrapping
 * it.
 *
 * Configures CATapDescription with anti-feedback process exclusion (for cdsp's
 * own process), assigns private tap visibility, creates an
 * AudioHardwareProcessTap object, and packages it into a private aggregate
 * AudioDeviceID that can be bound to a HAL Output AudioUnit exactly like any
 * other capture device.
 *
 * The aggregate is composed following Apple's documented tap recipe: it
 * declares the target/default device as its main sub-device (clock source),
 * disables stacking, enables tap auto-start and sub-tap drift compensation.
 * Composition is asynchronous inside the HAL, so this function also waits until
 * the tap's channels are actually visible on the aggregate before returning.
 *
 * @param device_name Name of the output device to tap, or NULL/empty for the
 * current default output device.
 * @param[out] out_handle Pointer to receive the resolved tap composition
 * details.
 * @return 0 (noErr) on success, or an OSStatus error code.
 */
OSStatus cdsp_tap_create(const char *device_name,
                         cdsp_tap_handle_t *out_handle);

/**
 * @brief Helper to create a CoreAudio tap and format backend_error_t on
 * failure.
 *
 * @param device_name Name of the output device to tap, or NULL/empty.
 * @param[out] out_handle Pointer to receive the tap handle.
 * @param[out] err Optional error structure to populate on failure.
 * @return true on success, false on failure.
 */
bool cdsp_tap_open(const char *device_name, cdsp_tap_handle_t *out_handle,
                   backend_error_t *err);

/**
 * @brief Tear down and destroy a previously created Tap and its private
 * Aggregate Device.
 *
 * @param tap_id The AudioObjectID of the device tap to destroy.
 * @param aggregate_dev_id The AudioDeviceID of the aggregate device to destroy.
 * @return 0 (noErr) on success, or an OSStatus error code.
 */
OSStatus cdsp_tap_destroy(AudioObjectID tap_id, AudioDeviceID aggregate_dev_id);

/**
 * @brief Tear down and reset a tap handle.
 *
 * Safe to call even if the handle is empty/zeroed.
 * Destroys the aggregate device and process tap, then resets @p handle to zero.
 *
 * @param handle Pointer to the tap handle to destroy and reset.
 * @return 0 (noErr) on success, or an OSStatus error code.
 */
OSStatus cdsp_tap_destroy_handle(cdsp_tap_handle_t *handle);

/**
 * @brief Configure an AUHAL AudioUnit channel map to route channels starting
 * at the tap offset (Apple TN2091).
 *
 * If the tapped device exposes its own input channels (e.g. duplex devices or
 * audio interfaces), those appear first in the aggregate's flat channel list.
 * If tap handle has tap_channel_offset == 0, this function is a no-op and
 * returns true. Otherwise, it installs an AUHAL channel map property
 * (kAudioOutputUnitProperty_ChannelMap) on scope Output, bus 1 (the input
 * unit's output bus) to route from the tap's offset.
 *
 * @param audio_unit The configured HAL Output AudioUnit.
 * @param tap Pointer to the tap composition handle.
 * @param channels Number of channels to capture.
 * @param[out] err Optional error structure to populate on failure.
 * @return true on success, false on failure.
 */
bool cdsp_tap_apply_channel_map(AudioUnit audio_unit,
                                const cdsp_tap_handle_t *tap, size_t channels,
                                backend_error_t *err);

#endif // ENABLE_COREAUDIO

#endif // CLIB_BACKEND_CORE_AUDIO_TAP_BRIDGE_H
