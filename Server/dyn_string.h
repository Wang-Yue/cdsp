#ifndef CLIB_SERVER_DYN_STRING_H
#define CLIB_SERVER_DYN_STRING_H

#include <stddef.h>

typedef struct {
  char* data;
  size_t capacity;
  size_t length;
} dyn_string_t;

void dyn_string_init(dyn_string_t* ds, size_t initial_cap);
void dyn_string_free(dyn_string_t* ds);
void dyn_string_printf(dyn_string_t* ds, const char* fmt, ...);

#endif  // CLIB_SERVER_DYN_STRING_H
