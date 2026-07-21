// Lock-free, allocation-free high performance logger for real-time audio
// threads

#ifndef CLIB_LOGGING_APP_LOGGER_H
#define CLIB_LOGGING_APP_LOGGER_H

/**
 * @file app_logger.h
 * @brief Lock-free, allocation-free logger for real-time audio threads.
 *
 * This module provides a logging framework optimized for execution in real-time
 * context, ensuring no memory allocations or blocking locks are encountered.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "Config/log_level.h"

/**
 * @brief Types of arguments that can be passed to log statements.
 */
typedef enum {
  LOG_ARG_NONE = 0,   /**< No argument */
  LOG_ARG_INT = 1,    /**< Integer argument (int64_t) */
  LOG_ARG_DOUBLE = 2, /**< Double argument (double) */
  LOG_ARG_STRING = 3  /**< String argument (const char*) */
} log_arg_type_t;

/**
 * @brief A container for a single log statement argument.
 */
typedef struct {
  log_arg_type_t type; /**< The type of the argument. */
  union {
    int64_t i;   /**< Integer value. */
    double d;    /**< Double value. */
    char s[256]; /**< String value copy (compact to prevent stack overflow). */
  } val;         /**< The union containing the argument value. */
} log_argument_t;

/**
 * @brief Creates an empty log argument.
 * @return An empty log_argument_t.
 */
static inline log_argument_t log_arg_none(void) {
  log_argument_t a = {LOG_ARG_NONE, {0}};
  return a;
}

/**
 * @brief Creates an integer log argument.
 * @param i The integer value.
 * @return A log_argument_t containing the integer.
 */
static inline log_argument_t log_arg_int(int64_t i) {
  log_argument_t a;
  a.type = LOG_ARG_INT;
  a.val.i = i;
  return a;
}

/**
 * @brief Creates a double precision floating point log argument.
 * @param d The double value.
 * @return A log_argument_t containing the double.
 */
static inline log_argument_t log_arg_double(double d) {
  log_argument_t a;
  a.type = LOG_ARG_DOUBLE;
  a.val.d = d;
  return a;
}

/**
 * @brief Creates a string log argument.
 * @param s The string pointer.
 * @return A log_argument_t containing the string.
 */
static inline log_argument_t log_arg_string(const char* s) {
  log_argument_t a;
  a.type = LOG_ARG_STRING;
  if (s) {
    size_t len = strlen(s);
    size_t max_len = sizeof(a.val.s) - 1;
    if (len > max_len) {
      len = max_len;
    }
    memcpy(a.val.s, s, len);
    a.val.s[len] = '\0';
  } else {
    a.val.s[0] = '\0';
  }
  return a;
}

/**
 * @brief Represents a single log record entry.
 */
typedef struct {
  log_level_t level;   /**< Log level of this record. */
  const char* label;   /**< Component label. */
  const char* message; /**< Message format string. */
  log_argument_t arg1; /**< First optional argument. */
  log_argument_t arg2; /**< Second optional argument. */
  log_argument_t arg3; /**< Third optional argument. */
  log_argument_t arg4; /**< Fourth optional argument. */
} log_record_t;

/**
 * @brief Opaque struct representing the logger backend.
 */
struct app_logger_s;
typedef struct app_logger_s app_logger_t;

/**
 * @brief Eagerly initializes the logger singleton and background thread.
 *
 * Calling this before logging ensures that the very first log statement is
 * 100% allocation-free and produces no thread startup overhead.
 */
void app_logger_init(void);

/**
 * @brief Gets the shared singleton instance of the logger.
 * @return Pointer to the shared app_logger_t instance.
 */
app_logger_t* app_logger_get_shared(void);

/**
 * @brief Gets the process-wide log level.
 *
 * Stored as an atomic uint8_t so the real-time audio path can read it without
 * locks.
 *
 * @return The current log_level_t.
 */
log_level_t app_logger_get_level(void);

/**
 * @brief Sets the process-wide log level.
 * @param level The new log_level_t.
 */
void app_logger_set_level(log_level_t level);

/**
 * @brief Logs a message using the specified logger.
 *
 * @param logger Pointer to the logger instance.
 * @param level Log severity level.
 * @param label Component label.
 * @param message Message format string.
 * @param arg1 First optional argument.
 * @param arg2 Second optional argument.
 * @param arg3 Third optional argument.
 * @param arg4 Fourth optional argument.
 */
void app_logger_log(app_logger_t* logger, log_level_t level, const char* label,
                    const char* message, log_argument_t arg1,
                    log_argument_t arg2, log_argument_t arg3,
                    log_argument_t arg4);

/**
 * @brief Flushes any pending log records and stops the logger thread/backend.
 * @param logger Pointer to the logger instance.
 */
void app_logger_flush_and_stop(app_logger_t* logger);

/**
 * @brief Logger handle used for components.
 */
typedef struct {
  const char*
      label; /**< Label prepended to all messages logged using this handle. */
} logger_t;

/**
 * @brief Creates a logger handle with a specific label.
 * @param label The label string.
 * @return A new logger_t handle.
 */
static inline logger_t logger_create(const char* label) {
  logger_t l = {label};
  return l;
}

/**
 * @brief Implementation function for logging an informational message.
 * @param logger Pointer to the logger handle.
 * @param msg Message format string.
 * @param a1 First optional argument.
 * @param a2 Second optional argument.
 * @param a3 Third optional argument.
 * @param a4 Fourth optional argument.
 */
static inline void logger_info_impl(const logger_t* logger, const char* msg,
                                    log_argument_t a1, log_argument_t a2,
                                    log_argument_t a3, log_argument_t a4) {
  app_logger_log(app_logger_get_shared(), LOG_LEVEL_INFO, logger->label, msg,
                 a1, a2, a3, a4);
}

/**
 * @brief Implementation function for logging a warning message.
 * @param logger Pointer to the logger handle.
 * @param msg Message format string.
 * @param a1 First optional argument.
 * @param a2 Second optional argument.
 * @param a3 Third optional argument.
 * @param a4 Fourth optional argument.
 */
static inline void logger_warn_impl(const logger_t* logger, const char* msg,
                                    log_argument_t a1, log_argument_t a2,
                                    log_argument_t a3, log_argument_t a4) {
  app_logger_log(app_logger_get_shared(), LOG_LEVEL_WARN, logger->label, msg,
                 a1, a2, a3, a4);
}

/**
 * @brief Implementation function for logging an error message.
 * @param logger Pointer to the logger handle.
 * @param msg Message format string.
 * @param a1 First optional argument.
 * @param a2 Second optional argument.
 * @param a3 Third optional argument.
 * @param a4 Fourth optional argument.
 */
static inline void logger_error_impl(const logger_t* logger, const char* msg,
                                     log_argument_t a1, log_argument_t a2,
                                     log_argument_t a3, log_argument_t a4) {
  app_logger_log(app_logger_get_shared(), LOG_LEVEL_ERROR, logger->label, msg,
                 a1, a2, a3, a4);
}

/**
 * @brief Implementation function for logging a debugging message.
 * @param logger Pointer to the logger handle.
 * @param msg Message format string.
 * @param a1 First optional argument.
 * @param a2 Second optional argument.
 * @param a3 Third optional argument.
 * @param a4 Fourth optional argument.
 */
static inline void logger_debug_impl(const logger_t* logger, const char* msg,
                                     log_argument_t a1, log_argument_t a2,
                                     log_argument_t a3, log_argument_t a4) {
  app_logger_log(app_logger_get_shared(), LOG_LEVEL_DEBUG, logger->label, msg,
                 a1, a2, a3, a4);
}

/**
 * @brief Implementation function for logging a trace message.
 * @param logger Pointer to the logger handle.
 * @param msg Message format string.
 * @param a1 First optional argument.
 * @param a2 Second optional argument.
 * @param a3 Third optional argument.
 * @param a4 Fourth optional argument.
 */
static inline void logger_trace_impl(const logger_t* logger, const char* msg,
                                     log_argument_t a1, log_argument_t a2,
                                     log_argument_t a3, log_argument_t a4) {
  app_logger_log(app_logger_get_shared(), LOG_LEVEL_TRACE, logger->label, msg,
                 a1, a2, a3, a4);
}

static inline log_argument_t _log_arg_auto_int(int64_t v) {
  return log_arg_int(v);
}
static inline log_argument_t _log_arg_auto_double(double v) {
  return log_arg_double(v);
}
static inline log_argument_t _log_arg_auto_string(const char* s) {
  return log_arg_string(s);
}
static inline log_argument_t _log_arg_auto_struct(log_argument_t a) {
  return a;
}

static inline double _log_float_to_dbl(float f) { return (double)f; }
static inline double _log_dbl_to_dbl(double d) { return d; }

static inline double _log_float_val(float f) { return (double)f; }
static inline double _log_dbl_val(double d) { return d; }

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define _LOG_VAL_STRUCT(x) \
  _Generic((x), log_argument_t: (x), default: log_arg_none())

#define _LOG_DBL_ARG(x) \
  _Generic((x) + 0, char*: 0.0, const char*: 0.0, default: (x))

#define _LOG_VAL_INT(x)    \
  _Generic((x),            \
      log_argument_t: 0LL, \
      float: 0LL,          \
      double: 0LL,         \
      default: (int64_t)(uintptr_t)(uint64_t)(x))

#define _LOG_VAL_STR(x)       \
  _Generic((x),               \
      log_argument_t: "",     \
      float: "",              \
      double: "",             \
      bool: "",               \
      char: "",               \
      signed char: "",        \
      unsigned char: "",      \
      short: "",              \
      unsigned short: "",     \
      int: "",                \
      unsigned int: "",       \
      long: "",               \
      unsigned long: "",      \
      long long: "",          \
      unsigned long long: "", \
      default: (const char*)(uintptr_t)(uint64_t)(x))

#define log_arg(x)                                                  \
  _Generic((x),                                                     \
      log_argument_t: _log_arg_auto_struct(_LOG_VAL_STRUCT(x)),     \
      float: _log_arg_auto_double(_log_float_val(_LOG_DBL_ARG(x))), \
      double: _log_arg_auto_double(_log_dbl_val(_LOG_DBL_ARG(x))),  \
      default: _Generic((x) + 0,                                    \
          char*: _log_arg_auto_string(_LOG_VAL_STR(x)),             \
          const char*: _log_arg_auto_string(_LOG_VAL_STR(x)),       \
          default: _log_arg_auto_int(_LOG_VAL_INT(x))))
#else
#define log_arg(x) (x)
#endif

#define _LOG_PAD_0() \
  log_arg_none(), log_arg_none(), log_arg_none(), log_arg_none()
#define _LOG_PAD_1(a1) \
  log_arg(a1), log_arg_none(), log_arg_none(), log_arg_none()
#define _LOG_PAD_2(a1, a2) \
  log_arg(a1), log_arg(a2), log_arg_none(), log_arg_none()
#define _LOG_PAD_3(a1, a2, a3) \
  log_arg(a1), log_arg(a2), log_arg(a3), log_arg_none()
#define _LOG_PAD_4(a1, a2, a3, a4) \
  log_arg(a1), log_arg(a2), log_arg(a3), log_arg(a4)

#define _LOG_GET_MACRO(_0, _1, _2, _3, _4, NAME, ...) NAME
#define _LOG_PAD(...)                                                  \
  _LOG_GET_MACRO(0 __VA_OPT__(, ) __VA_ARGS__, _LOG_PAD_4, _LOG_PAD_3, \
                 _LOG_PAD_2, _LOG_PAD_1, _LOG_PAD_0)(__VA_ARGS__)

/**
 * @brief Logs an informational message with 0 to 4 optional log arguments.
 * @param logger Pointer to the logger handle.
 * @param msg Message format string.
 * @param ... Optional log arguments (log_arg_int, log_arg_double,
 * log_arg_string).
 */
#define logger_info(logger, msg, ...) \
  logger_info_impl((logger), (msg), _LOG_PAD(__VA_ARGS__))

/**
 * @brief Logs a warning message with 0 to 4 optional log arguments.
 * @param logger Pointer to the logger handle.
 * @param msg Message format string.
 * @param ... Optional log arguments (log_arg_int, log_arg_double,
 * log_arg_string).
 */
#define logger_warn(logger, msg, ...) \
  logger_warn_impl((logger), (msg), _LOG_PAD(__VA_ARGS__))

/**
 * @brief Logs an error message with 0 to 4 optional log arguments.
 * @param logger Pointer to the logger handle.
 * @param msg Message format string.
 * @param ... Optional log arguments (log_arg_int, log_arg_double,
 * log_arg_string).
 */
#define logger_error(logger, msg, ...) \
  logger_error_impl((logger), (msg), _LOG_PAD(__VA_ARGS__))

/**
 * @brief Logs a debugging message with 0 to 4 optional log arguments.
 * @param logger Pointer to the logger handle.
 * @param msg Message format string.
 * @param ... Optional log arguments (log_arg_int, log_arg_double,
 * log_arg_string).
 */
#define logger_debug(logger, msg, ...) \
  logger_debug_impl((logger), (msg), _LOG_PAD(__VA_ARGS__))

/**
 * @brief Logs a trace message with 0 to 4 optional log arguments.
 * @param logger Pointer to the logger handle.
 * @param msg Message format string.
 * @param ... Optional log arguments.
 */
#define logger_trace(logger, msg, ...) \
  logger_trace_impl((logger), (msg), _LOG_PAD(__VA_ARGS__))

/**
 * @brief Logs a control-plane message with a direct raw string parameter.
 *
 * Bypasses log_argument_t array packing for long control-plane strings
 * (such as JSON configurations) to allow unlimited string length without
 * stack allocation overhead or truncation.
 *
 * @param logger Pointer to the logger handle.
 * @param level Severity level.
 * @param msg Message format string or prefix.
 * @param str Raw string pointer to log.
 */
void app_logger_log_raw_str(const logger_t* logger, log_level_t level,
                            const char* msg, const char* str);

/**
 * @brief Logs an informational control-plane message with a direct raw string.
 */
#define logger_info_str(logger, msg, str) \
  app_logger_log_raw_str((logger), LOG_LEVEL_INFO, (msg), (str))

#endif  // CLIB_LOGGING_APP_LOGGER_H
