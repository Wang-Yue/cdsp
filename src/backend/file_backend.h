/**
 * @file file_backend.h
 * @brief File-based audio capture and playback backends.
 */

#ifndef CLIB_BACKEND_FILE_BACKEND_H
#define CLIB_BACKEND_FILE_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "backend/audio_backend.h"
#include "wav/wav_reader.h"
#include "wav/wav_types.h"

/**
 * @brief Opaque structure representing the file capture backend.
 */
typedef struct file_capture file_capture_t;

/**
 * @brief Opaque structure representing the file playback backend.
 */
typedef struct file_playback file_playback_t;

/**
 * @brief Global virtual method table for File capture backend.
 */
extern const capture_backend_vtable_t g_file_capture_vtable;

/**
 * @brief Global virtual method table for File playback backend.
 */
extern const playback_backend_vtable_t g_file_playback_vtable;

/**
 * @brief Set the pipeline (playback) sample rate for file capture backend.
 *
 * Plumbs the downstream pipeline sample rate so that resampling_ratio and
 * extra_samples scaling match CamillaDSP upstream when capture rate != pipeline
 * rate.
 */
void file_capture_set_pipeline_sample_rate(capture_backend_t *backend,
                                           int pipeline_sample_rate);

/**
 * @brief Explicitly set the resampling ratio for file capture backend.
 */
void file_capture_set_resampling_ratio(capture_backend_t *backend,
                                       double ratio);

/**
 * @brief Get the current resampling ratio for file capture backend.
 */
double file_capture_get_resampling_ratio(const capture_backend_t *backend);

#endif // CLIB_BACKEND_FILE_BACKEND_H
