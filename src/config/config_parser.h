#ifndef CDSP_CONFIG_PARSER_H
#define CDSP_CONFIG_PARSER_H

/**
 * @file config_parser.h
 * @brief Utility helpers and parsers for configuration schemas.
 */

#include <stdbool.h>
#include <stddef.h>

#include "config/config_error.h"

typedef struct cJSON cJSON;

/**
 * @brief One accepted spelling of a string-tagged enum variant.
 */
typedef struct {
  const char *name; /**< The variant name as it appears in the config. */
  int value;        /**< The enum value it maps to. */
} config_enum_variant_t;

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
 * @brief Parses an array of double numbers with strict error checking.
 */
int parse_double_array_strict(const cJSON *arr, const char *field_name,
                              const char *section_name, double **out_values,
                              size_t *out_count, config_error_t *err);

/**
 * @brief Parses a scalar string field, validating type and max destination
 * size.
 *
 * @param obj The object holding the field.
 * @param key The field name.
 * @param section_name Section name for error reporting.
 * @param dest Receives the string value when present.
 * @param dest_sz Destination buffer size.
 * @param present Optional; set to whether the field was present.
 * @param err Optional error sink.
 * @return 0 if absent or valid, -1 if present and invalid.
 */
int parse_json_str_strict(const cJSON *obj, const char *key,
                          const char *section_name, char *dest, size_t dest_sz,
                          bool *present, config_error_t *err);

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
int parse_json_size_t_strict(const cJSON *obj, const char *key,
                             const char *section_name, size_t *dest,
                             bool *present, config_error_t *err);

/**
 * @brief Parses a scalar int field, rejecting non-integers or out-of-range
 * values.
 */
int parse_json_int_strict(const cJSON *obj, const char *key,
                          const char *section_name, int *dest, bool *present,
                          config_error_t *err);

/**
 * @brief Parses a scalar boolean field, rejecting non-boolean values.
 */
int parse_json_bool_strict(const cJSON *obj, const char *key,
                           const char *section_name, bool *dest, bool *present,
                           config_error_t *err);

/**
 * @brief Parses a scalar finite double field, rejecting non-finite values.
 */
int parse_json_double_strict(const cJSON *obj, const char *key,
                             const char *section_name, double *dest,
                             bool *present, config_error_t *err);

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
int validate_unknown_fields(const cJSON *obj, const char *const allowed_keys[],
                            const char *section_name, config_error_t *err);

/**
 * @brief Formats the accepted variant names as "A, B, C" for an error message.
 *
 * @param variants Array of variants, terminated by a NULL `name`.
 * @param buf Destination buffer.
 * @param buf_len Size of @p buf.
 */
void format_enum_variants(const config_enum_variant_t *variants, char *buf,
                          size_t buf_len);

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
int parse_enum_required(const cJSON *obj, const char *key,
                        const config_enum_variant_t *variants,
                        const char *section_name, int *out,
                        config_error_t *err);

/**
 * @brief Like parse_enum_required(), but the field may be absent.
 *
 * When the key is missing, @p out is left untouched and 0 is returned. A
 * present-but-invalid value is still an error.
 *
 * @return 0 if absent or valid, -1 if present and invalid.
 */
int parse_enum_optional(const cJSON *obj, const char *key,
                        const config_enum_variant_t *variants,
                        const char *section_name, int *out, bool *present,
                        config_error_t *err);

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
int require_json_fields(const cJSON *obj, const char *const keys[],
                        const char *section_name, const char *variant,
                        config_error_t *err);

#endif // CDSP_CONFIG_PARSER_H
