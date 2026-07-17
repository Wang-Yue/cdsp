#ifndef CDSP_PATH_H
#define CDSP_PATH_H

#include <stddef.h>
#include <stdio.h>

/**
 * @brief Expand leading tilde (~) in a file path to the user's home directory.
 *
 * @param path Input file path (e.g. "~/music.wav").
 * @param out_buf Output buffer to receive expanded path.
 * @param buf_len Size of output buffer.
 */
void cdsp_expand_path(const char* path, char* out_buf, size_t buf_len);

/**
 * @brief Open a file with automatic tilde (~) path expansion.
 *
 * @param path File path.
 * @param mode fopen mode string (e.g. "rb", "wb").
 * @return FILE pointer or NULL on failure.
 */
FILE* cdsp_fopen(const char* path, const char* mode);

#endif  // CDSP_PATH_H
