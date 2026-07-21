#ifndef CDSP_PUBLIC_CONFIG_H
#define CDSP_PUBLIC_CONFIG_H

#include <stdbool.h>

#include "cdsp_pub_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Get the currently loaded configuration file path (WebSocket:
 * GetConfigFilePath).
 * @param engine Pointer to the engine.
 * @return The path string, or NULL if none is loaded (caller must free).
 */
CDSP_API char* cdsp_get_config_file_path(const dsp_engine_t* engine);

/**
 * @brief Set the path to the configuration file (WebSocket: SetConfigFilePath).
 * @param engine Pointer to the engine.
 * @param path Path to the config file.
 */
CDSP_API void cdsp_set_config_file_path(dsp_engine_t* engine, const char* path);

/**
 * @brief Get the active configuration in JSON format.
 *
 * The output string is allocated dynamically. The caller must free it when
 * done.
 *
 * @param engine Pointer to the engine.
 * @param out_json Pointer to write the allocated JSON string to.
 * @return true on success, false on failure.
 */
CDSP_API bool cdsp_get_active_config_json(const dsp_engine_t* engine,
                                          char** out_json);

/**
 * @brief Get the active configuration in YAML format.
 *
 * The output string is allocated dynamically. The caller must free it when
 * done.
 *
 * @param engine Pointer to the engine.
 * @param out_yaml Pointer to write the allocated YAML string to.
 * @return true on success, false on failure.
 */
CDSP_API bool cdsp_get_active_config_yaml(const dsp_engine_t* engine,
                                          char** out_yaml);

/**
 * @brief Get the previously active configuration in JSON format.
 *
 * The output string is allocated dynamically. The caller must free it when
 * done.
 *
 * @param engine Pointer to the engine.
 * @param out_json Pointer to write the allocated JSON string to.
 * @return true on success, false on failure.
 */
CDSP_API bool cdsp_get_previous_config_json(const dsp_engine_t* engine,
                                            char** out_json);

/**
 * @brief Get the previously active configuration in YAML format.
 *
 * The output string is allocated dynamically. The caller must free it when
 * done.
 *
 * @param engine Pointer to the engine.
 * @param out_yaml Pointer to write the allocated YAML string to.
 * @return true on success, false on failure.
 */
CDSP_API bool cdsp_get_previous_config_yaml(const dsp_engine_t* engine,
                                            char** out_yaml);

/**
 * @brief Upload and immediately apply a new configuration from a JSON string.
 *
 * @param engine Pointer to the engine.
 * @param json_str JSON configuration string.
 * @param out_err Pointer to write backend error information if application
 * fails.
 * @return true on success, false on failure.
 */
CDSP_API bool cdsp_set_config_json(dsp_engine_t* engine, const char* json_str,
                                   cdsp_backend_error_t* out_err);

/**
 * @brief Upload and immediately apply a new configuration from a YAML string.
 *
 * @param engine Pointer to the engine.
 * @param yaml_str YAML configuration string.
 * @param out_err Pointer to write backend error information if application
 * fails.
 * @return true on success, false on failure.
 */
CDSP_API bool cdsp_set_config_yaml(dsp_engine_t* engine, const char* yaml_str,
                                   cdsp_backend_error_t* out_err);

/**
 * @brief Parse, configure, and start the engine using a config file on disk,
 * with optional overrides.
 *
 * This utility abstracts the file reading, override application, and engine
 * configuration sequence.
 *
 * @param engine Pointer to the engine.
 * @param path Path to the configuration file (YAML/JSON).
 * @param samplerate_override Override target sample rate (set to <= 0 to
 * ignore).
 * @param channels_override Override capture channels (set to <= 0 to ignore).
 * @param format_override Override capture format string (set to NULL to
 * ignore).
 * @param extra_samples_override Override capture extra samples (set to < 0 to
 * ignore).
 * @param out_err Pointer to write backend error information if application
 * fails.
 * @return true on success, false on failure.
 */
CDSP_API bool cdsp_engine_set_config_file(
    dsp_engine_t* engine, const char* path, int samplerate_override,
    int channels_override, const char* format_override,
    int extra_samples_override, cdsp_backend_error_t* out_err);

/**
 * @brief Read the title field from the active configuration.
 * @param engine Pointer to the engine.
 * @return Allocated title string, or NULL if not present. Caller must free it.
 */
CDSP_API char* cdsp_get_config_title(const dsp_engine_t* engine);

/**
 * @brief Read the description field from the active configuration.
 * @param engine Pointer to the engine.
 * @return Allocated description string, or NULL if not present. Caller must
 * free it.
 */
CDSP_API char* cdsp_get_config_description(const dsp_engine_t* engine);

/**
 * @brief Read a single value from the active configuration using a JSON Pointer
 * path (RFC 6901).
 * @param engine Pointer to the engine.
 * @param json_ptr JSON Pointer path.
 * @return Allocated JSON string representation of the value, or NULL if path
 * not found. Caller must free it.
 */
CDSP_API char* cdsp_get_config_value(const dsp_engine_t* engine,
                                     const char* json_ptr);

/**
 * @brief Set a single value in the active configuration using a JSON Pointer
 * path.
 *
 * If valid, the updated configuration takes effect immediately.
 *
 * @param engine Pointer to the engine.
 * @param json_ptr JSON Pointer path.
 * @param val_json JSON representation of the new value.
 * @param out_err Pointer to write backend error information if configuration
 * fails.
 * @return true on success, false on failure.
 */
CDSP_API bool cdsp_set_config_value(dsp_engine_t* engine, const char* json_ptr,
                                    const char* val_json,
                                    cdsp_backend_error_t* out_err);

/**
 * @brief Apply a partial patch to the active configuration.
 *
 * If the resulting configuration is valid, it takes effect immediately.
 *
 * @param engine Pointer to the engine.
 * @param patch_json JSON patch string.
 * @param out_err Pointer to write backend error if application fails.
 * @return true on success, false on failure.
 */
CDSP_API bool cdsp_patch_config(dsp_engine_t* engine, const char* patch_json,
                                cdsp_backend_error_t* out_err);

/**
 * @brief Reload the active configuration file from disk.
 * @param engine Pointer to the engine.
 * @param out_err Pointer to write backend error if reload fails.
 * @return true on success, false on failure.
 */
CDSP_API bool cdsp_reload_config(dsp_engine_t* engine,
                                 cdsp_backend_error_t* out_err);

/**
 * @brief Parse and fill defaults for a JSON configuration string without
 * applying it.
 *
 * @param json_str Inputs JSON configuration string.
 * @param out_result Output allocated JSON string with default values filled (or
 * error description).
 * @param is_error Out boolean set to true if parsing/validation failed.
 * @return true on success, false on failure.
 */
CDSP_API bool cdsp_validate_config_json(const char* json_str, char** out_result,
                                        cdsp_config_error_type_t* out_err_type);

/**
 * @brief Parse and fill defaults for a YAML configuration string without
 * applying it.
 *
 * @param yaml_str Inputs YAML configuration string.
 * @param out_result Output allocated YAML string with default values filled (or
 * error description).
 * @param out_err_type Out error type enum pointer.
 * @return true on success, false on failure.
 */
CDSP_API bool cdsp_validate_config_yaml(const char* yaml_str, char** out_result,
                                        cdsp_config_error_type_t* out_err_type);

/**
 * @brief Parse and fill defaults for a configuration file on disk without
 * applying it.
 *
 * @param path Path to the configuration file.
 * @param out_result Output allocated configuration string (YAML format) with
 * default values filled (or error description).
 * @param out_err_type Out error type enum pointer.
 * @return true on success, false on failure.
 */
CDSP_API bool cdsp_validate_config_file(const char* path, char** out_result,
                                        cdsp_config_error_type_t* out_err_type);

#ifdef __cplusplus
}
#endif

#endif  // CDSP_PUBLIC_CONFIG_H
