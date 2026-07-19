/**
 * @file core_audio_playback.h
 * @brief CoreAudio playback backend for macOS.
 *
 * Real-time discipline
 * --------------------
 * The render callback runs on a high-priority audio thread driven by
 * CoreAudio. It is absolutely forbidden to take locks, allocate, or
 * otherwise call into the Swift runtime in a way that could block. To
 * honour that:
 *   - sample rings are SPSC `SPSCAudioRingBuffer<Float>` instances —
 *     producer and consumer are wait-free, no `NSLock`.
 *   - the render callback writes directly into the AudioBufferList
 *     provided by CoreAudio, consuming from the pre-allocated SPSC rings.
 */

#ifndef CLIB_BACKEND_CORE_AUDIO_PLAYBACK_H
#define CLIB_BACKEND_CORE_AUDIO_PLAYBACK_H

#if defined(ENABLE_COREAUDIO)

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "Utils/lock_free_ring_buffer.h"
#include "audio_backend.h"
#include "core_audio_device.h"

/**
 * @brief Opaque structure representing the CoreAudio playback backend.
 */
typedef struct core_audio_playback core_audio_playback_t;

/**
 * @brief Create a CoreAudio playback backend instance.
 *
 * @param config Pointer to the playback device configuration.
 * @param sample_rate The initial sample rate in Hz.
 * @param chunk_size The size of each audio chunk in frames.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return Pointer to the created playback_backend_t instance, or NULL on
 * failure.
 */
playback_backend_t* core_audio_playback_create(
    const playback_device_config_t* config, int sample_rate, size_t chunk_size,
    backend_error_t* err);

#endif  // ENABLE_COREAUDIO

#endif  // CLIB_BACKEND_CORE_AUDIO_PLAYBACK_H
