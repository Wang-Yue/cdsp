/**
 * @file audio_chunk.h
 * @brief Non-interleaved float buffers, one vector per channel.
 */

#ifndef CLIB_AUDIO_AUDIO_CHUNK_H
#define CLIB_AUDIO_AUDIO_CHUNK_H

#include <stdbool.h>
#include <stddef.h>

#include "audio/audio_buffers.h"
#include "audio/sample_format.h"
#include "utils/double_helpers.h"

/**
 * @struct audio_chunk
 * @brief A chunk of non-interleaved audio data flowing through the pipeline.
 *
 * Storage is heap-backed (@ref audio_buffers_t) so per-channel mutable pointers
 * stay stable across struct copies and the audio thread can mutate samples
 * without going through copy-on-write uniqueness checks. Two @ref audio_chunk_t
 * values that share an @ref audio_buffers_t see the same samples — this is a
 * deliberate trade against value semantics, made to remove allocations on the
 * hot path.
 */
typedef struct audio_chunk audio_chunk_t;

/**
 * @brief Callback signature for audio chunk processing taps.
 */
typedef void (*chunk_callback_t)(void *ctx, const audio_chunk_t *chunk);

/**
 * @struct round_robin_chunk_pool
 * @brief A pool of audio chunks for reuse to avoid allocations in the hot path.
 */
typedef struct round_robin_chunk_pool round_robin_chunk_pool_t;

/**
 * @brief Create a new silent audio chunk with freshly allocated storage.
 *
 * @param frames Number of frames (samples per channel).
 * @param channels Number of channels.
 * @return Pointer to the allocated audio_chunk_t, or NULL on failure.
 */
audio_chunk_t *audio_chunk_create(size_t frames, size_t channels);

/**
 * @brief Free the audio chunk.
 *
 * This will also free the underlying audio buffers if this chunk owns them.
 *
 * @param chunk Pointer to the audio_chunk_t to free.
 */
void audio_chunk_free(audio_chunk_t *chunk);

/**
 * @brief Get the sample capacity per channel of the chunk.
 *
 * @param chunk Pointer to the audio_chunk_t.
 * @return The number of frames capacity.
 */
size_t audio_chunk_get_frames(const audio_chunk_t *chunk);

/**
 * @brief Get the number of channels in the chunk.
 *
 * @param chunk Pointer to the audio_chunk_t.
 * @return The number of channels.
 */
size_t audio_chunk_get_channels(const audio_chunk_t *chunk);

/**
 * @brief Get a mutable pointer to the data for a specific channel.
 *
 * The pointer is stable for the lifetime of the underlying @ref audio_buffers_t
 * and aliases across struct copies — no CoW.
 *
 * @param chunk Pointer to the audio_chunk_t.
 * @param ch Channel index.
 * @return Mutable pointer to the channel's audio data.
 */
mutable_waveform_t audio_chunk_get_channel(const audio_chunk_t *chunk,
                                           size_t ch);

/**
 * @brief Get the number of valid frames in the chunk.
 *
 * May be less than @ref audio_chunk_get_frames at the end of a stream.
 *
 * @param chunk Pointer to the audio_chunk_t.
 * @return The number of valid frames.
 */
size_t audio_chunk_get_valid_frames(const audio_chunk_t *chunk);

/**
 * @brief Set the number of valid frames in the chunk.
 *
 * @param chunk Pointer to the audio_chunk_t.
 * @param valid_frames The number of valid frames to set.
 */
void audio_chunk_set_valid_frames(audio_chunk_t *chunk, size_t valid_frames);

/**
 * @brief Zero all sample storage in the chunk across all channels.
 *
 * @param chunk Pointer to the audio chunk.
 */
void audio_chunk_zero(audio_chunk_t *chunk);

/**
 * @brief Zero the sample storage beyond valid_frames up to frames across all
 * channels.
 *
 * @param chunk Pointer to the audio chunk.
 */
void audio_chunk_zero_tail(audio_chunk_t *chunk);

/**
 * @brief Create a round-robin chunk pool.
 *
 * @param capacity The number of chunks in the pool.
 * @param frames The capacity of each chunk in frames.
 * @param channels The number of channels in each chunk.
 * @return Pointer to the created pool, or NULL on failure.
 */
round_robin_chunk_pool_t *
round_robin_chunk_pool_create(size_t capacity, size_t frames, size_t channels);

/**
 * @brief Retrieves the next available unique chunk buffer from the pool.
 *
 * @param pool Pointer to the pool.
 * @return Pointer to an audio_chunk_t, or NULL if none are available.
 */
audio_chunk_t *round_robin_chunk_pool_next(round_robin_chunk_pool_t *pool);

/**
 * @brief Free the round-robin chunk pool and all its chunks.
 *
 * @param pool Pointer to the pool to free.
 */
void round_robin_chunk_pool_free(round_robin_chunk_pool_t *pool);

/**
 * @brief Sets the used channels mask on all chunks in the pool.
 *
 * @param pool Pointer to the pool.
 * @param used_channels Array of booleans indicating used channels, or NULL to
 * clear.
 */
void round_robin_chunk_pool_set_used_channels(round_robin_chunk_pool_t *pool,
                                              const bool *used_channels);

/**
 * @brief Sums multiple audio channels of a chunk into a single buffer.
 *
 * This function performs a vector addition to sum the samples from all
 * specified channels frame-by-frame, writing the result into the provided
 * output buffer. This is typically used to create a mono sum for sidechain
 * processing.
 *
 * @param chunk The audio chunk to sum from.
 * @param channels Array of channel indices to sum.
 * @param channels_count Number of channels in the channels array.
 * @param out_sum Output buffer to write the summed samples.
 * @param frames Number of frames to sum.
 */
void audio_chunk_sum_channels(const audio_chunk_t *chunk,
                              const size_t *channels, size_t channels_count,
                              double *out_sum, size_t frames);

/**
 * @brief Applies sample-by-sample linear gain to multiple channels in a chunk.
 *
 * Multiplies the wave samples of each specified channel by the corresponding
 * multiplier from the gain_multipliers buffer.
 *
 * @param chunk The audio chunk containing the channels to process.
 * @param channels Array of channel indices to apply gain to.
 * @param channels_count Number of channels in the channels array.
 * @param gain_multipliers Array of linear gain multipliers (one per frame).
 * @param frames Number of frames to process.
 */
void audio_chunk_apply_gain(audio_chunk_t *chunk, const size_t *channels,
                            size_t channels_count,
                            const double *gain_multipliers, size_t frames);

/**
 * @brief Decodes interleaved raw byte samples into planar double audio chunk.
 *
 * @param src Pointer to interleaved raw bytes.
 * @param fmt Binary sample format of the source buffer.
 * @param channels Number of audio channels in source.
 * @param frames Number of audio frames to decode.
 * @param chunk Destination audio chunk to populate.
 * @return True on success, false on invalid format or parameters.
 */
bool audio_chunk_decode_interleaved(const void *src, binary_sample_format_t fmt,
                                    size_t channels, size_t frames,
                                    audio_chunk_t *chunk);

/**
 * @brief Decodes interleaved raw byte samples into planar double audio chunk
 * starting at a specific destination frame offset.
 *
 * @param src Pointer to interleaved raw byte data.
 * @param fmt Binary sample format of raw bytes.
 * @param channels Number of audio channels in source data.
 * @param frames Number of audio frames to decode.
 * @param chunk Destination audio chunk.
 * @param start_frame Starting frame index inside the chunk's channel buffers.
 * @return True on success, false on invalid format or parameters.
 */
bool audio_chunk_decode_interleaved_offset(const void *src,
                                           binary_sample_format_t fmt,
                                           size_t channels, size_t frames,
                                           audio_chunk_t *chunk,
                                           size_t start_frame);

/**
 * @brief Encodes planar double audio chunk into interleaved raw byte samples.
 *
 * @param chunk Source audio chunk.
 * @param fmt Binary sample format of destination buffer.
 * @param channels Number of audio channels to encode.
 * @param frames Number of audio frames to encode.
 * @param dst Destination buffer to receive interleaved raw bytes.
 * @return True on success, false on invalid format or parameters.
 */
bool audio_chunk_encode_interleaved(const audio_chunk_t *chunk,
                                    binary_sample_format_t fmt, size_t channels,
                                    size_t frames, void *dst);

/**
 * @brief Encodes planar double audio chunk into interleaved raw byte samples
 * starting at a specific source frame offset.
 *
 * @param chunk Source audio chunk.
 * @param fmt Binary sample format of destination buffer.
 * @param channels Number of audio channels to encode.
 * @param frames Number of audio frames to encode.
 * @param dst Destination buffer to receive interleaved raw bytes.
 * @param start_frame Starting frame index inside the chunk's channel buffers.
 * @return True on success, false on invalid format or parameters.
 */
bool audio_chunk_encode_interleaved_offset(const audio_chunk_t *chunk,
                                           binary_sample_format_t fmt,
                                           size_t channels, size_t frames,
                                           void *dst, size_t start_frame);

/**
 * @brief Decodes a single planar channel from raw bytes into an audio chunk at
 * a given frame offset.
 *
 * @param src Pointer to channel raw byte buffer.
 * @param fmt Binary sample format of source bytes.
 * @param frames Number of frames to decode.
 * @param chunk Destination audio chunk.
 * @param channel Destination channel index in chunk.
 * @param start_frame Starting frame offset in chunk channel.
 * @return True on success, false on invalid format or parameters.
 */
bool audio_chunk_decode_channel(const void *src, binary_sample_format_t fmt,
                                size_t frames, audio_chunk_t *chunk,
                                size_t channel, size_t start_frame);

/**
 * @brief Encodes a single planar channel from an audio chunk at a given frame
 * offset into raw bytes.
 *
 * @param chunk Source audio chunk.
 * @param fmt Binary sample format of destination bytes.
 * @param frames Number of frames to encode.
 * @param dst Pointer to destination channel byte buffer.
 * @param channel Source channel index in chunk.
 * @param start_frame Starting frame offset in chunk channel.
 * @return True on success, false on invalid format or parameters.
 */
bool audio_chunk_encode_channel(const audio_chunk_t *chunk,
                                binary_sample_format_t fmt, size_t frames,
                                void *dst, size_t channel, size_t start_frame);

/**
 * @brief Decodes planar (non-interleaved) raw audio channels into an audio
 * chunk at a given frame offset.
 *
 * @param src_channels Array of pointers to channel byte buffers.
 * @param fmt Binary sample format of source bytes.
 * @param channels Number of channels.
 * @param frames Number of frames to decode.
 * @param chunk Destination audio chunk.
 * @param start_frame Starting frame offset in the destination chunk.
 * @return True on success, false on invalid format or parameters.
 */
bool audio_chunk_decode_planar_offset(const void *const *src_channels,
                                      binary_sample_format_t fmt,
                                      size_t channels, size_t frames,
                                      audio_chunk_t *chunk, size_t start_frame);

/**
 * @brief Decodes planar (non-interleaved) raw audio channels into an audio
 * chunk.
 *
 * @param src_channels Array of pointers to channel byte buffers.
 * @param fmt Binary sample format of source bytes.
 * @param channels Number of channels.
 * @param frames Number of frames to decode.
 * @param chunk Destination audio chunk.
 * @return True on success, false on invalid format or parameters.
 */
bool audio_chunk_decode_planar(const void *const *src_channels,
                               binary_sample_format_t fmt, size_t channels,
                               size_t frames, audio_chunk_t *chunk);

/**
 * @brief Encodes an audio chunk into planar (non-interleaved) raw audio
 * channels at a given frame offset.
 *
 * @param chunk Source audio chunk.
 * @param fmt Binary sample format of destination buffers.
 * @param channels Number of channels.
 * @param frames Number of frames to encode.
 * @param dst_channels Array of pointers to channel destination byte buffers.
 * @param start_frame Starting frame offset in the chunk.
 * @return True on success, false on invalid format or parameters.
 */
bool audio_chunk_encode_planar_offset(const audio_chunk_t *chunk,
                                      binary_sample_format_t fmt,
                                      size_t channels, size_t frames,
                                      void *const *dst_channels,
                                      size_t start_frame);

/**
 * @brief Encodes an audio chunk into planar (non-interleaved) raw audio
 * channels.
 *
 * @param chunk Source audio chunk.
 * @param fmt Binary sample format of destination buffers.
 * @param channels Number of channels.
 * @param frames Number of frames to encode.
 * @param dst_channels Array of pointers to channel destination byte buffers.
 * @return True on success, false on invalid format or parameters.
 */
bool audio_chunk_encode_planar(const audio_chunk_t *chunk,
                               binary_sample_format_t fmt, size_t channels,
                               size_t frames, void *const *dst_channels);

/**
 * @brief Sets the used channels mask for the chunk.
 *
 * @param chunk Pointer to audio chunk.
 * @param used_channels Array of booleans of length channels, or NULL to clear.
 */
void audio_chunk_set_used_channels(audio_chunk_t *chunk,
                                   const bool *used_channels);

/**
 * @brief Gets the used channels mask for the chunk.
 *
 * @param chunk Pointer to audio chunk.
 * @return Pointer to boolean mask array, or NULL if not set.
 */
const bool *audio_chunk_get_used_channels(const audio_chunk_t *chunk);

/**
 * @brief Computes the peak-to-peak value range across used channels in the
 * chunk.
 *
 * Evaluates max(sample) - min(sample) folded from 0.0 only over channels
 * where used_channels[ch] is true (or all channels if used_channels is NULL),
 * matching upstream CamillaDSP chunk value range calculation for silence
 * detection.
 *
 * @param chunk Pointer to audio chunk.
 * @param used_channels Array of booleans indicating used channels, or NULL.
 * @return Peak-to-peak value range (maxval - minval).
 */
double audio_chunk_get_value_range_used(const audio_chunk_t *chunk,
                                        const bool *used_channels);

/**
 * @brief Computes the peak-to-peak value range across all channels (or used
 * channels if previously set on the chunk).
 *
 * Evaluates max(sample) - min(sample) across all channels folded from 0.0,
 * matching upstream CamillaDSP chunk value range calculation for silence
 * detection.
 *
 * @param chunk Pointer to audio chunk.
 * @return Peak-to-peak value range (maxval - minval).
 */
double audio_chunk_get_value_range(const audio_chunk_t *chunk);

#endif // CLIB_AUDIO_AUDIO_CHUNK_H
