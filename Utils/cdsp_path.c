#include "Utils/cdsp_path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void cdsp_expand_path(const char* path, char* out_buf, size_t buf_len) {
  if (!path || !out_buf || buf_len == 0) return;

  if (path[0] == '~' && (path[1] == '/' || path[1] == '\0')) {
    const char* home = getenv("HOME");
#if defined(_WIN32)
    if (!home) home = getenv("USERPROFILE");
#endif
    if (home && home[0] != '\0') {
      snprintf(out_buf, buf_len, "%s%s", home, path + 1);
      return;
    }
  }

  strncpy(out_buf, path, buf_len - 1);
  out_buf[buf_len - 1] = '\0';
}

FILE* cdsp_fopen(const char* path, const char* mode) {
  if (!path) return NULL;
  char expanded[1024];
  cdsp_expand_path(path, expanded, sizeof(expanded));
  return fopen(expanded, mode);
}
