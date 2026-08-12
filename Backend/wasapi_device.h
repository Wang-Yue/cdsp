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
#include <mmdeviceapi.h>
#include <stdbool.h>
#include <stdint.h>
#include <windows.h>

#include "Backend/backend_error.h"
#include "Config/engine_config_types.h"
#include "Logging/app_logger.h"

extern const logger_t g_wasapi_logger;

#define SAFE_RELEASE(punk)         \
  if ((punk) != NULL) {            \
    (punk)->lpVtbl->Release(punk); \
    (punk) = NULL;                 \
  }

typedef enum {
  WASAPI_BINARY_FORMAT_S16_LE = 0,
  WASAPI_BINARY_FORMAT_S24_3_LE,
  WASAPI_BINARY_FORMAT_S24_4_LJ_LE,
  WASAPI_BINARY_FORMAT_S32_LE,
  WASAPI_BINARY_FORMAT_F32_LE
} wasapi_binary_sample_format_t;

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
void wasapi_build_wave_format(wasapi_binary_sample_format_t fmt, int samplerate,
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
    wasapi_binary_sample_format_t* out_bin_fmt);

/**
 * @brief Determines the negotiated WAVEFORMAT descriptor and binary sample
 * format for a device. Matches CamillaDSP get_device_format.
 */
bool wasapi_get_device_format(
    IAudioClient* client, int samplerate, int channels,
    wasapi_sample_format_t requested_format, bool has_requested_format,
    bool exclusive, const char* direction_name, WAVEFORMATEXTENSIBLE* out_wfx,
    bool* out_is_std_wfx, wasapi_binary_sample_format_t* out_bin_fmt,
    backend_error_t* err);

/**
 * @brief Locates an IMMDevice by friendly name, GUID, or default endpoint.
 * Matches CamillaDSP find_device.
 */
IMMDevice* wasapi_find_device(IMMDeviceEnumerator* enumerator,
                              const char* devname, bool is_capture,
                              bool loopback);

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
