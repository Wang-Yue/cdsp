#ifndef CDSP_WAV_READER_H
#define CDSP_WAV_READER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "Wav/wav_types.h"

/**
 * @brief Parse WAV / RF64 / BW64 header from an open file stream.
 *
 * Reads chunks in any order (e.g. data before fmt), parses ds64 chunks,
 * extensible fmt chunks with GUID, and extracts format, sample rate, channel
 * count, and audio data boundaries.
 *
 * If the file is seekable, this function seeks back to data_start_offset before
 * returning.
 *
 * @param f Open file stream positioned at start of file.
 * @param info Output structure to receive parsed metadata.
 * @param err_msg Output buffer for error message if parsing fails.
 * @param err_msg_len Size of err_msg buffer.
 * @return true if header was parsed successfully, false otherwise.
 */
bool wav_read_header(FILE* f, wav_info_t* info, char* err_msg,
                     size_t err_msg_len);

/**
 * @brief Open a WAV file and read its header metadata.
 *
 * @param filename File path.
 * @param info Output structure to receive parsed metadata.
 * @param err_msg Output buffer for error message if parsing fails.
 * @param err_msg_len Size of err_msg buffer.
 * @return true if file was opened and parsed successfully, false otherwise.
 */
bool wav_read_info_from_file(const char* filename, wav_info_t* info,
                             char* err_msg, size_t err_msg_len);

/**
 * @brief Backward-compatible alias for existing file_backend callers.
 */
bool cdsp_wav_file_read_info(const char* filename, cdsp_wav_info_t* info,
                             char* err_msg, size_t err_msg_len);

/**
 * @brief Load a single channel of audio samples from a WAV file as double
 * precision values.
 *
 * Used by convolution filters to load impulse responses. Supports U8, S16,
 * S24_3, S24_4_LJ, S32, F32, and F64 sample formats.
 *
 * @param path File path.
 * @param channel Zero-based channel index to extract.
 * @param out_count Output pointer to receive number of decoded samples.
 * @param err_msg Output buffer for error message if reading fails.
 * @param err_msg_len Size of err_msg buffer.
 * @return Dynamically allocated array of double samples (caller frees with
 * free()), or NULL on failure.
 */
double* wav_read_channel_samples(const char* path, int channel,
                                 size_t* out_count, char* err_msg,
                                 size_t err_msg_len);

#endif  // CDSP_WAV_READER_H
