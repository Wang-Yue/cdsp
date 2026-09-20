/**
 * @file configuration.h
 * @brief Top-level configuration data structures and validation logic.
 *
 * This file owns:
 *   1. Top-level configuration models (dsp_config_t and pipeline_step_t).
 *   2. Cross-component validation logic, including schema checks and the
 *      pipeline walk that tracks channel layouts.
 */

#ifndef CLIB_CONFIG_CONFIGURATION_H
#define CLIB_CONFIG_CONFIGURATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config/config_error.h"
#include "config/config_gen.h"

typedef struct dsp_config_overrides_t dsp_config_overrides_t;

/**
 * @brief Validates the DSP configuration.
 *
 * Checks schema validity and performs a pipeline walk to verify channel
 * layouts.
 *
 * @param config Pointer to the configuration to validate.
 * @param err Pointer to a config_error_t struct to receive error details if
 * validation fails.
 * @return 0 if valid, non-zero if invalid.
 */
int dsp_config_validate(const dsp_config_t *config, config_error_t *err);

/**
 * @brief Parses a JSON string into a DSP configuration.
 *
 * Allocates and populates a dsp_config_t structure.
 *
 * @param json The JSON string to parse.
 * @param out_config Pointer to a pointer to receive the allocated
 * configuration.
 * @param err Pointer to a config_error_t struct to receive error details if
 * parsing fails.
 * @return 0 on success, non-zero on failure.
 */
int dsp_config_parse_json(const char *json, dsp_config_t **out_config,
                          config_error_t *err);

/**
 * @brief Parses a DSP configuration from JSON with a base directory for
 * resolving relative file paths.
 *
 * @param json The JSON string to parse.
 * @param config_dir Base directory path for resolving relative file paths (e.g.
 * IR files).
 * @param out_config Pointer to receive the allocated configuration.
 * @param err Pointer to receive error details.
 * @return 0 on success, non-zero on failure.
 */
int dsp_config_parse_json_with_dir(const char *json, const char *config_dir,
                                   dsp_config_t **out_config,
                                   config_error_t *err);

/**
 * @brief Parses a DSP configuration from JSON with directory resolution and
 * overrides.
 */
int dsp_config_parse_json_with_dir_and_overrides(
    const char *json, const char *config_dir,
    const dsp_config_overrides_t *overrides, dsp_config_t **out_config,
    config_error_t *err);

/**
 * @brief Parses a DSP configuration from JSON with directory resolution,
 * overrides, and optional full pipeline validation.
 */
int dsp_config_parse_json_with_dir_and_overrides_ext(
    const char *json, const char *config_dir,
    const dsp_config_overrides_t *overrides, dsp_config_t **out_config,
    bool validate, config_error_t *err);

/**
 * @brief Parses a DSP configuration from JSON without running full pipeline
 * and cross-field validation.
 */
int dsp_config_parse_json_no_validate(const char *json,
                                      dsp_config_t **out_config,
                                      config_error_t *err);

/**
 * @brief Frees a DSP configuration.
 *
 * Deallocates the configuration structure and all associated nested structures.
 *
 * @param config Pointer to the configuration to free.
 */
void dsp_config_free(dsp_config_t *config);

/**
 * @brief Retrieves a filter configuration by name.
 *
 * @param config Pointer to the top-level configuration.
 * @param name Name of the filter to retrieve.
 * @return Pointer to the filter configuration, or NULL if not found.
 */
filter_config_t *dsp_config_get_filter(const dsp_config_t *config,
                                       const char *name);

/**
 * @brief Retrieves a mixer configuration by name.
 *
 * @param config Pointer to the top-level configuration.
 * @param name Name of the mixer to retrieve.
 * @return Pointer to the mixer configuration, or NULL if not found.
 */
mixer_config_t *dsp_config_get_mixer(const dsp_config_t *config,
                                     const char *name);

/**
 * @brief Retrieves a processor configuration by name.
 *
 * @param config Pointer to the top-level configuration.
 * @param name Name of the processor to retrieve.
 * @return Pointer to the processor configuration, or NULL if not found.
 */
processor_config_t *dsp_config_get_processor(const dsp_config_t *config,
                                             const char *name);

#endif // CLIB_CONFIG_CONFIGURATION_H
