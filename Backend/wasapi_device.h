/**
 * @file wasapi_device.h
 * @brief Common WASAPI device definitions and helper functions.
 */

#ifndef CLIB_BACKEND_WASAPI_DEVICE_H
#define CLIB_BACKEND_WASAPI_DEVICE_H

#if defined(ENABLE_WASAPI)

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <stdbool.h>
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
 * @brief Custom COM implementation of IAudioSessionEvents for session notifications.
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
IAudioSessionEvents* wasapi_session_events_create(void* parent, wasapi_format_change_callback_t callback);

/**
 * @brief Sets up the wave format structure for WASAPI shared-mode streams.
 *
 * @param client Pointer to the active IAudioClient.
 * @param target_sample_rate The target sample rate configured by the user.
 * @param out_final_wfx Output pointer to receive the allocated WAVEFORMATEX structure.
 * @param out_bits_per_sample Output pointer to receive the bits per sample value.
 * @param out_valid_bits Output pointer to receive the valid bits per sample value.
 * @param out_is_float Output pointer to receive whether the format is floating-point.
 * @return true if format setup succeeded, false otherwise.
 */
bool wasapi_setup_shared_format(IAudioClient* client, int target_sample_rate,
                                WAVEFORMATEX** out_final_wfx,
                                int* out_bits_per_sample,
                                int* out_valid_bits,
                                bool* out_is_float);

/**
 * @brief Queries the OS mix rate of the specified device.
 *
 * @param device_name Name of the target audio device.
 * @param is_capture True if capture device, false if playback.
 * @return The current mix format sample rate, or 0.0 on failure.
 */
double wasapi_device_get_current_mix_rate(const char* device_name, bool is_capture);

/**
 * @brief Calculates the closest hardware period aligned to a given frame/byte boundary.
 *
 * @param client Pointer to the active IAudioClient.
 * @param desired_period The desired periodicity/buffer duration in 100ns units.
 * @param align_bytes Optional byte alignment constraint (e.g. 128 bytes). Pass 0 for no alignment constraint.
 * @param wfx The extensible wave format descriptor of the stream.
 * @return The aligned period in 100ns units.
 */
REFERENCE_TIME wasapi_calculate_aligned_period_near(
    IAudioClient* client, REFERENCE_TIME desired_period, uint32_t align_bytes,
    const WAVEFORMATEXTENSIBLE* wfx);

/**
 * @brief Checks if a format is supported by the device, falling back to standard WAVEFORMATEX for stereo if needed.
 *
 * @param client Pointer to the active IAudioClient.
 * @param mode Sharing mode (shared/exclusive).
 * @param ext_wfx The extensible wave format descriptor to test.
 * @param out_std_wfx Output pointer to standard WAVEFORMATEX initialized if fallback succeeded.
 * @param out_use_ext Output pointer set to true if extensible format should be used, false if standard fallback should be used.
 * @return true if format is supported (either directly or via fallback), false otherwise.
 */
bool wasapi_check_format_supported(IAudioClient* client, AUDCLNT_SHAREMODE mode,
                                   const WAVEFORMATEXTENSIBLE* ext_wfx,
                                   WAVEFORMATEX* out_std_wfx,
                                   bool* out_use_ext);

#endif // ENABLE_WASAPI

#endif // CLIB_BACKEND_WASAPI_DEVICE_H
