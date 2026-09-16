/**
 * @file core_audio_tap_bridge.h
 * @brief CoreAudio Hardware & Process Tap C Bridge for macOS 14.2+.
 *
 * Encapsulates CoreAudio audio tapping APIs, private aggregate device
 * composition, and process/device enumeration.
 *
 * Supports both hardware device taps (bound to a physical output device) and
 * process taps (capturing audio from an application, named as "app:app_name").
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
 * @brief Check if the running macOS system supports CoreAudio process/device
 * taps (macOS 14.2+).
 *
 * @return true if running on macOS 14.2 or later with tap support available.
 */
bool cdsp_tap_is_supported(void);

/**
 * @brief Check if a device name specifies an application tap (e.g. "app:Google
 * Chrome", "app:Music").
 *
 * @param device_name The device string to inspect.
 * @return true if device_name begins with "app:" (case-insensitive).
 */
bool cdsp_tap_is_app_device(const char *device_name);

/**
 * @brief Extract the application name component from an "app:app_name" string.
 *
 * Skips the "app:" prefix and any leading whitespace.
 *
 * @param device_name The device string starting with "app:".
 * @return Pointer within device_name to the application name, or NULL if
 * invalid.
 */
const char *cdsp_tap_parse_app_name(const char *device_name);

/**
 * @brief Enumerate currently running applications available for process
 * tapping.
 *
 * Discovers applications from both NSWorkspace and CoreAudio process
 * registries, returning formatted device names prefixed with "app:" (e.g.
 * "app:Google Chrome", "app:Music").
 *
 * @param out_names Buffer to store the discovered device names.
 * @param max_names Maximum number of names to return.
 * @return Number of discovered application tap names.
 */
int cdsp_tap_get_available_app_names(char out_names[][256], int max_names);

/**
 * @brief Check whether a named application or process is currently running /
 * registered in CoreAudio.
 *
 * @param app_name Name of the application (e.g. "Google Chrome", "Music",
 * "com.apple.Music").
 * @param[out] out_matched_name Optional buffer to receive the canonical
 * application name.
 * @param max_len Size of out_matched_name buffer.
 * @return true if the application/process was found, false otherwise.
 */
bool cdsp_tap_find_app(const char *app_name, char *out_matched_name,
                       size_t max_len);

/**
 * @brief Resolve the active output AudioDeviceID used by an application.
 *
 * Inspects the application's CoreAudio Process object using
 * kAudioProcessPropertyDevices with output scope. If the app is currently
 * active on an output device, that AudioDeviceID is returned; otherwise,
 * falls back to the system default output device.
 *
 * @param app_name Name of the application (e.g. "Google Chrome", "Music").
 * @return AudioDeviceID of the active output device, or system default output
 * device.
 */
AudioDeviceID cdsp_tap_get_active_device_for_app(const char *app_name);

/**
 * @brief Validate an application tap device name and resolve its output
 * AudioDeviceID.
 *
 * Validates that the device name format is valid, that the system supports
 * CoreAudio process taps (macOS 14.2+), that the requested application is
 * running with CoreAudio support, and that the operation is capture (process
 * taps only support capture). Resolves the underlying active/default output
 * AudioDeviceID.
 *
 * @param device_name Application tap name (e.g. "app:Google Chrome").
 * @param is_capture Must be true (app taps only support capture).
 * @param[out] out_device_id Output pointer for resolved AudioDeviceID.
 * @param[out] err Optional pointer to device_error_t to receive error details
 * on failure.
 * @return true if valid and resolved, false on validation failure.
 */
bool cdsp_tap_resolve_app_device(const char *device_name, bool is_capture,
                                 AudioDeviceID *out_device_id,
                                 device_error_t *err);

/**
 * @brief Create a CoreAudio Device Tap and a private Aggregate Device wrapping
 * it.
 *
 * Configures CATapDescription with anti-feedback process exclusions (for device
 * taps) or process inclusion list (for "app:app_name" process taps), assigns
 * private tap visibility, creates an AudioHardwareProcessTap object, and
 * packages it into a private aggregate AudioDeviceID that can be bound to a HAL
 * Output AudioUnit exactly like any other capture device.
 *
 * The aggregate is composed following Apple's documented tap recipe: it
 * declares the target/default device as its main sub-device (clock source),
 * disables stacking, enables tap auto-start and sub-tap drift compensation.
 * Composition is asynchronous inside the HAL, so this function also waits until
 * the tap's channels are actually visible on the aggregate before returning.
 *
 * @param device_name Name of the output device to tap, or NULL/empty for the
 * current default output device, or "app:app_name" for an application tap.
 * @param[out] out_handle Pointer to receive the resolved tap composition
 * details.
 * @return 0 (noErr) on success, or an OSStatus error code.
 */
#include "backend/backend_error.h"

OSStatus cdsp_tap_create(const char *device_name,
                         cdsp_tap_handle_t *out_handle);

/**
 * @brief Helper to create a CoreAudio tap and format backend_error_t on
 * failure.
 *
 * @param device_name Name of the output device to tap or "app:app_name".
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
