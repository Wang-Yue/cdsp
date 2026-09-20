#ifndef CLIB_ENGINE_ENGINE_STATE_TYPES_H
#define CLIB_ENGINE_ENGINE_STATE_TYPES_H

/**
 * @file engine_state_types.h
 * @brief Engine lifecycle states, stop reasons, and state update snapshot
 * structures.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "utils/cdsp_macros.h"

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
 */
uint8_t processing_state_to_raw_byte(processing_state_t state);

/**
 * @brief Converts a raw byte back to processing state.
 */
processing_state_t processing_state_from_raw_byte(uint8_t raw_byte);

/**
 * @brief Converts processing state to string.
 */
const char *processing_state_to_string(processing_state_t state);

/**
 * @brief Parses processing state from string.
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

#endif // CLIB_ENGINE_ENGINE_STATE_TYPES_H
