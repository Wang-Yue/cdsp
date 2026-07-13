#ifndef CDSP_CONFIG_PARSE_DEVICES_H
#define CDSP_CONFIG_PARSE_DEVICES_H

/**
 * @file config_parse_devices.h
 * @brief Sub-parser for the devices, resampler, capture, and playback sections.
 */

#include "cJSON.h"
#include "config_error.h"
#include "configuration.h"

/**
 * @brief Parses the top-level devices section from JSON configuration.
 *
 * @param dev_obj The cJSON object representing the "devices" section.
 * @param config Pointer to the top-level dsp_config_t structure to populate.
 * @param err Pointer to config_error_t to record errors.
 * @return 0 on success, or -1 on error (with err populated).
 */
int config_parse_devices(const cJSON* dev_obj, dsp_config_t* config,
                         config_error_t* err);

#endif  // CDSP_CONFIG_PARSE_DEVICES_H
