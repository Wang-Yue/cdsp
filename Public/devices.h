#ifndef CDSP_PUBLIC_DEVICES_H
#define CDSP_PUBLIC_DEVICES_H

#include <stdbool.h>
#include <stddef.h>

#include "cdsp_pub_types.h"

/**
 * @brief Representation of an audio device list entry.
 */
typedef struct {
  char identifier[256]; /**< Backend-specific device identifier (e.g. "hw:0,0").
                         */
  char name[256];       /**< Readable description or name. */
  bool has_name; /**< true if name/description is valid, false if null. */
} cdsp_device_info_t;

/**
 * @brief List available audio devices for a backend.
 *
 * The output array out_devices is allocated dynamically. The caller must free
 * it when done.
 *
 * @param backend Name of backend (e.g. "Alsa", "CoreAudio", "Wasapi", "Asio").
 * @param is_input true for capture devices, false for playback devices.
 * @param out_devices Output pointer to the allocated array of device info
 * structures.
 * @param out_count Output count of devices found.
 * @return true on success, false on failure.
 */
bool cdsp_get_available_devices(const char* backend, bool is_input,
                                cdsp_device_info_t** out_devices,
                                size_t* out_count);

/**
 * @brief Get detailed capabilities for a specific audio device.
 *
 * The returned descriptor is allocated dynamically. The caller must free it by
 * calling cdsp_free_device_capabilities.
 *
 * @param backend Name of backend.
 * @param device Device identifier.
 * @param is_capture true if capture, false if playback.
 * @param out_desc Output pointer to the allocated descriptor.
 * @param out_err Output pointer to write detailed device error if the query
 * fails.
 * @return true on success, false on failure.
 */
bool cdsp_get_device_capabilities(const char* backend, const char* device,
                                  bool is_capture,
                                  cdsp_device_descriptor_t** out_desc,
                                  cdsp_device_error_t* out_err);

/**
 * @brief Free the audio device capability descriptor structure.
 * @param desc Pointer to the descriptor structure.
 */
void cdsp_free_device_capabilities(cdsp_device_descriptor_t* desc);

#endif  // CDSP_PUBLIC_DEVICES_H
