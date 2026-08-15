#ifndef CDSP_CONFIG_PARSER_INTERNAL_H
#define CDSP_CONFIG_PARSER_INTERNAL_H

/**
 * @file config_parser_internal.h
 * @brief Internal utility helpers shared across config sub-parsers.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "Config/cJSON.h"

/**
 * @brief Parses an array of string labels from a cJSON array.
 *
 * @param labels_arr The cJSON array containing the labels.
 * @param out_labels Output pointer to store allocated array of string pointers.
 * @param out_count Output pointer to store the size of the parsed labels array.
 * @param out_has_labels Output pointer set to true if labels were successfully
 * parsed.
 */
void parse_labels_array(const cJSON* labels_arr, char*** out_labels,
                        size_t* out_count, bool* out_has_labels);

/**
 * @brief Parses an array of double numbers from a cJSON array.
 *
 * @param arr The cJSON array containing floating point values.
 * @param out_count Output pointer storing array length.
 * @return Dynamically allocated double array or NULL.
 */
double* parse_double_array(const cJSON* arr, size_t* out_count);

/**
 * @brief Parses an array of integer numbers from a cJSON array.
 *
 * @param arr The cJSON array containing integer values.
 * @param out_count Output pointer storing array length.
 * @return Dynamically allocated integer array or NULL.
 */
int* parse_int_array(const cJSON* arr, size_t* out_count);

static inline bool parse_json_str(const cJSON* obj, const char* key, char* dest,
                                  size_t dest_sz) {
  if (dest_sz == 0) return false;
  const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (cJSON_IsString(item) && item->valuestring) {
    size_t len = strlen(item->valuestring);
    if (len >= dest_sz) {
      len = dest_sz - 1;
    }
    memcpy(dest, item->valuestring, len);
    dest[len] = '\0';
    return true;
  }
  return false;
}

static inline bool parse_json_int(const cJSON* obj, const char* key,
                                  int* dest) {
  const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (cJSON_IsNumber(item)) {
    *dest = item->valueint;
    return true;
  }
  return false;
}

static inline bool parse_json_size_t(const cJSON* obj, const char* key,
                                     size_t* dest) {
  const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (cJSON_IsNumber(item) && item->valueint >= 0) {
    *dest = (size_t)item->valueint;
    return true;
  }
  return false;
}

static inline bool parse_json_bool(const cJSON* obj, const char* key,
                                   bool* dest) {
  const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (cJSON_IsBool(item)) {
    *dest = cJSON_IsTrue(item);
    return true;
  }
  return false;
}

static inline bool parse_json_double(const cJSON* obj, const char* key,
                                     double* dest) {
  const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (cJSON_IsNumber(item)) {
    *dest = item->valuedouble;
    return true;
  }
  return false;
}

#include "Config/config_error.h"

/**
 * @brief Validates that all keys in a cJSON object are present in an allowed
 * list.
 *
 * @param obj The cJSON object to inspect.
 * @param allowed_keys A NULL-terminated array of allowed key string literals.
 * @param section_name Name of the section or object for error reporting.
 * @param err Optional config_error_t to populate if an unknown key is found.
 * @return 0 if all keys are allowed, -1 if an unknown key is encountered.
 */
static inline int validate_unknown_fields(const cJSON* obj,
                                          const char* const allowed_keys[],
                                          const char* section_name,
                                          config_error_t* err) {
  if (!obj || !cJSON_IsObject(obj)) return 0;
  const cJSON* child = NULL;
  cJSON_ArrayForEach(child, obj) {
    if (!child->string) continue;
    bool found = false;
    for (size_t i = 0; allowed_keys[i] != NULL; i++) {
      if (strcmp(child->string, allowed_keys[i]) == 0) {
        found = true;
        break;
      }
    }
    if (!found) {
      if (err) {
        char msg[256];
        snprintf(msg, sizeof(msg), "unknown field '%s' in %s", child->string,
                 section_name ? section_name : "object");
        config_error_set(err, CONFIG_ERR_PARSE, "%s", msg);
      }
      return -1;
    }
  }
  return 0;
}

#endif  // CDSP_CONFIG_PARSER_INTERNAL_H
