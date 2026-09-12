#ifndef CLIB_ENGINE_THREAD_PRIORITY_H
#define CLIB_ENGINE_THREAD_PRIORITY_H

/**
 * @file thread_priority.h
 * @brief Helper for promoting threads to Mach real-time priority based on audio
 * parameters (buffer frames and sample rate).
 */

#include <stddef.h>

typedef struct realtime_thread_handle realtime_thread_handle_t;

/**
 * @brief Promotes the calling thread to real-time priority, returning a handle
 * for later demotion.
 *
 * @param name A descriptive name of the thread (e.g. "Capture", "Playback",
 * "Processing").
 * @param buffer_frames The buffer size in frames.
 * @param sample_rate The sample rate in Hz.
 * @return Opaque handle to the promoted thread, or NULL on failure.
 */
realtime_thread_handle_t* promote_current_thread_to_realtime(
    const char* name, size_t buffer_frames, size_t sample_rate);

/**
 * @brief Demotes a thread previously promoted to real-time priority back to its
 * original scheduling policy and priority, freeing the handle.
 *
 * @param handle The handle returned by promote_current_thread_to_realtime.
 */
void demote_current_thread_from_realtime(realtime_thread_handle_t* handle);

#endif  // CLIB_ENGINE_THREAD_PRIORITY_H
