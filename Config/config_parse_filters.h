#ifndef CDSP_CONFIG_PARSE_FILTERS_H
#define CDSP_CONFIG_PARSE_FILTERS_H

/**
 * @file config_parse_filters.h
 * @brief Sub-parser for filters and processors configuration sections.
 */

#include "cJSON.h"
#include "config_error.h"
#include "configuration.h"

/**
 * @brief Parses filters defined in the configuration.
 *
 * @param filters_obj The cJSON object containing filter definitions.
 * @param config Pointer to the top-level configuration structure.
 * @param err Pointer to config_error_t to record errors.
 * @return 0 on success, or -1 on error.
 */
int config_parse_filters(const cJSON* filters_obj, dsp_config_t* config,
                         config_error_t* err);

/**
 * @brief Parses processors defined in the configuration.
 *
 * @param processors_obj The cJSON object containing processor definitions.
 * @param config Pointer to the top-level configuration structure.
 * @param err Pointer to config_error_t to record errors.
 * @return 0 on success, or -1 on error.
 */
int config_parse_processors(const cJSON* processors_obj, dsp_config_t* config,
                            config_error_t* err);

#endif  // CDSP_CONFIG_PARSE_FILTERS_H
