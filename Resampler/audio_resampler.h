/**
 * @file audio_resampler.h
 * @brief Unified interface and wrappers for various audio resampler
 * implementations.
 *
 * This file defines the `audio_resampler_t` interface, which abstracts
 * different resampling algorithms (synchronous, asynchronous sinc, asynchronous
 * polynomial, etc.). It supports zero-allocation processing on the hot path by
 * requiring pre-allocated buffers.
 */

#ifndef CLIB_RESAMPLER_AUDIO_RESAMPLER_H
#define CLIB_RESAMPLER_AUDIO_RESAMPLER_H

#include <stdbool.h>
#include <stddef.h>

#include "Audio/audio_chunk.h"
#include "Config/resampler_config_types.h"
#include "resampler_error.h"

/**
 * @brief Identifiers for the underlying resampler implementations.
 */
typedef enum {
  /** FFT-based fixed-ratio resampler. */
  RESAMPLER_IMPL_SYNCHRONOUS = 0,
  /** Asynchronous windowed-sinc resampler. */
  RESAMPLER_IMPL_ASYNC_SINC,
  /** Asynchronous polynomial resampler. */
  RESAMPLER_IMPL_ASYNC_POLY
} resampler_impl_type_t;

/**
 * @struct resampler_vtable
 * @brief Virtual method table for audio resampler implementations.
 */
typedef struct resampler_vtable {
  int (*validate)(const resampler_config_t* config, config_error_t* err);
  void* (*create)(const resampler_config_t* config, size_t input_rate,
                  size_t output_rate, size_t channels, size_t chunk_size,
                  config_error_t* err);
  resampler_error_t (*process)(void* impl, const audio_chunk_t* input,
                               audio_chunk_t* output);
  void (*set_relative_ratio)(void* impl, double multiplier);
  double (*get_ratio)(const void* impl);
  size_t (*get_max_output_frames)(const void* impl);
  size_t (*get_chunk_size)(const void* impl);
  size_t (*get_input_frames_next)(const void* impl);
  size_t (*get_output_frames_next)(const void* impl);
  size_t (*get_channels)(const void* impl);
  void (*free)(void* impl);
} resampler_vtable_t;

/**
 * @struct resampler
 * @brief Polymorphic interface wrapper for resamplers.
 */
typedef struct resampler {
  resampler_impl_type_t type;
  void* impl;
  const resampler_vtable_t* vtable;
} resampler_t;

/**
 * @brief Validates a resampler configuration.
 *
 * @param config Pointer to the resampler configuration to validate.
 * @param err Pointer to a config error struct to populate on failure.
 * @return 0 on success, -1 on failure.
 */
int resampler_config_validate(const resampler_config_t* config,
                              config_error_t* err);

/**
 * @brief Creates a resampler based on the provided configuration.
 *
 * @param config Configuration parameters for the resampler.
 * @param input_rate Input sample rate in Hz.
 * @param output_rate Output sample rate in Hz.
 * @param channels Number of audio channels.
 * @param chunk_size Fixed number of input frames expected per process call.
 * @param err Pointer to a config error struct to populate on failure.
 * @return A new resampler instance, or NULL on error.
 */
resampler_t* resampler_create_from_config(const resampler_config_t* config,
                                          size_t input_rate, size_t output_rate,
                                          size_t channels, size_t chunk_size,
                                          config_error_t* err);

/**
 * @brief Processes a chunk of audio data.
 *
 * This function performs the resampling. It is a zero-allocation call.
 * The caller must pre-allocate the output buffer.
 *
 * @param resampler The resampler instance.
 * @param input The input audio chunk. `input->validFrames` must equal
 * `chunk_size`.
 * @param output The output audio chunk. Must have capacity for at least the
 * worst-case number of output frames (see @ref
 * resampler_get_max_output_frames). This function updates
 * `output->validFrames` with the actual number of frames written.
 * @return @ref RESAMPLER_OK on success, or an error code on failure.
 */
resampler_error_t resampler_process(resampler_t* resampler,
                                    const audio_chunk_t* input,
                                    audio_chunk_t* output);

/**
 * @brief Adjusts the resampling ratio dynamically.
 *
 * Applies a multiplicative correction factor to the base resampling ratio.
 * Note: Synchronous resamplers may ignore this adjustment.
 *
 * @param resampler The resampler instance.
 * @param multiplier The correction factor (e.g. 1.0001 to slightly increase
 * output rate).
 */
void resampler_set_relative_ratio(resampler_t* resampler, double multiplier);

/**
 * @brief Gets the current effective resampling ratio.
 *
 * The effective ratio is `base_ratio * relative_ratio`.
 *
 * @param resampler The resampler instance.
 * @return The current resampling ratio as a double.
 */
double resampler_get_ratio(const resampler_t* resampler);

/**
 * @brief Gets the maximum number of output frames that could be generated.
 *
 * Use this to size the output buffer before calling @ref
 * resampler_process.
 *
 * @param resampler The resampler instance.
 * @return The maximum number of output frames.
 */
size_t resampler_get_max_output_frames(const resampler_t* resampler);

/**
 * @brief Gets the fixed input chunk size expected by the resampler.
 *
 * @param resampler The resampler instance.
 * @return The expected input chunk size in frames.
 */
size_t resampler_get_chunk_size(const resampler_t* resampler);

/**
 * @brief Gets the number of input frames required for the next process call.
 *
 * @param resampler The resampler instance.
 * @return The required input frame count.
 */
size_t resampler_get_input_frames_next(const resampler_t* resampler);

/**
 * @brief Gets the number of output frames that will be generated in the next
 * process call.
 *
 * @param resampler The resampler instance.
 * @return The expected output frame count.
 */
size_t resampler_get_output_frames_next(const resampler_t* resampler);

/**
 * @brief Gets the number of channels the resampler is configured for.
 *
 * @param resampler The resampler instance.
 * @return The number of audio channels.
 */
size_t resampler_get_channels(const resampler_t* resampler);

/**
 * @brief Frees the resampler instance and its resources.
 *
 * @param resampler The resampler instance to free.
 */
void resampler_free(resampler_t* resampler);

#endif  // CLIB_RESAMPLER_AUDIO_RESAMPLER_H
