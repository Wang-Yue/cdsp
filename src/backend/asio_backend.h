#ifndef CLIB_BACKEND_ASIO_BACKEND_H
#define CLIB_BACKEND_ASIO_BACKEND_H

#if defined(ENABLE_ASIO)

#include <stdbool.h>

#include "backend/asio_types.h"
#include "backend/audio_backend.h"

/**
 * @file asio_backend.h
 * @brief ASIO capture and playback backend factory and shared driver registry.
 *
 * Provides factory functions to create capture and playback backend instances
 * using the ASIO (Audio Stream Input/Output) API, and shared driver operations.
 */

/**
 * @struct asio_capture
 * @brief Opaque structure representing the ASIO capture backend.
 */
typedef struct asio_capture asio_capture_t;

/**
 * @struct asio_playback
 * @brief Opaque structure representing the ASIO playback backend.
 */
typedef struct asio_playback asio_playback_t;

/**
 * @brief Global virtual method table for ASIO capture backend.
 */
extern const capture_backend_vtable_t g_asio_capture_vtable;

/**
 * @brief Global virtual method table for ASIO playback backend.
 */
extern const playback_backend_vtable_t g_asio_playback_vtable;

/**
 * @brief Enumerate registered ASIO device names from Windows registry.
 * Matches CamillaDSP driver.rs:list_device_names.
 */
int asio_list_device_names(char out_names[][256], int max_names);

/**
 * @brief Load an ASIO driver by name and initialize COM on the calling thread.
 * Matches CamillaDSP driver.rs:load_driver_by_name.
 */
bool asio_driver_load_by_name(const char *name, IASIO **out_iasio,
                              backend_error_t *err);

/**
 * @brief Release and tear down the driver loaded for devname.
 * Matches CamillaDSP driver.rs:teardown_asio_driver.
 */
void asio_driver_teardown(const char *devname);

/**
 * @brief Checks if a driver is currently loaded for `devname`.
 * Matches CamillaDSP driver.rs:driver_is_loaded.
 */
bool asio_driver_is_loaded(const char *devname);

IASIO *asio_driver_lookup(const char *devname);

/**
 * @brief Lock the per-driver mutex for devname and increment active reference
 * count. Matches upstream driver handle Mutex locking.
 */
bool asio_driver_lock(const char *devname);

/**
 * @brief Unlock the per-driver mutex for devname and decrement active reference
 * count.
 */
void asio_driver_unlock(const char *devname);

/**
 * @brief Callback function type for asio_with_driver.
 */
typedef bool (*asio_driver_action_fn)(IASIO *iasio, void *user_data,
                                      backend_error_t *err);

/**
 * @brief Run action with the driver loaded for devname, holding the per-driver
 * mutex. Matches CamillaDSP driver.rs:with_driver.
 */
bool asio_with_driver(const char *devname, asio_driver_action_fn action,
                      void *user_data, backend_error_t *err);

/**
 * @brief Whether this driver needs to be recreated for a sample rate change to
 * take effect. Matches CamillaDSP driver.rs:needs_rate_reload.
 */
bool asio_needs_rate_reload(const char *devname);

/**
 * @brief Whether this driver is refused outright.
 * Matches CamillaDSP driver.rs:is_unsupported_driver.
 */
bool asio_is_unsupported_driver(const char *devname);

/**
 * @brief Whether only one instance of this driver may be created per process.
 * Matches CamillaDSP driver.rs:is_single_instance_driver.
 * @deprecated Use asio_is_unsupported_driver instead.
 */
bool asio_is_single_instance_driver(const char *devname);

/**
 * @brief Returns the human-readable sample type name for an ASIO sample type
 * ID. Matches CamillaDSP utils.rs:asio_sample_type_name.
 */
const char *asio_sample_type_name(int type_id);

/**
 * @brief Converts an ASIO sample type ID to asio_sample_format_t.
 * Matches CamillaDSP utils.rs:asio_sample_type_to_format.
 */
asio_sample_format_t asio_sample_type_to_format(int type_id);

/**
 * @brief Returns the string representation of an asio_sample_format_t.
 * Matches CamillaDSP utils.rs:asio_format_to_str.
 */
const char *asio_format_to_str(asio_sample_format_t fmt);

/**
 * @brief Initialize COM on calling thread as Single-Threaded Apartment (STA).
 * Matches CamillaDSP driver.rs:com_init_this_thread.
 */
bool asio_com_init_this_thread(backend_error_t *err);

#endif // ENABLE_ASIO

#endif // CLIB_BACKEND_ASIO_BACKEND_H
