/**
 * @file dsp_engine.h
 * @brief High-level interface for the CamillaDSP monitor engine.
 *
 * Provides control, status, and visualization interfaces for the DSP engine.
 */

#ifndef CLIB_ENGINE_DSP_ENGINE_H
#define CLIB_ENGINE_DSP_ENGINE_H

#include "Audio/spectrum_analyzer.h"
#include "Backend/audio_backend.h"
#include "Config/configuration.h"
#include "Config/engine_config_types.h"
#include "Config/log_level.h"
/* Platform capability discovery is encapsulated in audio_backend_registry */
#include <stdbool.h>
#include <stddef.h>


/**
 * @brief Main DSP engine structure.
 *
 * Manages the DSP core, visualization, and configuration state.
 */
typedef struct dsp_engine dsp_engine_t;

/**
 * @brief Create a new DSP engine instance.
 * @return Pointer to the created engine, or NULL on failure.
 */
dsp_engine_t* dsp_engine_create(void);

/**
 * @brief Free the DSP engine instance and its resources.
 * @param engine Pointer to the engine instance.
 */
void dsp_engine_free(dsp_engine_t* engine);

/**
 * @brief Set the active configuration from a JSON string.
 *
 * @param engine Pointer to the engine instance.
 * @param json JSON configuration string.
 * @param err Pointer to store backend errors if configuration fails.
 * @return True on success, false on failure.
 */
bool dsp_engine_set_config(dsp_engine_t* engine, const char* json,
                           audio_backend_error_t* err);

/**
 * @brief Set the active configuration from a configuration structure.
 *
 * @param engine Pointer to the engine instance.
 * @param config Pointer to the configuration structure.
 * @param err Pointer to store backend errors if configuration fails.
 * @return True on success, false on failure.
 */
bool dsp_engine_set_config_struct(dsp_engine_t* engine, dsp_config_t* config,
                                  audio_backend_error_t* err);

/**
 * @brief Stop the DSP engine processing.
 * @param engine Pointer to the engine instance.
 */
void dsp_engine_stop(dsp_engine_t* engine);

/**
 * @brief Set fader volume.
 *
 * @param engine Pointer to the engine instance.
 * @param fader Fader index.
 * @param db Volume in decibels.
 * @param instant If true, apply volume instantly (bypass ramp).
 */
void dsp_engine_set_fader_volume(dsp_engine_t* engine, fader_t fader, float db,
                                 bool instant);

/**
 * @brief Set fader mute state.
 *
 * @param engine Pointer to the engine instance.
 * @param fader Fader index.
 * @param mute True to mute, false to unmute.
 */
void dsp_engine_set_fader_mute(dsp_engine_t* engine, fader_t fader, bool mute);

/**
 * @brief Get fader volume.
 *
 * @param engine Pointer to the engine instance.
 * @param fader Fader index.
 * @return Current fader volume in decibels.
 */
float dsp_engine_get_fader_volume(const dsp_engine_t* engine, fader_t fader);

/**
 * @brief Check if fader is muted.
 *
 * @param engine Pointer to the engine instance.
 * @param fader Fader index.
 * @return True if muted, false otherwise.
 */
bool dsp_engine_is_fader_muted(const dsp_engine_t* engine, fader_t fader);

/**
 * @brief Set the path for the state file (for persisting volume/mute states).
 *
 * @param engine Pointer to the engine instance.
 * @param path Path to the state file.
 */
void dsp_engine_set_state_file(dsp_engine_t* engine, const char* path);

/**
 * @brief Set the path to save/load config files.
 *
 * @param engine Pointer to the engine instance.
 * @param path Path to config directory or file.
 */
void dsp_engine_set_config_path(dsp_engine_t* engine, const char* path);

/**
 * @brief Poll the engine for status updates and house keeping.
 *
 * Should be called regularly (e.g. from main loop or websocket thread).
 *
 * @param engine Pointer to the engine instance.
 */
void dsp_engine_poll(dsp_engine_t* engine);

typedef struct {
  void* ctx;
  bool (*get_status)(void* ctx, state_update_t* out_status);
  int (*get_active_samplerate)(void* ctx);
  bool (*get_processing_status)(void* ctx, double* out_rate_adjust,
                                double* out_buffer_level,
                                uint64_t* out_clipped_samples,
                                double* out_processing_load,
                                double* out_resampler_load);
  void (*reset_clipped_samples)(void* ctx);
  bool (*get_active_config_json)(void* ctx, char** out_json);
  bool (*get_previous_config_json)(void* ctx, char** out_json);
  bool (*get_vu_levels)(void* ctx, vu_levels_t* out_vu);
  bool (*get_available_devices)(void* ctx, const char* backend, bool is_input,
                                audio_device_t** out_devices,
                                size_t* out_count);
  bool (*get_device_capabilities)(void* ctx, const char* backend,
                                  const char* device, bool is_capture,
                                  audio_device_descriptor_t** out_desc,
                                  device_error_t* out_err);
  bool (*get_spectrum)(void* ctx, bool is_capture, uint32_t channel,
                       double min_freq, double max_freq, uint32_t n_bins,
                       spectrum_t* out_spec);
  bool (*set_config_json)(void* ctx, const char* json_str,
                          audio_backend_error_t* out_err);
  void (*stop)(void* ctx);
  float (*get_fader_volume)(void* ctx, fader_t fader);
  bool (*is_fader_muted)(void* ctx, fader_t fader);
  void (*set_fader_volume)(void* ctx, fader_t fader, float db, bool instant);
  void (*set_fader_mute)(void* ctx, fader_t fader, bool mute);

  // Path & persistence callbacks
  const char* (*get_state_file)(void* ctx);
  bool (*is_state_dirty)(void* ctx);
  char* (*get_config_path)(void* ctx);
  void (*set_config_path)(void* ctx, const char* path);
} dsp_engine_interface_t;

/**
 * @brief Get the interface function pointer table.
 *
 * Used for hooking up backend callbacks.
 *
 * @param engine Pointer to the engine instance.
 * @return Pointer to the interface structure.
 */
dsp_engine_interface_t* dsp_engine_get_interface(dsp_engine_t* engine);

/**
 * @brief Get the current status/state of the engine.
 *
 * @param engine Pointer to the engine instance.
 * @return State update structure.
 */
state_update_t dsp_engine_get_status(const dsp_engine_t* engine);

/**
 * @brief Get current VU levels.
 *
 * @param engine Pointer to the engine instance.
 * @return VU levels structure.
 */
vu_levels_t dsp_engine_get_vu_levels(const dsp_engine_t* engine);

/**
 * @brief Free VU levels structure.
 * @param levels Pointer to levels to free.
 */
void dsp_engine_free_vu_levels(vu_levels_t* levels);

/**
 * @brief Get spectrum data for a channel.
 *
 * @param engine Pointer to the engine instance.
 * @param is_capture True for capture, false for playback.
 * @param channel Channel index.
 * @param min_freq Minimum frequency.
 * @param max_freq Maximum frequency.
 * @param n_bins Number of bins requested.
 * @param out_result Pointer to store the result.
 * @return Status of spectrum retrieval.
 */
spectrum_status_t dsp_engine_get_spectrum(dsp_engine_t* engine, bool is_capture,
                                          int channel, double min_freq,
                                          double max_freq, size_t n_bins,
                                          spectrum_result_t* out_result);

/**
 * @brief Get raw audio samples from the history buffer.
 *
 * @param engine Pointer to the engine instance.
 * @param is_capture True for capture, false for playback.
 * @param n_frames Number of frames requested.
 * @param err Pointer to store error code.
 * @return Pointer to audio samples structure, or NULL on failure.
 */
audio_samples_t* dsp_engine_get_samples(dsp_engine_t* engine, bool is_capture,
                                        size_t n_frames,
                                        audio_backend_error_t* err);

/**
 * @brief Free audio samples structure.
 * @param samples Pointer to samples to free.
 */
void dsp_engine_free_samples(audio_samples_t* samples);

/**
 * @brief Set global log level.
 * @param level Log level.
 */
void dsp_engine_set_log_level(log_level_t level);

/**
 * @brief Get available audio devices for a backend.
 *
 * @param backend Backend name.
 * @param input True for input/capture devices, false for output/playback.
 * @param out_devices Array to store found devices.
 * @param max_devices Maximum number of devices to return.
 * @return Number of devices found, or negative error code.
 */
int dsp_engine_get_available_devices(const char* backend, bool input,
                                     audio_device_t* out_devices,
                                     int max_devices);

/**
 * @brief Get detailed capabilities for a specific device.
 *
 * @param backend Backend name.
 * @param device Device name.
 * @param is_capture True if capture, false if playback.
 * @param err Pointer to store error code.
 * @return Pointer to device descriptor, or NULL on failure.
 */
audio_device_descriptor_t* dsp_engine_get_device_capabilities(
    const char* backend, const char* device, bool is_capture,
    device_error_t* err);

/**
 * @brief Free device capabilities descriptor.
 * @param desc Pointer to descriptor to free.
 */
void dsp_engine_free_device_capabilities(audio_device_descriptor_t* desc);

/**
 * @brief Get active configuration.
 * @param engine Pointer to the engine instance.
 * @return Pointer to active config struct.
 */
const dsp_config_t* dsp_engine_get_active_config(const dsp_engine_t* engine);

/**
 * @brief Get processing parameters.
 * @param engine Pointer to the engine instance.
 * @return Pointer to processing parameters struct (caller must NOT free, owned
 * by engine).
 */
processing_parameters_t* dsp_engine_get_processing_parameters(
    const dsp_engine_t* engine);

#endif  // CLIB_ENGINE_DSP_ENGINE_H
