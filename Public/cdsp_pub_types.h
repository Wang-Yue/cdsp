#ifndef CDSP_PUBLIC_TYPES_H
#define CDSP_PUBLIC_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Self-contained interface handle representing the DSP engine.
 */
typedef struct dsp_engine dsp_engine_t;

/**
 * @brief Engine processing state.
 */
typedef enum {
  CDSP_PROCESSING_STATE_INACTIVE = 0, /**< Engine is inactive. */
  CDSP_PROCESSING_STATE_STARTING = 1, /**< Engine is starting. */
  CDSP_PROCESSING_STATE_RUNNING = 2,  /**< Engine is running. */
  CDSP_PROCESSING_STATE_PAUSED = 3,   /**< Engine is paused. */
  CDSP_PROCESSING_STATE_STALLED =
      4 /**< Engine is stalled (waiting for data). */
} cdsp_processing_state_t;

/**
 * @brief Stop reason types.
 */
typedef enum {
  CDSP_STOP_REASON_NONE = 0,
  CDSP_STOP_REASON_DONE,
  CDSP_STOP_REASON_CAPTURE_ERROR,
  CDSP_STOP_REASON_PLAYBACK_ERROR,
  CDSP_STOP_REASON_CAPTURE_FORMAT_CHANGE,
  CDSP_STOP_REASON_PLAYBACK_FORMAT_CHANGE,
  CDSP_STOP_REASON_UNKNOWN_ERROR
} cdsp_stop_reason_type_t;

/**
 * @brief Detailed stop reason structure.
 */
typedef struct {
  cdsp_stop_reason_type_t type;
  char message[256];
  int format_change_rate;
} cdsp_stop_reason_t;

/**
 * @brief VU level snapshot for playback and capture.
 */
typedef struct {
  double* playback_rms;
  double* playback_peak;
  double* capture_rms;
  double* capture_peak;
  size_t playback_channels;
  size_t capture_channels;
} cdsp_vu_levels_t;

/**
 * @brief Structure representing detailed device error.
 */
typedef enum {
  CDSP_DEVICE_ERROR_NONE = 0,
  CDSP_DEVICE_ERROR_NOT_FOUND,
  CDSP_DEVICE_ERROR_BUSY,
  CDSP_DEVICE_ERROR_UNKNOWN
} cdsp_device_error_type_t;

typedef struct {
  cdsp_device_error_type_t type;
  char message[256];
} cdsp_device_error_t;

typedef enum {
  CDSP_BACKEND_ERR_SUCCESS = 0,
  CDSP_BACKEND_ERR_CONFIG_PARSE,
  CDSP_BACKEND_ERR_DEVICE_NOT_FOUND,
  CDSP_BACKEND_ERR_DEVICE_BUSY,
  CDSP_BACKEND_ERR_UNKNOWN
} cdsp_backend_error_type_t;

/**
 * @brief Structure representing backend error.
 */
typedef struct {
  cdsp_backend_error_type_t type;
  char message[256];
} cdsp_backend_error_t;

/**
 * @brief Sample rate capability entry.
 */
typedef struct {
  int samplerate;
  char** formats;
  size_t formats_count;
} cdsp_samplerate_capability_t;

/**
 * @brief Channel capability entry.
 */
typedef struct {
  int channels;
  cdsp_samplerate_capability_t* samplerates;
  size_t samplerates_count;
} cdsp_channel_capability_t;

/**
 * @brief Set of capabilities for a specific access mode.
 */
typedef struct {
  char mode[64];  // e.g. "Unified", "Shared", "Exclusive"
  cdsp_channel_capability_t* capabilities;
  size_t capabilities_count;
} cdsp_device_capability_set_t;

/**
 * @brief Audio device capability descriptor.
 */
typedef struct {
  char name[256];
  char description[256];
  cdsp_device_capability_set_t* capability_sets;
  size_t capability_sets_count;
} cdsp_device_descriptor_t;

/**
 * @brief Fader identifiers.
 */
typedef enum {
  CDSP_FADER_MAIN = 0,  /**< Main fader. */
  CDSP_FADER_AUX1 = 1,  /**< Auxiliary fader 1. */
  CDSP_FADER_AUX2 = 2,  /**< Auxiliary fader 2. */
  CDSP_FADER_AUX3 = 3,  /**< Auxiliary fader 3. */
  CDSP_FADER_AUX4 = 4,  /**< Auxiliary fader 4. */
  CDSP_FADER_COUNT = 5, /**< Number of faders. */
  CDSP_FADER_NONE = -1  /**< No fader. */
} cdsp_fader_t;

/**
 * @brief Public representation of captured or playback audio samples.
 */
typedef struct {
  double** channels;     /**< Array of channel buffers. */
  size_t channels_count; /**< Number of channels. */
  size_t frames;         /**< Number of frames per channel. */
} cdsp_audio_samples_t;

#ifdef __cplusplus
}
#endif

#endif  // CDSP_PUBLIC_TYPES_H
