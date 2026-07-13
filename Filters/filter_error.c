#include "Filters/filter_error.h"

#include <stdarg.h>
#include <stdio.h>

const char* filter_error_to_string(filter_error_code_t code) {
  switch (code) {
    case FILTER_ERR_NONE:
      return "Success";
    case FILTER_ERR_INVALID_PARAM:
      return "Invalid parameter";
    case FILTER_ERR_UNSUPPORTED_SAMPLE_RATE:
      return "Unsupported sample rate";
    case FILTER_ERR_ALLOC_FAILURE:
      return "Allocation failure";
    case FILTER_ERR_FILE_IO:
      return "File I/O error";
    case FILTER_ERR_INVALID_STATE:
      return "Invalid state";
    default:
      return "Unknown filter error";
  }
}

void filter_error_set(filter_error_t* err, filter_error_code_t code,
                      const char* format, ...) {
  if (!err) return;
  err->code = code;
  if (format) {
    va_list args;
    va_start(args, format);
    vsnprintf(err->message, sizeof(err->message), format, args);
    va_end(args);
  } else {
    snprintf(err->message, sizeof(err->message), "%s",
             filter_error_to_string(code));
  }
}
