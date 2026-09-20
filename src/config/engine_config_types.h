/**
 * @file engine_config_types.h
 * @brief Standalone Engine Configuration and API Types.
 */

#ifndef CLIB_CONFIG_ENGINE_CONFIG_TYPES_H
#define CLIB_CONFIG_ENGINE_CONFIG_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config/config_error.h"
#include "config/config_gen.h"

/**
 * @brief Engine processing state.
 */
typedef enum {
  PROCESSING_STATE_INACTIVE = 0, /**< Engine is inactive. */
  PROCESSING_STATE_STARTING = 1, /**< Engine is starting. */
  PROCESSING_STATE_RUNNING = 2,  /**< Engine is running. */
  PROCESSING_STATE_PAUSED = 3,   /**< Engine is paused. */
  PROCESSING_STATE_STALLED =
      4 /**< Engine is stalled (e.g., waiting for data). */
} processing_state_t;

/**
 * @brief Converts processing state to a raw byte for transmission/storage.
 * @param state The processing state.
 * @return Raw byte representation.
 */
uint8_t processing_state_to_raw_byte(processing_state_t state);

/**
 * @brief Converts a raw byte back to processing state.
 * @param raw_byte The raw byte.
 * @return The processing state.
 */
processing_state_t processing_state_from_raw_byte(uint8_t raw_byte);

/**
 * @brief Converts processing state to string.
 * @param state The processing state.
 * @return String representation.
 */
const char *processing_state_to_string(processing_state_t state);

/**
 * @brief Parses processing state from string.
 * @param str The string representation.
 * @return The processing state.
 */
processing_state_t processing_state_from_string(const char *str);

/**
 * @brief Reason why the engine stopped.
 */
typedef enum {
  STOP_REASON_NONE = 0,               /**< Not stopped. */
  STOP_REASON_DONE,                   /**< Finished processing (e.g., EOF). */
  STOP_REASON_CAPTURE_ERROR,          /**< Error in capture device. */
  STOP_REASON_PLAYBACK_ERROR,         /**< Error in playback device. */
  STOP_REASON_CAPTURE_FORMAT_CHANGE,  /**< Capture format changed. */
  STOP_REASON_PLAYBACK_FORMAT_CHANGE, /**< Playback format changed. */
  STOP_REASON_UNKNOWN_ERROR           /**< Unknown error. */
} processing_stop_reason_type_t;

/**
 * @brief Structure containing detailed stop reason.
 */
typedef struct {
  processing_stop_reason_type_t type; /**< Type of stop reason. */
  char message[256];                  /**< Detailed error message. */
  int format_change_rate;             /**< New sample rate if format changed. */
} processing_stop_reason_t;

/**
 * @brief State update structure.
 */
typedef struct {
  processing_state_t state; /**< Current processing state. */
  processing_stop_reason_t
      stop_reason; /**< Stop reason (if inactive/stopped). */
} state_update_t;

/**
 * @brief Representation of an audio device.
 */
typedef struct {
  char name[256]; /**< Device name. */
} audio_device_t;

/**
 * @brief Audio backend error types.
 */
typedef enum {
  AUDIO_BACKEND_ERR_CONFIG_PARSE = 0, /**< Configuration parsing error. */
  AUDIO_BACKEND_ERR_COMMAND_SEND,     /**< Error sending command to backend. */
  AUDIO_BACKEND_ERR_INVALID_SAMPLERATE, /**< Invalid sample rate. */
  AUDIO_BACKEND_ERR_SPECTRUM_COMPUTE,   /**< Error computing spectrum. */
  AUDIO_BACKEND_ERR_ENGINE_NOT_RUNNING, /**< Engine is not running. */
  AUDIO_BACKEND_ERR_BUFFER_EMPTY,       /**< Buffer is empty. */
  AUDIO_BACKEND_ERR_DEVICE_NOT_FOUND,   /**< Audio device not found. */
  AUDIO_BACKEND_ERR_DEVICE_BUSY,        /**< Audio device is busy. */
  AUDIO_BACKEND_ERR_CONFIG_READ         /**< Configuration read/syntax error. */
} audio_backend_error_type_t;

/**
 * @brief Audio backend error structure.
 */
typedef struct {
  audio_backend_error_type_t type; /**< Error type. */
  char message[256];               /**< Error message. */
} audio_backend_error_t;

/**
 * @brief Gets description of audio backend error.
 * @param err The error.
 * @param out_buf Output buffer.
 * @param buf_len Output buffer length.
 */
void audio_backend_error_description(const audio_backend_error_t *err,
                                     char *out_buf, size_t buf_len);

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

// MARK: - Capability data model

/**
 * @brief Sample rate capabilities.
 */
typedef struct {
  int samplerate;       /**< Supported sample rate. */
  char **formats;       /**< Supported formats at this sample rate. */
  size_t formats_count; /**< Number of formats. */
} samplerate_capability_t;

/**
 * @brief Channel capabilities.
 */
typedef struct {
  size_t channels; /**< Supported number of channels. */
  samplerate_capability_t
      *samplerates; /**< Supported sample rates for this channel count. */
  size_t samplerates_count; /**< Number of sample rates. */
} channel_capability_t;

/**
 * @brief Device capability set.
 */
typedef struct {
  char mode[64]; /**< Access mode (e.g. "Unified", "Shared", "Exclusive"). */
  channel_capability_t *capabilities; /**< Array of channel capabilities. */
  size_t capabilities_count;          /**< Number of channel capabilities. */
} device_capability_set_t;

/**
 * @brief Detailed description of an audio device and its capabilities.
 */
typedef struct {
  char name[256];                           /**< Device name. */
  device_capability_set_t *capability_sets; /**< Array of capability sets. */
  size_t capability_sets_count;             /**< Number of capability sets. */
} audio_device_descriptor_t;

/**
 * @brief Curated list of standard audio sample rates matching CamillaDSP
 * STANDARD_RATES (src/lib.rs).
 */
#define STANDARD_RATES_COUNT 17
extern const uint32_t STANDARD_RATES[STANDARD_RATES_COUNT];

// MARK: - Device Config Models

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
 * @return String representation.
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
 * @return String representation (e.g. "S16_LE", "S24_3_LE", "S32_LE", "F32_LE",
 * "F64_LE").
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

#if defined(ENABLE_COREAUDIO)
/**
 * @brief Maps CoreAudio sample format to universal binary format.
 */
binary_sample_format_t
coreaudio_sample_format_to_binary_format(coreaudio_sample_format_t fmt);

/**
 * @brief Maps universal binary format to CoreAudio sample format.
 */
coreaudio_sample_format_t
coreaudio_sample_format_from_binary_format(binary_sample_format_t fmt);
#endif

#if defined(ENABLE_ALSA)
/**
 * @brief Maps ALSA sample format enum to universal binary format.
 */
binary_sample_format_t
alsa_sample_format_to_binary_format(alsa_sample_format_t fmt);

/**
 * @brief Maps universal binary format to ALSA sample format enum.
 */
alsa_sample_format_t
alsa_sample_format_from_binary_format(binary_sample_format_t fmt);
#endif

#if defined(ENABLE_WASAPI)
/**
 * @brief Maps WASAPI sample format enum to universal binary format.
 */
binary_sample_format_t
wasapi_sample_format_to_binary_format(wasapi_sample_format_t fmt);

/**
 * @brief Maps universal binary format to WASAPI sample format enum.
 */
wasapi_sample_format_t
wasapi_sample_format_from_binary_format(binary_sample_format_t fmt);
#endif

#if defined(ENABLE_ASIO)
/**
 * @brief Maps ASIO sample format enum to universal binary format.
 */
binary_sample_format_t
asio_sample_format_to_binary_format(asio_sample_format_t fmt, bool is_lsb);

/**
 * @brief Maps universal binary format to ASIO sample format enum.
 */
asio_sample_format_t
asio_sample_format_from_binary_format(binary_sample_format_t fmt);
#endif

/**
 * @brief Gets the number of channels from a capture device configuration.
 * @param config Pointer to the configuration.
 * @return Number of channels.
 */
size_t
capture_device_config_get_channels(const capture_device_config_t *config);

/**
 * @brief Gets the device name from a capture device configuration.
 * @param config Pointer to the configuration.
 * @return Device name string, or NULL if not applicable/specified.
 */
const char *
capture_device_config_get_device(const capture_device_config_t *config);

/**
 * @brief Gets universal binary sample format from a capture device
 * configuration.
 * @param config Pointer to the configuration.
 * @return Universal binary sample format.
 */
binary_sample_format_t
capture_device_config_get_binary_format(const capture_device_config_t *config);

#if defined(ENABLE_COREAUDIO)
/**
 * @brief Gets CoreAudio sample format from a capture device configuration.
 * @param config Pointer to the configuration.
 * @return CoreAudio sample format.
 */
coreaudio_sample_format_t
capture_device_config_get_format(const capture_device_config_t *config);
#endif

/**
 * @brief Gets bypass DoP setting from a capture device configuration.
 * @param config Pointer to the configuration.
 * @return True if DoP is bypassed.
 */
bool capture_device_config_get_bypass_dop(
    const capture_device_config_t *config);

/**
 * @brief Gets DoP cutoff frequency from a capture device configuration.
 * @param config Pointer to the configuration.
 * @return Cutoff frequency in Hz.
 */
double
capture_device_config_get_dop_cutoff_hz(const capture_device_config_t *config);

/**
 * @brief Calculates carrier bit depth (8, 16, or 32) for a capture device
 * configuration.
 * @param config Pointer to the configuration.
 * @return Carrier bit depth.
 */
size_t capture_device_config_calculate_carrier_bits(
    const capture_device_config_t *config);

/**
 * @brief Gets DSD mode (PCM, DoP, or Native DSD) for a capture device
 * configuration.
 * @param config Pointer to the configuration.
 * @return DSD mode.
 */
dsd_mode_t
capture_device_config_get_dsd_mode(const capture_device_config_t *config);

/**
 * @brief Gets the number of channels from a playback device configuration.
 * @param config Pointer to the configuration.
 * @return Number of channels.
 */
size_t
playback_device_config_get_channels(const playback_device_config_t *config);

/**
 * @brief Gets the device name from a playback device configuration.
 * @param config Pointer to the configuration.
 * @return Device name string, or NULL.
 */
const char *
playback_device_config_get_device(const playback_device_config_t *config);

/**
 * @brief Gets universal binary sample format from a playback device
 * configuration.
 * @param config Pointer to the configuration.
 * @return Universal binary sample format.
 */
binary_sample_format_t playback_device_config_get_binary_format(
    const playback_device_config_t *config);

#if defined(ENABLE_COREAUDIO)
/**
 * @brief Gets CoreAudio sample format from a playback device configuration.
 * @param config Pointer to the configuration.
 * @return CoreAudio sample format.
 */
coreaudio_sample_format_t
playback_device_config_get_format(const playback_device_config_t *config);
#endif

/**
 * @brief Gets exclusive mode setting from a playback device configuration.
 * @param config Pointer to the configuration.
 * @return True if exclusive mode is enabled.
 */
bool playback_device_config_get_exclusive(
    const playback_device_config_t *config);

/**
 * @brief Calculates the DSD carrier bits per container frame for a playback
 * device configuration.
 * @param config Pointer to playback_device_config_t structure.
 * @return The carrier bits (8, 16, or 32).
 */
size_t playback_device_config_calculate_carrier_bits(
    const playback_device_config_t *config);

/**
 * @brief Gets the DSD mode (Native, DoP, or PCM) for a playback device
 * configuration based on format and output_dop.
 * @param config Pointer to the configuration.
 * @return DSD mode (DSD_MODE_NATIVE, DSD_MODE_DOP, or DSD_MODE_PCM).
 */
dsd_mode_t
playback_device_config_get_dsd_mode(const playback_device_config_t *config);

/**
 * @brief Gets DSD encoder filter from a playback device configuration.
 * @param config Pointer to the configuration.
 * @return SDM filter type.
 */
sdm_filter_t playback_device_config_get_dsd_encoder_filter(
    const playback_device_config_t *config);

/**
 * @brief Sets the number of channels in a capture device configuration.
 * @param config Pointer to the configuration.
 * @param channels Number of channels.
 */
void capture_device_config_set_channels(capture_device_config_t *config,
                                        size_t channels);

/**
 * @brief Recursively frees the memory allocated for an audio device descriptor.
 *
 * This function deallocates the top-level descriptor struct as well as all
 * dynamically allocated capability sets, channels, sample rates, and format
 * strings nested within it. Safe to call with NULL.
 *
 * @param desc Pointer to the descriptor to free.
 */
void free_audio_device_descriptor(audio_device_descriptor_t *desc);

/**
 * @brief Active command-line or programmatic configuration overrides.
 */
typedef struct {
  int samplerate; /**< Overridden sample rate (> 0), or -1 / 0 if none. */
  int channels;   /**< Overridden capture channels (> 0), or -1 / 0 if none. */
  binary_sample_format_t
      sample_format;      /**< Overridden capture sample format. */
  bool has_sample_format; /**< True if sample_format is set. */
  int extra_samples; /**< Overridden extra samples (>= 0), or -1 if none. */
  bool has_extra_samples; /**< True if extra_samples is set. */
} dsp_config_overrides_t;

#endif // CLIB_CONFIG_ENGINE_CONFIG_TYPES_H
