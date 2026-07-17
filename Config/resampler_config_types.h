#ifndef CLIB_CONFIG_RESAMPLER_CONFIG_TYPES_H
#define CLIB_CONFIG_RESAMPLER_CONFIG_TYPES_H

/**
 * @file resampler_config_types.h
 * @brief Configuration types and functions for CamillaDSP resamplers.
 *
 * This file defines the structures, enums, and functions used to configure
 * resamplers in CamillaDSP, supporting synchronous and asynchronous resamplers,
 * and Apple's AudioConverter (if enabled).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Supported resampler types.
 */
typedef enum {
  RESAMPLER_TYPE_SYNCHRONOUS = 0, /**< Synchronous resampler. */
  RESAMPLER_TYPE_ASYNC_SINC,      /**< Asynchronous Sinc resampler. */
  RESAMPLER_TYPE_ASYNC_POLY       /**< Asynchronous Polyphase resampler. */
} resampler_type_t;

/**
 * @brief Fixed chunk side for asynchronous resamplers.
 */
typedef enum {
  FIXED_ASYNC_INPUT = 0, /**< Input chunk size is fixed. */
  FIXED_ASYNC_OUTPUT = 1 /**< Output chunk size is fixed. */
} fixed_async_t;

/**
 * @brief Resampler profiles defining performance/accuracy trade-offs.
 */
typedef enum {
  RESAMPLER_PROFILE_VERY_FAST = 0, /**< Very fast, lower accuracy. */
  RESAMPLER_PROFILE_FAST,          /**< Fast, moderate accuracy. */
  RESAMPLER_PROFILE_BALANCED,      /**< Balanced performance and accuracy. */
  RESAMPLER_PROFILE_ACCURATE       /**< Accurate, higher processing cost. */
} resampler_profile_t;

/**
 * @brief Configuration for a resampler.
 */
typedef struct {
  resampler_type_t type;  /**< The type of resampler. */
  char profile[32];       /**< Profile name (e.g. "balanced"). */
  bool has_profile;       /**< Flag indicating if profile is specified. */
  char interpolation[32]; /**< Interpolation method. */
  bool has_interpolation; /**< Flag indicating if interpolation is specified. */
  int sinc_len;           /**< Length of the Sinc filter. */
  bool has_sinc_len;      /**< Flag indicating if sinc_len is specified. */
  int oversampling_factor;      /**< Oversampling factor. */
  bool has_oversampling_factor; /**< Flag indicating if oversampling_factor is
                                   specified. */
  char window[32];              /**< Window function name. */
  bool has_window;              /**< Flag indicating if window is specified. */
  double f_cutoff;              /**< Cutoff frequency. */
  bool has_f_cutoff; /**< Flag indicating if f_cutoff is specified. */
} resampler_config_t;

/**
 * @brief Convert a resampler type enum to its string representation.
 *
 * @param type The resampler type.
 * @return The string representation.
 */
const char* resampler_type_to_string(resampler_type_t type);

/**
 * @brief Convert a string representation to a resampler type enum.
 *
 * @param str The string representation.
 * @return The corresponding resampler type enum.
 */
resampler_type_t resampler_type_from_string(const char* str);

/**
 * @brief Convert a resampler profile enum to its string representation.
 *
 * @param profile The profile enum.
 * @return The string representation.
 */
const char* resampler_profile_to_string(resampler_profile_t profile);

/**
 * @brief Convert a string representation to a resampler profile enum.
 *
 * @param str The string representation.
 * @return The corresponding profile enum.
 */
resampler_profile_t resampler_profile_from_string(const char* str);

/**
 * @brief Initialize a resampler configuration with default values for a type.
 *
 * @param config Pointer to the configuration struct to initialize.
 * @param type The resampler type to set.
 */
void resampler_config_init(resampler_config_t* config, resampler_type_t type);

/**
 * @brief Get a string description of the resampler configuration.
 *
 * Useful for logging or debugging.
 *
 * @param config Pointer to the configuration.
 * @param out_buf Buffer to write the description to.
 * @param buf_len Size of the output buffer.
 */
void resampler_config_description(const resampler_config_t* config,
                                  char* out_buf, size_t buf_len);

#endif  // CLIB_CONFIG_RESAMPLER_CONFIG_TYPES_H
