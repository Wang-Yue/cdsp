# CamillaDSP 3-Tier API Alignment Matrix & WebSocket Standardization

This document maps all CamillaDSP operations across three distinct architectural layers, using the **WebSocket Command Specification** (`~/camilladsp/websocket.md`) as the single source of truth:

1. **WebSocket Command (Signal of Truth)** — The canonical command name defined in the CamillaDSP WebSocket protocol.
2. **Engine VTable Method (`Engine/dsp_engine.h`)** — Instance methods belonging to `dsp_engine_t` for stateful processing core management.
3. **Public C API Functions (`Public/`)** — C library procedural exports and standalone utilities.

---

## 1. Architectural Boundary Principles

- **Stateful Engine Operations**: Operations that interact with a running audio pipeline, active configuration, internal mutexes, history buffers, or faders are instance methods on `dsp_engine_t` and belong in the VTable.
- **Stateless Utility Operations**: Pure utility functions (such as version queries, device type enumerations, YAML/JSON schema default populating, offline state file parsing) do **not** depend on a running `dsp_engine_t` instance and are explicitly kept out of the VTable.

---

## 2. Master 3-Tier Alignment Table

All legacy non-standardized aliases (`cdsp_is_fader_muted`, `cdsp_is_state_dirty`, `cdsp_get_config_path`, `cdsp_set_config_path`) have been completely purged and standardized to match the WebSocket specification.

Dynamic memory allocation functions are paired directly with their companion cleanup/free routines.

| WebSocket Command (Single Source of Truth) | State Dependence | Engine VTable Pointer (`Engine/dsp_engine.h`) | Exported Public C API (`Public/`) | Companion Cleanup/Free Companion | Header Location | Description & Dispatch Contract |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| `GetVersion` | Stateless | N/A *(Stateless)* | [`cdsp_get_version()`](Public/general.h#L12) | N/A *(Static const string)* | [`Public/general.h`](Public/general.h) | Returns CamillaDSP version string. |
| `GetSupportedDeviceTypes` | Stateless | N/A *(Stateless)* | [`cdsp_get_supported_device_types(...)`](Public/general.h#L25) | [`cdsp_free_device_types(...)`](Public/general.h#L35) | [`Public/general.h`](Public/general.h) | Queries supported audio hardware backends. |
| `Stop` | Stateful | `void (*stop)(void* ctx)` | [`cdsp_stop(engine)`](Public/general.h#L67) | N/A | [`Public/general.h`](Public/general.h) | Stops processing core; calls `engine->stop(ctx)`. |
| N/A *(Engine Allocator)* | Stateful | Factory | [`cdsp_engine_create()`](Public/general.h#L41) | [`cdsp_engine_free(engine)`](Public/general.h#L47) | [`Public/general.h`](Public/general.h) | Creates/frees `dsp_engine_t` instance. Calls `engine->free(ctx)`. |
| N/A *(Engine Poller)* | Stateful | `void (*poll)(void* ctx)` | [`cdsp_engine_poll(engine)`](Public/general.h#L54) | N/A | [`Public/general.h`](Public/general.h) | Background polling tick; calls `engine->poll(ctx)`. |
| N/A *(Log Level)* | Stateless | `void (*set_log_level)(void* ctx, ...)` | [`cdsp_set_log_level(level_str)`](Public/general.h#L61) | N/A | [`Public/general.h`](Public/general.h) | Sets global application logging level. |
| `GetState` | Stateful | `processing_state_t (*get_state)(void* ctx)` | [`cdsp_get_state(engine)`](Public/processing.h#L14) | N/A | [`Public/processing.h`](Public/processing.h) | Queries current processing state enum. |
| `GetStopReason` | Stateful | `bool (*get_stop_reason)(void* ctx, ...)` | [`cdsp_get_stop_reason(engine, ...)`](Public/processing.h#L21) | N/A | [`Public/processing.h`](Public/processing.h) | Queries stop reason structure snapshot. |
| `GetCaptureRate` | Stateful | `int (*get_capture_rate)(void* ctx)` | [`cdsp_get_capture_rate(engine)`](Public/processing.h#L29) | N/A | [`Public/processing.h`](Public/processing.h) | Queries active pipeline sample rate in Hz. |
| N/A *(Signal Range)* | Stateful | Direct Call | [`cdsp_get_signal_range(engine)`](Public/processing.h#L36) | N/A | [`Public/processing.h`](Public/processing.h) | Returns peak-to-peak signal range of last chunk. |
| `GetRateAdjust` | Stateful | `bool (*get_processing_status)(void* ctx, ...)` | [`cdsp_get_processing_status(...)`](Public/processing.h#L50) | N/A | [`Public/processing.h`](Public/processing.h) | Extracts rate adjustment factor from processing status. |
| `GetBufferLevel` | Stateful | `bool (*get_processing_status)(void* ctx, ...)` | [`cdsp_get_processing_status(...)`](Public/processing.h#L50) | N/A | [`Public/processing.h`](Public/processing.h) | Extracts buffer fill ratio from processing status. |
| `GetClippedSamples` | Stateful | `bool (*get_processing_status)(void* ctx, ...)` | [`cdsp_get_processing_status(...)`](Public/processing.h#L50) | N/A | [`Public/processing.h`](Public/processing.h) | Extracts clipped sample count from processing status. |
| `ResetClippedSamples` | Stateful | `void (*reset_clipped_samples)(void* ctx)` | [`cdsp_reset_clipped_samples(engine)`](Public/processing.h#L61) | N/A | [`Public/processing.h`](Public/processing.h) | Resets clipped samples counter to zero. |
| `GetProcessingLoad` | Stateful | `bool (*get_processing_status)(void* ctx, ...)` | [`cdsp_get_processing_status(...)`](Public/processing.h#L50) | N/A | [`Public/processing.h`](Public/processing.h) | Extracts pipeline CPU load ratio. |
| `GetResamplerLoad` | Stateful | `bool (*get_processing_status)(void* ctx, ...)` | [`cdsp_get_processing_status(...)`](Public/processing.h#L50) | N/A | [`Public/processing.h`](Public/processing.h) | Extracts resampler CPU load ratio. |
| `GetStateFilePath` | Stateful | `const char* (*get_state_file_path)(void* ctx)`| [`cdsp_get_state_file_path(engine)`](Public/processing.h#L68) | N/A | [`Public/processing.h`](Public/processing.h) | Returns state file path string. |
| `SetStateFilePath` | Stateful | `void (*set_state_file_path)(void* ctx, ...)` | [`cdsp_set_state_file_path(engine, path)`](Public/processing.h#L76) | N/A | [`Public/processing.h`](Public/processing.h) | Sets state file path for volume persistence. |
| `GetStateFileUpdated` | Stateful | `bool (*get_state_file_updated)(void* ctx)` | [`cdsp_get_state_file_updated(engine)`](Public/processing.h#L84) | N/A | [`Public/processing.h`](Public/processing.h) | Returns `true` if state file is clean/saved. |
| N/A *(PCM Waveform)* | Stateful | `audio_samples_t* (*get_samples)(void* ctx, ...)` | [`cdsp_get_samples(engine, ...)`](Public/processing.h#L98) | [`cdsp_free_samples(samples)`](Public/processing.h#L106)| [`Public/processing.h`](Public/processing.h) | Fetches raw PCM audio sample buffers. |
| `GetConfig` | Stateful | `bool (*get_active_config_json)(void* ctx, ...)` | [`cdsp_get_active_config_yaml(engine, &str)`](Public/config.h#L45) | `free(str)` | [`Public/config.h`](Public/config.h) | Converts active JSON config to YAML string. |
| `GetConfigJson` | Stateful | `bool (*get_active_config_json)(void* ctx, ...)` | [`cdsp_get_active_config_json(engine, &str)`](Public/config.h#L33) | `free(str)` | [`Public/config.h`](Public/config.h) | Returns active configuration JSON string. |
| `GetPreviousConfig` | Stateful | `bool (*get_previous_config_json)(void* ctx, ...)` | [`cdsp_get_previous_config_yaml(engine, &str)`](Public/config.h#L69)| `free(str)` | [`Public/config.h`](Public/config.h) | Converts previous JSON config to YAML string. |
| `GetPreviousConfigJson`|Stateful | `bool (*get_previous_config_json)(void* ctx, ...)` | [`cdsp_get_previous_config_json(engine, &str)`](Public/config.h#L57)| `free(str)` | [`Public/config.h`](Public/config.h) | Returns previous configuration JSON string. |
| `SetConfig` | Stateful | `bool (*set_config_json)(void* ctx, ...)` | [`cdsp_set_config_yaml(engine, str, &err)`](Public/config.h#L92) | N/A | [`Public/config.h`](Public/config.h) | Converts YAML input string to JSON and applies live. |
| `SetConfigJson` | Stateful | `bool (*set_config_json)(void* ctx, ...)` | [`cdsp_set_config_json(engine, str, &err)`](Public/config.h#L80) | N/A | [`Public/config.h`](Public/config.h) | Applies JSON configuration payload live. |
| N/A *(Set Config File)*| Stateful | Helper Utility | [`cdsp_engine_set_config_file(...)`](Public/config.h#L115) | N/A | [`Public/config.h`](Public/config.h) | Parses, sets up, and starts engine from config file. |
| `GetConfigFilePath` | Stateful | `char* (*get_config_file_path)(void* ctx)` | [`cdsp_get_config_file_path(engine)`](Public/config.h#L14) | `free(str)` | [`Public/config.h`](Public/config.h) | Gets active configuration file path. |
| `SetConfigFilePath` | Stateful | `void (*set_config_file_path)(void* ctx, ...)` | [`cdsp_set_config_file_path(engine, path)`](Public/config.h#L21) | N/A | [`Public/config.h`](Public/config.h) | Sets active configuration file path. |
| `GetConfigTitle` | Stateful | `bool (*get_active_config_json)(void* ctx, ...)` | [`cdsp_get_config_title(engine)`](Public/config.h#L126) | `free(title)` | [`Public/config.h`](Public/config.h) | Extracts `"title"` from active configuration. |
| `GetConfigDescription`|Stateful | `bool (*get_active_config_json)(void* ctx, ...)` | [`cdsp_get_config_description(engine)`](Public/config.h#L134) | `free(description)` | [`Public/config.h`](Public/config.h) | Extracts `"description"` from active configuration. |
| `GetConfigValue` | Stateful | `bool (*get_active_config_json)(void* ctx, ...)` | [`cdsp_get_config_value(engine, ptr)`](Public/config.h#L144) | `free(val)` | [`Public/config.h`](Public/config.h) | Resolves RFC 6901 JSON Pointer path value. |
| `SetConfigValue` | Stateful | `bool (*set_config_json)(void* ctx, ...)` | [`cdsp_set_config_value(engine, ptr, val, &err)`](Public/config.h#L159)| N/A | [`Public/config.h`](Public/config.h) | Mutates single JSON Pointer path value & reloads. |
| `PatchConfig` | Stateful | `bool (*set_config_json)(void* ctx, ...)` | [`cdsp_patch_config(engine, patch, &err)`](Public/config.h#L172) | N/A | [`Public/config.h`](Public/config.h) | Merges RFC 6902 JSON Patch & reloads. |
| `Reload` | Stateful | `bool (*set_config_json)(void* ctx, ...)` | [`cdsp_reload_config(engine, &err)`](Public/config.h#L181) | N/A | [`Public/config.h`](Public/config.h) | Re-reads config file from disk & reloads. |
| `GetAvailableCaptureDevices` | Stateful/Backend | `bool (*get_available_devices)(void* ctx, ...)` | [`cdsp_get_available_devices(backend, true, ...)` ](Public/devices.h#L32)| `free(devices)` | [`Public/devices.h`](Public/devices.h) | Lists available hardware capture devices. |
| `GetAvailablePlaybackDevices` | Stateful/Backend | `bool (*get_available_devices)(void* ctx, ...)` | [`cdsp_get_available_devices(backend, false, ...)` ](Public/devices.h#L32)| `free(devices)` | [`Public/devices.h`](Public/devices.h) | Lists available hardware playback devices. |
| `GetCaptureDeviceCapabilities` | Stateful/Backend | `bool (*get_device_capabilities)(void* ctx, ...)` | [`cdsp_get_device_capabilities(backend, dev, true, ...)` ](Public/devices.h#L50)| [`cdsp_free_device_capabilities(desc)`](Public/devices.h#L59) | [`Public/devices.h`](Public/devices.h) | Queries hardware capabilities for capture device. |
| `GetPlaybackDeviceCapabilities` | Stateful/Backend | `bool (*get_device_capabilities)(void* ctx, ...)` | [`cdsp_get_device_capabilities(backend, dev, false, ...)` ](Public/devices.h#L50)| [`cdsp_free_device_capabilities(desc)`](Public/devices.h#L59) | [`Public/devices.h`](Public/devices.h) | Queries hardware capabilities for playback device. |
| `GetChannelLabels` | Stateful | `bool (*get_active_config_json)(void* ctx, ...)` | [`cdsp_get_channel_labels(engine, ...)` ](Public/signal_levels.h#L42)| [`cdsp_free_channel_labels(labels, count)`](Public/signal_levels.h#L53) | [`Public/signal_levels.h`](Public/signal_levels.h) | Returns display labels for audio channels. |
| `GetVuLevels` / `GetSignalLevels` | Stateful | `bool (*get_vu_levels)(void* ctx, ...)` | [`cdsp_get_vu_levels(engine, &vu)`](Public/signal_levels.h#L19) | [`cdsp_free_vu_levels(&vu)`](Public/signal_levels.h#L25) | [`Public/signal_levels.h`](Public/signal_levels.h) | Snapshot of RMS and Peak signal levels per channel. |
| `GetSpectrum` | Stateful | `bool (*get_spectrum)(void* ctx, ...)` | [`cdsp_get_spectrum(engine, side, channel_ptr, ...)` ](Public/spectrum.h#L33) | [`cdsp_free_spectrum(&spec)`](Public/spectrum.h#L39) | [`Public/spectrum.h`](Public/spectrum.h) | Computes FFT logarithmic frequency spectrum. |
| `GetVolume` / Main Fader | Stateful | `float (*get_fader_volume)(void* ctx, ...)` | [`cdsp_get_volume(engine)`](Public/volume.h#L14) | N/A | [`Public/volume.h`](Public/volume.h) | Shorthand queries volume in dB for main fader. |
| `SetVolume` / Main Fader | Stateful | `void (*set_volume)(void* ctx, ...)` | [`cdsp_set_volume(engine, db, instant)`](Public/volume.h#L22) | N/A | [`Public/volume.h`](Public/volume.h) | Shorthand sets volume in dB for main fader. |
| `GetMute` / Main Fader | Stateful | `bool (*get_fader_mute)(void* ctx, ...)` | [`cdsp_get_mute(engine)`](Public/volume.h#L29) | N/A | [`Public/volume.h`](Public/volume.h) | Shorthand queries mute boolean for main fader. |
| `SetMute` / Main Fader | Stateful | `void (*set_fader_mute)(void* ctx, ...)` | [`cdsp_set_mute(engine, mute)`](Public/volume.h#L36) | N/A | [`Public/volume.h`](Public/volume.h) | Shorthand toggles mute state for main fader. |
| `GetFaderVolume` / `GetVolume` | Stateful | `float (*get_fader_volume)(void* ctx, ...)` | [`cdsp_get_fader_volume(engine, fader)`](Public/volume.h#L44) | N/A | [`Public/volume.h`](Public/volume.h) | Queries volume gain setting in dB for a fader. |
| `GetFaderMute` / `GetMute` | Stateful | `bool (*get_fader_mute)(void* ctx, ...)` | [`cdsp_get_fader_mute(engine, fader)`](Public/volume.h#L62) | N/A | [`Public/volume.h`](Public/volume.h) | Queries mute state boolean for a fader. |
| `SetFaderVolume` / `SetVolume` | Stateful | `void (*set_fader_volume)(void* ctx, ...)` | [`cdsp_set_fader_volume(engine, fader, db, instant)`](Public/volume.h#L53) | N/A | [`Public/volume.h`](Public/volume.h) | Sets volume gain setting in dB for a fader. |
| `SetFaderMute` / `SetMute` | Stateful | `void (*set_fader_mute)(void* ctx, ...)` | [`cdsp_set_fader_mute(engine, fader, mute)`](Public/volume.h#L70) | N/A | [`Public/volume.h`](Public/volume.h) | Toggles mute state for a fader. |
| `ReadConfigJson` / `ValidateConfigJson` | Stateless | N/A *(Stateless)* | [`cdsp_validate_config_json(str, &res, &err_type)`](Public/config.h#L197) | `free(res)` | [`Public/config.h`](Public/config.h) | Offline JSON schema default resolution. |
| `ReadConfig` / `ValidateConfig` | Stateless | N/A *(Stateless)* | [`cdsp_validate_config_yaml(str, &res, &err_type)`](Public/config.h#L210) | `free(res)` | [`Public/config.h`](Public/config.h) | Offline YAML schema default resolution. |
| `ReadConfigFile` | Stateless | N/A *(Stateless)* | [`cdsp_validate_config_file(path, &res, &err_type)`](Public/config.h#L223) | `free(res)` | [`Public/config.h`](Public/config.h) | Offline file schema default resolution. |
| N/A *(State File Manager)* | Stateless | N/A *(Stateless)* | [`cdsp_state_create()`](Public/state.h#L23) / [`cdsp_state_load`](Public/state.h#L37) / [`cdsp_state_save`](Public/state.h#L45)| [`cdsp_state_free(state)`](Public/state.h#L28) | [`Public/state.h`](Public/state.h) | Standalone state file parsing & serialization. |
