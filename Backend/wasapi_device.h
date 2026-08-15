/**
 * @file wasapi_device.h
 * @brief Common WASAPI device definitions and helper functions.
 */

#ifndef CLIB_BACKEND_WASAPI_DEVICE_H
#define CLIB_BACKEND_WASAPI_DEVICE_H

#if defined(ENABLE_WASAPI)

#ifndef COBJMACROS
#define COBJMACROS
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <audioclient.h>
#include <audiopolicy.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <windows.h>

#include "Audio/audio_chunk.h"
#include "Backend/backend_error.h"
#include "Config/engine_config_types.h"
#include "Logging/app_logger.h"

extern const logger_t g_wasapi_logger;

#define SAFE_RELEASE(punk)         \
  if ((punk) != NULL) {            \
    (punk)->lpVtbl->Release(punk); \
    (punk) = NULL;                 \
  }

typedef void (*wasapi_format_change_callback_t)(void* parent, double new_rate);

/**
 * @struct CDSPAudioSessionEvents
 * @brief Custom COM implementation of IAudioSessionEvents for session
 * notifications.
 */
typedef struct {
  IAudioSessionEventsVtbl* lpVtbl;
  LONG ref_count;
  void* parent;
  wasapi_format_change_callback_t callback;
} CDSPAudioSessionEvents;

/**
 * @brief Creates a new IAudioSessionEvents listener instance.
 *
 * @param parent Pointer to the capture/playback backend context.
 * @param callback Callback function triggered on format change event.
 * @return Pointer to IAudioSessionEvents interface.
 */
IAudioSessionEvents* wasapi_session_events_create(
    void* parent, wasapi_format_change_callback_t callback);

/**
 * @brief Subscribes to IAudioSessionControl format change notifications.
 *
 * @param client Active IAudioClient.
 * @param parent Pointer to backend context.
 * @param callback Callback triggered when the session format changes.
 * @param out_control Pointer to receive the IAudioSessionControl interface.
 * @param out_listener Pointer to receive the created IAudioSessionEvents
 * listener.
 */
void wasapi_register_session_events(IAudioClient* client, void* parent,
                                    wasapi_format_change_callback_t callback,
                                    IAudioSessionControl** out_control,
                                    IAudioSessionEvents** out_listener);

/**
 * @brief Unregisters and releases audio session event listeners.
 *
 * @param control Pointer to IAudioSessionControl pointer (will be set to NULL).
 * @param listener Pointer to IAudioSessionEvents pointer (will be set to NULL).
 */
void wasapi_unregister_session_events(IAudioSessionControl** control,
                                      IAudioSessionEvents** listener);

/**
 * @brief Checks if a pending sample rate change occurred and resolves the new
 * rate.
 *
 * @param device Device name or ID.
 * @param is_capture True if capture stream, false if playback stream.
 * @param pending_rate The pending rate reported by the session callback.
 * @param has_pending_rate_change Pointer to atomic flag indicating pending rate
 * change.
 * @param out_rate Pointer to receive the resolved sample rate.
 * @return True if rate change was resolved, false otherwise.
 */
bool wasapi_check_and_resolve_pending_rate(
    const char* device, bool is_capture, double pending_rate,
    _Atomic bool* has_pending_rate_change, double* out_rate);

/**
 * @brief Generates a simple bitmask for the given channel count.
 * Matches wasapi-rs make_simple_channelmask.
 */
uint32_t wasapi_make_simple_channelmask(size_t channels);

/**
 * @brief Generates candidate speaker channel bitmasks for a given channel
 * count. Matches wasapi-rs make_channelmasks.
 */
size_t wasapi_make_channelmasks(size_t channels, DWORD masks[8]);

/**
 * @brief Builds a WAVEFORMATEXTENSIBLE descriptor for given sample parameters.
 * Matches wasapi-rs WaveFormat::new / build_wave_format.
 */
void wasapi_build_wave_format(binary_sample_format_t fmt, int samplerate,
                              int channels, uint32_t channel_mask,
                              bool has_mask, WAVEFORMATEXTENSIBLE* out_wfx);

/**
 * @brief Probes format support in exclusive mode, testing standard WAVEFORMATEX
 * fallback (for <=2 channels) and candidate channel masks.
 * Matches wasapi-rs is_supported_exclusive_with_quirks.
 */
bool wasapi_is_supported_exclusive_with_quirks(
    IAudioClient* client, const WAVEFORMATEXTENSIBLE* in_wfx,
    WAVEFORMATEXTENSIBLE* out_wfx, bool* out_is_std_wfx);

/**
 * @brief Checks if a sample format is supported with optional preferred channel
 * mask. Matches CamillaDSP get_supported_wave_format_with_channel_mask.
 */
bool wasapi_get_supported_wave_format_with_channel_mask(
    IAudioClient* client, wasapi_sample_format_t sample_format, int samplerate,
    int channels, bool exclusive, uint32_t channel_mask, bool has_mask,
    WAVEFORMATEXTENSIBLE* out_wfx, bool* out_is_std_wfx,
    binary_sample_format_t* out_bin_fmt);

/**
 * @brief Determines the negotiated WAVEFORMAT descriptor and binary sample
 * format for a device. Matches CamillaDSP get_device_format.
 */
bool wasapi_get_device_format(
    IAudioClient* client, int samplerate, int channels,
    wasapi_sample_format_t requested_format, bool has_requested_format,
    bool exclusive, const char* direction_name, WAVEFORMATEXTENSIBLE* out_wfx,
    bool* out_is_std_wfx, binary_sample_format_t* out_bin_fmt,
    backend_error_t* err);

/**
 * @brief Initializes an IAudioClient stream with appropriate timing, share
 * mode, flags, period calculations, and sets up event-driven notifications if
 * requested.
 *
 * @param client Pointer to the active IAudioClient.
 * @param wfx Pointer to the negotiated WAVEFORMATEXTENSIBLE descriptor.
 * @param samplerate Stream sample rate in Hz.
 * @param blockalign Frame block align in bytes.
 * @param exclusive True for exclusive mode, false for shared mode.
 * @param polling True for polling mode, false for event-driven mode.
 * @param loopback True for loopback capture, false otherwise.
 * @param out_def_period Optional pointer to receive default device period in
 * 100ns units.
 * @param out_event_handle Optional pointer to receive the created event handle.
 * @param out_buffer_frame_count Pointer to receive device buffer frame count.
 * @param direction_name String describing stream direction ("Capture",
 * "Playback", "Render").
 * @param err Pointer to backend_error_t to record errors.
 * @return True on success, false on failure.
 */
bool wasapi_initialize_stream(IAudioClient* client,
                              const WAVEFORMATEXTENSIBLE* wfx, int samplerate,
                              size_t blockalign, bool exclusive, bool polling,
                              bool loopback, REFERENCE_TIME* out_def_period,
                              HANDLE* out_event_handle,
                              UINT32* out_buffer_frame_count,
                              const char* direction_name, backend_error_t* err);

/**
 * @brief Locates an IMMDevice by friendly name, GUID, or default endpoint.
 * Matches CamillaDSP find_device.
 */
IMMDevice* wasapi_find_device(IMMDeviceEnumerator* enumerator,
                              const char* devname, bool is_capture,
                              bool loopback);

/**
 * @brief Creates an MMDeviceEnumerator, locates the requested IMMDevice, and
 * activates its IAudioClient interface.
 *
 * @param devname Device name, GUID, or "default".
 * @param is_capture True for capture endpoint, false for render endpoint.
 * @param loopback True for loopback capture on a render endpoint.
 * @param out_enumerator Pointer to receive the IMMDeviceEnumerator.
 * @param out_device Pointer to receive the activated IMMDevice.
 * @param out_client Pointer to receive the activated IAudioClient.
 * @param err Pointer to backend_error_t to record errors.
 * @return True on success, false on failure.
 */
bool wasapi_create_device_and_client(const char* devname, bool is_capture,
                                     bool loopback,
                                     IMMDeviceEnumerator** out_enumerator,
                                     IMMDevice** out_device,
                                     IAudioClient** out_client,
                                     backend_error_t* err);

/**
 * @brief Releases all WASAPI device, client, and session COM resources,
 * closes event handles, and optionally calls CoUninitialize.
 *
 * @param client Pointer to IAudioClient pointer.
 * @param sub_client Pointer to IUnknown/IAudioCaptureClient/IAudioRenderClient
 * pointer.
 * @param session_control Pointer to IAudioSessionControl pointer.
 * @param session_events_listener Pointer to IAudioSessionEvents pointer.
 * @param event_handle Pointer to event handle.
 * @param mm_device Pointer to IMMDevice pointer.
 * @param enumerator Pointer to IMMDeviceEnumerator pointer.
 * @param com_initialized Pointer to bool indicating if COM was initialized.
 */
void wasapi_cleanup_device_resources(
    IAudioClient** client, IUnknown** sub_client,
    IAudioSessionControl** session_control,
    IAudioSessionEvents** session_events_listener, HANDLE* event_handle,
    IMMDevice** mm_device, IMMDeviceEnumerator** enumerator,
    bool* com_initialized);

/**
 * @brief Extracts and normalizes device name string from configuration.
 *
 * @param has_device True if device field is configured.
 * @param config_device Device name string from config.
 * @param out_device Output buffer to receive device name.
 * @param max_len Size of output buffer.
 */
void wasapi_extract_device_name(bool has_device, const char* config_device,
                                char* out_device, size_t max_len);

/**
 * @brief Queries the OS mix rate of the specified device.
 *
 * @param device_name Name of the target audio device.
 * @param is_capture True if capture device, false if playback.
 * @return The current mix format sample rate, or 0.0 on failure.
 */
double wasapi_device_get_current_mix_rate(const char* device_name,
                                          bool is_capture);

/**
 * @brief Calculates the closest hardware period aligned to a given frame/byte
 * boundary.
 * Matches wasapi-rs calculate_aligned_period_near.
 *
 * @param client Pointer to the active IAudioClient.
 * @param desired_period The desired periodicity/buffer duration in 100ns units.
 * @param align_bytes Optional byte alignment constraint (e.g. 128 bytes). Pass
 * 0 for no alignment constraint.
 * @param samplerate Stream sample rate in Hz.
 * @param block_align Block alignment in bytes (frame size).
 * @return The aligned period in 100ns units.
 */
REFERENCE_TIME wasapi_calculate_aligned_period_near(
    IAudioClient* client, REFERENCE_TIME desired_period, uint32_t align_bytes,
    int samplerate, int block_align);

/**
 * @brief Legacy channel mask lookup for backwards compatibility.
 */
DWORD wasapi_get_default_channel_mask(int channels);

#endif  // ENABLE_WASAPI

#endif  // CLIB_BACKEND_WASAPI_DEVICE_H
