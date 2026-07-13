#ifndef CDSP_CONFIG_PARSE_PIPELINE_H
#define CDSP_CONFIG_PARSE_PIPELINE_H

/**
 * @file config_parse_pipeline.h
 * @brief Sub-parser for pipeline section configuration.
 */

#include "cJSON.h"
#include "config_error.h"
#include "configuration.h"

/**
 * @brief Parses the pipeline steps from the JSON configuration array.
 *
 * @param pipe_arr The cJSON array representing the "pipeline" section.
 * @param config Pointer to the top-level configuration structure.
 * @param err Pointer to config_error_t to record errors.
 * @return 0 on success, or -1 on error.
 */
int config_parse_pipeline(const cJSON* pipe_arr, dsp_config_t* config,
                          config_error_t* err);

#endif  // CDSP_CONFIG_PARSE_PIPELINE_H
