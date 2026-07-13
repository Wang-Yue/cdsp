#ifndef CDSP_CONFIG_PARSER_INTERNAL_H
#define CDSP_CONFIG_PARSER_INTERNAL_H

/**
 * @file config_parser_internal.h
 * @brief Internal utility helpers shared across config sub-parsers.
 */

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"

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

#endif  // CDSP_CONFIG_PARSER_INTERNAL_H
