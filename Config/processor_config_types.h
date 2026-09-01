#ifndef CLIB_CONFIG_PROCESSOR_CONFIG_TYPES_H
#define CLIB_CONFIG_PROCESSOR_CONFIG_TYPES_H

/**
 * @file processor_config_types.h
 * @brief Configuration types and functions for CamillaDSP processors.
 *
 * This file defines the structures, enums, and functions used to configure
 * processors in CamillaDSP, such as compressors, noise gates, and RACE.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "Config/config_error.h"
#include "Config/filter_config_types.h"

/**
 * @brief Supported processor types.
 */
typedef enum {
  PROCESSOR_TYPE_INVALID = -1,   /**< Invalid/Unknown processor type. */
  PROCESSOR_TYPE_COMPRESSOR = 0, /**< Compressor processor. */
  PROCESSOR_TYPE_NOISE_GATE,     /**< Noise gate processor. */
  PROCESSOR_TYPE_RACE, /**< RACE (Receiver Active Crosstalk Cancellation)
                         processor. */
  PROCESSOR_TYPE_LOOKAHEAD_LIMITER, /**< Lookahead limiter processor. */
  PROCESSOR_TYPE_BIQUAD             /**< Multi-channel biquad processor. */
} processor_type_t;

/**
 * @brief Convert a processor type enum to its string representation.
 *
 * @param type The processor type.
 * @return The string representation of the processor type.
 */
const char* processor_type_to_string(processor_type_t type);

/**
 * @brief Convert a string representation to a processor type enum.
 *
 * @param str The string representation.
 * @return The corresponding processor type enum.
 */
processor_type_t processor_type_from_string(const char* str);

/**
 * @brief Parameters for the Compressor processor.
 */
typedef struct {
  size_t channels;               /**< Number of channels. */
  size_t* monitor_channels;      /**< Array of monitor channels. */
  size_t monitor_channels_count; /**< Number of monitor channels. */
  size_t* process_channels;      /**< Array of channels to process. */
  size_t process_channels_count; /**< Number of process channels. */
  double attack;                 /**< Attack time. */
  time_unit_t attack_unit;       /**< Unit of attack time. */
  double release;                /**< Release time. */
  time_unit_t release_unit;      /**< Unit of release time. */
  double threshold;              /**< Threshold level. */
  double factor;                 /**< Compression factor/ratio. */
  double makeup_gain;            /**< Makeup gain value. */
  bool has_makeup_gain; /**< Flag indicating if makeup gain is specified. */
  bool soft_clip;       /**< Flag indicating if soft clipping is enabled. */
  double clip_limit;    /**< Soft clip limit. */
  bool has_clip_limit;  /**< Flag indicating if soft clip limit is specified. */
} compressor_config_t;

/**
 * @brief Parameters for the Noise Gate processor.
 */
typedef struct {
  size_t channels;               /**< Number of channels. */
  size_t* monitor_channels;      /**< Array of monitor channels. */
  size_t monitor_channels_count; /**< Number of monitor channels. */
  size_t* process_channels;      /**< Array of channels to process. */
  size_t process_channels_count; /**< Number of process channels. */
  double attack;                 /**< Attack time. */
  time_unit_t attack_unit;       /**< Unit of attack time. */
  double release;                /**< Release time. */
  time_unit_t release_unit;      /**< Unit of release time. */
  double threshold;              /**< Threshold level. */
  double attenuation;            /**< Attenuation level. */
} noise_gate_config_t;

/**
 * @brief Parameters for the RACE processor.
 */
typedef struct {
  size_t channels;      /**< Number of channels. */
  size_t channel_a;     /**< Channel A index. */
  size_t channel_b;     /**< Channel B index. */
  double delay;         /**< Delay value. */
  bool subsample_delay; /**< Flag indicating if subsample delay is enabled. */
  bool has_subsample_delay; /**< Flag indicating if subsample_delay is
                               specified. */
  delay_unit_t delay_unit;  /**< Unit of the delay value. */
  bool has_delay_unit;      /**< Flag indicating if delay_unit is specified. */
  double attenuation;       /**< Attenuation level. */
} race_config_t;

/**
 * @brief Parameters for the Lookahead Limiter processor.
 */
typedef struct {
  size_t channels;               /**< Number of channels. */
  size_t* monitor_channels;      /**< Array of monitor channels. */
  size_t monitor_channels_count; /**< Number of monitor channels. */
  size_t* process_channels;      /**< Array of channels to process. */
  size_t process_channels_count; /**< Number of process channels. */
  double limit;                  /**< Limit value in dB. */
  double attack;                 /**< Attack time. */
  time_unit_t attack_unit;       /**< Unit of attack time. */
  double release;                /**< Release time. */
  time_unit_t release_unit;      /**< Unit of release time. */
  bool delay_processed_only;     /**< Only delay processed channels. */
} lookahead_limiter_processor_config_t;

struct filter;
typedef struct filter filter_t;

/**
 * @brief Configuration parameters for internal multi-channel biquad processor.
 */
typedef struct {
  const size_t* channels;          /**< Array of channel indices. */
  size_t channels_count;           /**< Number of channels. */
  filter_t* const* const* filters; /**< Filter pointers per channel. */
  const size_t* filters_counts;    /**< Number of filters per channel. */
} biquad_processor_config_t;

/**
 * @brief Configuration for a processor, containing its type and parameters.
 */
typedef struct {
  processor_type_t type; /**< The type of the processor. */
  union {
    compressor_config_t compressor; /**< Compressor parameters. */
    noise_gate_config_t noise_gate; /**< Noise gate parameters. */
    race_config_t race;             /**< RACE parameters. */
    lookahead_limiter_processor_config_t
        lookahead_limiter;          /**< Lookahead Limiter parameters. */
    biquad_processor_config_t
        biquad;                     /**< Biquad processor parameters. */
  } parameters;                     /**< Union of processor parameters. */
} processor_config_t;

#endif  // CLIB_CONFIG_PROCESSOR_CONFIG_TYPES_H
