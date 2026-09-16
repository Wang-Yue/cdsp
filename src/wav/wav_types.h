#ifndef CDSP_WAV_TYPES_H
#define CDSP_WAV_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "config/engine_config_types.h"

#ifdef _WIN32
#define cdsp_fseek64 _fseeki64
#define cdsp_ftell64 _ftelli64
#else
#define cdsp_fseek64 fseeko
#define cdsp_ftell64 ftello
#endif

/**
 * @brief WAV / RF64 / BW64 file metadata parsed from header.
 */
typedef struct wav_info {
  uint32_t sample_rate;          /**< Sampling frequency in Hz. */
  uint16_t channels;             /**< Number of interleaved audio channels. */
  binary_sample_format_t format; /**< cdsp binary sample format. */
  uint64_t data_bytes;        /**< Size of raw audio data payload in bytes. */
  uint64_t data_start_offset; /**< Byte offset to start of audio data. */
  bool is_rf64;               /**< True if RF64 or BW64 64-bit container. */
  uint16_t audio_format;      /**< WAV format code (1 = PCM, 3 = IEEE Float). */
  uint16_t bits_per_sample; /**< Bit depth per sample (e.g. 16, 24, 32, 64). */
  uint16_t block_align;     /**< Block align (channels * container_bytes). */
  uint16_t valid_bits;      /**< Valid bits per sample (from extensible fmt). */
  size_t container_bytes;   /**< Bytes per sample per channel (e.g. 1, 2, 3, 4,
                               8). */
} wav_info_t;

/**
 * @brief Backward-compatible alias for existing file_backend callers.
 */
typedef wav_info_t cdsp_wav_info_t;

#endif // CDSP_WAV_TYPES_H
