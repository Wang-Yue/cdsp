/**
 * @file cdsp_memory.h
 * @brief Cross-platform aligned memory allocation helpers.
 */

#ifndef CDSP_UTILS_MEMORY_H
#define CDSP_UTILS_MEMORY_H

#if defined(__APPLE__) || defined(__linux__) || defined(__FreeBSD__) || \
    defined(__NetBSD__) || defined(__OpenBSD__)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include <stddef.h>
#include <stdlib.h>

#if defined(_WIN32)
#include <malloc.h>
#endif

/**
 * @brief Allocate memory aligned to the specified byte boundary.
 *
 * @param alignment Alignment boundary in bytes (must be a power of 2).
 * @param size Number of bytes to allocate.
 * @return Pointer to allocated memory, or NULL on failure.
 */
static inline void* cdsp_aligned_alloc(size_t alignment, size_t size) {
#if defined(_WIN32)
  return _aligned_malloc(size, alignment);
#elif defined(__APPLE__) || defined(__linux__) || defined(__FreeBSD__) || \
    defined(__NetBSD__) || defined(__OpenBSD__)
  void* ptr = NULL;
  if (posix_memalign(&ptr, alignment, size) != 0) {
    return NULL;
  }
  return ptr;
#else
  size_t rounded_size = (size + alignment - 1) & ~(alignment - 1);
  return aligned_alloc(alignment, rounded_size);
#endif
}

/**
 * @brief Free memory previously allocated by @ref cdsp_aligned_alloc.
 *
 * @param ptr Pointer to memory to free (may be NULL).
 */
static inline void cdsp_aligned_free(void* ptr) {
  if (!ptr) return;
#if defined(_WIN32)
  _aligned_free(ptr);
#else
  free(ptr);
#endif
}

#endif  // CDSP_UTILS_MEMORY_H
