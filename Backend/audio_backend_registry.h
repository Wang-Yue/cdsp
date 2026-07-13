#ifndef CLIB_BACKEND_AUDIO_BACKEND_REGISTRY_H
#define CLIB_BACKEND_AUDIO_BACKEND_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>

#include "Config/engine_config_types.h"
#include "backend_error.h"

/**
 * @file audio_backend_registry.h
 * @brief Unified registry for audio backend device discovery and capabilities
 * lookup.
 */

/**
 * @brief Get available audio devices for a backend.
 *
 * @param backend Backend name string (e.g. "coreaudio", "alsa", "wasapi",
 * "asio").
 * @param input True for input/capture devices, false for output/playback.
 * @param out_devices Array to store found devices.
 * @param max_devices Maximum number of devices to return.
 * @return Number of devices found, or negative error code.
 */
int audio_backend_registry_get_available_devices(const char* backend,
                                                 bool input,
                                                 audio_device_t* out_devices,
                                                 int max_devices);

/**
 * @brief Get detailed capabilities for a specific device.
 *
 * @param backend Backend name string.
 * @param device Device name string.
 * @param is_capture True if capture, false if playback.
 * @param err Pointer to store error details on failure.
 * @return Pointer to device descriptor, or NULL on failure. Caller owns
 * descriptor.
 */
audio_device_descriptor_t* audio_backend_registry_get_device_capabilities(
    const char* backend, const char* device, bool is_capture,
    device_error_t* err);

#endif  // CLIB_BACKEND_AUDIO_BACKEND_REGISTRY_H
