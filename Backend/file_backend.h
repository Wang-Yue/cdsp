/**
 * @file file_backend.h
 * @brief File-based audio capture and playback backends.
 */

#ifndef CLIB_BACKEND_FILE_BACKEND_H
#define CLIB_BACKEND_FILE_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "Backend/audio_backend.h"
#include "Config/engine_config_types.h"

/**
 * @brief Opaque structure representing the file capture backend.
 */
typedef struct file_capture file_capture_t;

/**
 * @brief Opaque structure representing the file playback backend.
 */
typedef struct file_playback file_playback_t;

/**
 * @brief Global virtual method table for File capture backend.
 */
extern const capture_backend_vtable_t g_file_capture_vtable;

/**
 * @brief Global virtual method table for File playback backend.
 */
extern const playback_backend_vtable_t g_file_playback_vtable;

/**
 * @brief WAV file metadata parsed from header.
 */
typedef struct {
  uint32_t sample_rate;
  uint16_t channels;
  binary_sample_format_t format;
  uint64_t data_bytes;
  uint64_t data_start_offset;
} cdsp_wav_info_t;

/**
 * @brief Read WAV file header and extract audio format parameters.
 *
 * @param filename Path to WAV file.
 * @param info Output structure to store parsed WAV info.
 * @param err_msg Output buffer for error message if parsing fails.
 * @param err_msg_len Size of err_msg buffer.
 * @return true if successfully read, false otherwise.
 */
bool cdsp_wav_file_read_info(const char* filename, cdsp_wav_info_t* info,
                             char* err_msg, size_t err_msg_len);

#endif  // CLIB_BACKEND_FILE_BACKEND_H
