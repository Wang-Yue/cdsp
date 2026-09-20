#ifndef CLIB_AUDIO_SAMPLE_FORMAT_H
#define CLIB_AUDIO_SAMPLE_FORMAT_H

/**
 * @file sample_format.h
 * @brief Universal audio sample formats, DSD output modes, and telemetry
 * structs.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "utils/cdsp_macros.h"

/**
 * @brief Binary audio sample format types.
 */
typedef enum {
  BINARY_SAMPLE_FORMAT_INVALID = -1,
  BINARY_SAMPLE_FORMAT_S16_LE,
  BINARY_SAMPLE_FORMAT_S24_3_LE,
  BINARY_SAMPLE_FORMAT_S24_4_RJ_LE,
  BINARY_SAMPLE_FORMAT_S24_4_LJ_LE,
  BINARY_SAMPLE_FORMAT_S32_LE,
  BINARY_SAMPLE_FORMAT_F32_LE,
  BINARY_SAMPLE_FORMAT_F64_LE,
  BINARY_SAMPLE_FORMAT_DSD_U8,
  BINARY_SAMPLE_FORMAT_DSD_U16_LE,
  BINARY_SAMPLE_FORMAT_DSD_U16_BE,
  BINARY_SAMPLE_FORMAT_DSD_U32_LE,
  BINARY_SAMPLE_FORMAT_DSD_U32_BE,
  BINARY_SAMPLE_FORMAT_DSD_U32_REVERSED
} binary_sample_format_t;

const char *binary_sample_format_to_string(binary_sample_format_t val);
binary_sample_format_t binary_sample_format_from_string(const char *str);

/**
 * @brief DSD output processing modes.
 */
typedef enum {
  DSD_MODE_PCM = 0,   /**< PCM output mode (disabled / passthrough). */
  DSD_MODE_DOP = 1,   /**< DoP output mode (DSD over PCM with markers). */
  DSD_MODE_NATIVE = 2 /**< Native DSD output mode (raw stream). */
} dsd_mode_t;

/**
 * @brief Converts DSD mode to string.
 * @param mode The DSD mode.
 * @return String representation ("pcm", "dop", "dsd").
 */
const char *dsd_mode_to_string(dsd_mode_t mode);

/**
 * @brief Parses DSD mode from string.
 * @param str The string representation.
 * @return The DSD mode.
 */
dsd_mode_t dsd_mode_from_string(const char *str);

/**
 * @brief Converts file sample format to string.
 * @param fmt The format.
 * @return String representation.
 */
const char *file_sample_format_to_string(binary_sample_format_t fmt);

/**
 * @brief Parses file sample format from string.
 * @param str The string representation.
 * @return The format, or BINARY_SAMPLE_FORMAT_INVALID if unsupported.
 */
binary_sample_format_t file_sample_format_from_string(const char *str);

/**
 * @brief Returns the byte width per sample for a binary sample format.
 * @param fmt The binary sample format.
 * @return Size in bytes per single sample channel.
 */
size_t sample_format_bytes_per_sample(binary_sample_format_t fmt);

/**
 * @brief Checks if a binary sample format is a native DSD format.
 * @param fmt The binary sample format.
 * @return True if DSD, false otherwise.
 */
bool sample_format_is_dsd(binary_sample_format_t fmt);

/**
 * @brief Checks if a binary sample format is a floating-point format (F32/F64).
 * @param fmt The binary sample format.
 * @return True if float, false otherwise.
 */
bool sample_format_is_float(binary_sample_format_t fmt);

/**
 * @brief VU levels for playback and capture.
 */
typedef struct {
  float *playback_rms;  /**< Caller-allocated array for playback RMS levels. */
  float *playback_peak; /**< Caller-allocated array for playback peak levels. */
  float *capture_rms;   /**< Caller-allocated array for capture RMS levels. */
  float *capture_peak;  /**< Caller-allocated array for capture peak levels. */
  size_t playback_channels; /**< Total playback channels populated. */
  size_t capture_channels;  /**< Total capture channels populated. */
} vu_levels_t;

/**
 * @brief Frequency spectrum data.
 */
typedef struct {
  float *frequencies;      /**< Caller-allocated array of frequencies. */
  float *magnitudes;       /**< Caller-allocated array of magnitudes. */
  size_t count;            /**< Number of bins computed. */
  char error_message[128]; /**< Error message on failure. */
} spectrum_t;

/**
 * @brief Audio samples buffer.
 */
typedef struct {
  float **channels;      /**< Caller-allocated array of channel pointers. */
  size_t channels_count; /**< Number of channels populated. */
  size_t frames;         /**< Number of frames written per channel. */
} audio_samples_t;

#endif // CLIB_AUDIO_SAMPLE_FORMAT_H
