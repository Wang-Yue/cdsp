#ifndef CDSP_WAV_WRITER_H
#define CDSP_WAV_WRITER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "audio/audio_chunk.h"
#include "audio/sample_format.h"
#include "wav/wav_types.h"

/**
 * @brief Check if adding bytes_to_add would exceed the standard 4 GB RIFF WAV
 * boundary.
 *
 * @param total_written Number of audio data bytes written so far.
 * @param bytes_to_add Number of bytes to be written in the next chunk.
 * @param is_wav True if writing WAV file.
 * @param is_seekable True if output file stream is seekable.
 * @param use_rf64 True if 64-bit RF64 container is active.
 * @return true if write is permitted, false if plain WAV 4 GB limit would be
 * breached.
 */
bool wav_can_write_bytes(uint64_t total_written, size_t bytes_to_add,
                         bool is_wav, bool is_seekable, bool use_rf64);

/**
 * @brief Write a standard RIFF WAV header (44 bytes or extended 68/80 bytes) to
 * the file stream.
 *
 * Automatically selects WAVE_FORMAT_EXTENSIBLE when channels > 2 or for S24_4
 * formats. Appends a 'fact' chunk for float or extensible formats.
 *
 * @param f File pointer to write to.
 * @param channels Number of audio channels.
 * @param format Binary sample format.
 * @param sample_rate Sampling rate in Hz.
 * @param data_bytes Data chunk payload length in bytes (or 0xFFFFFFFF for
 * streaming placeholder).
 * @param is_seekable If true, seeks to offset 0 before writing.
 * @return true on success, false on write error.
 */
bool wav_write_header(FILE *f, size_t channels, binary_sample_format_t format,
                      uint32_t sample_rate, uint32_t data_bytes,
                      bool is_seekable);

/**
 * @brief Write an RF64 header with 64-bit 'ds64' chunk to the file stream.
 *
 * Used when RF64 format is explicitly configured or required for >4 GB files.
 *
 * @param f File pointer to write to.
 * @param channels Number of audio channels.
 * @param format Binary sample format.
 * @param sample_rate Sampling rate in Hz.
 * @param data_bytes Size of data payload in bytes.
 * @return true on success, false on write error.
 */
bool wav_write_rf64_header(FILE *f, size_t channels,
                           binary_sample_format_t format, uint32_t sample_rate,
                           uint64_t data_bytes);

/**
 * @brief Finalize and rewrite WAV/RF64 header with actual total bytes written
 * upon closing.
 *
 * @param f File pointer to write to.
 * @param channels Number of audio channels.
 * @param format Binary sample format.
 * @param sample_rate Sampling rate in Hz.
 * @param total_bytes_written Total audio payload bytes written.
 * @param use_rf64 True to write RF64 header, false to write standard WAV
 * header.
 * @return true on success, false on write error.
 */
bool wav_update_header(FILE *f, size_t channels, binary_sample_format_t format,
                       uint32_t sample_rate, uint64_t total_bytes_written,
                       bool use_rf64);

/**
 * @brief Encode and write an audio chunk to a WAV or raw file stream.
 *
 * Handles 4 GB plain WAV boundary checks, capacity reallocation of raw_buf,
 * interleaved encoding, and file writing.
 *
 * @param f Open destination file stream.
 * @param chunk Audio chunk to encode and write.
 * @param channels Number of channels expected.
 * @param format Target binary sample format.
 * @param is_wav True if output has a WAV header.
 * @param is_seekable True if destination stream is seekable.
 * @param use_rf64 True if RF64 64-bit format is used.
 * @param total_bytes_written Pointer to accumulator tracking payload bytes
 * written.
 * @param raw_buf In/out pointer to temporary byte buffer for interleaved
 * encoded data.
 * @param raw_buf_capacity In/out pointer to current allocated capacity of
 * raw_buf.
 * @param reached_4gb_limit Out pointer set to true if plain WAV 4GB limit was
 * reached.
 * @param err_msg Output buffer for error description on failure.
 * @param err_msg_len Size of err_msg buffer.
 * @return true on success, false on error or clean 4GB limit stop.
 */
bool wav_write_audio_chunk(FILE *f, const audio_chunk_t *chunk, size_t channels,
                           binary_sample_format_t format, bool is_wav,
                           bool is_seekable, bool use_rf64,
                           uint64_t *total_bytes_written, uint8_t **raw_buf,
                           size_t *raw_buf_capacity, bool *reached_4gb_limit,
                           char *err_msg, size_t err_msg_len);

/**
 * @brief Write a single channel of normalized double samples to a WAV file.
 *
 * @param path Destination file path.
 * @param samples Array of sample values in [-1.0, 1.0].
 * @param num_samples Number of samples to write.
 * @param sample_rate Sampling rate in Hz.
 * @param format Binary sample format.
 * @param err_msg Output buffer for error description on failure.
 * @param err_msg_len Size of err_msg buffer.
 * @return true on success, false on failure.
 */
bool wav_write_channel_samples(const char *path, const double *samples,
                               size_t num_samples, uint32_t sample_rate,
                               binary_sample_format_t format, char *err_msg,
                               size_t err_msg_len);

/**
 * @brief Write multi-channel audio data to a WAV file.
 *
 * @param path Destination file path.
 * @param channel_data Array of pointers to channel sample buffers (length
 * channels).
 * @param channels Number of audio channels.
 * @param frames Number of audio frames to write.
 * @param sample_rate Sampling rate in Hz.
 * @param format Binary sample format.
 * @param use_rf64 True to force RF64 64-bit format.
 * @param err_msg Output buffer for error description on failure.
 * @param err_msg_len Size of err_msg buffer.
 * @return true on success, false on failure.
 */
bool wav_write_file(const char *path, const double *const *channel_data,
                    size_t channels, size_t frames, uint32_t sample_rate,
                    binary_sample_format_t format, bool use_rf64, char *err_msg,
                    size_t err_msg_len);

#endif // CDSP_WAV_WRITER_H
