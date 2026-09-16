/**
 * @file pipeline.h
 * @brief Main audio processing pipeline.
 *
 * This module manages the audio processing pipeline, including filters, mixers,
 * and processors. It handles processing audio chunks and updating parameters
 * dynamically.
 */

#ifndef CLIB_PIPELINE_PIPELINE_H
#define CLIB_PIPELINE_PIPELINE_H

#include <stdbool.h>
#include <stddef.h>

#include "audio/audio_chunk.h"
#include "audio/processing_parameters.h"
#include "config/config_error.h"
#include "config/configuration.h"

/**
 * @brief Pipeline error codes.
 */
typedef enum {
  PIPELINE_OK = 0,                           ///< No error.
  PIPELINE_ERR_INPUT_SIZE_MISMATCH = -1,     ///< Input size mismatch.
  PIPELINE_ERR_OUTPUT_BUFFER_TOO_SMALL = -2, ///< Output buffer too small.
  PIPELINE_ERR_CHANNEL_COUNT_MISMATCH = -3   ///< Channel count mismatch.
} pipeline_error_t;

/**
 * @brief Convert pipeline error code to human-readable string.
 * @param err Error code.
 * @return Constant string description.
 */
const char *pipeline_error_description(pipeline_error_t err);

struct pipeline_s;

/**
 * @brief Opaque structure representing the audio processing pipeline.
 */
typedef struct pipeline_s pipeline_t;

/**
 * @brief Initialize the main audio processing pipeline.
 *
 * @param[in] config The DSP configuration to initialize the pipeline with.
 * @param[in,out] proc_params Processing parameters.
 * @param[in] explicit_chunk_size Explicit chunk size, or 0 to use config
 * default.
 * @param[out] err Pointer to a config error struct to receive error details on
 * failure.
 * @return Pointer to the created pipeline, or NULL on failure.
 */
pipeline_t *pipeline_create(const dsp_config_t *config,
                            processing_parameters_t *proc_params,
                            size_t explicit_chunk_size, config_error_t *err);

/**
 * @brief Process an input audio chunk into an output audio chunk.
 *
 * @param[in,out] pipeline The pipeline instance.
 * @param[in] input The input audio chunk.
 * @param[out] output The output audio chunk.
 * @return pipeline_error_t error code.
 */
pipeline_error_t pipeline_process(pipeline_t *pipeline,
                                  const audio_chunk_t *input,
                                  audio_chunk_t *output);

/**
 * @brief Transfers all stateful filter/processor history variables from src to
 * dest pipeline.
 *
 * Scans the pipeline steps, matches filters/processors by name and type, and
 * transfers their internal states to prevent dynamic glitches on swaps.
 *
 * @param dest The destination pipeline instance (newly built).
 * @param src The source pipeline instance (currently active).
 * @param transfer_filters True if filter/processor states should be
 * transferred.
 */
void pipeline_transfer_state(pipeline_t *dest, const pipeline_t *src,
                             bool transfer_filters);

/**
 * @brief Destroy and free the pipeline.
 *
 * @param[in] pipeline The pipeline instance to free.
 */
void pipeline_free(pipeline_t *pipeline);

/**
 * @brief Get the expected number of channels for the last error.
 *
 * @param[in] pipeline The pipeline instance.
 * @return Expected number of channels.
 */
size_t pipeline_get_last_error_needed(const pipeline_t *pipeline);

/**
 * @brief Get the actual number of channels for the last error.
 *
 * @param[in] pipeline The pipeline instance.
 * @return Actual number of channels.
 */
size_t pipeline_get_last_error_got(const pipeline_t *pipeline);

/**
 * @brief Validates pipeline configuration structure and channel tracking.
 *
 * Scans the pipeline steps, ensures referenced components exist and channel
 * bounds match available channels, and verifies output channel count matches
 * playback configuration.
 *
 * @param[in] config The top-level DSP configuration.
 * @param[out] err Pointer to a config error struct to receive error details on
 * failure.
 * @return 0 on success, -1 on failure.
 */
int pipeline_config_validate(const dsp_config_t *config, config_error_t *err);

/**
 * @brief Get whether the pipeline is configured for multithreading.
 *
 * @param[in] pipeline The pipeline instance.
 * @return true if multithreaded, false otherwise.
 */
bool pipeline_is_multithreaded(const pipeline_t *pipeline);

/**
 * @brief Get the configured worker threads count.
 *
 * @param[in] pipeline The pipeline instance.
 * @return Number of worker threads (0 = default/auto).
 */
size_t pipeline_get_worker_threads(const pipeline_t *pipeline);

/**
 * @brief Computes the used capture channels mask based on the first
 * non-bypassed mixer in the pipeline.
 *
 * If there is a non-bypassed mixer in the pipeline, a capture channel is marked
 * true if and only if it is routed as an unmuted source in at least one unmuted
 * mapping of that mixer. If there is no non-bypassed mixer, all capture
 * channels are marked true.
 *
 * @param[in] config DSP configuration.
 * @param[out] out_used Boolean array of size at least channels_count.
 * @param[in] channels_count Number of capture channels.
 * @return true on success, false on error or if config is NULL.
 */
bool pipeline_compute_used_capture_channels(const dsp_config_t *config,
                                            bool *out_used,
                                            size_t channels_count);

/**
 * @brief Get the used capture channels mask stored on the pipeline.
 *
 * @param[in] pipeline The pipeline instance.
 * @param[out] out_count Optional pointer to receive the channel count.
 * @return Pointer to boolean mask array of used capture channels, or NULL.
 */
const bool *pipeline_get_used_capture_channels(const pipeline_t *pipeline,
                                               size_t *out_count);

#endif // CLIB_PIPELINE_PIPELINE_H
