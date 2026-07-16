#ifndef CDSP_YAML_H
#define CDSP_YAML_H

#include <stdbool.h>

#include "Config/cJSON.h"

/**
 * @brief Converts a cJSON structure to a clean YAML formatted string.
 *
 * The output string is allocated dynamically. The caller must free it when
 * done.
 *
 * @param json Pointer to the cJSON object tree.
 * @return Allocated null-terminated YAML string, or NULL on failure.
 */
char* cdsp_json_to_yaml(const cJSON* json);

/**
 * @brief Parses a YAML string into a cJSON structure.
 *
 * The returned cJSON object must be freed by the caller using cJSON_Delete().
 *
 * @param yaml_str Null-terminated YAML string input.
 * @param out_err Optional output pointer for error details. Caller must free if
 * allocated.
 * @return Pointer to parsed cJSON root object, or NULL on parse failure.
 */
cJSON* cdsp_yaml_to_json(const char* yaml_str, char** out_err);

#endif  // CDSP_YAML_H
