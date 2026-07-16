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
 * @brief Poll the engine for status updates and house keeping.
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

  audio_samples_t* (*get_samples)(void* ctx, bool is_capture, size_t n_frames,
                                  audio_backend_error_t* err);

  // Path & persistence callbacks
  const char* (*get_state_file)(void* ctx);
  void (*set_state_file)(void* ctx, const char* path);
  bool (*is_state_dirty)(void* ctx);
  char* (*get_config_path)(void* ctx);
  void (*set_config_path)(void* ctx, const char* path);
  void (*set_log_level)(void* ctx, log_level_t level);
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

#ifdef CDSP_TEST
// Direct functions used by internal test suites (test_dsp_engine.c)
const dsp_config_t* dsp_engine_get_active_config(dsp_engine_t* engine);
bool dsp_engine_set_config(dsp_engine_t* engine, const char* json,
                           audio_backend_error_t* err);
bool dsp_engine_set_config_struct(dsp_engine_t* engine, dsp_config_t* config,
                                  audio_backend_error_t* err);
#endif

#endif  // CLIB_ENGINE_DSP_ENGINE_H
