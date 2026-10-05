/**
 * @file webaudio_backend.h
 * @brief WebAudio backend interface and vtable declarations.
 */

#ifndef CLIB_BACKEND_WEBAUDIO_BACKEND_H
#define CLIB_BACKEND_WEBAUDIO_BACKEND_H

#if defined(ENABLE_WEBAUDIO)

#include "backend/audio_backend.h"

/**
 * @brief Global virtual method table for WebAudio capture backend.
 */
extern const capture_backend_vtable_t g_webaudio_capture_vtable;

/**
 * @brief Global virtual method table for WebAudio playback backend.
 */
extern const playback_backend_vtable_t g_webaudio_playback_vtable;

/**
 * @brief Push planar float audio frames into WebAudio capture backend.
 *
 * @param backend Pointer to capture backend.
 * @param channels Array of channel float pointers (planar layout).
 * @param frames Number of frames to push.
 * @return Number of frames actually pushed.
 */
size_t webaudio_capture_push_samples(capture_backend_t *backend,
                                     const float *const *channels,
                                     size_t frames);

/**
 * @brief Render planar float audio frames from WebAudio playback backend.
 *
 * @param backend Pointer to playback backend.
 * @param channels Array of destination channel float pointers (planar layout).
 * @param frames Number of frames to render.
 * @return Number of frames actually rendered (padded with silence on underrun).
 */
size_t webaudio_playback_render_samples(playback_backend_t *backend,
                                        float *const *channels,
                                        size_t frames);

#endif // ENABLE_WEBAUDIO

#endif // CLIB_BACKEND_WEBAUDIO_BACKEND_H
