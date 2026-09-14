/**
 * @file config_parser.c
 * @brief Top-level configuration parser delegating section parsing to modular
 * sub-parsers.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Config/cJSON.h"
#include "Config/config_error.h"
#include "Config/config_parse_devices.h"
#include "Config/config_parse_filters.h"
#include "Config/config_parse_mixers.h"
#include "Config/config_parse_pipeline.h"
#include "Config/config_parser_internal.h"
#include "Config/configuration.h"
#include "Config/engine_config_types.h"
#include "Logging/app_logger.h"

static const logger_t g_logger = {"dsp.config.parser"};

static int compare_named_filters(const void* a, const void* b) {
  const char* na = ((const named_filter_config_t*)a)->name;
  const char* nb = ((const named_filter_config_t*)b)->name;
  if (!na && !nb) return 0;
  if (!na) return -1;
  if (!nb) return 1;
  return strcmp(na, nb);
}

static int compare_named_mixers(const void* a, const void* b) {
  const char* na = ((const named_mixer_config_t*)a)->name;
  const char* nb = ((const named_mixer_config_t*)b)->name;
  if (!na && !nb) return 0;
  if (!na) return -1;
  if (!nb) return 1;
  return strcmp(na, nb);
}

static int compare_named_processors(const void* a, const void* b) {
  const char* na = ((const named_processor_config_t*)a)->name;
  const char* nb = ((const named_processor_config_t*)b)->name;
  if (!na && !nb) return 0;
  if (!na) return -1;
  if (!nb) return 1;
  return strcmp(na, nb);
}

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
  parse_labels_array_strict(labels_arr, out_labels, out_count, out_has_labels);
}

int parse_labels_array_strict(const cJSON* labels_arr, char*** out_labels,
                              size_t* out_count, bool* out_has_labels) {
  if (!labels_arr) return 0;
  if (!cJSON_IsArray(labels_arr)) return -1;
  int size = cJSON_GetArraySize(labels_arr);
  if (size < 0) return -1;
  if (size == 0) {
    *out_labels = NULL;
    *out_count = 0;
    if (out_has_labels) *out_has_labels = true;
    return 0;
  }

  char** arr = (char**)calloc(size, sizeof(char*));
  if (!arr) return -1;

  for (int k = 0; k < size; k++) {
    cJSON* el = cJSON_GetArrayItem(labels_arr, k);
    if (cJSON_IsString(el) && el->valuestring) {
      arr[k] = strdup(el->valuestring);
      if (!arr[k]) {
        for (int j = 0; j < k; j++) free(arr[j]);
        free(arr);
        return -1;
      }
    } else if (cJSON_IsNull(el)) {
      arr[k] = NULL;
    } else {
      for (int j = 0; j < k; j++) free(arr[j]);
      free(arr);
      return -1;
    }
  }

  *out_labels = arr;
  *out_count = (size_t)size;
  if (out_has_labels) *out_has_labels = true;
  return 0;
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
    if (!cJSON_IsNumber(el)) {
      free(values);
      *out_count = 0;
      return NULL;
    }
    values[i] = el->valuedouble;
  }
  *out_count = (size_t)size;
  return values;
}

int parse_size_t_array_strict(const cJSON* arr, const char* field_name,
                              const char* section_name, size_t** out_values,
                              size_t* out_count, config_error_t* err) {
  *out_values = NULL;
  *out_count = 0;
  if (!cJSON_IsArray(arr)) return 0;
  int size = cJSON_GetArraySize(arr);
  if (size <= 0) return 0;
  size_t* values = (size_t*)calloc((size_t)size, sizeof(size_t));
  if (!values) {
    config_error_set(err, CONFIG_ERR_PARSE, "out of memory parsing '%s' in %s",
                     field_name, section_name ? section_name : "object");
    return -1;
  }
  for (int i = 0; i < size; i++) {
    const cJSON* el = cJSON_GetArrayItem(arr, i);
    if (!cJSON_IsNumber(el) || el->valuedouble < 0.0 ||
        floor(el->valuedouble) != el->valuedouble ||
        el->valuedouble > (double)SIZE_MAX) {
      config_error_set(err, CONFIG_ERR_PARSE,
                       "element %d of '%s' in %s must be a non-negative "
                       "integer",
                       i, field_name, section_name ? section_name : "object");
      free(values);
      return -1;
    }
    values[i] = (size_t)el->valuedouble;
  }
  *out_values = values;
  *out_count = (size_t)size;
  return 0;
}

static void replace_tokens_in_string_node(cJSON* node, int samplerate,
                                          int channels) {
  if (!node || !cJSON_IsString(node) || !node->valuestring) return;
  const char* str = node->valuestring;
  if (strstr(str, "$samplerate$") == NULL &&
      strstr(str, "$channels$") == NULL) {
    return;
  }
  char sr_buf[32];
  char ch_buf[32];
  snprintf(sr_buf, sizeof(sr_buf), "%d", samplerate);
  snprintf(ch_buf, sizeof(ch_buf), "%d", channels);

  size_t in_len = strlen(str);
  size_t cap = in_len + 128;
  char* new_val = (char*)malloc(cap);
  if (!new_val) return;
  size_t out_len = 0;
  for (size_t i = 0; i < in_len;) {
    const char* to_append = NULL;
    size_t append_len = 0;
    if (strncmp(str + i, "$samplerate$", 12) == 0) {
      to_append = sr_buf;
      append_len = strlen(sr_buf);
      i += 12;
    } else if (strncmp(str + i, "$channels$", 10) == 0) {
      to_append = ch_buf;
      append_len = strlen(ch_buf);
      i += 10;
    } else {
      to_append = str + i;
      append_len = 1;
      i++;
    }

    if (out_len + append_len + 1 > cap) {
      cap = (out_len + append_len + 1) * 2;
      char* resized = (char*)realloc(new_val, cap);
      if (!resized) {
        free(new_val);
        return;
      }
      new_val = resized;
    }
    memcpy(new_val + out_len, to_append, append_len);
    out_len += append_len;
  }
  new_val[out_len] = '\0';
  cJSON_SetValuestring(node, new_val);
  free(new_val);
}

static void replace_tokens_in_config_json(cJSON* root, int samplerate,
                                          int channels) {
  if (!root) return;
  cJSON* filters = cJSON_GetObjectItemCaseSensitive(root, "filters");
  if (cJSON_IsObject(filters)) {
    cJSON* filter = filters->child;
    while (filter) {
      cJSON* type_item = cJSON_GetObjectItemCaseSensitive(filter, "type");
      if (cJSON_IsString(type_item) && type_item->valuestring &&
          strcmp(type_item->valuestring, "Conv") == 0) {
        cJSON* params = cJSON_GetObjectItemCaseSensitive(filter, "parameters");
        if (cJSON_IsObject(params)) {
          cJSON* fn = cJSON_GetObjectItemCaseSensitive(params, "filename");
          if (fn) {
            replace_tokens_in_string_node(fn, samplerate, channels);
          }
        }
      }
      filter = filter->next;
    }
  }
  cJSON* pipeline = cJSON_GetObjectItemCaseSensitive(root, "pipeline");
  if (cJSON_IsArray(pipeline)) {
    int sz = cJSON_GetArraySize(pipeline);
    for (int i = 0; i < sz; i++) {
      cJSON* step = cJSON_GetArrayItem(pipeline, i);
      if (!cJSON_IsObject(step)) continue;
      cJSON* type_item = cJSON_GetObjectItemCaseSensitive(step, "type");
      const char* tstr =
          (type_item && cJSON_IsString(type_item) && type_item->valuestring)
              ? type_item->valuestring
              : "";
      if (strcmp(tstr, "Filter") == 0) {
        cJSON* names = cJSON_GetObjectItemCaseSensitive(step, "names");
        if (cJSON_IsArray(names)) {
          int nsz = cJSON_GetArraySize(names);
          for (int j = 0; j < nsz; j++) {
            replace_tokens_in_string_node(cJSON_GetArrayItem(names, j),
                                          samplerate, channels);
          }
        }
      } else if (strcmp(tstr, "Mixer") == 0 || strcmp(tstr, "Processor") == 0) {
        cJSON* name = cJSON_GetObjectItemCaseSensitive(step, "name");
        if (name) {
          replace_tokens_in_string_node(name, samplerate, channels);
        }
      }
    }
  }
}

#ifdef _WIN32
#include <io.h>

#define F_OK 0
#define access _access
#else
#include <unistd.h>
#endif

static void resolve_relative_paths_in_filters(cJSON* filters_obj,
                                              const char* config_dir) {
  if (!filters_obj || !config_dir || config_dir[0] == '\0') return;
  cJSON* filter = filters_obj->child;
  while (filter) {
    cJSON* type_item = cJSON_GetObjectItemCaseSensitive(filter, "type");
    if (type_item && cJSON_IsString(type_item) &&
        strcmp(type_item->valuestring, "Conv") == 0) {
      cJSON* params = cJSON_GetObjectItemCaseSensitive(filter, "parameters");
      if (params && cJSON_IsObject(params)) {
        cJSON* fn_node = cJSON_GetObjectItemCaseSensitive(params, "filename");
        if (fn_node && cJSON_IsString(fn_node) && fn_node->valuestring) {
          const char* str = fn_node->valuestring;
          if (str[0] != '\0' && str[0] != '/' &&
              !(strlen(str) >= 2 && str[1] == ':')) {
            char resolved[1024];
            size_t dir_len = strlen(config_dir);
            bool needs_slash = (dir_len > 0 && config_dir[dir_len - 1] != '/');
            int n = snprintf(resolved, sizeof(resolved), "%s%s%s", config_dir,
                             needs_slash ? "/" : "", str);
            if (n >= 0 && (size_t)n < sizeof(resolved)) {
              if (access(resolved, F_OK) == 0) {
                cJSON_SetValuestring(fn_node, resolved);
              }
            }
          }
        }
      }
    }
    filter = filter->next;
  }
}

int dsp_config_parse_json(const char* json, dsp_config_t** out_config,
                          config_error_t* err) {
  return dsp_config_parse_json_with_dir_and_overrides(json, NULL, NULL,
                                                      out_config, err);
}

int dsp_config_parse_json_with_dir(const char* json, const char* config_dir,
                                   dsp_config_t** out_config,
                                   config_error_t* err) {
  return dsp_config_parse_json_with_dir_and_overrides(json, config_dir, NULL,
                                                      out_config, err);
}

int dsp_config_parse_json_with_dir_and_overrides_ext(
    const char* json, const char* config_dir,
    const dsp_config_overrides_t* overrides, dsp_config_t** out_config,
    bool validate, config_error_t* err) {
  if (!json || !out_config) {
    config_error_set(err, CONFIG_ERR_PARSE,
                     "JSON string or output pointer is NULL");
    logger_error(
        &g_logger,
        "Config parsing failed: JSON string or output pointer is NULL");
    return -1;
  }

  *out_config = NULL;

  dsp_config_t* config = (dsp_config_t*)calloc(1, sizeof(dsp_config_t));
  if (!config) {
    config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
    logger_error(&g_logger, "Config parsing failed: Memory allocation failure");
    return -1;
  }

  cJSON* root = cJSON_Parse(json);
  if (!root) {
    dsp_config_free(config);
    config_error_set(err, CONFIG_ERR_PARSE,
                     "Failed to parse JSON (syntax error or invalid JSON)");
    logger_error(&g_logger,
                 "Config parsing failed: Syntax error or invalid JSON");
    return -1;
  }

  static const char* const allowed_root_keys[] = {
      "title",  "description", "devices",  "filters",
      "mixers", "processors",  "pipeline", NULL};
  if (validate_unknown_fields(root, allowed_root_keys, "root configuration",
                              err) != 0) {
    cJSON_Delete(root);
    dsp_config_free(config);
    logger_error(&g_logger, "Config parsing failed: %s",
                 err ? err->message : "unknown field in root");
    return -1;
  }

  if (parse_json_str_strict(root, "title", "root configuration", config->title,
                            sizeof(config->title), NULL, err) != 0 ||
      parse_json_str_strict(root, "description", "root configuration",
                            config->description, sizeof(config->description),
                            NULL, err) != 0) {
    cJSON_Delete(root);
    dsp_config_free(config);
    return -1;
  }

  cJSON* devices_obj = cJSON_GetObjectItemCaseSensitive(root, "devices");
  if (!devices_obj) {
    cJSON_Delete(root);
    dsp_config_free(config);
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

  // Apply WAV file and command-line overrides matching upstream CamillaDSP
  // apply_overrides (src/config/utils.rs:130-265)
  if (dsp_config_apply_overrides(config, overrides, err) != 0) {
    cJSON_Delete(root);
    dsp_config_free(config);
    return -1;
  }

  // Replace tokens in JSON with final effective samplerate and channels
  int final_sr = (int)config->devices.samplerate;
  int final_ch = capture_device_config_get_channels(&config->devices.capture);
  if (final_sr > 0 || final_ch > 0) {
    replace_tokens_in_config_json(root, final_sr, final_ch);
  }

  // Resolve relative paths in filters against config directory after token
  // replacement
  if (config_dir && config_dir[0] != '\0') {
    cJSON* filters_obj = cJSON_GetObjectItemCaseSensitive(root, "filters");
    if (filters_obj) {
      resolve_relative_paths_in_filters(filters_obj, config_dir);
    }
  }

  cJSON* pipeline_arr = cJSON_GetObjectItemCaseSensitive(root, "pipeline");
  if (pipeline_arr && !cJSON_IsNull(pipeline_arr)) {
    if (!cJSON_IsObject(pipeline_arr) || cJSON_GetArraySize(pipeline_arr) > 0) {
      if (config_parse_pipeline(pipeline_arr, config, err) != 0) {
        cJSON_Delete(root);
        dsp_config_free(config);
        logger_error(&g_logger, "Config parsing failed in pipeline section: %s",
                     err ? err->message : "");
        return -1;
      }
    }
  }

  cJSON* mixers_obj = cJSON_GetObjectItemCaseSensitive(root, "mixers");
  if (mixers_obj && !cJSON_IsNull(mixers_obj)) {
    if (config_parse_mixers(mixers_obj, config, err) != 0) {
      cJSON_Delete(root);
      dsp_config_free(config);
      logger_error(&g_logger, "Config parsing failed in mixers section: %s",
                   err ? err->message : "");
      return -1;
    }
  }

  cJSON* filters_obj = cJSON_GetObjectItemCaseSensitive(root, "filters");
  if (filters_obj && !cJSON_IsNull(filters_obj)) {
    if (config_parse_filters(filters_obj, config, err) != 0) {
      cJSON_Delete(root);
      dsp_config_free(config);
      logger_error(&g_logger, "Config parsing failed in filters section: %s",
                   err ? err->message : "");
      return -1;
    }
  }

  cJSON* processors_obj = cJSON_GetObjectItemCaseSensitive(root, "processors");
  if (processors_obj && !cJSON_IsNull(processors_obj)) {
    if (config_parse_processors(processors_obj, config, err) != 0) {
      cJSON_Delete(root);
      dsp_config_free(config);
      logger_error(&g_logger, "Config parsing failed in processors section: %s",
                   err ? err->message : "");
      return -1;
    }
  }

  // Sort filters, mixers, and processors alphabetically by name to make config
  // comparison order-independent.
  if (config->filters && config->filters_count > 1) {
    qsort(config->filters, config->filters_count, sizeof(named_filter_config_t),
          compare_named_filters);
  }
  if (config->mixers && config->mixers_count > 1) {
    qsort(config->mixers, config->mixers_count, sizeof(named_mixer_config_t),
          compare_named_mixers);
  }
  if (config->processors && config->processors_count > 1) {
    qsort(config->processors, config->processors_count,
          sizeof(named_processor_config_t), compare_named_processors);
  }

  cJSON_Delete(root);

  if (validate) {
    /* Validate the populated configuration structure.
     * This checks schema constraints and traces channel flows through the
     * pipeline to catch configuration inconsistencies before return. */
    if (dsp_config_validate(config, err) != 0) {
      logger_error(&g_logger, "Config validation failed: %s",
                   err ? err->message : "");
      dsp_config_free(config);
      return -1;
    }
  }

  logger_info(&g_logger,
              "Configuration successfully parsed (samplerate=%d, "
              "chunksize=%d)",
              config->devices.samplerate, config->devices.chunksize);
  *out_config = config;
  return 0;
}

int dsp_config_parse_json_with_dir_and_overrides(
    const char* json, const char* config_dir,
    const dsp_config_overrides_t* overrides, dsp_config_t** out_config,
    config_error_t* err) {
  return dsp_config_parse_json_with_dir_and_overrides_ext(
      json, config_dir, overrides, out_config, true, err);
}

int dsp_config_parse_json_no_validate(const char* json,
                                      dsp_config_t** out_config,
                                      config_error_t* err) {
  return dsp_config_parse_json_with_dir_and_overrides_ext(
      json, NULL, NULL, out_config, false, err);
}
