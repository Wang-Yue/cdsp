#ifndef CDSP_CONFIG_PARSE_MIXERS_H
#define CDSP_CONFIG_PARSE_MIXERS_H

/**
 * @file config_parse_mixers.h
 * @brief Sub-parser for mixers configuration section.
 */

#include "cJSON.h"
#include "config_error.h"
#include "configuration.h"

/**
 * @brief Parses mixers defined in the configuration.
 *
 * @param mixers_obj The cJSON object containing mixer definitions.
 * @param config Pointer to the top-level configuration structure.
 * @param err Pointer to config_error_t to record errors.
 * @return 0 on success, or -1 on error.
 */
int config_parse_mixers(const cJSON* mixers_obj, dsp_config_t* config,
                        config_error_t* err);

#endif  // CDSP_CONFIG_PARSE_MIXERS_H
