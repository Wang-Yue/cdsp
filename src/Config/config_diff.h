/**
 * @file config_diff.h
 * @brief Configuration difference detection and reporting.
 *
 * This file provides functions to compare two DSP configurations and
 * identify the types of changes (e.g., filter parameters, mixer parameters,
 * pipeline, devices) and retrieve the names of changed elements.
 */

#ifndef CLIB_CONFIG_CONFIG_DIFF_H
#define CLIB_CONFIG_CONFIG_DIFF_H

#include <stdbool.h>
#include <stddef.h>

#include "Config/configuration.h"
#include "Config/engine_config_types.h"

/**
 * @enum config_change_type_t
 * @brief Identifies the type of configuration change.
 */
typedef enum {
  CONFIG_CHANGE_NONE = 0,          /**< No changes detected. */
  CONFIG_CHANGE_FILTER_PARAMETERS, /**< Filter parameters changed. */
  CONFIG_CHANGE_MIXER_PARAMETERS,  /**< Mixer parameters changed. */
  CONFIG_CHANGE_PIPELINE,          /**< Pipeline configuration changed. */
  CONFIG_CHANGE_DEVICES            /**< Device configuration changed. */
} config_change_type_t;

/**
 * @brief Compares two configurations and returns the change type.
 *
 * @param current The current configuration.
 * @param new_conf The new configuration to compare against.
 * @return The highest severity change type detected.
 */
config_change_type_t config_diff(const dsp_config_t* current,
                                 const dsp_config_t* new_conf);

/**
 * @brief Compares two devices configurations for equality.
 * @param a Pointer to first devices configuration.
 * @param b Pointer to second devices configuration.
 * @return true if configurations are equal, false otherwise.
 */
bool devices_config_equal(const devices_config_t* a, const devices_config_t* b);

#endif  // CLIB_CONFIG_CONFIG_DIFF_H
