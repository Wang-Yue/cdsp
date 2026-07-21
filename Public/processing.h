#ifndef CDSP_PUBLIC_PROCESSING_H
#define CDSP_PUBLIC_PROCESSING_H

#include <stdbool.h>
#include <stdint.h>

#include "cdsp_pub_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Get the current processing state of the engine.
 * @param engine Pointer to the engine.
 * @return The processing state enum.
 */
CDSP_API cdsp_processing_state_t cdsp_get_state(const dsp_engine_t* engine);

/**
 * @brief Get the reason why the engine last stopped.
 * @param engine Pointer to the engine.
 * @param out_reason Pointer to write the stop reason details into.
 */
CDSP_API void cdsp_get_stop_reason(const dsp_engine_t* engine,
                                   cdsp_stop_reason_t* out_reason);

/**
 * @brief Get the active sample rate of the capture device.
 * @param engine Pointer to the engine.
 * @return Measured capture sample rate in Hz, or 0 if inactive/stalled.
 */
CDSP_API int cdsp_get_capture_rate(const dsp_engine_t* engine);

/**
 * @brief Get the peak-to-peak signal range of the last processed chunk.
 * @param engine Pointer to the engine.
 * @return Range value (2.0 = full level).
 */
CDSP_API double cdsp_get_signal_range(const dsp_engine_t* engine);

/**
 * @brief Retrieve all real-time processing status metrics in a single call.
 *
 * @param engine Pointer to the engine.
 * @param out_rate_adjust Output rate adjustment factor.
 * @param out_buffer_level Output playback buffer level in frames.
 * @param out_clipped_samples Output total clipped samples since configuration
 * load.
 * @param out_processing_load Output pipeline processing CPU load in percent.
 * @param out_resampler_load Output resampler CPU load in percent.
 * @return true on success, false on failure (e.g. engine not running).
 */
CDSP_API bool cdsp_get_processing_status(const dsp_engine_t* engine,
                                         double* out_rate_adjust,
                                         double* out_buffer_level,
                                         uint64_t* out_clipped_samples,
                                         double* out_processing_load,
                                         double* out_resampler_load);

/**
 * @brief Reset the clipped samples counter to zero.
 * @param engine Pointer to the engine.
 */
CDSP_API void cdsp_reset_clipped_samples(dsp_engine_t* engine);

/**
 * @brief Get the path to the state file, if configured.
 * @param engine Pointer to the engine.
 * @return The path string, or NULL if none is configured (do not free).
 */
CDSP_API const char* cdsp_get_state_file_path(const dsp_engine_t* engine);

/**
 * @brief Set the path to the state file for persisting volume and mute states
 * (WebSocket: SetStateFilePath).
 * @param engine Pointer to the engine.
 * @param path Path to the state file.
 */
CDSP_API void cdsp_set_state_file_path(dsp_engine_t* engine, const char* path);

/**
 * @brief Check whether all pending changes have been saved to the state file
 * (WebSocket: GetStateFileUpdated).
 * @param engine Pointer to the engine.
 * @return true if state file is updated, false if pending changes exist.
 */
CDSP_API bool cdsp_get_state_file_updated(const dsp_engine_t* engine);

/**
 * @brief Retrieve a chunk of audio samples from the engine's internal history
 * buffers.
 *
 * @param engine Pointer to the engine.
 * @param is_capture True to retrieve capture samples, false to retrieve
 * playback samples.
 * @param n_frames Number of frames to retrieve.
 * @param out_err Pointer to retrieve error details on failure.
 * @return Pointer to a newly allocated cdsp_audio_samples_t structure, or NULL
 * on failure. Must be freed with cdsp_free_samples().
 */
CDSP_API cdsp_audio_samples_t* cdsp_get_samples(dsp_engine_t* engine,
                                                bool is_capture,
                                                size_t n_frames,
                                                cdsp_backend_error_t* out_err);

/**
 * @brief Free the resources allocated for audio samples.
 * @param samples Pointer to the audio samples structure.
 */
CDSP_API void cdsp_free_samples(cdsp_audio_samples_t* samples);

#ifdef __cplusplus
}
#endif

#endif  // CDSP_PUBLIC_PROCESSING_H
