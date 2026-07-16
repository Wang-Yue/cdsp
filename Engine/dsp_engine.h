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
 * pointers, eliminating extra pointer indirections and type casting.
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
   * @brief Retrieve current processing state and stop reason details.
   * @param ctx Pointer to internal engine context.
   * @param out_status Output pointer for the state snapshot.
   * @return true on success, false on invalid args or uninitialized engine.
   */
  bool (*get_status)(void* ctx, state_update_t* out_status);

  /**
   * @brief Query the active sample rate of the running processing core.
   * @param ctx Pointer to internal engine context.
   * @return Sample rate in Hz, or 0 if inactive.
   */
  int (*get_active_samplerate)(void* ctx);

  /**
   * @brief Query processing statistics, load metrics, and clip counts.
   * @param ctx Pointer to internal engine context.
   * @param out_rate_adjust Output pointer for current resampling rate
   * adjustment factor.
   * @param out_buffer_level Output pointer for ring buffer fill level ratio
   * (0.0 to 1.0).
   * @param out_clipped_samples Output pointer for cumulative count of clipped
   * samples.
   * @param out_processing_load Output pointer for overall processing thread CPU
   * load ratio.
   * @param out_resampler_load Output pointer for resampler CPU load ratio.
   * @return true on success, false if engine is inactive.
   */
  bool (*get_processing_status)(void* ctx, double* out_rate_adjust,
                                double* out_buffer_level,
                                uint64_t* out_clipped_samples,
                                double* out_processing_load,
                                double* out_resampler_load);

  /**
   * @brief Reset the cumulative clipped samples accumulator.
   * @param ctx Pointer to internal engine context.
   */
  void (*reset_clipped_samples)(void* ctx);

  /**
   * @brief Get the JSON string representation of the active configuration.
   * @param ctx Pointer to internal engine context.
   * @param out_json Output string pointer (allocated, must be freed by caller).
   * @return true on success, false if no config active.
   */
  bool (*get_active_config_json)(void* ctx, char** out_json);

  /**
   * @brief Get the JSON string representation of the previous configuration.
   * @param ctx Pointer to internal engine context.
   * @param out_json Output string pointer (allocated, must be freed by caller).
   * @return true on success, false if no previous config exists.
   */
  bool (*get_previous_config_json)(void* ctx, char** out_json);

  /**
   * @brief Apply a new configuration supplied as a JSON payload.
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
   * @brief Get the active state file path string.
   * @param ctx Pointer to internal engine context.
   * @return Static or internal pointer to the state file path, or NULL.
   */
  const char* (*get_state_file)(void* ctx);

  /**
   * @brief Set the path to the state file for volume and mute state
   * persistence.
   * @param ctx Pointer to internal engine context.
   * @param path File path for persisting state changes.
   */
  void (*set_state_file)(void* ctx, const char* path);

  /**
   * @brief Check if there are unsaved state changes (dirty flag).
   * @param ctx Pointer to internal engine context.
   * @return true if state changes are pending serialization, false otherwise.
   */
  bool (*is_state_dirty)(void* ctx);

  /**
   * @brief Get the active configuration file path.
   * @param ctx Pointer to internal engine context.
   * @return Allocated copy of the config path string (must be freed by caller),
   * or NULL.
   */
  char* (*get_config_path)(void* ctx);

  /**
   * @brief Set the active configuration file path.
   * @param ctx Pointer to internal engine context.
   * @param path Configuration file path.
   */
  void (*set_config_path)(void* ctx, const char* path);

  /**
   * @brief Set the global application logging level.
   * @param ctx Pointer to internal engine context (unused).
   * @param level Target log severity level.
   */
  void (*set_log_level)(void* ctx, log_level_t level);

  /**
   * @brief Discover available input/output audio hardware devices for a given
   * backend.
   * @param ctx Pointer to internal engine context.
   * @param backend Audio backend identifier (e.g. "coreaudio", "alsa",
   * "pipewire", "wasapi").
   * @param is_input true for capture/input devices, false for playback/output
   * devices.
   * @param out_devices Allocated array of discovered audio devices (must be
   * freed by caller).
   * @param out_count Output count of discovered devices.
   * @return true on success, false on failure.
   */
  bool (*get_available_devices)(void* ctx, const char* backend, bool is_input,
                                audio_device_t** out_devices,
                                size_t* out_count);

  /**
   * @brief Query hardware sample rates and channel capabilities for a device.
   * @param ctx Pointer to internal engine context.
   * @param backend Audio backend name.
   * @param device Device name identifier.
   * @param is_capture true if querying capture device capabilities, false for
   * playback.
   * @param out_desc Allocated descriptor structure containing capabilities.
   * @param out_err Output error info structure on failure.
   * @return true on success, false on failure.
   */
  bool (*get_device_capabilities)(void* ctx, const char* backend,
                                  const char* device, bool is_capture,
                                  audio_device_descriptor_t** out_desc,
                                  device_error_t* out_err);

  /**
   * @brief Compute real-time frequency spectrum analysis bins for a given
   * channel.
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
   * @brief Read recent raw audio frame samples from capture or playback history
   * buffers.
   * @param ctx Pointer to internal engine context.
   * @param is_capture true for capture audio samples, false for playback
   * samples.
   * @param n_frames Number of requested frame samples.
   * @param err Output backend error descriptor on failure.
   * @return Allocated audio_samples_t structure (must be freed by calling
   * cdsp_free_samples).
   */
  audio_samples_t* (*get_samples)(void* ctx, bool is_capture, size_t n_frames,
                                  audio_backend_error_t* err);

  /**
   * @brief Fetch active RMS and peak signal level measurements per channel.
   * @param ctx Pointer to internal engine context.
   * @param out_vu Output VU levels snapshot structure.
   * @return true on success, false if engine is inactive.
   */
  bool (*get_vu_levels)(void* ctx, vu_levels_t* out_vu);

  /**
   * @brief Stop processing core and transition engine to inactive state.
   * @param ctx Pointer to internal engine context.
   */
  void (*stop)(void* ctx);

  /**
   * @brief Query current gain volume setting for a specific fader.
   * @param ctx Pointer to internal engine context.
   * @param fader Target fader identifier.
   * @return Volume gain in dB.
   */
  float (*get_fader_volume)(void* ctx, fader_t fader);

  /**
   * @brief Check whether a specific fader is currently muted.
   * @param ctx Pointer to internal engine context.
   * @param fader Target fader identifier.
   * @return true if fader is muted, false otherwise.
   */
  bool (*is_fader_muted)(void* ctx, fader_t fader);

  /**
   * @brief Adjust gain volume level for a specific fader.
   * @param ctx Pointer to internal engine context.
   * @param fader Target fader identifier.
   * @param db Target volume level in dB.
   * @param instant true to skip volume ramping and set gain immediately.
   */
  void (*set_fader_volume)(void* ctx, fader_t fader, float db, bool instant);

  /**
   * @brief Toggle mute state for a specific fader.
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

#ifdef CDSP_TEST
// Direct functions used by internal test suites (test_dsp_engine.c)
const dsp_config_t* dsp_engine_get_active_config(dsp_engine_t* engine);
bool dsp_engine_set_config(dsp_engine_t* engine, const char* json,
                           audio_backend_error_t* err);
bool dsp_engine_set_config_struct(dsp_engine_t* engine, dsp_config_t* config,
                                  audio_backend_error_t* err);
#endif

#endif  // CLIB_ENGINE_DSP_ENGINE_H
