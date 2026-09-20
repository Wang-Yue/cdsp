#ifndef CDSP_RAW_WRITER_H
#define CDSP_RAW_WRITER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "audio/audio_chunk.h"
#include "audio/sample_format.h"

/**
 * @brief Encode a single normalized double sample into target binary format
 * bytes.
 *
 * @param val Double sample value in [-1.0, 1.0].
 * @param dst Pointer to destination buffer (must have at least
 * sample_format_bytes_per_sample).
 * @param format Binary sample format.
 */
void raw_encode_sample(double val, uint8_t *dst, binary_sample_format_t format);

/**
 * @brief Write multi-channel interleaved audio samples to an open binary
 * stream.
 *
 * @param f Open destination stream positioned at write location.
 * @param channel_data Array of pointers to channel sample buffers (length
 * channels).
 * @param channels Number of audio channels.
 * @param frames Number of audio frames to write.
 * @param format Binary sample format.
 * @param err_msg Output buffer for error description on failure.
 * @param err_msg_len Size of err_msg buffer.
 * @return true on success, false on failure.
 */
bool raw_write_interleaved_stream(FILE *f, const double *const *channel_data,
                                  size_t channels, size_t frames,
                                  binary_sample_format_t format, char *err_msg,
                                  size_t err_msg_len);

/**
 * @brief Encode and write an audio chunk to an open binary stream.
 *
 * Handles capacity reallocation of raw_buf, interleaved encoding, and stream
 * write.
 *
 * @param f Open destination file stream.
 * @param chunk Audio chunk to encode and write.
 * @param channels Number of channels expected.
 * @param format Target binary sample format.
 * @param total_bytes_written Pointer to accumulator tracking payload bytes
 * written.
 * @param raw_buf In/out pointer to temporary byte buffer for interleaved
 * encoded data.
 * @param raw_buf_capacity In/out pointer to current allocated capacity of
 * raw_buf.
 * @param err_msg Output buffer for error description on failure.
 * @param err_msg_len Size of err_msg buffer.
 * @return true on success, false on write error.
 */
bool raw_write_audio_chunk(FILE *f, const audio_chunk_t *chunk, size_t channels,
                           binary_sample_format_t format,
                           uint64_t *total_bytes_written, uint8_t **raw_buf,
                           size_t *raw_buf_capacity, char *err_msg,
                           size_t err_msg_len);

/**
 * @brief Write single-channel audio samples to a raw binary file.
 *
 * @param path Destination file path.
 * @param samples Array of sample values in [-1.0, 1.0].
 * @param num_samples Number of samples to write.
 * @param format Binary sample format.
 * @param err_msg Output buffer for error description on failure.
 * @param err_msg_len Size of err_msg buffer.
 * @return true on success, false on failure.
 */
bool raw_write_samples(const char *path, const double *samples,
                       size_t num_samples, binary_sample_format_t format,
                       char *err_msg, size_t err_msg_len);

/**
 * @brief Write multi-channel interleaved audio samples to a raw binary file.
 *
 * @param path Destination file path.
 * @param channel_data Array of pointers to channel sample buffers (length
 * channels).
 * @param channels Number of audio channels.
 * @param frames Number of audio frames to write.
 * @param format Binary sample format.
 * @param err_msg Output buffer for error description on failure.
 * @param err_msg_len Size of err_msg buffer.
 * @return true on success, false on failure.
 */
bool raw_write_interleaved_file(const char *path,
                                const double *const *channel_data,
                                size_t channels, size_t frames,
                                binary_sample_format_t format, char *err_msg,
                                size_t err_msg_len);

/**
 * @brief Write audio samples or filter coefficients to an ASCII text file (one
 * float per line).
 *
 * Formatted with full precision (%.17g) compatible with CamillaDSP's TEXT
 * parser.
 *
 * @param path Destination file path.
 * @param samples Array of sample values.
 * @param num_samples Number of samples to write.
 * @param err_msg Output buffer for error description on failure.
 * @param err_msg_len Size of err_msg buffer.
 * @return true on success, false on failure.
 */
bool raw_write_text_samples(const char *path, const double *samples,
                            size_t num_samples, char *err_msg,
                            size_t err_msg_len);

/**
 * @brief High-level raw sample writer supporting both binary formats and
 * "TEXT".
 *
 * @param path Destination file path.
 * @param format_str Format string (e.g. "TEXT", "S16_LE", "S24_3_LE", "S32_LE",
 * "F32_LE", "F64_LE").
 * @param samples Array of sample values.
 * @param num_samples Number of samples to write.
 * @param err_msg Output buffer for error description on failure.
 * @param err_msg_len Size of err_msg buffer.
 * @return true on success, false on failure.
 */
bool raw_write_file(const char *path, const char *format_str,
                    const double *samples, size_t num_samples, char *err_msg,
                    size_t err_msg_len);

#endif // CDSP_RAW_WRITER_H
