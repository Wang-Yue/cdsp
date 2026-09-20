/**
 * @file config_parser.c
 * @brief Top-level configuration parser delegating JSON deserialization to
 * auto-generated codegen.
 */

#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#define F_OK 0
#define access _access
#else
#include <unistd.h>
#endif

#include "audio/sample_format.h"
#include "backend/audio_backend.h"
#include "config/cJSON.h"
#include "config/config_error.h"
#include "config/config_parser.h"
#include "config/configuration.h"
#include "config_gen.h"
#include "logging/app_logger.h"

static const logger_t g_logger = {"dsp.config.parser"};

static int compare_named_filters(const void *a, const void *b) {
  const char *na = ((const named_filter_config_t *)a)->name;
  const char *nb = ((const named_filter_config_t *)b)->name;
  if (!na && !nb)
    return 0;
  if (!na)
    return -1;
  if (!nb)
    return 1;
  return strcmp(na, nb);
}

static int compare_named_mixers(const void *a, const void *b) {
  const char *na = ((const named_mixer_config_t *)a)->name;
  const char *nb = ((const named_mixer_config_t *)b)->name;
  if (!na && !nb)
    return 0;
  if (!na)
    return -1;
  if (!nb)
    return 1;
  return strcmp(na, nb);
}

static int compare_named_processors(const void *a, const void *b) {
  const char *na = ((const named_processor_config_t *)a)->name;
  const char *nb = ((const named_processor_config_t *)b)->name;
  if (!na && !nb)
    return 0;
  if (!na)
    return -1;
  if (!nb)
    return 1;
  return strcmp(na, nb);
}

int parse_labels_array_strict(const cJSON *labels_arr, char ***out_labels,
                              size_t *out_count, bool *out_has_labels) {
  if (!labels_arr)
    return 0;
  if (!cJSON_IsArray(labels_arr))
    return -1;
  int size = cJSON_GetArraySize(labels_arr);
  if (size < 0)
    return -1;
  if (size == 0) {
    *out_labels = NULL;
    *out_count = 0;
    if (out_has_labels)
      *out_has_labels = true;
    return 0;
  }

  char **arr = (char **)calloc((size_t)size, sizeof(char *));
  if (!arr)
    return -1;

  for (int k = 0; k < size; k++) {
    cJSON *el = cJSON_GetArrayItem(labels_arr, k);
    if (cJSON_IsString(el) && el->valuestring) {
      arr[k] = strdup(el->valuestring);
      if (!arr[k]) {
        for (int j = 0; j < k; j++)
          free(arr[j]);
        free(arr);
        return -1;
      }
    } else if (cJSON_IsNull(el)) {
      arr[k] = NULL;
    } else {
      for (int j = 0; j < k; j++)
        free(arr[j]);
      free(arr);
      return -1;
    }
  }

  *out_labels = arr;
  *out_count = (size_t)size;
  if (out_has_labels)
    *out_has_labels = true;
  return 0;
}

int parse_size_t_array_strict(const cJSON *arr, const char *field_name,
                              const char *section_name, size_t **out_values,
                              size_t *out_count, config_error_t *err) {
  *out_values = NULL;
  *out_count = 0;
  if (!cJSON_IsArray(arr))
    return 0;
  int size = cJSON_GetArraySize(arr);
  if (size <= 0)
    return 0;
  size_t *values = (size_t *)calloc((size_t)size, sizeof(size_t));
  if (!values) {
    config_error_set(err, CONFIG_ERR_PARSE, "out of memory parsing '%s' in %s",
                     field_name, section_name ? section_name : "object");
    return -1;
  }
  for (int i = 0; i < size; i++) {
    const cJSON *el = cJSON_GetArrayItem(arr, i);
    if (!cJSON_IsNumber(el) || el->valuedouble < 0.0 ||
        floor(el->valuedouble) != el->valuedouble ||
        el->valuedouble > (double)SIZE_MAX) {
      config_error_set(
          err, CONFIG_ERR_PARSE,
          "element %d of '%s' in %s must be a non-negative integer", i,
          field_name, section_name ? section_name : "object");
      free(values);
      return -1;
    }
    values[i] = (size_t)el->valuedouble;
  }
  *out_values = values;
  *out_count = (size_t)size;
  return 0;
}

int parse_double_array_strict(const cJSON *arr, const char *field_name,
                              const char *section_name, double **out_values,
                              size_t *out_count, config_error_t *err) {
  *out_values = NULL;
  *out_count = 0;
  if (!cJSON_IsArray(arr))
    return 0;
  int size = cJSON_GetArraySize(arr);
  if (size <= 0)
    return 0;
  double *values = (double *)calloc((size_t)size, sizeof(double));
  if (!values) {
    config_error_set(err, CONFIG_ERR_PARSE, "out of memory parsing '%s' in %s",
                     field_name, section_name ? section_name : "object");
    return -1;
  }
  for (int i = 0; i < size; i++) {
    const cJSON *el = cJSON_GetArrayItem(arr, i);
    if (!cJSON_IsNumber(el)) {
      config_error_set(err, CONFIG_ERR_PARSE,
                       "element %d of '%s' in %s must be a number", i,
                       field_name, section_name ? section_name : "object");
      free(values);
      return -1;
    }
    values[i] = el->valuedouble;
  }
  *out_values = values;
  *out_count = (size_t)size;
  return 0;
}

int parse_json_str_strict(const cJSON *obj, const char *key,
                          const char *section_name, char *dest, size_t dest_sz,
                          bool *present, config_error_t *err) {
  if (present)
    *present = false;
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (!item || cJSON_IsNull(item))
    return 0;
  if (!cJSON_IsString(item) || !item->valuestring) {
    config_error_set(err, CONFIG_ERR_PARSE, "field '%s' in %s must be a string",
                     key, section_name ? section_name : "object");
    return -1;
  }
  size_t len = strlen(item->valuestring);
  if (dest_sz > 0 && len >= dest_sz) {
    config_error_set(err, CONFIG_ERR_PARSE,
                     "string '%s' in %s exceeds maximum length of %zu", key,
                     section_name ? section_name : "object", dest_sz - 1);
    return -1;
  }
  if (dest && dest_sz > 0) {
    memcpy(dest, item->valuestring, len + 1);
  }
  if (present)
    *present = true;
  return 0;
}

int parse_json_size_t_strict(const cJSON *obj, const char *key,
                             const char *section_name, size_t *dest,
                             bool *present, config_error_t *err) {
  if (present)
    *present = false;
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (!item || cJSON_IsNull(item))
    return 0;
  if (!cJSON_IsNumber(item) || item->valuedouble < 0.0 ||
      floor(item->valuedouble) != item->valuedouble ||
      item->valuedouble > (double)SIZE_MAX) {
    config_error_set(err, CONFIG_ERR_PARSE,
                     "field '%s' in %s must be a non-negative integer", key,
                     section_name ? section_name : "object");
    return -1;
  }
  if (dest)
    *dest = (size_t)item->valuedouble;
  if (present)
    *present = true;
  return 0;
}

int parse_json_int_strict(const cJSON *obj, const char *key,
                          const char *section_name, int *dest, bool *present,
                          config_error_t *err) {
  if (present)
    *present = false;
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (!item || cJSON_IsNull(item))
    return 0;
  if (!cJSON_IsNumber(item) || floor(item->valuedouble) != item->valuedouble ||
      item->valuedouble < (double)INT_MIN ||
      item->valuedouble > (double)INT_MAX) {
    config_error_set(err, CONFIG_ERR_PARSE,
                     "field '%s' in %s must be an integer", key,
                     section_name ? section_name : "object");
    return -1;
  }
  if (dest)
    *dest = (int)item->valuedouble;
  if (present)
    *present = true;
  return 0;
}

int parse_json_bool_strict(const cJSON *obj, const char *key,
                           const char *section_name, bool *dest, bool *present,
                           config_error_t *err) {
  if (present)
    *present = false;
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (!item || cJSON_IsNull(item))
    return 0;
  if (!cJSON_IsBool(item)) {
    config_error_set(err, CONFIG_ERR_PARSE,
                     "field '%s' in %s must be a boolean", key,
                     section_name ? section_name : "object");
    return -1;
  }
  if (dest)
    *dest = cJSON_IsTrue(item);
  if (present)
    *present = true;
  return 0;
}

int parse_json_double_strict(const cJSON *obj, const char *key,
                             const char *section_name, double *dest,
                             bool *present, config_error_t *err) {
  if (present)
    *present = false;
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (!item || cJSON_IsNull(item))
    return 0;
  if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble)) {
    config_error_set(err, CONFIG_ERR_PARSE,
                     "field '%s' in %s must be a finite number", key,
                     section_name ? section_name : "object");
    return -1;
  }
  if (dest)
    *dest = item->valuedouble;
  if (present)
    *present = true;
  return 0;
}

int validate_unknown_fields(const cJSON *obj, const char *const allowed_keys[],
                            const char *section_name, config_error_t *err) {
  if (!obj || !cJSON_IsObject(obj))
    return 0;
  const cJSON *child = NULL;
  cJSON_ArrayForEach(child, obj) {
    if (!child->string)
      continue;
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

void format_enum_variants(const config_enum_variant_t *variants, char *buf,
                          size_t buf_len) {
  if (!buf || buf_len == 0)
    return;
  buf[0] = '\0';
  size_t off = 0;
  for (size_t i = 0; variants && variants[i].name != NULL; i++) {
    int n = snprintf(buf + off, buf_len - off, "%s%s", i == 0 ? "" : ", ",
                     variants[i].name);
    if (n < 0 || (size_t)n >= buf_len - off) {
      // Ran out of room; leave an ellipsis so the message stays honest.
      if (buf_len >= 4) {
        snprintf(buf + (buf_len - 4), 4, "...");
      }
      return;
    }
    off += (size_t)n;
  }
}

int parse_enum_required(const cJSON *obj, const char *key,
                        const config_enum_variant_t *variants,
                        const char *section_name, int *out,
                        config_error_t *err) {
  const char *where = section_name ? section_name : "object";
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (!item) {
    config_error_set(err, CONFIG_ERR_PARSE, "missing field '%s' in %s", key,
                     where);
    return -1;
  }
  if (!cJSON_IsString(item) || !item->valuestring) {
    config_error_set(err, CONFIG_ERR_PARSE, "field '%s' in %s must be a string",
                     key, where);
    return -1;
  }
  for (size_t i = 0; variants && variants[i].name != NULL; i++) {
    if (strcmp(item->valuestring, variants[i].name) == 0) {
      if (out)
        *out = variants[i].value;
      return 0;
    }
  }
  char expected[320];
  format_enum_variants(variants, expected, sizeof(expected));
  config_error_set(err, CONFIG_ERR_PARSE,
                   "unknown variant '%s' for '%s' in %s, expected one of: %s",
                   item->valuestring, key, where, expected);
  return -1;
}

int parse_enum_optional(const cJSON *obj, const char *key,
                        const config_enum_variant_t *variants,
                        const char *section_name, int *out, bool *present,
                        config_error_t *err) {
  if (present)
    *present = false;
  if (!cJSON_GetObjectItemCaseSensitive(obj, key))
    return 0;
  if (present)
    *present = true;
  return parse_enum_required(obj, key, variants, section_name, out, err);
}

int require_json_fields(const cJSON *obj, const char *const keys[],
                        const char *section_name, const char *variant,
                        config_error_t *err) {
  const char *where = section_name ? section_name : "object";
  for (size_t i = 0; keys && keys[i] != NULL; i++) {
    if (!cJSON_GetObjectItemCaseSensitive(obj, keys[i])) {
      if (variant) {
        config_error_set(err, CONFIG_ERR_PARSE,
                         "missing field '%s' in %s for type '%s'", keys[i],
                         where, variant);
      } else {
        config_error_set(err, CONFIG_ERR_PARSE, "missing field '%s' in %s",
                         keys[i], where);
      }
      return -1;
    }
  }
  return 0;
}

static void replace_tokens_in_string(char *str, size_t max_len, int samplerate,
                                     int channels) {
  if (!str || (!strstr(str, "$samplerate$") && !strstr(str, "$channels$")))
    return;
  char sr_buf[32];
  char ch_buf[32];
  snprintf(sr_buf, sizeof(sr_buf), "%d", samplerate);
  snprintf(ch_buf, sizeof(ch_buf), "%d", channels);

  char buf[1024];
  size_t out_len = 0;
  size_t in_len = strlen(str);
  for (size_t i = 0; i < in_len;) {
    if (strncmp(str + i, "$samplerate$", 12) == 0) {
      size_t l = strlen(sr_buf);
      if (out_len + l < sizeof(buf)) {
        memcpy(buf + out_len, sr_buf, l);
        out_len += l;
      }
      i += 12;
    } else if (strncmp(str + i, "$channels$", 10) == 0) {
      size_t l = strlen(ch_buf);
      if (out_len + l < sizeof(buf)) {
        memcpy(buf + out_len, ch_buf, l);
        out_len += l;
      }
      i += 10;
    } else {
      if (out_len + 1 < sizeof(buf)) {
        buf[out_len++] = str[i];
      }
      i++;
    }
  }
  buf[out_len] = '\0';
  snprintf(str, max_len, "%s", buf);
}

static void replace_tokens_in_config(dsp_config_t *config, int samplerate,
                                     int channels) {
  if (!config)
    return;
  for (size_t i = 0; i < config->filters_count; i++) {
    replace_tokens_in_string(config->filters[i].name,
                             sizeof(config->filters[i].name), samplerate,
                             channels);
    if (config->filters[i].filter.type == FILTER_TYPE_CONV) {
      conv_config_t *conv = &config->filters[i].filter.parameters.conv;
      replace_tokens_in_string(conv->filename, sizeof(conv->filename),
                               samplerate, channels);
    }
  }
  for (size_t i = 0; i < config->mixers_count; i++) {
    replace_tokens_in_string(config->mixers[i].name,
                             sizeof(config->mixers[i].name), samplerate,
                             channels);
  }
  for (size_t i = 0; i < config->processors_count; i++) {
    replace_tokens_in_string(config->processors[i].name,
                             sizeof(config->processors[i].name), samplerate,
                             channels);
  }
  for (size_t i = 0; i < config->pipeline_count; i++) {
    pipeline_step_config_t *step = &config->pipeline[i];
    if (step->has_name) {
      replace_tokens_in_string(step->name, sizeof(step->name), samplerate,
                               channels);
    }
    if (step->has_names && step->names) {
      for (size_t j = 0; j < step->names_count; j++) {
        if (step->names[j]) {
          char temp[1024];
          snprintf(temp, sizeof(temp), "%s", step->names[j]);
          replace_tokens_in_string(temp, sizeof(temp), samplerate, channels);
          if (strcmp(temp, step->names[j]) != 0) {
            free(step->names[j]);
            step->names[j] = strdup(temp);
          }
        }
      }
    }
  }
}

static void resolve_relative_paths(dsp_config_t *config,
                                   const char *config_dir) {
  if (!config || !config_dir || config_dir[0] == '\0')
    return;
  for (size_t i = 0; i < config->filters_count; i++) {
    if (config->filters[i].filter.type == FILTER_TYPE_CONV) {
      conv_config_t *conv = &config->filters[i].filter.parameters.conv;
      const char *str = conv->filename;
      if (str[0] != '\0' && str[0] != '/' &&
          !(strlen(str) >= 2 && str[1] == ':')) {
        char resolved[1024];
        size_t dir_len = strlen(config_dir);
        bool needs_slash = (dir_len > 0 && config_dir[dir_len - 1] != '/');
        int n = snprintf(resolved, sizeof(resolved), "%s%s%s", config_dir,
                         needs_slash ? "/" : "", str);
        if (n >= 0 && (size_t)n < sizeof(resolved)) {
          if (access(resolved, F_OK) == 0) {
            snprintf(conv->filename, sizeof(conv->filename), "%s", resolved);
          }
        }
      }
    }
  }
}

int dsp_config_parse_json(const char *json, dsp_config_t **out_config,
                          config_error_t *err) {
  return dsp_config_parse_json_with_dir_and_overrides(json, NULL, NULL,
                                                      out_config, err);
}

int dsp_config_parse_json_with_dir(const char *json, const char *config_dir,
                                   dsp_config_t **out_config,
                                   config_error_t *err) {
  return dsp_config_parse_json_with_dir_and_overrides(json, config_dir, NULL,
                                                      out_config, err);
}

int dsp_config_parse_json_with_dir_and_overrides(
    const char *json, const char *config_dir,
    const dsp_config_overrides_t *overrides, dsp_config_t **out_config,
    config_error_t *err) {
  return dsp_config_parse_json_with_dir_and_overrides_ext(
      json, config_dir, overrides, out_config, true, err);
}

int dsp_config_parse_json_no_validate(const char *json,
                                      dsp_config_t **out_config,
                                      config_error_t *err) {
  return dsp_config_parse_json_with_dir_and_overrides_ext(
      json, NULL, NULL, out_config, false, err);
}

int dsp_config_parse_json_with_dir_and_overrides_ext(
    const char *json, const char *config_dir,
    const dsp_config_overrides_t *overrides, dsp_config_t **out_config,
    bool validate, config_error_t *err) {
  if (!json || !out_config) {
    config_error_set(err, CONFIG_ERR_PARSE,
                     "JSON string or output pointer is NULL");
    logger_error(
        &g_logger,
        "Config parsing failed: JSON string or output pointer is NULL");
    return -1;
  }

  *out_config = NULL;

  dsp_config_t *config = (dsp_config_t *)calloc(1, sizeof(dsp_config_t));
  if (!config) {
    config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
    logger_error(&g_logger, "Config parsing failed: Memory allocation failure");
    return -1;
  }

  cJSON *root = cJSON_Parse(json);
  if (!root) {
    dsp_config_free(config);
    config_error_set(err, CONFIG_ERR_PARSE,
                     "Failed to parse JSON (syntax error or invalid JSON)");
    logger_error(&g_logger,
                 "Config parsing failed: Syntax error or invalid JSON");
    return -1;
  }

  static const char *const allowed_root_keys[] = {
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

  if (parse_dsp_config(root, "root configuration", config, err) != 0) {
    cJSON_Delete(root);
    dsp_config_free(config);
    logger_error(&g_logger, "Config parsing failed: %s",
                 err ? err->message : "");
    return -1;
  }

  cJSON_Delete(root);

  // Apply WAV file and command-line overrides
  if (audio_backend_apply_device_overrides(&config->devices, overrides, err) !=
      0) {
    dsp_config_free(config);
    return -1;
  }

  // Token replacement and relative path resolution
  int final_sr = (int)config->devices.samplerate;
  int final_ch = capture_device_config_get_channels(&config->devices.capture);
  if (final_sr > 0 || final_ch > 0) {
    replace_tokens_in_config(config, final_sr, final_ch);
  }

  if (config_dir && config_dir[0] != '\0') {
    resolve_relative_paths(config, config_dir);
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

  if (validate) {
    if (dsp_config_validate(config, err) != 0) {
      logger_error(&g_logger, "Config validation failed: %s",
                   err ? err->message : "");
      dsp_config_free(config);
      return -1;
    }
  }

  logger_info(
      &g_logger,
      "Configuration successfully parsed (samplerate=%zu, chunksize=%zu)",
      config->devices.samplerate, config->devices.chunksize);
  *out_config = config;
  return 0;
}
