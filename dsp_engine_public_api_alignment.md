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
| `GetVersion` | Stateless | N/A *(Stateless)* | [`cdsp_get_version()`](Public/general.h#L11) | N/A *(Static const string)* | [`Public/general.h`](Public/general.h) | Returns CamillaDSP version string. |
| `GetSupportedDeviceTypes` | Stateless | N/A *(Stateless)* | [`cdsp_get_supported_device_types(...)`](Public/general.h#L24) | [`cdsp_free_device_types(...)`](Public/general.h#L32) | [`Public/general.h`](Public/general.h) | Queries supported audio hardware backends. |
| `Stop` | Stateful | `void (*stop)(void* ctx)` | [`cdsp_stop(engine)`](Public/processing.h#L63) | N/A | [`Public/processing.h`](Public/processing.h) | Stops processing core; calls `engine->stop(ctx)`. |
| N/A *(Engine Allocator)* | Stateful | Factory | [`cdsp_engine_create()`](Public/general.h#L38) | [`cdsp_engine_free(engine)`](Public/general.h#L44) | [`Public/general.h`](Public/general.h) | Creates/frees `dsp_engine_t` instance. Calls `engine->free(ctx)`. |
| N/A *(Engine Poller)* | Stateful | `void (*poll)(void* ctx)` | [`cdsp_engine_poll(engine)`](Public/general.h#L50) | N/A | [`Public/general.h`](Public/general.h) | Background polling tick; calls `engine->poll(ctx)`. |
| `GetState` | Stateful | `processing_state_t (*get_state)(void* ctx)` | [`cdsp_get_state(engine)`](Public/processing.h#L13) | N/A | [`Public/processing.h`](Public/processing.h) | Queries current processing state enum. |
| `GetStopReason` | Stateful | `bool (*get_stop_reason)(void* ctx, ...)` | [`cdsp_get_stop_reason(engine, ...)`](Public/processing.h#L20) | N/A | [`Public/processing.h`](Public/processing.h) | Queries stop reason structure snapshot. |
| `GetCaptureRate` | Stateful | `int (*get_capture_rate)(void* ctx)` | [`cdsp_get_capture_rate(engine)`](Public/processing.h#L27) | N/A | [`Public/processing.h`](Public/processing.h) | Queries active pipeline sample rate in Hz. |
| `GetRateAdjust` | Stateful | `bool (*get_processing_status)(void* ctx, ...)` | [`cdsp_get_processing_status(...)`](Public/processing.h#L47) | N/A | [`Public/processing.h`](Public/processing.h) | Extracts rate adjustment factor from processing status. |
| `GetBufferLevel` | Stateful | `bool (*get_processing_status)(void* ctx, ...)` | [`cdsp_get_processing_status(...)`](Public/processing.h#L47) | N/A | [`Public/processing.h`](Public/processing.h) | Extracts buffer fill ratio from processing status. |
| `GetClippedSamples` | Stateful | `bool (*get_processing_status)(void* ctx, ...)` | [`cdsp_get_processing_status(...)`](Public/processing.h#L47) | N/A | [`Public/processing.h`](Public/processing.h) | Extracts clipped sample count from processing status. |
| `ResetClippedSamples` | Stateful | `void (*reset_clipped_samples)(void* ctx)` | [`cdsp_reset_clipped_samples(engine)`](Public/processing.h#L58) | N/A | [`Public/processing.h`](Public/processing.h) | Resets clipped samples counter to zero. |
| `GetProcessingLoad` | Stateful | `bool (*get_processing_status)(void* ctx, ...)` | [`cdsp_get_processing_status(...)`](Public/processing.h#L47) | N/A | [`Public/processing.h`](Public/processing.h) | Extracts pipeline CPU load ratio. |
| `GetResamplerLoad` | Stateful | `bool (*get_processing_status)(void* ctx, ...)` | [`cdsp_get_processing_status(...)`](Public/processing.h#L47) | N/A | [`Public/processing.h`](Public/processing.h) | Extracts resampler CPU load ratio. |
| `GetStateFilePath` | Stateful | `const char* (*get_state_file_path)(void* ctx)`| [`cdsp_get_state_file_path(engine)`](Public/processing.h#L65) | N/A | [`Public/processing.h`](Public/processing.h) | Returns state file path string. |
| N/A *(Set State Path)*| Stateful | `void (*set_state_file_path)(void* ctx, ...)` | [`cdsp_set_state_file_path(engine, path)`](Public/state.h#L30) | N/A | [`Public/state.h`](Public/state.h) | Sets state file path for volume persistence. |
| `GetStateFileUpdated` | Stateful | `bool (*get_state_file_updated)(void* ctx)` | [`cdsp_get_state_file_updated(engine)`](Public/processing.h#L71) | N/A | [`Public/processing.h`](Public/processing.h) | Returns `true` if state file is clean/saved. |
| `GetConfig` | Stateful | `bool (*get_active_config_json)(void* ctx, ...)` | [`cdsp_get_active_config_yaml(engine, &str)`](Public/config.h#L41) | `free(str)` | [`Public/config.h`](Public/config.h) | Converts active JSON config to YAML string. |
| `GetConfigJson` | Stateful | `bool (*get_active_config_json)(void* ctx, ...)` | [`cdsp_get_active_config_json(engine, &str)`](Public/config.h#L30) | `free(str)` | [`Public/config.h`](Public/config.h) | Returns active configuration JSON string. |
| `GetPreviousConfig` | Stateful | `bool (*get_previous_config_json)(void* ctx, ...)` | [`cdsp_get_previous_config_yaml(engine, &str)`](Public/config.h#L63)| `free(str)` | [`Public/config.h`](Public/config.h) | Converts previous JSON config to YAML string. |
| `GetPreviousConfigJson`|Stateful | `bool (*get_previous_config_json)(void* ctx, ...)` | [`cdsp_get_previous_config_json(engine, &str)`](Public/config.h#L52)| `free(str)` | [`Public/config.h`](Public/config.h) | Returns previous configuration JSON string. |
| `SetConfig` | Stateful | `bool (*set_config_json)(void* ctx, ...)` | [`cdsp_set_config_yaml(engine, str, &err)`](Public/config.h#L84) | N/A | [`Public/config.h`](Public/config.h) | Converts YAML input string to JSON and applies live. |
| `SetConfigJson` | Stateful | `bool (*set_config_json)(void* ctx, ...)` | [`cdsp_set_config_json(engine, str, &err)`](Public/config.h#L73) | N/A | [`Public/config.h`](Public/config.h) | Applies JSON configuration payload live. |
| `GetConfigFilePath` | Stateful | `char* (*get_config_file_path)(void* ctx)` | [`cdsp_get_config_file_path(engine)`](Public/config.h#L13) | `free(str)` | [`Public/config.h`](Public/config.h) | Gets active configuration file path. |
| `SetConfigFilePath` | Stateful | `void (*set_config_file_path)(void* ctx, ...)` | [`cdsp_set_config_file_path(engine, path)`](Public/config.h#L20) | N/A | [`Public/config.h`](Public/config.h) | Sets active configuration file path. |
| `GetConfigTitle` | Stateful | `bool (*get_active_config_json)(void* ctx, ...)` | [`cdsp_get_config_title(engine)`](Public/config.h#L111) | `free(title)` | [`Public/config.h`](Public/config.h) | Extracts `"title"` from active configuration. |
| `GetConfigDescription`|Stateful | `bool (*get_active_config_json)(void* ctx, ...)` | [`cdsp_get_config_description(engine)`](Public/config.h#L118) | `free(description)` | [`Public/config.h`](Public/config.h) | Extracts `"description"` from active configuration. |
| `GetConfigValue` | Stateful | `bool (*get_active_config_json)(void* ctx, ...)` | [`cdsp_get_config_value(engine, ptr)`](Public/config.h#L126) | `free(val)` | [`Public/config.h`](Public/config.h) | Resolves RFC 6901 JSON Pointer path value. |
| `SetConfigValue` | Stateful | `bool (*set_config_json)(void* ctx, ...)` | [`cdsp_set_config_value(engine, ptr, val, &err)`](Public/config.h#L139)| N/A | [`Public/config.h`](Public/config.h) | Mutates single JSON Pointer path value & reloads. |
| `PatchConfig` | Stateful | `bool (*set_config_json)(void* ctx, ...)` | [`cdsp_patch_config(engine, patch, &err)`](Public/config.h#L152) | N/A | [`Public/config.h`](Public/config.h) | Merges RFC 6902 JSON Patch & reloads. |
| `Reload` | Stateful | `bool (*set_config_json)(void* ctx, ...)` | [`cdsp_reload_config(engine, &err)`](Public/config.h#L161) | N/A | [`Public/config.h`](Public/config.h) | Re-reads config file from disk & reloads. |
| `GetAvailableCaptureDevices` | Stateful/Backend | `bool (*get_available_devices)(void* ctx, ...)` | [`cdsp_get_available_devices(backend, true, ...)` ](Public/devices.h#L28)| `free(devices)` | [`Public/devices.h`](Public/devices.h) | Lists available hardware capture devices. |
| `GetAvailablePlaybackDevices` | Stateful/Backend | `bool (*get_available_devices)(void* ctx, ...)` | [`cdsp_get_available_devices(backend, false, ...)` ](Public/devices.h#L28)| `free(devices)` | [`Public/devices.h`](Public/devices.h) | Lists available hardware playback devices. |
| `GetCaptureDeviceCapabilities` | Stateful/Backend | `bool (*get_device_capabilities)(void* ctx, ...)` | [`cdsp_get_device_capabilities(backend, dev, true, ...)` ](Public/devices.h#L43)| [`cdsp_free_device_capabilities(desc)`](Public/devices.h#L50) | [`Public/devices.h`](Public/devices.h) | Queries hardware capabilities for capture device. |
| `GetPlaybackDeviceCapabilities` | Stateful/Backend | `bool (*get_device_capabilities)(void* ctx, ...)` | [`cdsp_get_device_capabilities(backend, dev, false, ...)` ](Public/devices.h#L43)| [`cdsp_free_device_capabilities(desc)`](Public/devices.h#L50) | [`Public/devices.h`](Public/devices.h) | Queries hardware capabilities for playback device. |
| `GetChannelLabels` | Stateful | `bool (*get_active_config_json)(void* ctx, ...)` | [`cdsp_get_channel_labels(engine, ...)` ](Public/signal_levels.h#L40)| [`cdsp_free_channel_labels(labels, count)`](Public/signal_levels.h#L49) | [`Public/signal_levels.h`](Public/signal_levels.h) | Returns display labels for audio channels. |
| `GetVuLevels` / `GetSignalLevels` | Stateful | `bool (*get_vu_levels)(void* ctx, ...)` | [`cdsp_get_vu_levels(engine, &vu)`](Public/signal_levels.h#L18) | [`cdsp_free_vu_levels(&vu)`](Public/signal_levels.h#L24) | [`Public/signal_levels.h`](Public/signal_levels.h) | Snapshot of RMS and Peak signal levels per channel. |
| N/A *(PCM Waveform)* | Stateful | `audio_samples_t* (*get_samples)(void* ctx, ...)` | [`cdsp_get_samples(engine, ...)`](Public/signal_levels.h#L44) | [`cdsp_free_samples(samples)`](Public/cdsp_pub_types.h#L138)| [`Public/signal_levels.h`](Public/signal_levels.h) | Fetches raw PCM audio sample buffers. |
| `GetSpectrum` | Stateful | `bool (*get_spectrum)(void* ctx, ...)` | [`cdsp_get_spectrum(engine, ...)` ](Public/spectrum.h#L30) | [`cdsp_free_spectrum(&spec)`](Public/spectrum.h#L38) | [`Public/spectrum.h`](Public/spectrum.h) | Computes FFT logarithmic frequency spectrum. |
| `GetFaderVolume` / `GetVolume` | Stateful | `float (*get_fader_volume)(void* ctx, ...)` | [`cdsp_get_fader_volume(engine, fader)`](Public/volume.h#L43) | N/A | [`Public/volume.h`](Public/volume.h) | Queries volume gain setting in dB for a fader. |
| `GetFaderMute` / `GetMute` | Stateful | `bool (*get_fader_mute)(void* ctx, ...)` | [`cdsp_get_fader_mute(engine, fader)`](Public/volume.h#L60) | N/A | [`Public/volume.h`](Public/volume.h) | Queries mute state boolean for a fader. |
| `SetFaderVolume` / `SetVolume` | Stateful | `void (*set_fader_volume)(void* ctx, ...)` | [`cdsp_set_fader_volume(engine, fader, db, instant)`](Public/volume.h#L52) | N/A | [`Public/volume.h`](Public/volume.h) | Sets volume gain setting in dB for a fader. |
| `SetFaderMute` / `SetMute` | Stateful | `void (*set_fader_mute)(void* ctx, ...)` | [`cdsp_set_fader_mute(engine, fader, mute)`](Public/volume.h#L68) | N/A | [`Public/volume.h`](Public/volume.h) | Toggles mute state for a fader. |
| `ReadConfigJson` / `ValidateConfigJson` | Stateless | N/A *(Stateless)* | [`cdsp_validate_config_json(str, &res, &err)`](Public/config.h#L171) | `free(res)` | [`Public/config.h`](Public/config.h) | Offline JSON schema default resolution. |
| `ReadConfig` / `ValidateConfig` | Stateless | N/A *(Stateless)* | [`cdsp_validate_config_yaml(str, &res, &err)`](Public/config.h#L181) | `free(res)` | [`Public/config.h`](Public/config.h) | Offline YAML schema default resolution. |
| `ReadConfigFile` | Stateless | N/A *(Stateless)* | [`cdsp_validate_config_file(path, &res, &err)`](Public/config.h#L191) | `free(res)` | [`Public/config.h`](Public/config.h) | Offline file schema default resolution. |
| N/A *(State File Manager)* | Stateless | N/A *(Stateless)* | [`cdsp_state_create()`](Public/state.h#L22) / [`cdsp_state_load`](Public/state.h#L36) / [`cdsp_state_save`](Public/state.h#L44)| [`cdsp_state_free(state)`](Public/state.h#L28) | [`Public/state.h`](Public/state.h) | Standalone state file parsing & serialization. |
