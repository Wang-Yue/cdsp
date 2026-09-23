// Audio backend protocols.
//
// `ProcessingState` and `ProcessingStopReason` — used by both the
// engine internals and the public actor — live in `Engine/DSPEngine.swift`.

#ifndef CLIB_BACKEND_AUDIO_BACKEND_H
#define CLIB_BACKEND_AUDIO_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio/sample_format.h"
#include "backend/backend_error.h"
#include "config/config_error.h"
#include "utils/lock_free_ring_buffer.h"

typedef struct audio_chunk audio_chunk_t;
typedef struct devices_config_t devices_config_t;
typedef struct capture_device_config_t capture_device_config_t;
typedef struct playback_device_config_t playback_device_config_t;

typedef enum {
  AUDIO_BACKEND_TYPE_INVALID = -1,
#if defined(ENABLE_COREAUDIO)
  AUDIO_BACKEND_TYPE_CORE_AUDIO,
#endif
#if defined(ENABLE_ALSA)
  AUDIO_BACKEND_TYPE_ALSA,
#endif
#if defined(ENABLE_PIPEWIRE)
  AUDIO_BACKEND_TYPE_PIPEWIRE,
#endif
#if defined(ENABLE_WASAPI)
  AUDIO_BACKEND_TYPE_WASAPI,
#endif
#if defined(ENABLE_ASIO)
  AUDIO_BACKEND_TYPE_ASIO,
#endif
  AUDIO_BACKEND_TYPE_FILE,
  AUDIO_BACKEND_TYPE_STDIN_OUT,
  AUDIO_BACKEND_TYPE_GENERATOR
} audio_backend_type_t;

#if defined(ENABLE_COREAUDIO)
typedef enum {
  COREAUDIO_SAMPLE_FORMAT_INVALID = -1,
  COREAUDIO_SAMPLE_FORMAT_S16,
  COREAUDIO_SAMPLE_FORMAT_S24,
  COREAUDIO_SAMPLE_FORMAT_S32,
  COREAUDIO_SAMPLE_FORMAT_F32
} coreaudio_sample_format_t;
#endif

#if defined(ENABLE_ALSA)
typedef enum {
  ALSA_SAMPLE_FORMAT_INVALID = -1,
  ALSA_SAMPLE_FORMAT_S16_LE,
  ALSA_SAMPLE_FORMAT_S24_3_LE,
  ALSA_SAMPLE_FORMAT_S24_4_LE,
  ALSA_SAMPLE_FORMAT_S32_LE,
  ALSA_SAMPLE_FORMAT_F32_LE,
  ALSA_SAMPLE_FORMAT_F64_LE,
  ALSA_SAMPLE_FORMAT_DSD_U8,
  ALSA_SAMPLE_FORMAT_DSD_U16_LE,
  ALSA_SAMPLE_FORMAT_DSD_U16_BE,
  ALSA_SAMPLE_FORMAT_DSD_U32_LE,
  ALSA_SAMPLE_FORMAT_DSD_U32_BE
} alsa_sample_format_t;
#endif

#if defined(ENABLE_WASAPI)
typedef enum {
  WASAPI_SAMPLE_FORMAT_INVALID = -1,
  WASAPI_SAMPLE_FORMAT_S16,
  WASAPI_SAMPLE_FORMAT_S24,
  WASAPI_SAMPLE_FORMAT_S32,
  WASAPI_SAMPLE_FORMAT_F32
} wasapi_sample_format_t;
#endif

#if defined(ENABLE_ASIO)
typedef enum {
  ASIO_SAMPLE_FORMAT_INVALID = -1,
  ASIO_SAMPLE_FORMAT_S16_LE,
  ASIO_SAMPLE_FORMAT_S24_3_LE,
  ASIO_SAMPLE_FORMAT_S24_4_LE,
  ASIO_SAMPLE_FORMAT_S32_LE,
  ASIO_SAMPLE_FORMAT_F32_LE,
  ASIO_SAMPLE_FORMAT_F64_LE,
  ASIO_SAMPLE_FORMAT_DSD_INT8
} asio_sample_format_t;
#endif

#include "dsd/sigma_delta_modulator.h"

/**
 * @brief Active command-line or programmatic configuration overrides.
 */
struct dsp_config_overrides_t {
  int samplerate; /**< Overridden sample rate (> 0), or -1 / 0 if none. */
  int channels;   /**< Overridden capture channels (> 0), or -1 / 0 if none. */
  binary_sample_format_t
      sample_format;      /**< Overridden capture sample format. */
  bool has_sample_format; /**< True if sample_format is set. */
  int extra_samples; /**< Overridden extra samples (>= 0), or -1 if none. */
  bool has_extra_samples; /**< True if extra_samples is set. */
};
typedef struct dsp_config_overrides_t dsp_config_overrides_t;

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

#define STANDARD_RATES_COUNT 17
extern const uint32_t STANDARD_RATES[STANDARD_RATES_COUNT];

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

// MARK: - Device Config Accessors

/**
 * @brief Gets the number of channels from a capture device configuration.
 * @param config Pointer to the configuration.
 * @return Number of channels.
 */
size_t
capture_device_config_get_channels(const capture_device_config_t *config);

/**
 * @brief Sets the number of channels in a capture device configuration.
 * @param config Pointer to the configuration.
 * @param channels Number of channels.
 */
void capture_device_config_set_channels(capture_device_config_t *config,
                                        size_t channels);

/**
 * @brief Gets the number of channels from a playback device configuration.
 * @param config Pointer to the configuration.
 * @return Number of channels.
 */
size_t
playback_device_config_get_channels(const playback_device_config_t *config);

/**
 * @brief Gets the device name from a capture device configuration.
 * @param config Pointer to the configuration.
 * @return Device name string, or NULL if not applicable/specified.
 */
const char *
capture_device_config_get_device(const capture_device_config_t *config);

/**
 * @brief Gets the device name from a playback device configuration.
 * @param config Pointer to the configuration.
 * @return Device name string, or NULL.
 */
const char *
playback_device_config_get_device(const playback_device_config_t *config);

/**
 * @brief Gets universal binary sample format from a capture device
 * configuration.
 * @param config Pointer to the configuration.
 * @return Universal binary sample format.
 */
binary_sample_format_t
capture_device_config_get_binary_format(const capture_device_config_t *config);

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
 * @brief Gets CoreAudio sample format from a capture device configuration.
 * @param config Pointer to the configuration.
 * @return CoreAudio sample format.
 */
coreaudio_sample_format_t
capture_device_config_get_format(const capture_device_config_t *config);

/**
 * @brief Gets CoreAudio sample format from a playback device configuration.
 * @param config Pointer to the configuration.
 * @return CoreAudio sample format.
 */
coreaudio_sample_format_t
playback_device_config_get_format(const playback_device_config_t *config);
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
 * @brief Calculates the DSD carrier bits per container frame for a playback
 * device configuration.
 * @param config Pointer to playback_device_config_t structure.
 * @return The carrier bits (8, 16, or 32).
 */
size_t playback_device_config_calculate_carrier_bits(
    const playback_device_config_t *config);

/**
 * @brief Gets DSD mode (PCM, DoP, or Native DSD) for a capture device
 * configuration.
 * @param config Pointer to the configuration.
 * @return DSD mode.
 */
dsd_mode_t
capture_device_config_get_dsd_mode(const capture_device_config_t *config);

/**
 * @brief Gets the DSD mode (Native, DoP, or PCM) for a playback device
 * configuration based on format and output_dop.
 * @param config Pointer to the configuration.
 * @return DSD mode (DSD_MODE_NATIVE, DSD_MODE_DOP, or DSD_MODE_PCM).
 */
dsd_mode_t
playback_device_config_get_dsd_mode(const playback_device_config_t *config);

/**
 * @brief Gets exclusive mode setting from a playback device configuration.
 * @param config Pointer to the configuration.
 * @return True if exclusive mode is enabled.
 */
bool playback_device_config_get_exclusive(
    const playback_device_config_t *config);

/**
 * @brief Gets DSD encoder filter from a playback device configuration.
 * @param config Pointer to the configuration.
 * @return SDM filter type.
 */
sdm_filter_t playback_device_config_get_dsd_encoder_filter(
    const playback_device_config_t *config);

/**
 * @file audio_backend.h
 * @brief Interfaces and wrappers for audio capture and playback backends.
 *
 * Defines the Virtual Method Tables (vtables) and wrappers for the
 * capture and playback backends used by the CamillaDSP-Monitor engine.
 */

/**
 * @brief Validates backend-specific device configurations.
 *
 * Checks device constraints for active capture and playback backends (e.g.
 * File backend access/formats, WASAPI format/loopback rules, ASIO full-duplex
 * resampling, CoreAudio loopback matching, and target buffering limits).
 *
 * @param devices Pointer to devices configuration.
 * @param[out] err Configuration error sink.
 * @return 0 on success, -1 on validation failure.
 */
int audio_backend_validate_devices(const devices_config_t *devices,
                                   config_error_t *err);

/**
 * @brief Applies command-line and WAV file overrides to the device
 * configuration.
 *
 * Updates sample rates, chunk sizes, channel counts, format conversions, and
 * probes input WAV headers when applicable.
 *
 * @param devices Pointer to devices configuration to update.
 * @param overrides_in Pointer to override parameters (may be NULL for
 * defaults).
 * @param[out] err Configuration error sink.
 * @return 0 on success, -1 on error.
 */
int audio_backend_apply_device_overrides(
    devices_config_t *devices, const dsp_config_overrides_t *overrides_in,
    config_error_t *err);

typedef struct capture_backend capture_backend_t;
typedef struct playback_backend playback_backend_t;
typedef struct processing_parameters processing_parameters_t;

/**
 * @struct capture_backend_vtable
 * @brief Virtual table containing function pointers for audio capture
 * operations.
 */
typedef struct {
  /**
   * @brief Create a capture backend instance.
   * @param config Capture device configuration.
   * @param sample_rate Nominal sample rate in Hz.
   * @param chunk_size Buffer chunk size in frames.
   * @param full_duplex True if running in full duplex mode.
   * @param params Processing parameters.
   * @param[out] err Pointer to store error details on failure.
   * @return Allocated capture_backend_t interface pointer, or NULL on error.
   */
  capture_backend_t *(*create)(const capture_device_config_t *config,
                               int sample_rate, int chunk_size,
                               bool full_duplex,
                               processing_parameters_t *params,
                               backend_error_t *err);

  /**
   * @brief Open the capture device.
   * @param ctx Pointer to the backend instance context.
   * @param[out] err Pointer to store error details on failure.
   * @return true on success, false on failure.
   */
  bool (*open)(void *ctx, backend_error_t *err);

  /**
   * @brief Read a chunk of audio into the provided buffer.
   * @param ctx Pointer to the backend instance context.
   * @param frames Number of frames to read.
   * @param[out] chunk Pointer to store the read audio chunk.
   * @param[out] err Pointer to store error details on failure.
   * @return true on success, false on end-of-stream or error.
   */
  bool (*read)(void *ctx, size_t frames, audio_chunk_t *chunk,
               backend_error_t *err);

  /**
   * @brief Close the capture device.
   * @param ctx Pointer to the backend instance context.
   */
  void (*close)(void *ctx);

  /**
   * @brief Check for pending sample rate changes on the capture device.
   *
   * Polled by the engine each chunk to detect if a format change occurred.
   *
   * @param ctx Pointer to the backend instance context.
   * @param[out] out_rate Pointer to store the new sample rate if a change is
   * pending.
   * @return true if a change was detected, false otherwise.
   */
  bool (*get_pending_rate_change)(void *ctx, double *out_rate);

  /**
   * @brief Check if the capture device exposes a tunable clock.
   *
   * Used for rate-adjust loops sending pitch corrections instead of resampling
   * ratio nudges.
   *
   * @param ctx Pointer to the backend instance context.
   * @return true if pitch control is supported, false otherwise.
   */
  bool (*is_pitch_control_supported)(void *ctx);

  /**
   * @brief Apply a clock-pitch correction to the capture device.
   * @param ctx Pointer to the backend instance context.
   * @param multiplier The clock rate multiplier (typically close to 1.0).
   */
  void (*set_pitch)(void *ctx, double multiplier);

  /**
   * @brief Wait for new samples to become available.
   * @param ctx Pointer to the backend instance context.
   * @param timeout_ms Maximum time to wait in milliseconds.
   * @return true if data is available, false on timeout or error.
   */
  bool (*wait_for_data)(void *ctx, uint32_t timeout_ms);

  /**
   * @brief Notify the capture backend of the paused state of the processing
   * loop.
   * @param ctx Pointer to the backend instance context.
   * @param paused true if the loop is paused, false otherwise.
   */
  void (*set_is_paused)(void *ctx, bool paused);

  /**
   * @brief Stop the capture device immediately.
   * @param ctx Pointer to the backend instance context.
   */
  void (*stop)(void *ctx);

  /**
   * @brief Destroy the capture backend context.
   * @param ctx Pointer to the backend instance context to free.
   */
  void (*destroy)(void *ctx);
} capture_backend_vtable_t;

/**
 * @struct capture_backend
 * @brief Wrapper structure holding the capture backend context and its vtable.
 */
struct capture_backend {
  void *ctx;                              /**< Private context pointer */
  const capture_backend_vtable_t *vtable; /**< Virtual method table */
  bool is_realtime; /**< True if the backend operates in real-time */
};

/**
 * @struct playback_backend_vtable
 * @brief Virtual table containing function pointers for audio playback
 * operations.
 */
typedef struct {
  /**
   * @brief Create a playback backend instance.
   * @param config Playback device configuration.
   * @param sample_rate Nominal sample rate in Hz.
   * @param chunk_size Buffer chunk size in frames.
   * @param full_duplex True if running in full duplex mode.
   * @param params Processing parameters.
   * @param[out] err Pointer to store error details on failure.
   * @return Allocated playback_backend_t interface pointer, or NULL on error.
   */
  playback_backend_t *(*create)(const playback_device_config_t *config,
                                int sample_rate, int chunk_size,
                                bool full_duplex,
                                processing_parameters_t *params,
                                backend_error_t *err);

  /**
   * @brief Open the playback device.
   * @param ctx Pointer to the backend instance context.
   * @param[out] err Pointer to store error details on failure.
   * @return true on success, false on failure.
   */
  bool (*open)(void *ctx, backend_error_t *err);

  /**
   * @brief Write a chunk of audio to the playback device.
   * @param ctx Pointer to the backend instance context.
   * @param chunk Pointer to the audio chunk to write.
   * @param[out] err Pointer to store error details on failure.
   * @return true on success, false on failure.
   */
  bool (*write)(void *ctx, const audio_chunk_t *chunk, backend_error_t *err);

  /**
   * @brief Close the playback device.
   * @param ctx Pointer to the backend instance context.
   */
  void (*close)(void *ctx);

  /**
   * @brief Get the current playback buffer level in samples.
   * @param ctx Pointer to the backend instance context.
   * @return Buffer level in frames.
   */
  size_t (*get_buffer_level)(void *ctx);

  /**
   * @brief Check for pending sample rate changes on the playback device.
   * @param ctx Pointer to the backend instance context.
   * @param[out] out_rate Pointer to store the new sample rate if a change is
   * pending.
   * @return true if a change was detected, false otherwise.
   */
  bool (*get_pending_rate_change)(void *ctx, double *out_rate);

  /**
   * @brief Prefill the playback buffer with silence.
   *
   * Writes the specified number of silence frames before processing starts
   * to align the rate-adjust controller level.
   *
   * @param ctx Pointer to the backend instance context.
   * @param frames Number of silence frames to write.
   * @param[out] err Pointer to store error details on failure.
   * @return true on success, false on failure.
   */
  bool (*prefill_silence)(void *ctx, size_t frames, backend_error_t *err);

  /**
   * @brief Check if playback is paused.
   * @param ctx Pointer to the backend instance context.
   * @return true if paused, false otherwise.
   */
  bool (*get_is_paused)(void *ctx);

  /**
   * @brief Set the playback paused state.
   * @param ctx Pointer to the backend instance context.
   * @param paused true to pause, false to resume.
   */
  void (*set_is_paused)(void *ctx, bool paused);

  /**
   * @brief Check if the playback device supports pitch control.
   * @param ctx Pointer to the backend instance context.
   * @return true if supported, false otherwise.
   */
  bool (*pitch_control_supported)(void *ctx);

  /**
   * @brief Set pitch multiplier.
   * @param ctx Pointer to the backend instance context.
   * @param multiplier The clock multiplier.
   */
  void (*set_pitch)(void *ctx, double multiplier);

  /**
   * @brief Drain remaining audio in the playback device upon end-of-stream.
   * @param ctx Pointer to the backend instance context.
   */
  void (*drain)(void *ctx);

  /**
   * @brief Stop the playback device immediately.
   * @param ctx Pointer to the backend instance context.
   */
  void (*stop)(void *ctx);

  /**
   * @brief Destroy the playback backend context.
   * @param ctx Pointer to the backend instance context to free.
   */
  void (*destroy)(void *ctx);
} playback_backend_vtable_t;

/**
 * @struct playback_backend
 * @brief Wrapper structure holding the playback backend context and its vtable.
 */
struct playback_backend {
  void *ctx;                               /**< Private context pointer */
  const playback_backend_vtable_t *vtable; /**< Virtual method table */
};

// Factory functions

/**
 * @brief Create a capture backend instance based on the configuration.
 *
 * @param config Capture device configuration.
 * @param sample_rate Nominal sample rate in Hz.
 * @param chunk_size Buffer chunk size in frames.
 * @param full_duplex True if the engine is running in full duplex mode.
 * @param params Processing parameters.
 * @param[out] err Pointer to store error details if creation fails.
 * @return A pointer to the created capture_backend_t interface wrapper, or NULL
 * on error.
 */
capture_backend_t *create_capture_backend(const capture_device_config_t *config,
                                          int sample_rate, int chunk_size,
                                          bool full_duplex,
                                          processing_parameters_t *params,
                                          backend_error_t *err);

/**
 * @brief Create a playback backend instance based on the configuration.
 *
 * @param config Playback device configuration.
 * @param sample_rate Nominal sample rate in Hz.
 * @param chunk_size Buffer chunk size in frames.
 * @param full_duplex True if the engine is running in full duplex mode.
 * @param params Processing parameters.
 * @param[out] err Pointer to store error details if creation fails.
 * @return A pointer to the created playback_backend_t interface wrapper, or
 * NULL on error.
 */
playback_backend_t *
create_playback_backend(const playback_device_config_t *config, int sample_rate,
                        int chunk_size, bool full_duplex,
                        processing_parameters_t *params, backend_error_t *err);

// CaptureBackend wrapper methods

/**
 * @brief Open the capture device via wrapper.
 * @param backend Pointer to the capture backend.
 * @param[out] err Pointer to store error details on failure.
 * @return true on success, false on failure.
 */
bool capture_backend_open(capture_backend_t *backend, backend_error_t *err);

/**
 * @brief Read from the capture device via wrapper.
 * @param backend Pointer to the capture backend.
 * @param frames Number of frames to read.
 * @param[out] chunk Output audio chunk buffer.
 * @param[out] err Pointer to store error details on failure.
 * @return true on success, false on failure.
 */
bool capture_backend_read(capture_backend_t *backend, size_t frames,
                          audio_chunk_t *chunk, backend_error_t *err);

/**
 * @brief Close the capture device via wrapper.
 * @param backend Pointer to the capture backend.
 */
void capture_backend_close(capture_backend_t *backend);

/**
 * @brief Get pending rate changes from the capture device via wrapper.
 * @param backend Pointer to the capture backend.
 * @param[out] out_rate Pointer to store the new rate.
 * @return true if a rate change was detected.
 */
bool capture_backend_get_pending_rate_change(capture_backend_t *backend,
                                             double *out_rate);

/**
 * @brief Check if the capture device supports pitch control via wrapper.
 * @param backend Pointer to the capture backend.
 * @return true if supported.
 */
bool capture_backend_pitch_control_supported(capture_backend_t *backend);

/**
 * @brief Set the clock pitch correction for the capture device via wrapper.
 * @param backend Pointer to the capture backend.
 * @param multiplier The pitch multiplier.
 */
void capture_backend_set_pitch(capture_backend_t *backend, double multiplier);

/**
 * @brief Wait for data to be available on the capture device via wrapper.
 * @param backend Pointer to the capture backend.
 * @param timeout_ms Timeout in milliseconds.
 * @return true if data is available, false on timeout.
 */
bool capture_backend_wait(capture_backend_t *backend, uint32_t timeout_ms);

/**
 * @brief Notify the capture backend of paused state via wrapper.
 * @param backend Pointer to the capture backend.
 * @param paused true if paused.
 */
void capture_backend_set_is_paused(capture_backend_t *backend, bool paused);

/**
 * @brief Stop the capture device via wrapper.
 * @param backend Pointer to the capture backend.
 */
void capture_backend_stop(capture_backend_t *backend);

/**
 * @brief Free the capture backend and its context.
 * @param backend Pointer to the capture backend to free.
 */
void capture_backend_free(capture_backend_t *backend);

/**
 * @brief Check if the capture backend operates in real-time.
 * @param backend Pointer to the capture backend.
 * @return true if real-time, false otherwise.
 */
bool capture_backend_is_realtime(const capture_backend_t *backend);

// PlaybackBackend wrapper methods

/**
 * @brief Open the playback device via wrapper.
 * @param backend Pointer to the playback backend.
 * @param[out] err Pointer to store error details on failure.
 * @return true on success, false on failure.
 */
bool playback_backend_open(playback_backend_t *backend, backend_error_t *err);

/**
 * @brief Write to the playback device via wrapper.
 * @param backend Pointer to the playback backend.
 * @param chunk Audio chunk to write.
 * @param[out] err Pointer to store error details on failure.
 * @return true on success, false on failure.
 */
bool playback_backend_write(playback_backend_t *backend,
                            const audio_chunk_t *chunk, backend_error_t *err);

/**
 * @brief Close the playback device via wrapper.
 * @param backend Pointer to the playback backend.
 */
void playback_backend_close(playback_backend_t *backend);

/**
 * @brief Get current playback buffer level via wrapper.
 * @param backend Pointer to the playback backend.
 * @return Buffer level in frames.
 */
size_t playback_backend_get_buffer_level(playback_backend_t *backend);

/**
 * @brief Get pending rate changes from the playback device via wrapper.
 * @param backend Pointer to the playback backend.
 * @param[out] out_rate Pointer to store the new rate.
 * @return true if a rate change was detected.
 */
bool playback_backend_get_pending_rate_change(playback_backend_t *backend,
                                              double *out_rate);

/**
 * @brief Prefill silence to the playback device via wrapper.
 * @param backend Pointer to the playback backend.
 * @param frames Number of silence frames to write.
 * @param[out] err Pointer to store error details on failure.
 * @return true on success, false on failure.
 */
bool playback_backend_prefill_silence(playback_backend_t *backend,
                                      size_t frames, backend_error_t *err);

/**
 * @brief Check if paused status is set for the playback device via wrapper.
 * @param backend Pointer to the playback backend.
 * @return true if paused, false otherwise.
 */
bool playback_backend_get_is_paused(playback_backend_t *backend);

/**
 * @brief Set paused status for the playback device via wrapper.
 * @param backend Pointer to the playback backend.
 * @param paused true to pause, false to resume.
 */
void playback_backend_set_is_paused(playback_backend_t *backend, bool paused);

/**
 * @brief Check if the playback device supports pitch control via wrapper.
 * @param backend Pointer to the playback backend.
 * @return true if supported.
 */
bool playback_backend_pitch_control_supported(playback_backend_t *backend);

/**
 * @brief Set the clock pitch correction for the playback device via wrapper.
 * @param backend Pointer to the playback backend.
 * @param multiplier The pitch multiplier.
 */
void playback_backend_set_pitch(playback_backend_t *backend, double multiplier);

/**
 * @brief Notify the playback backend to drain remaining audio upon
 * end-of-stream.
 * @param backend Pointer to the playback backend.
 */
void playback_backend_drain(playback_backend_t *backend);

/**
 * @brief Stop the playback device via wrapper.
 * @param backend Pointer to the playback backend.
 */
void playback_backend_stop(playback_backend_t *backend);

/**
 * @brief Free the playback backend and its context.
 * @param backend Pointer to the playback backend to free.
 */
void playback_backend_free(playback_backend_t *backend);

#endif // CLIB_BACKEND_AUDIO_BACKEND_H
