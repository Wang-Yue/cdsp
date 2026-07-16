/**
 * @file dsp_engine.h
 * @brief High-level self-contained interface for the CamillaDSP monitor engine.
 *
 * Provides control, status, and visualization interfaces for the DSP engine.
 */

#ifndef CLIB_ENGINE_DSP_ENGINE_H
#define CLIB_ENGINE_DSP_ENGINE_H

#include <stdbool.h>
#include <stddef.h>

#include "Audio/spectrum_analyzer.h"
#include "Backend/audio_backend.h"
#include "Config/configuration.h"
#include "Config/engine_config_types.h"
#include "Config/log_level.h"

/**
 * @brief Self-contained interface handle representing the DSP engine.
 *
 * Exposes a VTable of control, status, metric, and visualization function
 * pointers matching the WebSocket protocol specification.
 */
typedef struct dsp_engine dsp_engine_t;

struct dsp_engine {
  /** Opaque pointer to internal implementation state context. */
  void* ctx;

  /**
   * @brief Free the DSP engine instance and release all associated resources.
   * @param ctx Pointer to internal engine implementation context.
   */
  void (*free)(void* ctx);

  /**
   * @brief Poll the engine for background tasks, state synchronization, and
   * status updates.
   * @param ctx Pointer to internal engine implementation context.
   */
  void (*poll)(void* ctx);

  /**
   * @brief Retrieve current processing state and stop reason details snapshot.
   * @param ctx Pointer to internal engine context.
   * @param out_status Output pointer for the state snapshot.
   * @return true on success, false on invalid args or uninitialized engine.
   */
  bool (*get_status)(void* ctx, state_update_t* out_status);

  /**
   * @brief Query current processing state enum (WebSocket: GetState).
   * @param ctx Pointer to internal engine context.
   * @return Current processing state enum.
   */
  processing_state_t (*get_state)(void* ctx);

  /**
   * @brief Query the reason processing last stopped (WebSocket: GetStopReason).
   * @param ctx Pointer to internal engine context.
   * @param out_reason Output pointer for stop reason structure.
   * @return true on success, false on invalid args or uninitialized engine.
   */
  bool (*get_stop_reason)(void* ctx, processing_stop_reason_t* out_reason);

  /**
   * @brief Query the active measured sample rate of the capture device
   * (WebSocket: GetCaptureRate).
   * @param ctx Pointer to internal engine context.
   * @return Measured sample rate in Hz, or 0 if inactive.
   */
  int (*get_capture_rate)(void* ctx);

  /**
   * @brief Query processing statistics, load metrics, and clip counts.
   * @param ctx Pointer to internal engine context.
   * @param out_rate_adjust Output pointer for resampler rate adjustment factor.
   * @param out_buffer_level Output pointer for ring buffer fill level ratio
   * (0.0 to 1.0).
   * @param out_clipped_samples Output pointer for cumulative clipped sample
   * count.
   * @param out_processing_load Output pointer for pipeline CPU load ratio.
   * @param out_resampler_load Output pointer for resampler CPU load ratio.
   * @return true on success, false if engine is inactive.
   */
  bool (*get_processing_status)(void* ctx, double* out_rate_adjust,
                                double* out_buffer_level,
                                uint64_t* out_clipped_samples,
                                double* out_processing_load,
                                double* out_resampler_load);

  /**
   * @brief Reset the cumulative clipped samples accumulator (WebSocket:
   * ResetClippedSamples).
   * @param ctx Pointer to internal engine context.
   */
  void (*reset_clipped_samples)(void* ctx);

  /**
   * @brief Get the JSON string representation of the active configuration
   * (WebSocket: GetConfigJson).
   * @param ctx Pointer to internal engine context.
   * @param out_json Output string pointer (allocated, must be freed by caller).
   * @return true on success, false if no config active.
   */
  bool (*get_active_config_json)(void* ctx, char** out_json);

  /**
   * @brief Get the JSON string representation of the previous configuration
   * (WebSocket: GetPreviousConfigJson).
   * @param ctx Pointer to internal engine context.
   * @param out_json Output string pointer (allocated, must be freed by caller).
   * @return true on success, false if no previous config exists.
   */
  bool (*get_previous_config_json)(void* ctx, char** out_json);

  /**
   * @brief Apply a new configuration supplied as a JSON payload (WebSocket:
   * SetConfigJson).
   * @param ctx Pointer to internal engine context.
   * @param json_str Null-terminated JSON string containing the new
   * configuration.
   * @param out_err Output pointer for detailed error info if validation or
   * setup fails.
   * @return true if config applied successfully, false otherwise.
   */
  bool (*set_config_json)(void* ctx, const char* json_str,
                          audio_backend_error_t* out_err);

  /**
   * @brief Get the path of the configured state file (WebSocket:
   * GetStateFilePath).
   * @param ctx Pointer to internal engine context.
   * @return Static or internal pointer to the state file path, or NULL.
   */
  const char* (*get_state_file_path)(void* ctx);

  /**
   * @brief Set the path to the state file for volume and mute persistence
   * (WebSocket: SetStateFilePath).
   * @param ctx Pointer to internal engine context.
   * @param path File path for persisting state changes.
   */
  void (*set_state_file_path)(void* ctx, const char* path);

  /**
   * @brief Check whether all pending state changes have been saved (WebSocket:
   * GetStateFileUpdated).
   * @param ctx Pointer to internal engine context.
   * @return true if all changes are saved to disk, false if state is dirty.
   */
  bool (*get_state_file_updated)(void* ctx);

  /**
   * @brief Get the path of the loaded config file (WebSocket:
   * GetConfigFilePath).
   * @param ctx Pointer to internal engine context.
   * @return Allocated copy of the config path string (must be freed by caller),
   * or NULL.
   */
  char* (*get_config_file_path)(void* ctx);

  /**
   * @brief Set active config file path (WebSocket: SetConfigFilePath).
   * @param ctx Pointer to internal engine context.
   * @param path Configuration file path.
   */
  void (*set_config_file_path)(void* ctx, const char* path);

  /**
   * @brief Set global application logging level.
   * @param ctx Pointer to internal engine context (unused).
   * @param level Target log severity level.
   */
  void (*set_log_level)(void* ctx, log_level_t level);

  /**
   * @brief Discover available input/output hardware devices for a backend
   * (WebSocket: GetAvailableCaptureDevices / GetAvailablePlaybackDevices).
   * @param ctx Pointer to internal engine context.
   * @param backend Audio backend identifier (e.g. "coreaudio", "alsa",
   * "wasapi", "asio").
   * @param is_input true for capture devices, false for playback devices.
   * @param out_devices Allocated array of discovered audio devices (must be
   * freed by caller).
   * @param out_count Output count of discovered devices.
   * @return true on success, false on failure.
   */
  bool (*get_available_devices)(void* ctx, const char* backend, bool is_input,
                                audio_device_t** out_devices,
                                size_t* out_count);

  /**
   * @brief Query sample rates & channels for a device (WebSocket:
   * GetCaptureDeviceCapabilities / GetPlaybackDeviceCapabilities).
   * @param ctx Pointer to internal engine context.
   * @param backend Audio backend name.
   * @param device Device name identifier.
   * @param is_capture true if capture, false for playback.
   * @param out_desc Allocated descriptor structure containing capabilities.
   * @param out_err Output error info structure on failure.
   * @return true on success, false on failure.
   */
  bool (*get_device_capabilities)(void* ctx, const char* backend,
                                  const char* device, bool is_capture,
                                  audio_device_descriptor_t** out_desc,
                                  device_error_t* out_err);

  /**
   * @brief Compute real-time frequency spectrum analysis bins (WebSocket:
   * GetSpectrum).
   * @param ctx Pointer to internal engine context.
   * @param is_capture true to sample capture stream, false to sample playback
   * stream.
   * @param channel 0-indexed audio channel number.
   * @param min_freq Minimum frequency boundary in Hz.
   * @param max_freq Maximum frequency boundary in Hz.
   * @param n_bins Desired number of frequency logarithmic bins.
   * @param out_spec Output structure containing frequency and magnitude arrays.
   * @return true on success, false if insufficient signal data or channel out
   * of bounds.
   */
  bool (*get_spectrum)(void* ctx, bool is_capture, uint32_t channel,
                       double min_freq, double max_freq, uint32_t n_bins,
                       spectrum_t* out_spec);

  /**
   * @brief Read recent raw audio frame samples from history buffers.
   * @param ctx Pointer to internal engine context.
   * @param is_capture true for capture samples, false for playback samples.
   * @param n_frames Number of requested frame samples.
   * @param err Output backend error descriptor on failure.
   * @return Allocated audio_samples_t structure (must be freed by calling
   * cdsp_free_samples).
   */
  audio_samples_t* (*get_samples)(void* ctx, bool is_capture, size_t n_frames,
                                  audio_backend_error_t* err);

  /**
   * @brief Fetch active RMS and peak signal level measurements (WebSocket:
   * GetVuLevels / GetSignalLevels).
   * @param ctx Pointer to internal engine context.
   * @param out_vu Output VU levels snapshot structure.
   * @return true on success, false if engine is inactive.
   */
  bool (*get_vu_levels)(void* ctx, vu_levels_t* out_vu);

  /**
   * @brief Stop processing core (WebSocket: Stop).
   * @param ctx Pointer to internal engine context.
   */
  void (*stop)(void* ctx);

  /**
   * @brief Query volume gain setting in dB for a specific fader (WebSocket:
   * GetFaderVolume / GetVolume).
   * @param ctx Pointer to internal engine context.
   * @param fader Target fader identifier.
   * @return Volume gain in dB.
   */
  float (*get_fader_volume)(void* ctx, fader_t fader);

  /**
   * @brief Query mute state boolean for a specific fader (WebSocket:
   * GetFaderMute / GetMute).
   * @param ctx Pointer to internal engine context.
   * @param fader Target fader identifier.
   * @return true if fader is muted, false otherwise.
   */
  bool (*get_fader_mute)(void* ctx, fader_t fader);

  /**
   * @brief Set gain volume level in dB for a specific fader (WebSocket:
   * SetFaderVolume / SetVolume).
   * @param ctx Pointer to internal engine context.
   * @param fader Target fader identifier.
   * @param db Target volume level in dB.
   * @param instant true to skip volume ramping and set gain immediately.
   */
  void (*set_fader_volume)(void* ctx, fader_t fader, float db, bool instant);

  /**
   * @brief Set mute state boolean for a specific fader (WebSocket: SetFaderMute
   * / SetMute).
   * @param ctx Pointer to internal engine context.
   * @param fader Target fader identifier.
   * @param mute true to mute fader, false to unmute.
   */
  void (*set_fader_mute)(void* ctx, fader_t fader, bool mute);
};

/**
 * @brief Create a new DSP engine instance.
 * @return Pointer to the created self-contained engine interface, or NULL on
 * failure.
 */
dsp_engine_t* dsp_engine_create(void);

#endif  // CLIB_ENGINE_DSP_ENGINE_H
