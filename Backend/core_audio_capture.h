// CoreAudio capture backend for macOS
//
// Real-time discipline
// --------------------
// The render callback runs on a high-priority audio thread driven by
// CoreAudio. It is absolutely forbidden to take locks, allocate, or
// otherwise call into the Swift runtime in a way that could block. To
// honour that:
//   - sample rings are SPSC `SPSCAudioRingBuffer<Float>` instances —
//     producer and consumer are wait-free, no `NSLock`.
//   - the AudioBufferList plus its per-channel raw data buffers are
//     preallocated in `open()` and reused for the lifetime of the unit;
//     the render callback only fills the existing struct.

#ifndef CLIB_BACKEND_CORE_AUDIO_CAPTURE_H
#define CLIB_BACKEND_CORE_AUDIO_CAPTURE_H

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
 * @file core_audio_capture.h
 * @brief CoreAudio capture backend for macOS.
 *
 * This backend handles audio capture from CoreAudio devices.
 *
 * @note Real-time discipline:
 * The render callback runs on a high-priority audio thread driven by CoreAudio.
 * It is forbidden to take locks, allocate, or call functions that might block.
 * - Sample rings are SPSC (Single Producer Single Consumer) lock-free buffers.
 * - AudioBufferList and per-channel raw data buffers are preallocated in open()
 *   and reused.
 */

/**
 * @brief Opaque structure representing a CoreAudio capture backend instance.
 */
typedef struct core_audio_capture core_audio_capture_t;

/**
 * @brief Global virtual method table for CoreAudio capture backend.
 */
extern const capture_backend_vtable_t g_core_audio_capture_vtable;

#endif  // ENABLE_COREAUDIO

#endif  // CLIB_BACKEND_CORE_AUDIO_CAPTURE_H
