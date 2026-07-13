/**
 * @file alsa_capabilities.h
 * @brief Functions for querying ALSA device capabilities.
 *
 * This file provides helper functions to query available ALSA devices,
 * their names, channel counts, and detailed descriptors of their capabilities.
 * These functions are only available if `ENABLE_ALSA` is defined.
 */

#ifndef CLIB_BACKEND_ALSA_CAPABILITIES_H
#define CLIB_BACKEND_ALSA_CAPABILITIES_H

#if defined(ENABLE_ALSA)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "Config/engine_config_types.h"
#include "backend_error.h"

/**
 * @brief Queries available ALSA device names.
 *
 * @param is_capture True to query capture devices, false for playback devices.
 * @param out_names 2D array to store the retrieved device names.
 * @param max_names Maximum number of names to retrieve (size of out_names
 * array).
 * @return The number of device names found and copied, or a negative value on
 * error.
 */
int alsa_capabilities_available_device_names(bool is_capture,
                                             char out_names[][256],
                                             int max_names);

/**
 * @brief Creates a detailed descriptor of an ALSA device's capabilities.
 *
 * @param device_name The ALSA device name.
 * @param is_capture True for capture, false for playback.
 * @param err Pointer to a device_error_t to receive error details on failure.
 * @return Pointer to the allocated audio_device_descriptor_t, or NULL on
 * failure. The returned descriptor must be freed with
 * free_audio_device_descriptor.
 */
audio_device_descriptor_t* alsa_capabilities_describe(const char* device_name,
                                                      bool is_capture,
                                                      device_error_t* err);

#endif  // ENABLE_ALSA

#endif  // CLIB_BACKEND_ALSA_CAPABILITIES_H
