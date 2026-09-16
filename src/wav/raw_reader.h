#ifndef CDSP_RAW_READER_H
#define CDSP_RAW_READER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "config/engine_config_types.h"

/**
 * @brief Read one line dynamically from a text stream, allocating the buffer.
 *
 * Trailing newline/carriage return is stripped. Caller must free() the returned
 * pointer.
 *
 * @param f Open text file stream.
 * @return Dynamically allocated string, or NULL on EOF/allocation failure.
 */
char *raw_read_dynamic_line(FILE *f);

/**
 * @brief Load coefficients or audio samples from an ASCII text file (one float
 * per line).
 *
 * Rejects empty lines, comments inside the data block, multiple values per
 * line, and hex floats, strictly matching upstream CamillaDSP parser semantics.
 *
 * @param path Path to the text file.
 * @param skip_lines Number of lines to skip at the beginning of the file.
 * @param read_lines Maximum number of lines to read (0 means read until EOF).
 * @param out_count Output pointer to receive number of parsed samples.
 * @param err_buf Output buffer for error message if parsing fails.
 * @param err_len Size of err_buf buffer.
 * @return Dynamically allocated double array of parsed samples (caller frees
 * with free()), or NULL on failure.
 */
double *raw_read_text_samples(const char *path, size_t skip_lines,
                              size_t read_lines, size_t *out_count,
                              char *err_buf, size_t err_len);

/**
 * @brief Decode a single binary audio sample into a normalized double in
 * [-1.0, 1.0].
 *
 * @param src Pointer to sample bytes.
 * @param format Binary sample format.
 * @param is_u8 True if 8-bit unsigned PCM.
 * @return Decoded sample value.
 */
double raw_decode_sample(const uint8_t *src, binary_sample_format_t format,
                         bool is_u8);

/**
 * @brief Read and decode a single channel of audio samples from an open binary
 * stream.
 *
 * Can be used for raw binary files (channels=1, channel=0) or interleaved
 * multi-channel audio streams (such as WAV data payload).
 *
 * @param f Open binary stream positioned at start of audio data.
 * @param channel Zero-based channel index to extract.
 * @param channels Total number of interleaved channels.
 * @param container_bytes Bytes per sample per channel.
 * @param format Binary sample format.
 * @param is_u8 True if 8-bit unsigned PCM.
 * @param num_frames Number of audio frames to read.
 * @param out_count Output pointer to receive number of decoded samples.
 * @param err_buf Output buffer for error message if reading fails.
 * @param err_len Size of err_buf buffer.
 * @return Dynamically allocated double array of decoded samples (caller frees
 * with free()), or NULL on failure.
 */
double *raw_read_channel_stream(FILE *f, int channel, size_t channels,
                                size_t container_bytes,
                                binary_sample_format_t format, bool is_u8,
                                size_t num_frames, size_t *out_count,
                                char *err_buf, size_t err_len);

/**
 * @brief Load raw binary PCM/float audio samples from a file.
 *
 * @param path Path to the raw binary file.
 * @param format Binary sample format of the file contents.
 * @param skip_bytes Number of bytes to skip from the beginning of the file.
 * @param read_bytes Maximum number of bytes to read (0 means read until EOF).
 * @param out_count Output pointer to receive number of parsed samples.
 * @param err_buf Output buffer for error message if parsing fails.
 * @param err_len Size of err_buf buffer.
 * @return Dynamically allocated double array of parsed samples (caller frees
 * with free()), or NULL on failure.
 */
double *raw_read_binary_samples(const char *path, binary_sample_format_t format,
                                size_t skip_bytes, size_t read_bytes,
                                size_t *out_count, char *err_buf,
                                size_t err_len);

/**
 * @brief High-level loader for raw coefficient/audio files.
 *
 * If format_str is "TEXT", delegates to raw_read_text_samples.
 * Otherwise, maps format_str to binary_sample_format_t and delegates to
 * raw_read_binary_samples.
 *
 * @param path Path to the file.
 * @param format_str Format string (e.g. "TEXT", "S16_LE", "S24_3_LE", "S32_LE",
 * "F32_LE", "F64_LE").
 * @param skip_bytes_lines Number of bytes (or lines for TEXT) to skip.
 * @param read_bytes_lines Max bytes (or lines for TEXT) to read (0 means entire
 * file).
 * @param out_count Output pointer to receive number of parsed samples.
 * @param err_buf Output buffer for error message if parsing fails.
 * @param err_len Size of err_buf buffer.
 * @return Dynamically allocated double array of parsed samples (caller frees
 * with free()), or NULL on failure.
 */
double *raw_read_samples(const char *path, const char *format_str,
                         size_t skip_bytes_lines, size_t read_bytes_lines,
                         size_t *out_count, char *err_buf, size_t err_len);

#endif // CDSP_RAW_READER_H
