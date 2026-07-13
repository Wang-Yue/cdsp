/**
 * @file config_parser.c
 * @brief Top-level configuration parser delegating section parsing to modular
 * sub-parsers.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "Config/config_parse_devices.h"
#include "Config/config_parse_filters.h"
#include "Config/config_parse_mixers.h"
#include "Config/config_parse_pipeline.h"
#include "Config/config_parser_internal.h"
#include "Logging/app_logger.h"
#include "cJSON.h"
#include "configuration.h"

static const logger_t g_logger = {"dsp.config.parser"};

/**
 * @brief Parses an array of string labels from a cJSON array.
 *
 * Allocates a string array and duplicates each label string.
 *
 * @param labels_arr The cJSON array containing the labels.
 * @param out_labels Output pointer to store the allocated array of string
 * pointers.
 * @param out_count Output pointer to store the size of the parsed labels array.
 * @param out_has_labels Output pointer set to true if labels were successfully
 * parsed.
 */
void parse_labels_array(const cJSON* labels_arr, char*** out_labels,
                        size_t* out_count, bool* out_has_labels) {
  if (!cJSON_IsArray(labels_arr)) return;
  int size = cJSON_GetArraySize(labels_arr);
  if (size <= 0) return;

  char** arr = (char**)calloc(size, sizeof(char*));
  if (!arr) return;

  for (int k = 0; k < size; k++) {
    cJSON* el = cJSON_GetArrayItem(labels_arr, k);
    if (cJSON_IsString(el) && el->valuestring) {
      arr[k] = strdup(el->valuestring);
    } else if (cJSON_IsNull(el)) {
      arr[k] = NULL;
    }
  }

  *out_labels = arr;
  *out_count = (size_t)size;
  *out_has_labels = true;
}

double* parse_double_array(const cJSON* arr, size_t* out_count) {
  if (!cJSON_IsArray(arr)) {
    *out_count = 0;
    return NULL;
  }
  int size = cJSON_GetArraySize(arr);
  if (size <= 0) {
    *out_count = 0;
    return NULL;
  }
  double* values = (double*)calloc(size, sizeof(double));
  if (!values) {
    *out_count = 0;
    return NULL;
  }
  for (int i = 0; i < size; i++) {
    cJSON* el = cJSON_GetArrayItem(arr, i);
    if (cJSON_IsNumber(el)) {
      values[i] = el->valuedouble;
    }
  }
  *out_count = (size_t)size;
  return values;
}

int* parse_int_array(const cJSON* arr, size_t* out_count) {
  if (!cJSON_IsArray(arr)) {
    *out_count = 0;
    return NULL;
  }
  int size = cJSON_GetArraySize(arr);
  if (size <= 0) {
    *out_count = 0;
    return NULL;
  }
  int* values = (int*)calloc(size, sizeof(int));
  if (!values) {
    *out_count = 0;
    return NULL;
  }
  for (int i = 0; i < size; i++) {
    cJSON* el = cJSON_GetArrayItem(arr, i);
    if (cJSON_IsNumber(el)) {
      values[i] = el->valueint;
    }
  }
  *out_count = (size_t)size;
  return values;
}

int dsp_config_parse_json(const char* json, dsp_config_t** out_config,
                          config_error_t* err) {
  if (!json || !out_config) {
    config_error_set(err, CONFIG_ERR_PARSE,
                     "JSON string or output pointer is NULL");
    logger_error(
        &g_logger,
        "Config parsing failed: JSON string or output pointer is NULL");
    return -1;
  }

  dsp_config_t* config = (dsp_config_t*)calloc(1, sizeof(dsp_config_t));
  if (!config) {
    config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
    logger_error(&g_logger, "Config parsing failed: Memory allocation failure");
    return -1;
  }

  cJSON* root = cJSON_Parse(json);
  if (!root) {
    free(config);
    config_error_set(err, CONFIG_ERR_PARSE,
                     "Failed to parse JSON (syntax error or invalid JSON)");
    logger_error(&g_logger,
                 "Config parsing failed: Syntax error or invalid JSON");
    return -1;
  }

  cJSON* devices_obj = cJSON_GetObjectItemCaseSensitive(root, "devices");
  if (!devices_obj) {
    cJSON_Delete(root);
    free(config);
    config_error_set(err, CONFIG_ERR_PARSE, "Config must contain 'devices'");
    logger_error(&g_logger,
                 "Config parsing failed: Config must contain 'devices' object");
    return -1;
  }

  if (config_parse_devices(devices_obj, config, err) != 0) {
    cJSON_Delete(root);
    dsp_config_free(config);
    logger_error(&g_logger, "Config parsing failed in devices section: %s",
                 err ? err->message : "");
    return -1;
  }

  cJSON* pipeline_arr = cJSON_GetObjectItemCaseSensitive(root, "pipeline");
  if (pipeline_arr) {
    if (config_parse_pipeline(pipeline_arr, config, err) != 0) {
      cJSON_Delete(root);
      dsp_config_free(config);
      logger_error(&g_logger, "Config parsing failed in pipeline section: %s",
                   err ? err->message : "");
      return -1;
    }
  }

  cJSON* mixers_obj = cJSON_GetObjectItemCaseSensitive(root, "mixers");
  if (mixers_obj) {
    if (config_parse_mixers(mixers_obj, config, err) != 0) {
      cJSON_Delete(root);
      dsp_config_free(config);
      logger_error(&g_logger, "Config parsing failed in mixers section: %s",
                   err ? err->message : "");
      return -1;
    }
  }

  cJSON* filters_obj = cJSON_GetObjectItemCaseSensitive(root, "filters");
  if (filters_obj) {
    if (config_parse_filters(filters_obj, config, err) != 0) {
      cJSON_Delete(root);
      dsp_config_free(config);
      logger_error(&g_logger, "Config parsing failed in filters section: %s",
                   err ? err->message : "");
      return -1;
    }
  }

  cJSON* processors_obj = cJSON_GetObjectItemCaseSensitive(root, "processors");
  if (processors_obj) {
    if (config_parse_processors(processors_obj, config, err) != 0) {
      cJSON_Delete(root);
      dsp_config_free(config);
      logger_error(&g_logger, "Config parsing failed in processors section: %s",
                   err ? err->message : "");
      return -1;
    }
  }

  cJSON_Delete(root);

  /* Validate the populated configuration structure.
   * This checks schema constraints and traces channel flows through the
   * pipeline to catch configuration inconsistencies before return. */
  if (dsp_config_validate(config, err) != 0) {
    logger_error(&g_logger, "Config validation failed: %s",
                 err ? err->message : "");
    dsp_config_free(config);
    return -1;
  }

  logger_info(&g_logger,
              "Configuration successfully parsed and validated (samplerate=%d, "
              "chunksize=%d)",
              config->devices.samplerate, config->devices.chunksize);
  *out_config = config;
  return 0;
}
