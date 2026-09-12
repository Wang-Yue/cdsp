#ifndef CDSP_FILTERS_FILTER_ERROR_H
#define CDSP_FILTERS_FILTER_ERROR_H

/**
 * @file filter_error.h
 * @brief Standardized error codes and diagnostic utilities for DSP filter
 * sub-modules.
 */

#include <stdbool.h>
#include <stddef.h>

/**
 * @brief Standard error codes for DSP filters.
 */
typedef enum {
  FILTER_ERR_NONE = 0,      /**< Operation succeeded without error. */
  FILTER_ERR_INVALID_PARAM, /**< Invalid filter parameter or out-of-range value.
                             */
  FILTER_ERR_UNSUPPORTED_SAMPLE_RATE, /**< Unsupported sample rate for filter
                                         coefficients. */
  FILTER_ERR_ALLOC_FAILURE, /**< Memory allocation failure during filter
                               creation. */
  FILTER_ERR_FILE_IO,       /**< File load failure for FIR coefficient files. */
  FILTER_ERR_INVALID_STATE  /**< Filter is in an uninitialized or invalid state.
                             */
} filter_error_code_t;

/**
 * @brief Standardized filter error structure.
 */
typedef struct {
  filter_error_code_t code;
  char message[256];
} filter_error_t;

/**
 * @brief Convert a filter error code to a human-readable description string.
 *
 * @param code The filter error code.
 * @return C string describing the error code.
 */
const char* filter_error_to_string(filter_error_code_t code);

/**
 * @brief Populate a filter_error_t structure.
 *
 * @param err Pointer to error structure (ignored if NULL).
 * @param code Error status code.
 * @param format Printf-style format string for error details.
 */
void filter_error_set(filter_error_t* err, filter_error_code_t code,
                      const char* format, ...);

#endif  // CDSP_FILTERS_FILTER_ERROR_H
