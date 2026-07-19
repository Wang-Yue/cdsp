#ifndef CDSP_EXPORT_H
#define CDSP_EXPORT_H

/**
 * @file cdsp_export.h
 * @brief Public API symbol visibility and export/import macro definitions.
 */

#if defined(_WIN32) || defined(__CYGWIN__)
  #if defined(CDSP_BUILD_SHARED)
    #define CDSP_API __declspec(dllexport)
  #elif defined(CDSP_USE_SHARED)
    #define CDSP_API __declspec(dllimport)
  #else
    #define CDSP_API
  #endif
#else
  #if defined(__GNUC__) && __GNUC__ >= 4
    #define CDSP_API __attribute__((visibility("default")))
  #else
    #define CDSP_API
  #endif
#endif

#endif  // CDSP_EXPORT_H
