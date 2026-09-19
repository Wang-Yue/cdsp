#ifndef CDSP_MACROS_H
#define CDSP_MACROS_H

/**
 * @file cdsp_macros.h
 * @brief Internal utility macros for assertions, unreachable annotations, etc.
 */

#include <assert.h>

#ifndef CDSP_UNREACHABLE
#if defined(__GNUC__) || defined(__clang__)
#define CDSP_UNREACHABLE()                                                     \
  do {                                                                         \
    assert(0 && "Unreachable code reached");                                   \
    __builtin_unreachable();                                                   \
  } while (0)
#elif defined(_MSC_VER)
#define CDSP_UNREACHABLE()                                                     \
  do {                                                                         \
    assert(0 && "Unreachable code reached");                                   \
    __assume(0);                                                               \
  } while (0)
#else
#define CDSP_UNREACHABLE()                                                     \
  do {                                                                         \
    assert(0 && "Unreachable code reached");                                   \
  } while (0)
#endif
#endif

#endif // CDSP_MACROS_H
