#ifndef CDSP_CONFIG_PARSER_INTERNAL_H
#define CDSP_CONFIG_PARSER_INTERNAL_H

/**
 * @file config_parser_internal.h
 * @brief Internal utility helpers shared across config sub-parsers.
 */

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "config/cJSON.h"

/**
 * @brief Parses an array of string labels from a cJSON array.
 *
 * @param labels_arr The cJSON array containing the labels.
 * @param out_labels Output pointer to store allocated array of string pointers.
 * @param out_count Output pointer to store the size of the parsed labels array.
 * @param out_has_labels Output pointer set to true if labels were successfully
 * parsed.
 */
void parse_labels_array(const cJSON *labels_arr, char ***out_labels,
                        size_t *out_count, bool *out_has_labels);

/**
 * @brief Parses an array of string labels with strict error reporting.
 *
 * @param labels_arr The cJSON array containing the labels.
 * @param out_labels Output pointer to store allocated array of string pointers.
 * @param out_count Output pointer to store the size of the parsed labels array.
 * @param out_has_labels Output pointer set to true if labels were present.
 * @return 0 on success, -1 on failure.
 */
int parse_labels_array_strict(const cJSON *labels_arr, char ***out_labels,
                              size_t *out_count, bool *out_has_labels);

/**
 * @brief Parses an array of double numbers from a cJSON array.
 *
 * @param arr The cJSON array containing floating point values.
 * @param out_count Output pointer storing array length.
 * @return Dynamically allocated double array or NULL.
 */
double *parse_double_array(const cJSON *arr, size_t *out_count);

static inline bool parse_json_str(const cJSON *obj, const char *key, char *dest,
                                  size_t dest_sz) {
  if (dest_sz == 0)
    return false;
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (cJSON_IsString(item) && item->valuestring) {
    size_t len = strlen(item->valuestring);
    if (len >= dest_sz) {
      return false;
    }
    memcpy(dest, item->valuestring, len);
    dest[len] = '\0';
    return true;
  }
  return false;
}

static inline bool parse_json_int(const cJSON *obj, const char *key,
                                  int *dest) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (cJSON_IsNumber(item) && item->valuedouble == (double)item->valueint) {
    *dest = item->valueint;
    return true;
  }
  return false;
}

/* parse_json_size_t() was removed: it answered `false` for a negative value,
 * which every caller treated as "absent" and so left the destination at 0 --
 * `channel: -1` silently became channel 0. Use parse_json_size_t_strict()
 * below, which reports the error. */

static inline bool parse_json_bool(const cJSON *obj, const char *key,
                                   bool *dest) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (cJSON_IsBool(item)) {
    *dest = cJSON_IsTrue(item);
    return true;
  }
  return false;
}

static inline bool parse_json_double(const cJSON *obj, const char *key,
                                     double *dest) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (cJSON_IsNumber(item)) {
    *dest = item->valuedouble;
    return true;
  }
  return false;
}

#include "config/config_error.h"

/**
 * @brief Parses an array of size_t numbers, rejecting anything that is not a
 * non-negative integer.
 *
 * Upstream these arrays are `Vec<usize>`, so a negative, fractional or
 * non-numeric element fails the load. The lenient parse_size_t_array() leaves
 * the calloc'ed zero in place instead, turning `channels: [-1, 1]` into
 * "channel 0 and channel 1" — which then trips the port's own
 * duplicate-channel check if channel 0 is also listed legitimately.
 *
 * @param arr The cJSON array; a non-array yields an empty result.
 * @param field_name Field name for error reporting.
 * @param section_name Section name for error reporting.
 * @param out_values Receives the allocated array (NULL when empty).
 * @param out_count Receives the element count.
 * @param err Optional error sink.
 * @return 0 on success, -1 if any element is invalid.
 */
int parse_size_t_array_strict(const cJSON *arr, const char *field_name,
                              const char *section_name, size_t **out_values,
                              size_t *out_count, config_error_t *err);

/**
 * @brief Parses a scalar size_t field, rejecting negative or fractional values.
 *
 * The lenient parse_json_size_t() simply returns false for `channel: -1`,
 * which leaves the destination at its zero-initialised value: the config is
 * accepted and the mapping silently addresses channel 0.
 *
 * @param obj The object holding the field.
 * @param key The field name.
 * @param section_name Section name for error reporting.
 * @param dest Receives the value when present.
 * @param present Optional; set to whether the field was present.
 * @param err Optional error sink.
 * @return 0 if absent or valid, -1 if present and invalid.
 */
static inline int parse_json_str_strict(const cJSON *obj, const char *key,
                                        const char *section_name, char *dest,
                                        size_t dest_sz, bool *present,
                                        config_error_t *err) {
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

static inline int parse_json_size_t_strict(const cJSON *obj, const char *key,
                                           const char *section_name,
                                           size_t *dest, bool *present,
                                           config_error_t *err) {
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

static inline int parse_json_bool_strict(const cJSON *obj, const char *key,
                                         const char *section_name, bool *dest,
                                         bool *present, config_error_t *err) {
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

static inline int parse_json_double_strict(const cJSON *obj, const char *key,
                                           const char *section_name,
                                           double *dest, bool *present,
                                           config_error_t *err) {
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
static inline int validate_unknown_fields(const cJSON *obj,
                                          const char *const allowed_keys[],
                                          const char *section_name,
                                          config_error_t *err) {
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

/**
 * @brief One accepted spelling of a string-tagged enum variant.
 */
typedef struct {
  const char *name; /**< The variant name as it appears in the config. */
  int value;        /**< The enum value it maps to. */
} config_enum_variant_t;

/**
 * @brief Formats the accepted variant names as "A, B, C" for an error message.
 *
 * @param variants Array of variants, terminated by a NULL `name`.
 * @param buf Destination buffer.
 * @param buf_len Size of @p buf.
 */
static inline void format_enum_variants(const config_enum_variant_t *variants,
                                        char *buf, size_t buf_len) {
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

/**
 * @brief Parses a string-tagged enum field, failing on anything unrecognised.
 *
 * Upstream derives `Deserialize` for these enums with no `#[serde(default)]`,
 * so a missing tag, a non-string tag, or a misspelled variant is a startup
 * error. Falling back to the zero variant instead (which is what a bare
 * if/else chain over a zero-initialised struct does) silently substitutes a
 * different filter -- for a biquad that means all-zero coefficients, i.e.
 * digital silence with no diagnostic.
 *
 * @param obj The object holding the field.
 * @param key The field name, normally "type".
 * @param variants Accepted variants, terminated by a NULL `name`.
 * @param section_name Section name used in error messages.
 * @param out Receives the parsed enum value on success.
 * @param err Optional error sink.
 * @return 0 on success, -1 on any failure.
 */
static inline int parse_enum_required(const cJSON *obj, const char *key,
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

/**
 * @brief Like parse_enum_required(), but the field may be absent.
 *
 * When the key is missing, @p out is left untouched and 0 is returned. A
 * present-but-invalid value is still an error.
 *
 * @return 0 if absent or valid, -1 if present and invalid.
 */
static inline int parse_enum_optional(const cJSON *obj, const char *key,
                                      const config_enum_variant_t *variants,
                                      const char *section_name, int *out,
                                      config_error_t *err) {
  if (!cJSON_GetObjectItemCaseSensitive(obj, key))
    return 0;
  return parse_enum_required(obj, key, variants, section_name, out, err);
}

/**
 * @brief Requires that a set of fields is present on an object.
 *
 * Upstream's parameter structs declare these fields without
 * `#[serde(default)]`, so serde reports "missing field `freq`" and the whole
 * configuration is rejected. The port used to leave the corresponding struct
 * member at its zero-initialised value, which for a biquad means a 0 Hz
 * corner frequency or a zero Q -- NaN coefficients, i.e. silence.
 *
 * @param obj The object holding the fields.
 * @param keys NULL-terminated array of required field names.
 * @param section_name Section name used in error messages.
 * @param variant Optional variant name to name in the message, may be NULL.
 * @param err Optional error sink.
 * @return 0 if all present, -1 otherwise.
 */
static inline int require_json_fields(const cJSON *obj,
                                      const char *const keys[],
                                      const char *section_name,
                                      const char *variant,
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

/**
 * @brief Requires that at least one of a set of alternative fields is present.
 *
 * Models upstream's untagged helper enums (`NotchWidth`, `PeakingWidth`,
 * `ShelfSteepness`), where the width can be given either as `q` or as
 * `bandwidth`/`slope` but one of them must be there for any variant to match.
 *
 * @param obj The object holding the fields.
 * @param keys NULL-terminated array of alternative field names.
 * @param section_name Section name used in error messages.
 * @param variant Optional variant name to name in the message, may be NULL.
 * @param err Optional error sink.
 * @return 0 if at least one is present, -1 otherwise.
 */
static inline int require_json_any_field(const cJSON *obj,
                                         const char *const keys[],
                                         const char *section_name,
                                         const char *variant,
                                         config_error_t *err) {
  const char *where = section_name ? section_name : "object";
  char expected[128];
  size_t off = 0;
  expected[0] = '\0';
  for (size_t i = 0; keys && keys[i] != NULL; i++) {
    if (cJSON_GetObjectItemCaseSensitive(obj, keys[i]))
      return 0;
    int n = snprintf(expected + off, sizeof(expected) - off, "%s'%s'",
                     i == 0 ? "" : " or ", keys[i]);
    if (n > 0 && (size_t)n < sizeof(expected) - off)
      off += (size_t)n;
  }
  if (variant) {
    config_error_set(err, CONFIG_ERR_PARSE,
                     "missing field %s in %s for type '%s'", expected, where,
                     variant);
  } else {
    config_error_set(err, CONFIG_ERR_PARSE, "missing field %s in %s", expected,
                     where);
  }
  return -1;
}

#endif // CDSP_CONFIG_PARSER_INTERNAL_H
