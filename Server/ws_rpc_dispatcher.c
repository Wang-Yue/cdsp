#include "Server/ws_rpc_dispatcher.h"

#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Config/cJSON.h"
#include "Public/cdsp_pub_types.h"
#include "Public/config.h"
#include "Public/devices.h"
#include "Public/general.h"
#include "Public/processing.h"
#include "Public/signal_levels.h"
#include "Public/spectrum.h"
#include "Public/volume.h"
#include "Server/websocket_server_internal.h"

static inline bool ws_engine_get_status(dsp_engine_t* engine,
                                        ws_state_update_t* out_status) {
  if (!engine || !out_status) return false;
  out_status->state = cdsp_get_state(engine);
  cdsp_get_stop_reason(engine, &out_status->stop_reason);
  return true;
}

cJSON* serialize_stop_reason(const cdsp_stop_reason_t* reason) {
  if (!reason) {
    return cJSON_CreateString("None");
  }
  cJSON* root = NULL;
  switch (reason->type) {
    case CDSP_STOP_REASON_NONE:
      return cJSON_CreateString("None");
    case CDSP_STOP_REASON_DONE:
      return cJSON_CreateString("Done");
    case CDSP_STOP_REASON_CAPTURE_ERROR:
      root = cJSON_CreateObject();
      cJSON_AddStringToObject(root, "CaptureError", reason->message);
      return root;
    case CDSP_STOP_REASON_PLAYBACK_ERROR:
      root = cJSON_CreateObject();
      cJSON_AddStringToObject(root, "PlaybackError", reason->message);
      return root;
    case CDSP_STOP_REASON_CAPTURE_FORMAT_CHANGE:
      root = cJSON_CreateObject();
      cJSON_AddNumberToObject(root, "CaptureFormatChange",
                              reason->format_change_rate);
      return root;
    case CDSP_STOP_REASON_PLAYBACK_FORMAT_CHANGE:
      root = cJSON_CreateObject();
      cJSON_AddNumberToObject(root, "PlaybackFormatChange",
                              reason->format_change_rate);
      return root;
    case CDSP_STOP_REASON_UNKNOWN_ERROR:
      root = cJSON_CreateObject();
      cJSON_AddStringToObject(root, "UnknownError", reason->message);
      return root;
    default:
      return cJSON_CreateString("None");
  }
}

cJSON* create_state_event_value(cdsp_processing_state_t state,
                                const cdsp_stop_reason_t* reason) {
  cJSON* val = cJSON_CreateObject();
  cJSON_AddStringToObject(val, "state", ws_processing_state_to_string(state));
  if (state == CDSP_PROCESSING_STATE_INACTIVE) {
    cJSON_AddItemToObject(val, "stop_reason", serialize_stop_reason(reason));
  }
  return val;
}

static void reply_ok(const char* cmd, cJSON* value_json, dyn_string_t* ds) {
  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "reply", cmd);
  cJSON_AddStringToObject(root, "result", "Ok");
  if (value_json) {
    cJSON_AddItemToObject(root, "value", value_json);
  }
  char* str = cJSON_PrintUnformatted(root);
  if (str) {
    dyn_string_printf(ds, "%s", str);
    free(str);
  }
  cJSON_Delete(root);
}

static void reply_error(const char* cmd, const char* error_name,
                        const char* message, dyn_string_t* ds) {
  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "reply", cmd);
  cJSON_AddStringToObject(root, "result",
                          error_name ? error_name : "ProcessingError");
  if (message && message[0] != '\0') {
    cJSON_AddStringToObject(root, "message", message);
  }
  char* str = cJSON_PrintUnformatted(root);
  if (str) {
    dyn_string_printf(ds, "%s", str);
    free(str);
  }
  cJSON_Delete(root);
}

static void reply_invalid(const char* error_message, dyn_string_t* ds) {
  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "reply", "Invalid");
  cJSON_AddStringToObject(root, "error",
                          error_message ? error_message : "Invalid JSON");
  char* str = cJSON_PrintUnformatted(root);
  if (str) {
    dyn_string_printf(ds, "%s", str);
    free(str);
  }
  cJSON_Delete(root);
}

static bool parse_adjust_volume_args(cJSON* root, float* out_value,
                                     float* out_min, float* out_max) {
  if (!root || !cJSON_IsObject(root)) return false;
  cJSON* val_node = cJSON_GetObjectItemCaseSensitive(root, "value");
  if (!val_node || !cJSON_IsNumber(val_node)) return false;
  *out_value = (float)val_node->valuedouble;
  *out_min = -150.0f;
  *out_max = 50.0f;
  cJSON* min_node = cJSON_GetObjectItemCaseSensitive(root, "min");
  if (min_node && cJSON_IsNumber(min_node)) {
    *out_min = (float)min_node->valuedouble;
  }
  cJSON* max_node = cJSON_GetObjectItemCaseSensitive(root, "max");
  if (max_node && cJSON_IsNumber(max_node)) {
    *out_max = (float)max_node->valuedouble;
  }
  return true;
}

static bool parse_adjust_fader_volume_args(cJSON* root, int* out_fader,
                                           float* out_value, float* out_min,
                                           float* out_max) {
  if (!root || !cJSON_IsObject(root)) return false;
  cJSON* fader_node = cJSON_GetObjectItemCaseSensitive(root, "fader");
  if (!fader_node || !cJSON_IsNumber(fader_node)) return false;
  *out_fader = fader_node->valueint;
  return parse_adjust_volume_args(root, out_value, out_min, out_max);
}

typedef enum {
  WS_CMD_UNKNOWN = 0,
  WS_CMD_GET_VERSION,
  WS_CMD_GET_STATE,
  WS_CMD_GET_STOP_REASON,
  WS_CMD_GET_CAPTURE_RATE,
  WS_CMD_GET_RATE_ADJUST,
  WS_CMD_GET_BUFFER_LEVEL,
  WS_CMD_GET_CLIPPED_SAMPLES,
  WS_CMD_RESET_CLIPPED_SAMPLES,
  WS_CMD_GET_PROCESSING_LOAD,
  WS_CMD_GET_RESAMPLER_LOAD,
  WS_CMD_GET_SUPPORTED_DEVICE_TYPES,
  WS_CMD_GET_UPDATE_INTERVAL,
  WS_CMD_SET_UPDATE_INTERVAL,
  WS_CMD_GET_VOLUME,
  WS_CMD_SET_VOLUME,
  WS_CMD_GET_MUTE,
  WS_CMD_SET_MUTE,
  WS_CMD_TOGGLE_MUTE,
  WS_CMD_GET_FADERS,
  WS_CMD_GET_FADER_VOLUME,
  WS_CMD_SET_FADER_VOLUME,
  WS_CMD_SET_FADER_EXTERNAL_VOLUME,
  WS_CMD_GET_FADER_MUTE,
  WS_CMD_SET_FADER_MUTE,
  WS_CMD_TOGGLE_FADER_MUTE,
  WS_CMD_ADJUST_VOLUME,
  WS_CMD_ADJUST_FADER_VOLUME,
  WS_CMD_GET_SPECTRUM,
  WS_CMD_GET_AVAILABLE_CAPTURE_DEVICES,
  WS_CMD_GET_AVAILABLE_PLAYBACK_DEVICES,
  WS_CMD_GET_CAPTURE_DEVICE_CAPABILITIES,
  WS_CMD_GET_PLAYBACK_DEVICE_CAPABILITIES,
  WS_CMD_GET_CAPTURE_SIGNAL_RMS,
  WS_CMD_GET_CAPTURE_SIGNAL_PEAK,
  WS_CMD_GET_PLAYBACK_SIGNAL_RMS,
  WS_CMD_GET_PLAYBACK_SIGNAL_PEAK,
  WS_CMD_GET_CAPTURE_SIGNAL_RMS_SINCE_LAST,
  WS_CMD_GET_CAPTURE_SIGNAL_PEAK_SINCE_LAST,
  WS_CMD_GET_PLAYBACK_SIGNAL_RMS_SINCE_LAST,
  WS_CMD_GET_PLAYBACK_SIGNAL_PEAK_SINCE_LAST,
  WS_CMD_GET_CAPTURE_SIGNAL_RMS_SINCE,
  WS_CMD_GET_CAPTURE_SIGNAL_PEAK_SINCE,
  WS_CMD_GET_PLAYBACK_SIGNAL_RMS_SINCE,
  WS_CMD_GET_PLAYBACK_SIGNAL_PEAK_SINCE,
  WS_CMD_GET_SIGNAL_LEVELS,
  WS_CMD_GET_SIGNAL_LEVELS_SINCE_LAST,
  WS_CMD_GET_SIGNAL_LEVELS_SINCE,
  WS_CMD_GET_SIGNAL_PEAKS_SINCE_START,
  WS_CMD_RESET_SIGNAL_PEAKS_SINCE_START,
  WS_CMD_GET_CHANNEL_LABELS,
  WS_CMD_GET_SIGNAL_RANGE,
  WS_CMD_GET_CONFIG_FILE_PATH,
  WS_CMD_GET_PREVIOUS_CONFIG,
  WS_CMD_GET_STATE_FILE_PATH,
  WS_CMD_GET_STATE_FILE_UPDATED,
  WS_CMD_GET_CONFIG,
  WS_CMD_GET_CONFIG_JSON,
  WS_CMD_GET_CONFIG_TITLE,
  WS_CMD_GET_CONFIG_DESCRIPTION,
  WS_CMD_RELOAD,
  WS_CMD_STOP,
  WS_CMD_EXIT,
  WS_CMD_SET_CONFIG_FILE_PATH,
  WS_CMD_SET_CONFIG,
  WS_CMD_SET_CONFIG_JSON,
  WS_CMD_GET_CONFIG_VALUE,
  WS_CMD_SET_CONFIG_VALUE,
  WS_CMD_PATCH_CONFIG,
  WS_CMD_READ_CONFIG,
  WS_CMD_READ_CONFIG_JSON,
  WS_CMD_READ_CONFIG_FILE,
  WS_CMD_VALIDATE_CONFIG,
  WS_CMD_VALIDATE_CONFIG_JSON,
  WS_CMD_VALIDATE_CONFIG_FILE,
  WS_CMD_SUBSCRIBE_STATE,
  WS_CMD_SUBSCRIBE_VU_LEVELS,
  WS_CMD_SUBSCRIBE_SIGNAL_LEVELS,
  WS_CMD_SUBSCRIBE_SPECTRUM,
  WS_CMD_STOP_SUBSCRIPTION
} websocket_command_t;

typedef struct {
  const char* name;
  websocket_command_t type;
} command_map_t;

static const command_map_t kCommandMap[] = {
    {"GetVersion", WS_CMD_GET_VERSION},
    {"GetState", WS_CMD_GET_STATE},
    {"GetStopReason", WS_CMD_GET_STOP_REASON},
    {"GetCaptureRate", WS_CMD_GET_CAPTURE_RATE},
    {"GetRateAdjust", WS_CMD_GET_RATE_ADJUST},
    {"GetBufferLevel", WS_CMD_GET_BUFFER_LEVEL},
    {"GetClippedSamples", WS_CMD_GET_CLIPPED_SAMPLES},
    {"ResetClippedSamples", WS_CMD_RESET_CLIPPED_SAMPLES},
    {"GetProcessingLoad", WS_CMD_GET_PROCESSING_LOAD},
    {"GetResamplerLoad", WS_CMD_GET_RESAMPLER_LOAD},
    {"GetSupportedDeviceTypes", WS_CMD_GET_SUPPORTED_DEVICE_TYPES},
    {"GetUpdateInterval", WS_CMD_GET_UPDATE_INTERVAL},
    {"SetUpdateInterval", WS_CMD_SET_UPDATE_INTERVAL},
    {"GetVolume", WS_CMD_GET_VOLUME},
    {"SetVolume", WS_CMD_SET_VOLUME},
    {"GetMute", WS_CMD_GET_MUTE},
    {"SetMute", WS_CMD_SET_MUTE},
    {"ToggleMute", WS_CMD_TOGGLE_MUTE},
    {"GetFaders", WS_CMD_GET_FADERS},
    {"GetFaderVolume", WS_CMD_GET_FADER_VOLUME},
    {"SetFaderVolume", WS_CMD_SET_FADER_VOLUME},
    {"SetFaderExternalVolume", WS_CMD_SET_FADER_EXTERNAL_VOLUME},
    {"GetFaderMute", WS_CMD_GET_FADER_MUTE},
    {"SetFaderMute", WS_CMD_SET_FADER_MUTE},
    {"ToggleFaderMute", WS_CMD_TOGGLE_FADER_MUTE},
    {"AdjustVolume", WS_CMD_ADJUST_VOLUME},
    {"AdjustFaderVolume", WS_CMD_ADJUST_FADER_VOLUME},
    {"GetSpectrum", WS_CMD_GET_SPECTRUM},
    {"GetAvailableCaptureDevices", WS_CMD_GET_AVAILABLE_CAPTURE_DEVICES},
    {"GetAvailablePlaybackDevices", WS_CMD_GET_AVAILABLE_PLAYBACK_DEVICES},
    {"GetCaptureDeviceCapabilities", WS_CMD_GET_CAPTURE_DEVICE_CAPABILITIES},
    {"GetPlaybackDeviceCapabilities", WS_CMD_GET_PLAYBACK_DEVICE_CAPABILITIES},
    {"GetCaptureSignalRms", WS_CMD_GET_CAPTURE_SIGNAL_RMS},
    {"GetCaptureSignalPeak", WS_CMD_GET_CAPTURE_SIGNAL_PEAK},
    {"GetPlaybackSignalRms", WS_CMD_GET_PLAYBACK_SIGNAL_RMS},
    {"GetPlaybackSignalPeak", WS_CMD_GET_PLAYBACK_SIGNAL_PEAK},
    {"GetCaptureSignalRmsSinceLast", WS_CMD_GET_CAPTURE_SIGNAL_RMS_SINCE_LAST},
    {"GetCaptureSignalPeakSinceLast",
     WS_CMD_GET_CAPTURE_SIGNAL_PEAK_SINCE_LAST},
    {"GetPlaybackSignalRmsSinceLast",
     WS_CMD_GET_PLAYBACK_SIGNAL_RMS_SINCE_LAST},
    {"GetPlaybackSignalPeakSinceLast",
     WS_CMD_GET_PLAYBACK_SIGNAL_PEAK_SINCE_LAST},
    {"GetCaptureSignalRmsSince", WS_CMD_GET_CAPTURE_SIGNAL_RMS_SINCE},
    {"GetCaptureSignalPeakSince", WS_CMD_GET_CAPTURE_SIGNAL_PEAK_SINCE},
    {"GetPlaybackSignalRmsSince", WS_CMD_GET_PLAYBACK_SIGNAL_RMS_SINCE},
    {"GetPlaybackSignalPeakSince", WS_CMD_GET_PLAYBACK_SIGNAL_PEAK_SINCE},
    {"GetSignalLevels", WS_CMD_GET_SIGNAL_LEVELS},
    {"GetSignalLevelsSinceLast", WS_CMD_GET_SIGNAL_LEVELS_SINCE_LAST},
    {"GetSignalLevelsSince", WS_CMD_GET_SIGNAL_LEVELS_SINCE},
    {"GetSignalPeaksSinceStart", WS_CMD_GET_SIGNAL_PEAKS_SINCE_START},
    {"ResetSignalPeaksSinceStart", WS_CMD_RESET_SIGNAL_PEAKS_SINCE_START},
    {"GetChannelLabels", WS_CMD_GET_CHANNEL_LABELS},
    {"GetSignalRange", WS_CMD_GET_SIGNAL_RANGE},
    {"GetConfigFilePath", WS_CMD_GET_CONFIG_FILE_PATH},
    {"GetPreviousConfig", WS_CMD_GET_PREVIOUS_CONFIG},
    {"GetStateFilePath", WS_CMD_GET_STATE_FILE_PATH},
    {"GetStateFileUpdated", WS_CMD_GET_STATE_FILE_UPDATED},
    {"GetConfig", WS_CMD_GET_CONFIG},
    {"GetConfigJson", WS_CMD_GET_CONFIG_JSON},
    {"GetConfigTitle", WS_CMD_GET_CONFIG_TITLE},
    {"GetConfigDescription", WS_CMD_GET_CONFIG_DESCRIPTION},
    {"Reload", WS_CMD_RELOAD},
    {"Stop", WS_CMD_STOP},
    {"Exit", WS_CMD_EXIT},
    {"SetConfigFilePath", WS_CMD_SET_CONFIG_FILE_PATH},
    {"SetConfig", WS_CMD_SET_CONFIG},
    {"SetConfigJson", WS_CMD_SET_CONFIG_JSON},
    {"GetConfigValue", WS_CMD_GET_CONFIG_VALUE},
    {"SetConfigValue", WS_CMD_SET_CONFIG_VALUE},
    {"PatchConfig", WS_CMD_PATCH_CONFIG},
    {"ReadConfig", WS_CMD_READ_CONFIG},
    {"ReadConfigJson", WS_CMD_READ_CONFIG_JSON},
    {"ReadConfigFile", WS_CMD_READ_CONFIG_FILE},
    {"ValidateConfig", WS_CMD_VALIDATE_CONFIG},
    {"ValidateConfigJson", WS_CMD_VALIDATE_CONFIG_JSON},
    {"ValidateConfigFile", WS_CMD_VALIDATE_CONFIG_FILE},
    {"SubscribeState", WS_CMD_SUBSCRIBE_STATE},
    {"SubscribeVuLevels", WS_CMD_SUBSCRIBE_VU_LEVELS},
    {"SubscribeSignalLevels", WS_CMD_SUBSCRIBE_SIGNAL_LEVELS},
    {"SubscribeSpectrum", WS_CMD_SUBSCRIBE_SPECTRUM},
    {"StopSubscription", WS_CMD_STOP_SUBSCRIPTION}};

static websocket_command_t lookup_command(const char* name) {
  if (!name) return WS_CMD_UNKNOWN;
  size_t count = sizeof(kCommandMap) / sizeof(kCommandMap[0]);
  for (size_t i = 0; i < count; i++) {
    if (strcmp(kCommandMap[i].name, name) == 0) {
      return kCommandMap[i].type;
    }
  }
  return WS_CMD_UNKNOWN;
}

static const char* get_websocket_error_key(cdsp_backend_error_type_t type) {
  switch (type) {
    case CDSP_BACKEND_ERR_CONFIG_READ:
      return "ConfigReadError";
    case CDSP_BACKEND_ERR_CONFIG_PARSE:
      return "ConfigValidationError";
    case CDSP_BACKEND_ERR_DEVICE_NOT_FOUND:
      return "DeviceNotFoundError";
    case CDSP_BACKEND_ERR_DEVICE_BUSY:
      return "DeviceBusyError";
    default:
      return "DeviceError";
  }
}

static const char* get_websocket_device_error_key(
    cdsp_device_error_type_t type) {
  switch (type) {
    case CDSP_DEVICE_ERROR_NOT_FOUND:
      return "DeviceNotFoundError";
    case CDSP_DEVICE_ERROR_BUSY:
      return "DeviceBusyError";
    default:
      return "DeviceError";
  }
}

static char* format_device_descriptor(const cdsp_device_descriptor_t* desc) {
  if (!desc) return strdup("null");
  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "name", desc->name);
  cJSON_AddStringToObject(root, "description", desc->description);

  cJSON* cs_arr = cJSON_CreateArray();
  cJSON_AddItemToObject(root, "capability_sets", cs_arr);

  for (size_t cs_idx = 0; cs_idx < desc->capability_sets_count; cs_idx++) {
    const cdsp_device_capability_set_t* cs = &desc->capability_sets[cs_idx];
    cJSON* cs_obj = cJSON_CreateObject();
    cJSON_AddItemToArray(cs_arr, cs_obj);
    cJSON_AddStringToObject(cs_obj, "mode", cs->mode);

    cJSON* caps_arr = cJSON_CreateArray();
    cJSON_AddItemToObject(cs_obj, "capabilities", caps_arr);

    for (size_t c_idx = 0; c_idx < cs->capabilities_count; c_idx++) {
      const cdsp_channel_capability_t* cap = &cs->capabilities[c_idx];
      cJSON* cap_obj = cJSON_CreateObject();
      cJSON_AddItemToArray(caps_arr, cap_obj);

      cJSON_AddNumberToObject(cap_obj, "channels", cap->channels);

      cJSON* sr_arr = cJSON_CreateArray();
      cJSON_AddItemToObject(cap_obj, "samplerates", sr_arr);

      for (size_t s_idx = 0; s_idx < cap->samplerates_count; s_idx++) {
        const cdsp_samplerate_capability_t* sr = &cap->samplerates[s_idx];
        cJSON* sr_obj = cJSON_CreateObject();
        cJSON_AddItemToArray(sr_arr, sr_obj);

        cJSON_AddNumberToObject(sr_obj, "samplerate", sr->samplerate);

        cJSON* formats_arr = cJSON_CreateArray();
        cJSON_AddItemToObject(sr_obj, "formats", formats_arr);

        for (size_t f_idx = 0; f_idx < sr->formats_count; f_idx++) {
          cJSON_AddItemToArray(formats_arr,
                               cJSON_CreateString(sr->formats[f_idx]));
        }
      }
    }
  }
  char* str = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  return str;
}

cJSON* serialize_spectrum(const cdsp_spectrum_t* spec) {
  if (!spec || spec->count == 0) return cJSON_CreateNull();
  cJSON* root = cJSON_CreateObject();
  cJSON_AddItemToObject(
      root, "frequencies",
      cJSON_CreateDoubleArray(spec->frequencies, (int)spec->count));
  cJSON_AddItemToObject(
      root, "magnitudes",
      cJSON_CreateDoubleArray(spec->magnitudes, (int)spec->count));
  return root;
}

static bool server_handle_adjust_volume_fader(websocket_server_t* server,
                                              cdsp_fader_t fader, float delta,
                                              float min_vol, float max_vol,
                                              dyn_string_t* ds,
                                              const char* cmd_name) {
  if (!server || !server->engine) {
    reply_error(cmd_name, "InvalidRequestError", "Server or engine unavailable",
                ds);
    return true;
  }

  if (max_vol < min_vol) {
    reply_error(cmd_name, "InvalidValueError",
                "Max volume must be bigger than min volume", ds);
    return true;
  }

  float current = cdsp_get_fader_volume(server->engine, fader);
  float new_vol = current + delta;
  if (new_vol < min_vol) new_vol = min_vol;
  if (new_vol > max_vol) new_vol = max_vol;

  cdsp_set_fader_volume(server->engine, fader, new_vol, false);

  if (strcmp(cmd_name, "AdjustVolume") == 0) {
    reply_ok(cmd_name, cJSON_CreateNumber(new_vol), ds);
  } else {
    cJSON* arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)fader));
    cJSON_AddItemToArray(arr, cJSON_CreateNumber(new_vol));
    reply_ok(cmd_name, arr, ds);
  }
  return true;
}

static void handle_cmd_get_volume(websocket_server_t* server, int client_idx,
                                  const char* cmd_name, cJSON* arg,
                                  dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  if (server && server->engine) {
    float vol = cdsp_get_fader_volume(server->engine, CDSP_FADER_MAIN);
    reply_ok(cmd_name, cJSON_CreateNumber(vol), ds);
  } else {
    reply_error(cmd_name, "InvalidRequestError", "Server or engine unavailable",
                ds);
  }
}

static inline bool validate_and_clamp_volume(float* inout_vol) {
  if (isnan(*inout_vol) || isinf(*inout_vol)) return false;
  if (*inout_vol > 50.0f) *inout_vol = 50.0f;
  if (*inout_vol < -150.0f) *inout_vol = -150.0f;
  return true;
}

static void handle_cmd_set_volume(websocket_server_t* server, int client_idx,
                                  const char* cmd_name, cJSON* root,
                                  dyn_string_t* ds) {
  (void)client_idx;
  cJSON* arg = cJSON_GetObjectItemCaseSensitive(root, "value");
  if (arg && cJSON_IsNumber(arg)) {
    float vol = (float)arg->valuedouble;
    if (!validate_and_clamp_volume(&vol)) {
      reply_error(cmd_name, "InvalidValueError",
                  "Volume must be a finite number", ds);
      return;
    }
    if (server && server->engine) {
      cdsp_set_fader_volume(server->engine, CDSP_FADER_MAIN, vol, false);
      reply_ok(cmd_name, NULL, ds);
    } else {
      reply_error(cmd_name, "InvalidRequestError",
                  "Server or engine unavailable", ds);
    }
  } else {
    reply_error(cmd_name, "InvalidRequestError", "Could not parse volume", ds);
  }
}

static void handle_cmd_get_mute(websocket_server_t* server, int client_idx,
                                const char* cmd_name, cJSON* root,
                                dyn_string_t* ds) {
  (void)client_idx;
  (void)root;
  if (server && server->engine) {
    bool mute = cdsp_get_fader_mute(server->engine, CDSP_FADER_MAIN);
    reply_ok(cmd_name, cJSON_CreateBool(mute), ds);
  } else {
    reply_error(cmd_name, "InvalidRequestError", "Server or engine unavailable",
                ds);
  }
}

static void handle_cmd_set_mute(websocket_server_t* server, int client_idx,
                                const char* cmd_name, cJSON* root,
                                dyn_string_t* ds) {
  (void)client_idx;
  cJSON* arg = cJSON_GetObjectItemCaseSensitive(root, "value");
  if (arg && cJSON_IsBool(arg)) {
    bool mute = cJSON_IsTrue(arg);
    if (server && server->engine) {
      cdsp_set_fader_mute(server->engine, CDSP_FADER_MAIN, mute);
      reply_ok(cmd_name, NULL, ds);
    } else {
      reply_error(cmd_name, "InvalidRequestError",
                  "Server or engine unavailable", ds);
    }
  } else {
    reply_error(cmd_name, "InvalidRequestError", "Could not parse mute", ds);
  }
}

static void handle_cmd_toggle_mute(websocket_server_t* server, int client_idx,
                                   const char* cmd_name, cJSON* root,
                                   dyn_string_t* ds) {
  (void)client_idx;
  (void)root;
  if (server && server->engine) {
    bool was_muted = cdsp_get_fader_mute(server->engine, CDSP_FADER_MAIN);
    cdsp_set_fader_mute(server->engine, CDSP_FADER_MAIN, !was_muted);
    reply_ok(cmd_name, cJSON_CreateBool(!was_muted), ds);
  } else {
    reply_error(cmd_name, "InvalidRequestError", "Server or engine unavailable",
                ds);
  }
}

static void handle_cmd_get_faders(websocket_server_t* server, int client_idx,
                                  const char* cmd_name, cJSON* root,
                                  dyn_string_t* ds) {
  (void)client_idx;
  (void)root;
  if (server && server->engine) {
    cJSON* arr = cJSON_CreateArray();
    for (int i = 0; i < CDSP_FADER_COUNT; i++) {
      cJSON* obj = cJSON_CreateObject();
      float vol = cdsp_get_fader_volume(server->engine, (cdsp_fader_t)i);
      bool mute = cdsp_get_fader_mute(server->engine, (cdsp_fader_t)i);
      cJSON_AddNumberToObject(obj, "volume", vol);
      cJSON_AddBoolToObject(obj, "mute", mute);
      cJSON_AddItemToArray(arr, obj);
    }
    reply_ok(cmd_name, arr, ds);
  } else {
    reply_error(cmd_name, "InvalidRequestError", "Server or engine unavailable",
                ds);
  }
}

static void handle_cmd_get_fader_volume(websocket_server_t* server,
                                        int client_idx, const char* cmd_name,
                                        cJSON* root, dyn_string_t* ds) {
  (void)client_idx;
  cJSON* fader_node = cJSON_GetObjectItemCaseSensitive(root, "fader");
  if (fader_node && cJSON_IsNumber(fader_node)) {
    int idx = fader_node->valueint;
    if (server && server->engine) {
      if (idx >= 0 && idx < CDSP_FADER_COUNT) {
        float vol = cdsp_get_fader_volume(server->engine, (cdsp_fader_t)idx);
        cJSON* arr = cJSON_CreateArray();
        cJSON_AddItemToArray(arr, cJSON_CreateNumber(idx));
        cJSON_AddItemToArray(arr, cJSON_CreateNumber(vol));
        reply_ok(cmd_name, arr, ds);
      } else {
        reply_error(cmd_name, "InvalidFaderError", NULL, ds);
      }
    } else {
      reply_error(cmd_name, "InvalidRequestError",
                  "Server or engine unavailable", ds);
    }
  } else {
    reply_error(cmd_name, "InvalidRequestError", "Could not parse fader index",
                ds);
  }
}

static void handle_cmd_set_fader_volume(websocket_server_t* server,
                                        int client_idx, const char* cmd_name,
                                        cJSON* root, dyn_string_t* ds) {
  (void)client_idx;
  cJSON* fader_node = cJSON_GetObjectItemCaseSensitive(root, "fader");
  cJSON* vol_node = cJSON_GetObjectItemCaseSensitive(root, "value");
  if (fader_node && vol_node && cJSON_IsNumber(fader_node) &&
      cJSON_IsNumber(vol_node)) {
    int idx = fader_node->valueint;
    float vol = (float)vol_node->valuedouble;
    if (!validate_and_clamp_volume(&vol)) {
      reply_error(cmd_name, "InvalidValueError",
                  "Volume must be a finite number", ds);
      return;
    }
    if (server && server->engine) {
      if (idx >= 0 && idx < CDSP_FADER_COUNT) {
        cdsp_set_fader_volume(server->engine, (cdsp_fader_t)idx, vol, false);
        reply_ok(cmd_name, NULL, ds);
      } else {
        reply_error(cmd_name, "InvalidFaderError", NULL, ds);
      }
    } else {
      reply_error(cmd_name, "InvalidRequestError",
                  "Server or engine unavailable", ds);
    }
  } else {
    reply_error(cmd_name, "InvalidRequestError",
                "Could not parse fader index or volume value", ds);
  }
}

static void handle_cmd_set_fader_external_volume(websocket_server_t* server,
                                                 int client_idx,
                                                 const char* cmd_name,
                                                 cJSON* root,
                                                 dyn_string_t* ds) {
  (void)client_idx;
  cJSON* fader_node = cJSON_GetObjectItemCaseSensitive(root, "fader");
  cJSON* vol_node = cJSON_GetObjectItemCaseSensitive(root, "value");
  if (fader_node && vol_node && cJSON_IsNumber(fader_node) &&
      cJSON_IsNumber(vol_node)) {
    int idx = fader_node->valueint;
    float vol = (float)vol_node->valuedouble;
    if (!validate_and_clamp_volume(&vol)) {
      reply_error(cmd_name, "InvalidValueError",
                  "Volume must be a finite number", ds);
      return;
    }
    if (server && server->engine) {
      if (idx >= 0 && idx < CDSP_FADER_COUNT) {
        cdsp_set_fader_volume(server->engine, (cdsp_fader_t)idx, vol, true);
        reply_ok(cmd_name, NULL, ds);
      } else {
        reply_error(cmd_name, "InvalidFaderError", NULL, ds);
      }
    } else {
      reply_error(cmd_name, "InvalidRequestError",
                  "Server or engine unavailable", ds);
    }
  } else {
    reply_error(cmd_name, "InvalidRequestError",
                "Could not parse fader index or external volume value", ds);
  }
}

static void handle_cmd_get_fader_mute(websocket_server_t* server,
                                      int client_idx, const char* cmd_name,
                                      cJSON* root, dyn_string_t* ds) {
  (void)client_idx;
  cJSON* fader_node = cJSON_GetObjectItemCaseSensitive(root, "fader");
  if (fader_node && cJSON_IsNumber(fader_node)) {
    int idx = fader_node->valueint;
    if (server && server->engine) {
      if (idx >= 0 && idx < CDSP_FADER_COUNT) {
        bool mute = cdsp_get_fader_mute(server->engine, (cdsp_fader_t)idx);
        cJSON* arr = cJSON_CreateArray();
        cJSON_AddItemToArray(arr, cJSON_CreateNumber(idx));
        cJSON_AddItemToArray(arr, cJSON_CreateBool(mute));
        reply_ok(cmd_name, arr, ds);
      } else {
        reply_error(cmd_name, "InvalidFaderError", NULL, ds);
      }
    } else {
      reply_error(cmd_name, "InvalidRequestError",
                  "Server or engine unavailable", ds);
    }
  } else {
    reply_error(cmd_name, "InvalidRequestError", "Could not parse fader index",
                ds);
  }
}

static void handle_cmd_set_fader_mute(websocket_server_t* server,
                                      int client_idx, const char* cmd_name,
                                      cJSON* root, dyn_string_t* ds) {
  (void)client_idx;
  cJSON* fader_node = cJSON_GetObjectItemCaseSensitive(root, "fader");
  cJSON* mute_node = cJSON_GetObjectItemCaseSensitive(root, "value");
  if (fader_node && mute_node && cJSON_IsNumber(fader_node) &&
      cJSON_IsBool(mute_node)) {
    int idx = fader_node->valueint;
    bool mute = cJSON_IsTrue(mute_node);
    if (server && server->engine) {
      if (idx >= 0 && idx < CDSP_FADER_COUNT) {
        cdsp_set_fader_mute(server->engine, (cdsp_fader_t)idx, mute);
        reply_ok(cmd_name, NULL, ds);
      } else {
        reply_error(cmd_name, "InvalidFaderError", NULL, ds);
      }
    } else {
      reply_error(cmd_name, "InvalidRequestError",
                  "Server or engine unavailable", ds);
    }
  } else {
    reply_error(cmd_name, "InvalidRequestError",
                "Could not parse fader index or mute value", ds);
  }
}

static void handle_cmd_toggle_fader_mute(websocket_server_t* server,
                                         int client_idx, const char* cmd_name,
                                         cJSON* root, dyn_string_t* ds) {
  (void)client_idx;
  cJSON* fader_node = cJSON_GetObjectItemCaseSensitive(root, "fader");
  if (fader_node && cJSON_IsNumber(fader_node)) {
    int idx = fader_node->valueint;
    if (server && server->engine) {
      if (idx >= 0 && idx < CDSP_FADER_COUNT) {
        bool was_muted = cdsp_get_fader_mute(server->engine, (cdsp_fader_t)idx);
        cdsp_set_fader_mute(server->engine, (cdsp_fader_t)idx, !was_muted);
        cJSON* arr = cJSON_CreateArray();
        cJSON_AddItemToArray(arr, cJSON_CreateNumber(idx));
        cJSON_AddItemToArray(arr, cJSON_CreateBool(!was_muted));
        reply_ok(cmd_name, arr, ds);
      } else {
        reply_error(cmd_name, "InvalidFaderError", NULL, ds);
      }
    } else {
      reply_error(cmd_name, "InvalidRequestError",
                  "Server or engine unavailable", ds);
    }
  } else {
    reply_error(cmd_name, "InvalidRequestError", "Could not parse fader index",
                ds);
  }
}

static void handle_cmd_adjust_volume(websocket_server_t* server, int client_idx,
                                     const char* cmd_name, cJSON* root,
                                     dyn_string_t* ds) {
  (void)client_idx;
  float delta = 0.0f;
  float min_vol = -150.0f;
  float max_vol = 50.0f;
  if (parse_adjust_volume_args(root, &delta, &min_vol, &max_vol)) {
    server_handle_adjust_volume_fader(server, CDSP_FADER_MAIN, delta, min_vol,
                                      max_vol, ds, cmd_name);
  } else {
    reply_error(cmd_name, "InvalidRequestError",
                "Could not parse AdjustVolume argument", ds);
  }
}

static void handle_cmd_adjust_fader_volume(websocket_server_t* server,
                                           int client_idx, const char* cmd_name,
                                           cJSON* root, dyn_string_t* ds) {
  (void)client_idx;
  int idx = -1;
  float delta = 0.0f;
  float min_vol = -150.0f;
  float max_vol = 50.0f;
  if (parse_adjust_fader_volume_args(root, &idx, &delta, &min_vol, &max_vol)) {
    if (idx >= 0 && idx < CDSP_FADER_COUNT) {
      server_handle_adjust_volume_fader(server, (cdsp_fader_t)idx, delta,
                                        min_vol, max_vol, ds, cmd_name);
    } else {
      reply_error(cmd_name, "InvalidFaderError", NULL, ds);
    }
  } else {
    reply_error(cmd_name, "InvalidRequestError",
                "Could not parse AdjustFaderVolume argument", ds);
  }
}

static void handle_cmd_subscribe_state(websocket_server_t* server,
                                       int client_idx, const char* cmd_name,
                                       cJSON* root, dyn_string_t* ds) {
  (void)root;
  if (server) {
    server->client_sessions[client_idx].state_subscribed = true;
  }
  reply_ok(cmd_name, NULL, ds);
}

static void handle_cmd_subscribe_vu_levels(websocket_server_t* server,
                                           int client_idx, const char* cmd_name,
                                           cJSON* root, dyn_string_t* ds) {
  float max_rate = 0.0f;
  float attack = 0.0f;
  float release = 0.0f;
  cJSON* arg = cJSON_GetObjectItemCaseSensitive(root, "value");
  if (arg && cJSON_IsObject(arg)) {
    cJSON* item;
    item = cJSON_GetObjectItemCaseSensitive(arg, "max_rate");
    if (item && cJSON_IsNumber(item)) max_rate = (float)item->valuedouble;
    item = cJSON_GetObjectItemCaseSensitive(arg, "attack");
    if (item && cJSON_IsNumber(item)) attack = (float)item->valuedouble;
    item = cJSON_GetObjectItemCaseSensitive(arg, "release");
    if (item && cJSON_IsNumber(item)) release = (float)item->valuedouble;
  }
  if (attack < 0.0f || attack > 60000.0f || release < 0.0f || release > 60000.0f) {
    reply_error(cmd_name, "InvalidValueError",
                "attack and release must be between 0 and 60000 ms", ds);
  } else {
    if (server) {
      server->client_sessions[client_idx].vu_subscribed = true;
      server->client_sessions[client_idx].vu_max_rate = max_rate;
      server->client_sessions[client_idx].vu_attack = attack;
      server->client_sessions[client_idx].vu_release = release;
      server->client_sessions[client_idx].last_vu_push_time = 0;
    }
    reply_ok(cmd_name, NULL, ds);
  }
}

static void handle_cmd_subscribe_signal_levels(websocket_server_t* server,
                                               int client_idx,
                                               const char* cmd_name,
                                               cJSON* root, dyn_string_t* ds) {
  char side[16] = "";
  cJSON* arg = cJSON_GetObjectItemCaseSensitive(root, "value");
  if (arg && cJSON_IsString(arg) && arg->valuestring) {
    strncpy(side, arg->valuestring, sizeof(side) - 1);
  }
  if (strcmp(side, "playback") == 0 || strcmp(side, "capture") == 0 ||
      strcmp(side, "both") == 0) {
    if (server) {
      server->client_sessions[client_idx].signal_levels_subscribed = true;
      snprintf(server->client_sessions[client_idx].signal_levels_side,
               sizeof(server->client_sessions[client_idx].signal_levels_side),
               "%s", side);
    }
    reply_ok(cmd_name, NULL, ds);
  } else {
    reply_error(cmd_name, "InvalidValueError",
                "side must be playback, capture, or both", ds);
  }
}

static void handle_cmd_subscribe_spectrum(websocket_server_t* server,
                                          int client_idx, const char* cmd_name,
                                          cJSON* root, dyn_string_t* ds) {
  if (!server || !server->engine ||
      cdsp_get_state(server->engine) == CDSP_PROCESSING_STATE_INACTIVE) {
    reply_error(cmd_name, "ProcessingNotRunningError",
                "Processing is not running", ds);
    return;
  }

  bool is_capture = true;
  uint32_t channel = (uint32_t)-1;
  float min_freq = 20.0f;
  float max_freq = 20000.0f;
  uint32_t n_bins = 1024;
  float max_rate = 0.0f;

  cJSON* arg = cJSON_GetObjectItemCaseSensitive(root, "value");
  if (!arg || !cJSON_IsObject(arg)) {
    reply_error(cmd_name, "InvalidRequestError",
                "Arguments must be a JSON object", ds);
    return;
  }

  cJSON* item_side = cJSON_GetObjectItemCaseSensitive(arg, "side");
  if (!item_side || !cJSON_IsString(item_side)) {
    reply_error(cmd_name, "InvalidRequestError",
                "Missing or invalid 'side' parameter", ds);
    return;
  }
  if (strcmp(item_side->valuestring, "capture") == 0) {
    is_capture = true;
  } else if (strcmp(item_side->valuestring, "playback") == 0) {
    is_capture = false;
  } else {
    reply_error(cmd_name, "InvalidValueError",
                "side must be 'capture' or 'playback'", ds);
    return;
  }

  cJSON* item_chan = cJSON_GetObjectItemCaseSensitive(arg, "channel");
  if (item_chan && !cJSON_IsNull(item_chan)) {
    if (cJSON_IsNumber(item_chan)) {
      if (item_chan->valueint < 0) {
        reply_error(cmd_name, "InvalidValueError",
                    "channel must be non-negative", ds);
        return;
      }
      channel = (uint32_t)item_chan->valueint;
    } else {
      reply_error(cmd_name, "InvalidValueError",
                  "channel must be an integer or null", ds);
      return;
    }
  }

  cJSON* item_min = cJSON_GetObjectItemCaseSensitive(arg, "min_freq");
  if (item_min && cJSON_IsNumber(item_min)) {
    min_freq = (float)item_min->valuedouble;
  }
  if (min_freq <= 0.0f) {
    reply_error(cmd_name, "InvalidValueError",
                "min_freq must be greater than 0", ds);
    return;
  }

  cJSON* item_max = cJSON_GetObjectItemCaseSensitive(arg, "max_freq");
  if (item_max && cJSON_IsNumber(item_max)) {
    max_freq = (float)item_max->valuedouble;
  }
  if (max_freq <= min_freq) {
    reply_error(cmd_name, "InvalidValueError",
                "max_freq must be greater than min_freq", ds);
    return;
  }

  cJSON* item_bins = cJSON_GetObjectItemCaseSensitive(arg, "n_bins");
  if (item_bins && cJSON_IsNumber(item_bins)) {
    n_bins = (uint32_t)item_bins->valueint;
  }
  if (n_bins < 2) {
    reply_error(cmd_name, "InvalidRequestError", "n_bins must be at least 2",
                ds);
    return;
  }

  cJSON* item_rate = cJSON_GetObjectItemCaseSensitive(arg, "max_rate");
  if (item_rate && cJSON_IsNumber(item_rate)) {
    max_rate = (float)item_rate->valuedouble;
  }

  if (server) {
    server->client_sessions[client_idx].spectrum_subscribed = true;
    server->client_sessions[client_idx].spectrum_is_capture = is_capture;
    server->client_sessions[client_idx].spectrum_channel = channel;
    server->client_sessions[client_idx].spectrum_min_freq = min_freq;
    server->client_sessions[client_idx].spectrum_max_freq = max_freq;
    server->client_sessions[client_idx].spectrum_n_bins = n_bins;
    server->client_sessions[client_idx].spectrum_max_rate = max_rate;
    server->client_sessions[client_idx].last_spectrum_push_time = 0;
  }
  reply_ok(cmd_name, NULL, ds);
}

static void handle_cmd_stop_subscription(websocket_server_t* server,
                                         int client_idx, const char* cmd_name,
                                         cJSON* root, dyn_string_t* ds) {
  (void)root;
  if (server) {
    bool active =
        server->client_sessions[client_idx].state_subscribed ||
        server->client_sessions[client_idx].vu_subscribed ||
        server->client_sessions[client_idx].signal_levels_subscribed ||
        server->client_sessions[client_idx].spectrum_subscribed;
    if (active) {
      server->client_sessions[client_idx].state_subscribed = false;
      server->client_sessions[client_idx].vu_subscribed = false;
      server->client_sessions[client_idx].signal_levels_subscribed = false;
      server->client_sessions[client_idx].spectrum_subscribed = false;
      reply_ok(cmd_name, NULL, ds);
    } else {
      reply_error(cmd_name, "InvalidRequestError", "No active subscription",
                  ds);
    }
  } else {
    reply_error(cmd_name, "InvalidRequestError", "No active subscription", ds);
  }
}

static void handle_cmd_get_config_file_path(websocket_server_t* server,
                                            int client_idx,
                                            const char* cmd_name, cJSON* root,
                                            dyn_string_t* ds) {
  (void)client_idx;
  (void)root;
  char* path = (server && server->engine)
                   ? cdsp_get_config_file_path(server->engine)
                   : NULL;
  if (path) {
    reply_ok(cmd_name, cJSON_CreateString(path), ds);
    free(path);
  } else {
    reply_ok(cmd_name, cJSON_CreateNull(), ds);
  }
}

static void handle_cmd_get_previous_config(websocket_server_t* server,
                                           int client_idx, const char* cmd_name,
                                           cJSON* root, dyn_string_t* ds) {
  (void)client_idx;
  (void)root;
  char* prev = NULL;
  if (server && server->engine) {
    cdsp_get_previous_config_yaml(server->engine, &prev);
  }
  if (prev) {
    reply_ok(cmd_name, cJSON_CreateString(prev), ds);
    free(prev);
  } else {
    reply_ok(cmd_name, cJSON_CreateNull(), ds);
  }
}

static void handle_cmd_get_state_file_path(websocket_server_t* server,
                                           int client_idx, const char* cmd_name,
                                           cJSON* root, dyn_string_t* ds) {
  (void)client_idx;
  (void)root;
  const char* path = (server && server->engine)
                         ? cdsp_get_state_file_path(server->engine)
                         : NULL;
  if (path) {
    reply_ok(cmd_name, cJSON_CreateString(path), ds);
  } else {
    reply_ok(cmd_name, cJSON_CreateNull(), ds);
  }
}

static void handle_cmd_get_state_file_updated(websocket_server_t* server,
                                              int client_idx,
                                              const char* cmd_name, cJSON* root,
                                              dyn_string_t* ds) {
  (void)client_idx;
  (void)root;
  bool updated = (server && server->engine)
                     ? cdsp_get_state_file_updated(server->engine)
                     : true;
  reply_ok(cmd_name, cJSON_CreateBool(updated), ds);
}

static void handle_cmd_get_config(websocket_server_t* server, int client_idx,
                                  const char* cmd_name, cJSON* root,
                                  dyn_string_t* ds) {
  (void)client_idx;
  (void)root;
  char* config_str = NULL;
  bool ok = false;
  if (server && server->engine) {
    if (strcmp(cmd_name, "GetConfig") == 0) {
      ok = cdsp_get_active_config_yaml(server->engine, &config_str);
    } else {
      ok = cdsp_get_active_config_json(server->engine, &config_str);
    }
  }
  if (ok && config_str) {
    reply_ok(cmd_name, cJSON_CreateString(config_str), ds);
    free(config_str);
  } else {
    reply_error(cmd_name, "InvalidRequestError", "No active config", ds);
  }
}

static void handle_cmd_get_config_title(websocket_server_t* server,
                                        int client_idx, const char* cmd_name,
                                        cJSON* root, dyn_string_t* ds) {
  (void)client_idx;
  (void)root;
  char* title =
      (server && server->engine) ? cdsp_get_config_title(server->engine) : NULL;
  if (title) {
    reply_ok(cmd_name, cJSON_CreateString(title), ds);
    free(title);
  } else {
    reply_ok(cmd_name, cJSON_CreateString(""), ds);
  }
}

static void handle_cmd_get_config_description(websocket_server_t* server,
                                              int client_idx,
                                              const char* cmd_name, cJSON* root,
                                              dyn_string_t* ds) {
  (void)client_idx;
  (void)root;
  char* desc = (server && server->engine)
                   ? cdsp_get_config_description(server->engine)
                   : NULL;
  if (desc) {
    reply_ok(cmd_name, cJSON_CreateString(desc), ds);
    free(desc);
  } else {
    reply_ok(cmd_name, cJSON_CreateString(""), ds);
  }
}

static void handle_cmd_reload(websocket_server_t* server, int client_idx,
                              const char* cmd_name, cJSON* root,
                              dyn_string_t* ds) {
  (void)client_idx;
  (void)root;
  cdsp_backend_error_t err = {0};
  if (server && server->engine && cdsp_reload_config(server->engine, &err)) {
    reply_ok(cmd_name, NULL, ds);
  } else {
    reply_error(cmd_name, get_websocket_error_key(err.type),
                err.message[0] ? err.message : "Failed to reload config", ds);
  }
}

static void handle_cmd_stop(websocket_server_t* server, int client_idx,
                            const char* cmd_name, cJSON* root,
                            dyn_string_t* ds) {
  (void)client_idx;
  (void)root;
  if (server && server->engine) {
    cdsp_stop(server->engine);
  }
  reply_ok(cmd_name, NULL, ds);
}

static void handle_cmd_exit(websocket_server_t* server, int client_idx,
                            const char* cmd_name, cJSON* root,
                            dyn_string_t* ds) {
  (void)client_idx;
  (void)root;
  if (server && server->engine) {
    cdsp_stop(server->engine);
  }
  reply_ok(cmd_name, NULL, ds);
}

static void handle_cmd_set_config_file_path(websocket_server_t* server,
                                            int client_idx,
                                            const char* cmd_name, cJSON* root,
                                            dyn_string_t* ds) {
  (void)client_idx;
  cJSON* arg = cJSON_GetObjectItemCaseSensitive(root, "value");
  if (arg && cJSON_IsString(arg) && arg->valuestring) {
    const char* path = arg->valuestring;
    if (server && server->engine) {
      cdsp_set_config_file_path(server->engine, path);
    }
    reply_ok(cmd_name, NULL, ds);
  } else {
    reply_error(cmd_name, "InvalidRequestError",
                "Could not parse Config File Path", ds);
  }
}

static void handle_cmd_set_config_json(websocket_server_t* server,
                                       int client_idx, const char* cmd_name,
                                       cJSON* root, dyn_string_t* ds) {
  (void)client_idx;
  cJSON* arg = cJSON_GetObjectItemCaseSensitive(root, "value");
  if (arg && cJSON_IsString(arg) && arg->valuestring) {
    const char* new_json = arg->valuestring;
    cdsp_backend_error_t err = {0};
    bool ok = server && server->engine &&
              cdsp_set_config_json(server->engine, new_json, &err);
    if (ok) {
      reply_ok(cmd_name, NULL, ds);
    } else {
      reply_error(cmd_name, get_websocket_error_key(err.type), err.message, ds);
    }
  } else {
    reply_error(cmd_name, "InvalidRequestError", "Could not parse Config JSON",
                ds);
  }
}

static void handle_cmd_set_config_yaml(websocket_server_t* server,
                                       int client_idx, const char* cmd_name,
                                       cJSON* root, dyn_string_t* ds) {
  (void)client_idx;
  cJSON* arg = cJSON_GetObjectItemCaseSensitive(root, "value");
  if (arg && cJSON_IsString(arg) && arg->valuestring) {
    const char* new_yaml = arg->valuestring;
    cdsp_backend_error_t err = {0};
    bool ok = server && server->engine &&
              cdsp_set_config_yaml(server->engine, new_yaml, &err);
    if (ok) {
      reply_ok(cmd_name, NULL, ds);
    } else {
      reply_error(cmd_name, get_websocket_error_key(err.type), err.message, ds);
    }
  } else {
    reply_error(cmd_name, "InvalidRequestError", "Could not parse Config YAML",
                ds);
  }
}

static void handle_cmd_get_config_value(websocket_server_t* server,
                                        int client_idx, const char* cmd_name,
                                        cJSON* root, dyn_string_t* ds) {
  (void)client_idx;
  cJSON* arg = cJSON_GetObjectItemCaseSensitive(root, "value");
  if (arg && cJSON_IsString(arg) && arg->valuestring) {
    const char* pointer = arg->valuestring;
    char* val = (server && server->engine)
                    ? cdsp_get_config_value(server->engine, pointer)
                    : NULL;
    if (val) {
      cJSON* parsed_val = cJSON_Parse(val);
      if (parsed_val) {
        reply_ok(cmd_name, parsed_val, ds);
      } else {
        reply_ok(cmd_name, cJSON_CreateString(val), ds);
      }
      free(val);
    } else {
      char msg[256];
      snprintf(msg, sizeof(msg), "Path not found: %s", pointer);
      reply_error(cmd_name, "InvalidRequestError", msg, ds);
    }
  } else {
    reply_error(cmd_name, "InvalidRequestError", "Could not parse pointer", ds);
  }
}

static void handle_cmd_set_config_value(websocket_server_t* server,
                                        int client_idx, const char* cmd_name,
                                        cJSON* root, dyn_string_t* ds) {
  (void)client_idx;
  char pointer[256] = "";
  char* val_json = NULL;
  cJSON* arg = root;
  if (arg && cJSON_IsArray(arg) && cJSON_GetArraySize(arg) >= 2) {
    cJSON* p_node = cJSON_GetArrayItem(arg, 0);
    cJSON* v_node = cJSON_GetArrayItem(arg, 1);
    if (p_node && cJSON_IsString(p_node)) {
      strncpy(pointer, p_node->valuestring, sizeof(pointer) - 1);
    }
    if (v_node) {
      val_json = cJSON_PrintUnformatted(v_node);
    }
  } else if (arg && cJSON_IsObject(arg)) {
    cJSON* p_node = cJSON_GetObjectItemCaseSensitive(arg, "pointer");
    cJSON* v_node = cJSON_GetObjectItemCaseSensitive(arg, "value");
    if (p_node && cJSON_IsString(p_node)) {
      strncpy(pointer, p_node->valuestring, sizeof(pointer) - 1);
    }
    if (v_node) {
      val_json = cJSON_PrintUnformatted(v_node);
    }
  }
  if (pointer[0] != '\0' && val_json) {
    cdsp_backend_error_t err = {0};
    bool ok = server && server->engine &&
              cdsp_set_config_value(server->engine, pointer, val_json, &err);
    if (ok) {
      reply_ok(cmd_name, NULL, ds);
    } else {
      reply_error(cmd_name, get_websocket_error_key(err.type),
                  err.message[0] ? err.message : "Path not found", ds);
    }
    free(val_json);
  } else {
    reply_error(cmd_name, "InvalidRequestError",
                "Could not parse SetConfigValue command", ds);
  }
}

static void handle_cmd_patch_config(websocket_server_t* server, int client_idx,
                                    const char* cmd_name, cJSON* root,
                                    dyn_string_t* ds) {
  (void)client_idx;
  cJSON* arg = cJSON_GetObjectItemCaseSensitive(root, "value");
  if (arg && cJSON_IsObject(arg)) {
    char* patch_str = cJSON_PrintUnformatted(arg);
    if (patch_str) {
      cdsp_backend_error_t err = {0};
      bool ok = server && server->engine &&
                cdsp_patch_config(server->engine, patch_str, &err);
      if (ok) {
        reply_ok(cmd_name, NULL, ds);
      } else {
        reply_error(cmd_name, get_websocket_error_key(err.type),
                    err.message[0] ? err.message : "Invalid patch", ds);
      }
      free(patch_str);
    } else {
      reply_error(cmd_name, "InvalidRequestError",
                  "Could not format patch JSON", ds);
    }
  } else {
    reply_error(cmd_name, "InvalidRequestError",
                "Could not parse PatchConfig command", ds);
  }
}

static void handle_cmd_read_config_json(websocket_server_t* server,
                                        int client_idx, const char* cmd_name,
                                        cJSON* root, dyn_string_t* ds) {
  (void)server;
  (void)client_idx;
  cJSON* arg = cJSON_GetObjectItemCaseSensitive(root, "value");
  if (arg && cJSON_IsString(arg) && arg->valuestring) {
    const char* config_json = arg->valuestring;
    char* result = NULL;
    cdsp_config_error_type_t err_type = CDSP_CONFIG_ERR_NONE;
    if (cdsp_validate_config_json(config_json, &result, &err_type) &&
        err_type == CDSP_CONFIG_ERR_NONE) {
      reply_ok(cmd_name, cJSON_CreateString(result ? result : config_json), ds);
    } else {
      const char* err_key = (err_type == CDSP_CONFIG_ERR_PARSE)
                                ? "ConfigReadError"
                                : "ConfigValidationError";
      reply_error(cmd_name, err_key, result ? result : "Invalid config", ds);
    }
    if (result) free(result);
  } else {
    reply_error(cmd_name, "InvalidRequestError",
                "Could not parse input config JSON", ds);
  }
}

static void handle_cmd_read_config_yaml(websocket_server_t* server,
                                        int client_idx, const char* cmd_name,
                                        cJSON* root, dyn_string_t* ds) {
  (void)server;
  (void)client_idx;
  cJSON* arg = cJSON_GetObjectItemCaseSensitive(root, "value");
  if (arg && cJSON_IsString(arg) && arg->valuestring) {
    const char* config_yaml = arg->valuestring;
    char* result = NULL;
    cdsp_config_error_type_t err_type = CDSP_CONFIG_ERR_NONE;
    if (cdsp_validate_config_yaml(config_yaml, &result, &err_type) &&
        err_type == CDSP_CONFIG_ERR_NONE) {
      reply_ok(cmd_name, cJSON_CreateString(result ? result : config_yaml), ds);
    } else {
      const char* err_key = (err_type == CDSP_CONFIG_ERR_PARSE)
                                ? "ConfigReadError"
                                : "ConfigValidationError";
      reply_error(cmd_name, err_key, result ? result : "Invalid config", ds);
    }
    if (result) free(result);
  } else {
    reply_error(cmd_name, "InvalidRequestError",
                "Could not parse input config YAML", ds);
  }
}

static void handle_cmd_read_config_file(websocket_server_t* server,
                                        int client_idx, const char* cmd_name,
                                        cJSON* root, dyn_string_t* ds) {
  (void)server;
  (void)client_idx;
  cJSON* arg = cJSON_GetObjectItemCaseSensitive(root, "value");
  if (arg && cJSON_IsString(arg) && arg->valuestring) {
    const char* path = arg->valuestring;
    char* result = NULL;
    cdsp_config_error_type_t err_type = CDSP_CONFIG_ERR_NONE;
    if (cdsp_validate_config_file(path, &result, &err_type) &&
        err_type == CDSP_CONFIG_ERR_NONE) {
      reply_ok(cmd_name, cJSON_CreateString(result), ds);
    } else {
      const char* err_key = (err_type == CDSP_CONFIG_ERR_PARSE)
                                ? "ConfigReadError"
                                : "ConfigValidationError";
      reply_error(cmd_name, err_key, result ? result : "Invalid config file",
                  ds);
    }
    if (result) free(result);
  } else {
    reply_error(cmd_name, "InvalidRequestError",
                "Could not parse input config file path", ds);
  }
}

static void handle_get_signal_single(websocket_server_t* server,
                                     const char* cmd_name, bool is_capture,
                                     bool is_rms, dyn_string_t* ds) {
  cdsp_vu_levels_t vu = {0};
  if (server && server->engine && cdsp_get_vu_levels(server->engine, &vu)) {
    float* arr = NULL;
    size_t count = 0;
    if (is_capture) {
      arr = is_rms ? vu.capture_rms : vu.capture_peak;
      count = vu.capture_channels;
    } else {
      arr = is_rms ? vu.playback_rms : vu.playback_peak;
      count = vu.playback_channels;
    }
    reply_ok(cmd_name, cJSON_CreateFloatArray(arr, (int)count), ds);
    cdsp_free_vu_levels(&vu);
  } else {
    reply_ok(cmd_name, cJSON_CreateFloatArray(NULL, 0), ds);
  }
}

static void handle_get_signal_since_last(websocket_server_t* server,
                                         int client_idx, const char* cmd_name,
                                         bool is_capture, bool is_rms,
                                         dyn_string_t* ds) {
  if (server) {
    uint64_t since = 0;
    uint64_t now = get_time_ms();
    if (is_capture) {
      if (is_rms) {
        since = server->client_sessions[client_idx].last_cap_rms_time;
        server->client_sessions[client_idx].last_cap_rms_time = now;
      } else {
        since = server->client_sessions[client_idx].last_cap_peak_time;
        server->client_sessions[client_idx].last_cap_peak_time = now;
      }
    } else {
      if (is_rms) {
        since = server->client_sessions[client_idx].last_pb_rms_time;
        server->client_sessions[client_idx].last_pb_rms_time = now;
      } else {
        since = server->client_sessions[client_idx].last_pb_peak_time;
        server->client_sessions[client_idx].last_pb_peak_time = now;
      }
    }

    size_t ch = 0;
    float stack_vals[64];
    float* p_vals = stack_vals;
    float* dyn_vals = NULL;

    if (server->engine &&
        cdsp_get_signal_levels_since(server->engine, is_capture, is_rms, since,
                                     NULL, &ch) &&
        ch > 0) {
      if (ch > 64) {
        dyn_vals = (float*)malloc(ch * sizeof(float));
        p_vals = dyn_vals;
      }
      if (p_vals) {
        cdsp_get_signal_levels_since(server->engine, is_capture, is_rms, since,
                                     p_vals, &ch);
        reply_ok(cmd_name, cJSON_CreateFloatArray(p_vals, (int)ch), ds);
        if (dyn_vals) free(dyn_vals);
        return;
      }
    }
    reply_ok(cmd_name, cJSON_CreateFloatArray(NULL, 0), ds);
  } else {
    reply_ok(cmd_name, cJSON_CreateFloatArray(NULL, 0), ds);
  }
}

static void handle_get_signal_since(websocket_server_t* server,
                                    const char* cmd_name, cJSON* root,
                                    bool is_capture, bool is_rms,
                                    dyn_string_t* ds) {
  float secs = 0.0f;
  cJSON* arg = cJSON_GetObjectItemCaseSensitive(root, "value");
  if (arg && cJSON_IsNumber(arg)) {
    secs = (float)arg->valuedouble;
    if (server) {
      uint64_t now = get_time_ms();
      uint64_t since = now - (uint64_t)(secs * 1000.0f);

      size_t ch = 0;
      float stack_vals[64];
      float* p_vals = stack_vals;
      float* dyn_vals = NULL;

      if (server->engine &&
          cdsp_get_signal_levels_since(server->engine, is_capture, is_rms,
                                       since, NULL, &ch) &&
          ch > 0) {
        if (ch > 64) {
          dyn_vals = (float*)malloc(ch * sizeof(float));
          p_vals = dyn_vals;
        }
        if (p_vals) {
          cdsp_get_signal_levels_since(server->engine, is_capture, is_rms,
                                       since, p_vals, &ch);
          reply_ok(cmd_name, cJSON_CreateFloatArray(p_vals, (int)ch), ds);
          if (dyn_vals) free(dyn_vals);
          return;
        }
      }
      reply_ok(cmd_name, cJSON_CreateFloatArray(NULL, 0), ds);
    } else {
      reply_ok(cmd_name, cJSON_CreateFloatArray(NULL, 0), ds);
    }
  } else {
    reply_error(cmd_name, "InvalidRequestError", "Could not parse seconds", ds);
  }
}

static void handle_cmd_get_signal_levels(websocket_server_t* server,
                                         int client_idx, const char* cmd_name,
                                         cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  cdsp_vu_levels_t vu = {0};
  if (server && server->engine && cdsp_get_vu_levels(server->engine, &vu)) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddItemToObject(
        root, "playback_rms",
        cJSON_CreateFloatArray(vu.playback_rms, (int)vu.playback_channels));
    cJSON_AddItemToObject(
        root, "playback_peak",
        cJSON_CreateFloatArray(vu.playback_peak, (int)vu.playback_channels));
    cJSON_AddItemToObject(
        root, "capture_rms",
        cJSON_CreateFloatArray(vu.capture_rms, (int)vu.capture_channels));
    cJSON_AddItemToObject(
        root, "capture_peak",
        cJSON_CreateFloatArray(vu.capture_peak, (int)vu.capture_channels));
    reply_ok(cmd_name, root, ds);
    cdsp_free_vu_levels(&vu);
  } else {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "playback_rms",
                          cJSON_CreateFloatArray(NULL, 0));
    cJSON_AddItemToObject(root, "playback_peak",
                          cJSON_CreateFloatArray(NULL, 0));
    cJSON_AddItemToObject(root, "capture_rms",
                          cJSON_CreateFloatArray(NULL, 0));
    cJSON_AddItemToObject(root, "capture_peak",
                          cJSON_CreateFloatArray(NULL, 0));
    reply_ok(cmd_name, root, ds);
  }
}

static void handle_cmd_get_signal_levels_since_last(websocket_server_t* server,
                                                    int client_idx,
                                                    const char* cmd_name,
                                                    cJSON* arg,
                                                    dyn_string_t* ds) {
  (void)arg;
  if (server) {
    uint64_t cap_rms_since =
        server->client_sessions[client_idx].last_cap_rms_time;
    uint64_t cap_pk_since =
        server->client_sessions[client_idx].last_cap_peak_time;
    uint64_t pb_rms_since =
        server->client_sessions[client_idx].last_pb_rms_time;
    uint64_t pb_pk_since =
        server->client_sessions[client_idx].last_pb_peak_time;
    uint64_t now = get_time_ms();
    server->client_sessions[client_idx].last_cap_rms_time = now;
    server->client_sessions[client_idx].last_cap_peak_time = now;
    server->client_sessions[client_idx].last_pb_rms_time = now;
    server->client_sessions[client_idx].last_pb_peak_time = now;

    size_t c_ch = 0;
    size_t p_ch = 0;
    float* c_rms = NULL;
    float* c_pk = NULL;
    float* p_rms = NULL;
    float* p_pk = NULL;

    if (server->engine) {
      cdsp_get_signal_levels_since(server->engine, true, true, cap_rms_since,
                                   NULL, &c_ch);
      cdsp_get_signal_levels_since(server->engine, false, true, pb_rms_since,
                                   NULL, &p_ch);
      if (c_ch > 0) {
        c_rms = (float*)calloc(c_ch, sizeof(float));
        c_pk = (float*)calloc(c_ch, sizeof(float));
        if (c_rms) {
          cdsp_get_signal_levels_since(server->engine, true, true,
                                       cap_rms_since, c_rms, &c_ch);
        }
        if (c_pk) {
          cdsp_get_signal_levels_since(server->engine, true, false,
                                       cap_pk_since, c_pk, &c_ch);
        }
      }
      if (p_ch > 0) {
        p_rms = (float*)calloc(p_ch, sizeof(float));
        p_pk = (float*)calloc(p_ch, sizeof(float));
        if (p_rms) {
          cdsp_get_signal_levels_since(server->engine, false, true,
                                       pb_rms_since, p_rms, &p_ch);
        }
        if (p_pk) {
          cdsp_get_signal_levels_since(server->engine, false, false,
                                       pb_pk_since, p_pk, &p_ch);
        }
      }
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "playback_rms",
                          cJSON_CreateFloatArray(p_rms, (int)p_ch));
    cJSON_AddItemToObject(root, "playback_peak",
                          cJSON_CreateFloatArray(p_pk, (int)p_ch));
    cJSON_AddItemToObject(root, "capture_rms",
                          cJSON_CreateFloatArray(c_rms, (int)c_ch));
    cJSON_AddItemToObject(root, "capture_peak",
                          cJSON_CreateFloatArray(c_pk, (int)c_ch));

    reply_ok(cmd_name, root, ds);

    if (c_rms) free(c_rms);
    if (c_pk) free(c_pk);
    if (p_rms) free(p_rms);
    if (p_pk) free(p_pk);
  } else {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "playback_rms",
                          cJSON_CreateFloatArray(NULL, 0));
    cJSON_AddItemToObject(root, "playback_peak",
                          cJSON_CreateFloatArray(NULL, 0));
    cJSON_AddItemToObject(root, "capture_rms",
                          cJSON_CreateFloatArray(NULL, 0));
    cJSON_AddItemToObject(root, "capture_peak",
                          cJSON_CreateFloatArray(NULL, 0));
    reply_ok(cmd_name, root, ds);
  }
}

static void handle_cmd_get_signal_levels_since(websocket_server_t* server,
                                               int client_idx,
                                               const char* cmd_name, cJSON* arg,
                                               dyn_string_t* ds) {
  (void)client_idx;
  float secs = 0.0f;
  if (arg && cJSON_IsNumber(arg)) {
    secs = (float)arg->valuedouble;
    if (server) {
      uint64_t now = get_time_ms();
      uint64_t since = now - (uint64_t)(secs * 1000.0f);

      size_t c_ch = 0;
      size_t p_ch = 0;
      float* c_rms = NULL;
      float* c_pk = NULL;
      float* p_rms = NULL;
      float* p_pk = NULL;

      if (server->engine) {
        cdsp_get_signal_levels_since(server->engine, true, true, since, NULL,
                                     &c_ch);
        cdsp_get_signal_levels_since(server->engine, false, true, since, NULL,
                                     &p_ch);
        if (c_ch > 0) {
          c_rms = (float*)calloc(c_ch, sizeof(float));
          c_pk = (float*)calloc(c_ch, sizeof(float));
          if (c_rms) {
            cdsp_get_signal_levels_since(server->engine, true, true, since,
                                         c_rms, &c_ch);
          }
          if (c_pk) {
            cdsp_get_signal_levels_since(server->engine, true, false, since,
                                         c_pk, &c_ch);
          }
        }
        if (p_ch > 0) {
          p_rms = (float*)calloc(p_ch, sizeof(float));
          p_pk = (float*)calloc(p_ch, sizeof(float));
          if (p_rms) {
            cdsp_get_signal_levels_since(server->engine, false, true, since,
                                         p_rms, &p_ch);
          }
          if (p_pk) {
            cdsp_get_signal_levels_since(server->engine, false, false, since,
                                         p_pk, &p_ch);
          }
        }
      }

      cJSON* root = cJSON_CreateObject();
      cJSON_AddItemToObject(root, "playback_rms",
                            cJSON_CreateFloatArray(p_rms, (int)p_ch));
      cJSON_AddItemToObject(root, "playback_peak",
                            cJSON_CreateFloatArray(p_pk, (int)p_ch));
      cJSON_AddItemToObject(root, "capture_rms",
                            cJSON_CreateFloatArray(c_rms, (int)c_ch));
      cJSON_AddItemToObject(root, "capture_peak",
                            cJSON_CreateFloatArray(c_pk, (int)c_ch));

      reply_ok(cmd_name, root, ds);

      if (c_rms) free(c_rms);
      if (c_pk) free(c_pk);
      if (p_rms) free(p_rms);
      if (p_pk) free(p_pk);
    } else {
      cJSON* root = cJSON_CreateObject();
      cJSON_AddItemToObject(root, "playback_rms",
                            cJSON_CreateFloatArray(NULL, 0));
      cJSON_AddItemToObject(root, "playback_peak",
                            cJSON_CreateFloatArray(NULL, 0));
      cJSON_AddItemToObject(root, "capture_rms",
                            cJSON_CreateFloatArray(NULL, 0));
      cJSON_AddItemToObject(root, "capture_peak",
                            cJSON_CreateFloatArray(NULL, 0));
      reply_ok(cmd_name, root, ds);
    }
  } else {
    reply_error(cmd_name, "InvalidRequestError", "Could not parse seconds", ds);
  }
}

static void handle_cmd_get_signal_peaks_since_start(websocket_server_t* server,
                                                    int client_idx,
                                                    const char* cmd_name,
                                                    cJSON* arg,
                                                    dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  cJSON* root = cJSON_CreateObject();
  cJSON_AddItemToObject(
      root, "capture",
      cJSON_CreateDoubleArray(server->capture_global_peaks,
                              (int)server->capture_global_peaks_count));
  cJSON_AddItemToObject(
      root, "playback",
      cJSON_CreateDoubleArray(server->playback_global_peaks,
                              (int)server->playback_global_peaks_count));
  reply_ok(cmd_name, root, ds);
}

static void handle_cmd_reset_signal_peaks_since_start(
    websocket_server_t* server, int client_idx, const char* cmd_name,
    cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  for (size_t i = 0; i < server->capture_global_peaks_count; i++) {
    server->capture_global_peaks[i] = -1000.0;
  }
  for (size_t i = 0; i < server->playback_global_peaks_count; i++) {
    server->playback_global_peaks[i] = -1000.0;
  }
  reply_ok(cmd_name, NULL, ds);
}

static void handle_cmd_get_channel_labels(websocket_server_t* server,
                                          int client_idx, const char* cmd_name,
                                          cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  char** play_labels = NULL;
  size_t play_count = 0;
  char** cap_labels = NULL;
  size_t cap_count = 0;

  bool ok = server && server->engine &&
            cdsp_get_channel_labels(server->engine, &play_labels, &play_count,
                                    &cap_labels, &cap_count);

  cJSON* root = cJSON_CreateObject();

  cJSON* play_arr = NULL;
  if (ok && play_labels && play_count > 0) {
    play_arr = cJSON_CreateArray();
    for (size_t i = 0; i < play_count; i++) {
      if (play_labels[i]) {
        cJSON_AddItemToArray(play_arr, cJSON_CreateString(play_labels[i]));
      } else {
        cJSON_AddItemToArray(play_arr, cJSON_CreateNull());
      }
    }
  } else {
    play_arr = cJSON_CreateNull();
  }
  cJSON_AddItemToObject(root, "playback", play_arr);

  cJSON* cap_arr = NULL;
  if (ok && cap_labels && cap_count > 0) {
    cap_arr = cJSON_CreateArray();
    for (size_t i = 0; i < cap_count; i++) {
      if (cap_labels[i]) {
        cJSON_AddItemToArray(cap_arr, cJSON_CreateString(cap_labels[i]));
      } else {
        cJSON_AddItemToArray(cap_arr, cJSON_CreateNull());
      }
    }
  } else {
    cap_arr = cJSON_CreateNull();
  }
  cJSON_AddItemToObject(root, "capture", cap_arr);

  reply_ok(cmd_name, root, ds);

  if (play_labels) cdsp_free_channel_labels(play_labels, play_count);
  if (cap_labels) cdsp_free_channel_labels(cap_labels, cap_count);
}

static void handle_cmd_get_signal_range(websocket_server_t* server,
                                        int client_idx, const char* cmd_name,
                                        cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  cdsp_vu_levels_t vu = {0};
  if (server && server->engine && cdsp_get_vu_levels(server->engine, &vu)) {
    size_t count = vu.playback_channels;
    float max_peak = -1000.0f;
    for (size_t i = 0; i < count; i++) {
      float pk = vu.playback_peak[i];
      if (pk > max_peak) max_peak = pk;
    }
    float range = 2.0f * db_to_amplitude(max_peak);
    reply_ok(cmd_name, cJSON_CreateNumber(range), ds);
    cdsp_free_vu_levels(&vu);
  } else {
    reply_ok(cmd_name, cJSON_CreateNumber(0.0), ds);
  }
}

static void handle_cmd_get_spectrum(websocket_server_t* server, int client_idx,
                                    const char* cmd_name, cJSON* root,
                                    dyn_string_t* ds) {
  (void)client_idx;
  if (!server || !server->engine ||
      cdsp_get_state(server->engine) == CDSP_PROCESSING_STATE_INACTIVE) {
    reply_error(cmd_name, "ProcessingNotRunningError",
                "Processing is not running", ds);
    return;
  }

  bool is_capture = true;
  uint32_t channel = (uint32_t)-1;
  float min_freq = 20.0f;
  float max_freq = 20000.0f;
  uint32_t n_bins = 1024;

  cJSON* arg = cJSON_GetObjectItemCaseSensitive(root, "value");
  if (!arg || !cJSON_IsObject(arg)) {
    arg = root;
  }

  if (!arg || !cJSON_IsObject(arg)) {
    reply_error(cmd_name, "InvalidRequestError",
                "Arguments must be a JSON object", ds);
    return;
  }

  cJSON* item_side = cJSON_GetObjectItemCaseSensitive(arg, "side");
  if (!item_side || !cJSON_IsString(item_side)) {
    reply_error(cmd_name, "InvalidRequestError",
                "Missing or invalid 'side' parameter", ds);
    return;
  }
  if (strcmp(item_side->valuestring, "capture") == 0) {
    is_capture = true;
  } else if (strcmp(item_side->valuestring, "playback") == 0) {
    is_capture = false;
  } else {
    reply_error(cmd_name, "InvalidValueError",
                "side must be 'capture' or 'playback'", ds);
    return;
  }

  cJSON* item_chan = cJSON_GetObjectItemCaseSensitive(arg, "channel");
  if (item_chan && !cJSON_IsNull(item_chan)) {
    if (cJSON_IsNumber(item_chan)) {
      if (item_chan->valueint < 0) {
        reply_error(cmd_name, "InvalidValueError",
                    "channel must be non-negative", ds);
        return;
      }
      channel = (uint32_t)item_chan->valueint;
    } else {
      reply_error(cmd_name, "InvalidValueError",
                  "channel must be an integer or null", ds);
      return;
    }
  }

  cJSON* item_min = cJSON_GetObjectItemCaseSensitive(arg, "min_freq");
  if (item_min && cJSON_IsNumber(item_min)) {
    min_freq = (float)item_min->valuedouble;
  }
  if (min_freq <= 0.0f) {
    reply_error(cmd_name, "InvalidValueError",
                "min_freq must be greater than 0", ds);
    return;
  }

  cJSON* item_max = cJSON_GetObjectItemCaseSensitive(arg, "max_freq");
  if (item_max && cJSON_IsNumber(item_max)) {
    max_freq = (float)item_max->valuedouble;
  }
  if (max_freq <= min_freq) {
    reply_error(cmd_name, "InvalidValueError",
                "max_freq must be greater than min_freq", ds);
    return;
  }

  cJSON* item_bins = cJSON_GetObjectItemCaseSensitive(arg, "n_bins");
  if (item_bins && cJSON_IsNumber(item_bins)) {
    n_bins = (uint32_t)item_bins->valueint;
  }
  if (n_bins < 2) {
    reply_error(cmd_name, "InvalidRequestError", "n_bins must be at least 2",
                ds);
    return;
  }

  cdsp_spectrum_side_t side_val =
      is_capture ? CDSP_SPECTRUM_SIDE_CAPTURE : CDSP_SPECTRUM_SIDE_PLAYBACK;
  const uint32_t* chan_ptr = (channel == (uint32_t)-1) ? NULL : &channel;

  cdsp_spectrum_t spec = {0};
  bool spec_ok = server && server->engine &&
                 cdsp_get_spectrum(server->engine, side_val, chan_ptr, min_freq,
                                   max_freq, n_bins, &spec);
  if (spec_ok) {
    cJSON* spec_json = serialize_spectrum(&spec);
    if (spec_json) {
      reply_ok(cmd_name, spec_json, ds);
    } else {
      reply_error(cmd_name, "UnknownError", NULL, ds);
    }
    cdsp_free_spectrum(&spec);
  } else {
    reply_error(cmd_name, "DeviceError", "Failed to compute spectrum", ds);
  }
}

static void handle_get_available_devices(websocket_server_t* server,
                                         const char* cmd_name, cJSON* root,
                                         bool is_capture, dyn_string_t* ds) {
  const char* backend = NULL;
  cJSON* arg = cJSON_GetObjectItemCaseSensitive(root, "backend");
  if (!arg) {
    arg = cJSON_GetObjectItemCaseSensitive(root, "value");
  }
  if (arg && cJSON_IsString(arg) && arg->valuestring) {
    backend = arg->valuestring;
    cdsp_device_info_t* devs = NULL;
    size_t count = 0;
    bool ok = server && server->engine &&
              cdsp_get_available_devices(backend, is_capture, &devs, &count);
    if (ok && devs) {
      cJSON* arr = cJSON_CreateArray();
      for (size_t i = 0; i < count; i++) {
        cJSON* tuple = cJSON_CreateArray();
        const char* id =
            (devs[i].identifier[0] != '\0') ? devs[i].identifier : devs[i].name;
        cJSON_AddItemToArray(tuple, cJSON_CreateString(id));
        cJSON_AddItemToArray(tuple, cJSON_CreateString(devs[i].name));
        cJSON_AddItemToArray(arr, tuple);
      }
      reply_ok(cmd_name, arr, ds);
      free(devs);
    } else {
      if (devs) free(devs);
      reply_ok(cmd_name, cJSON_CreateArray(), ds);
    }
  } else {
    reply_error(cmd_name, "InvalidRequestError", "Could not parse backend", ds);
  }
}

static void handle_get_device_capabilities(websocket_server_t* server,
                                           const char* cmd_name, cJSON* root,
                                           bool is_capture, dyn_string_t* ds) {
  (void)server;
  char backend[128] = "";
  char device[256] = "";
  bool ok = false;
  cJSON* b_top = cJSON_GetObjectItemCaseSensitive(root, "backend");
  cJSON* d_top = cJSON_GetObjectItemCaseSensitive(root, "device");
  if (b_top && d_top && cJSON_IsString(b_top) && cJSON_IsString(d_top)) {
    strncpy(backend, b_top->valuestring, sizeof(backend) - 1);
    strncpy(device, d_top->valuestring, sizeof(device) - 1);
    ok = true;
  } else {
    cJSON* arg = cJSON_GetObjectItemCaseSensitive(root, "value");
    if (arg && cJSON_IsArray(arg) && cJSON_GetArraySize(arg) >= 2) {
      cJSON* b_node = cJSON_GetArrayItem(arg, 0);
      cJSON* d_node = cJSON_GetArrayItem(arg, 1);
      if (b_node && d_node && cJSON_IsString(b_node) &&
          cJSON_IsString(d_node)) {
        strncpy(backend, b_node->valuestring, sizeof(backend) - 1);
        strncpy(device, d_node->valuestring, sizeof(device) - 1);
        ok = true;
      }
    }
  }
  if (ok) {
    cdsp_device_descriptor_t* desc = NULL;
    cdsp_device_error_t d_err;
    memset(&d_err, 0, sizeof(d_err));
    bool cap_ok = cdsp_get_device_capabilities(backend, device, is_capture,
                                               &desc, &d_err);
    if (cap_ok && desc) {
      char* val = format_device_descriptor(desc);
      if (val) {
        cJSON* desc_obj = cJSON_Parse(val);
        free(val);
        if (desc_obj) {
          reply_ok(cmd_name, desc_obj, ds);
        } else {
          reply_error(cmd_name, "UnknownError", NULL, ds);
        }
      } else {
        reply_error(cmd_name, "DeviceError", "Out of memory", ds);
      }
      cdsp_free_device_capabilities(desc);
    } else {
      reply_error(cmd_name, get_websocket_device_error_key(d_err.type),
                  d_err.message, ds);
    }
  } else {
    reply_error(cmd_name, "InvalidRequestError",
                "Could not parse backend/device array", ds);
  }
}

static void handle_cmd_get_version(websocket_server_t* server, int client_idx,
                                   const char* cmd_name, cJSON* root,
                                   dyn_string_t* ds) {
  (void)server;
  (void)client_idx;
  (void)root;
  reply_ok(cmd_name, cJSON_CreateString(cdsp_get_version()), ds);
}

static void handle_cmd_get_state(websocket_server_t* server, int client_idx,
                                 const char* cmd_name, cJSON* root,
                                 dyn_string_t* ds) {
  (void)client_idx;
  (void)root;
  cdsp_processing_state_t state = CDSP_PROCESSING_STATE_INACTIVE;
  if (server && server->engine) {
    ws_state_update_t status;
    if (ws_engine_get_status(server->engine, &status)) {
      state = status.state;
    }
  }
  reply_ok(cmd_name, cJSON_CreateString(ws_processing_state_to_string(state)),
           ds);
}

static void handle_cmd_get_stop_reason(websocket_server_t* server,
                                       int client_idx, const char* cmd_name,
                                       cJSON* root, dyn_string_t* ds) {
  (void)client_idx;
  (void)root;
  cJSON* val = NULL;
  if (server && server->engine) {
    ws_state_update_t status = {0};
    if (ws_engine_get_status(server->engine, &status)) {
      val = serialize_stop_reason(&status.stop_reason);
    }
  }
  if (!val) {
    val = cJSON_CreateString("None");
  }
  reply_ok(cmd_name, val, ds);
}

static void handle_cmd_get_capture_rate(websocket_server_t* server,
                                        int client_idx, const char* cmd_name,
                                        cJSON* root, dyn_string_t* ds) {
  (void)client_idx;
  (void)root;
  ws_state_update_t status = {0};
  bool has_status =
      server && server->engine && ws_engine_get_status(server->engine, &status);
  int sr = 0;
  if (has_status && status.state == CDSP_PROCESSING_STATE_RUNNING) {
    sr = cdsp_get_capture_rate(server->engine);
  }
  reply_ok(cmd_name, cJSON_CreateNumber(sr), ds);
}

static void handle_cmd_get_rate_adjust(websocket_server_t* server,
                                       int client_idx, const char* cmd_name,
                                       cJSON* root, dyn_string_t* ds) {
  (void)client_idx;
  (void)root;
  double rate = 1.0;
  if (server && server->engine) {
    cdsp_get_processing_status(server->engine, &rate, NULL, NULL, NULL, NULL);
  }
  reply_ok(cmd_name, cJSON_CreateNumber(rate), ds);
}

static void handle_cmd_get_buffer_level(websocket_server_t* server,
                                        int client_idx, const char* cmd_name,
                                        cJSON* root, dyn_string_t* ds) {
  (void)client_idx;
  (void)root;
  double lvl = 0.0;
  if (server && server->engine) {
    cdsp_get_processing_status(server->engine, NULL, &lvl, NULL, NULL, NULL);
  }
  reply_ok(cmd_name, cJSON_CreateNumber((int)lvl), ds);
}

static void handle_cmd_get_clipped_samples(websocket_server_t* server,
                                           int client_idx, const char* cmd_name,
                                           cJSON* root, dyn_string_t* ds) {
  (void)client_idx;
  (void)root;
  uint64_t clips = 0;
  if (server && server->engine) {
    cdsp_get_processing_status(server->engine, NULL, NULL, &clips, NULL, NULL);
  }
  reply_ok(cmd_name, cJSON_CreateNumber((double)clips), ds);
}

static void handle_cmd_reset_clipped_samples(websocket_server_t* server,
                                             int client_idx,
                                             const char* cmd_name, cJSON* root,
                                             dyn_string_t* ds) {
  (void)client_idx;
  (void)root;
  if (server && server->engine) {
    cdsp_reset_clipped_samples(server->engine);
  }
  reply_ok(cmd_name, NULL, ds);
}

static void handle_cmd_get_processing_load(websocket_server_t* server,
                                           int client_idx, const char* cmd_name,
                                           cJSON* root, dyn_string_t* ds) {
  (void)client_idx;
  (void)root;
  double load = 0.0;
  if (server && server->engine) {
    cdsp_get_processing_status(server->engine, NULL, NULL, NULL, &load, NULL);
  }
  reply_ok(cmd_name, cJSON_CreateNumber(load), ds);
}

static void handle_cmd_get_resampler_load(websocket_server_t* server,
                                          int client_idx, const char* cmd_name,
                                          cJSON* root, dyn_string_t* ds) {
  (void)client_idx;
  (void)root;
  double load = 0.0;
  if (server && server->engine) {
    cdsp_get_processing_status(server->engine, NULL, NULL, NULL, NULL, &load);
  }
  reply_ok(cmd_name, cJSON_CreateNumber(load), ds);
}

static void handle_cmd_get_supported_device_types(websocket_server_t* server,
                                                  int client_idx,
                                                  const char* cmd_name,
                                                  cJSON* root,
                                                  dyn_string_t* ds) {
  (void)server;
  (void)client_idx;
  (void)root;
  char** play_types = NULL;
  size_t play_count = 0;
  char** cap_types = NULL;
  size_t cap_count = 0;

  cdsp_get_supported_device_types(&play_types, &play_count, &cap_types,
                                  &cap_count);

  cJSON* arr = cJSON_CreateArray();

  cJSON* play_arr = cJSON_CreateArray();
  if (play_types && play_count > 0) {
    for (size_t i = 0; i < play_count; i++) {
      cJSON_AddItemToArray(play_arr, cJSON_CreateString(play_types[i]));
    }
  }
  cJSON_AddItemToArray(arr, play_arr);

  cJSON* cap_arr = cJSON_CreateArray();
  if (cap_types && cap_count > 0) {
    for (size_t i = 0; i < cap_count; i++) {
      cJSON_AddItemToArray(cap_arr, cJSON_CreateString(cap_types[i]));
    }
  }
  cJSON_AddItemToArray(arr, cap_arr);

  reply_ok(cmd_name, arr, ds);

  if (play_types) cdsp_free_device_types(play_types, play_count);
  if (cap_types) cdsp_free_device_types(cap_types, cap_count);
}

static void handle_cmd_get_update_interval(websocket_server_t* server,
                                           int client_idx, const char* cmd_name,
                                           cJSON* root, dyn_string_t* ds) {
  (void)client_idx;
  (void)root;
  int interval = server ? (int)server->update_interval : 100;
  reply_ok(cmd_name, cJSON_CreateNumber(interval), ds);
}

static void handle_cmd_set_update_interval(websocket_server_t* server,
                                           int client_idx, const char* cmd_name,
                                           cJSON* root, dyn_string_t* ds) {
  (void)client_idx;
  cJSON* arg = cJSON_GetObjectItemCaseSensitive(root, "value");
  if (arg && cJSON_IsNumber(arg)) {
    float val = (float)arg->valuedouble;
    if (val >= 0.0f) {
      if (server) server->update_interval = (uint32_t)val;
      reply_ok(cmd_name, NULL, ds);
    } else {
      reply_error(cmd_name, "InvalidValueError", "Value must be >= 0", ds);
    }
  } else {
    reply_error(cmd_name, "InvalidRequestError",
                "Could not parse SetUpdateInterval argument", ds);
  }
}

void websocket_server_handle_command(websocket_server_t* server, int client_idx,
                                     const char* command_text,
                                     dyn_string_t* ds) {
  if (!server || !ds || !command_text || client_idx < 0 || client_idx >= 32)
    return;

  cJSON* root = cJSON_Parse(command_text);
  if (!root) {
    reply_invalid("Invalid JSON", ds);
    return;
  }

  if (!cJSON_IsObject(root)) {
    reply_invalid("Command must be a JSON object", ds);
    cJSON_Delete(root);
    return;
  }

  cJSON* cmd_node = cJSON_GetObjectItemCaseSensitive(root, "command");
  if (!cmd_node || !cJSON_IsString(cmd_node) || !cmd_node->valuestring) {
    reply_invalid("Missing or invalid 'command' field", ds);
    cJSON_Delete(root);
    return;
  }

  char cmd_name[128] = "";
  strncpy(cmd_name, cmd_node->valuestring, sizeof(cmd_name) - 1);
  const char* simple = cmd_name;

  pthread_mutex_lock(&server->sessions_mutex);

  websocket_command_t cmd_type = lookup_command(simple);
  switch (cmd_type) {
    case WS_CMD_GET_VERSION:
      handle_cmd_get_version(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_GET_STATE:
      handle_cmd_get_state(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_GET_STOP_REASON:
      handle_cmd_get_stop_reason(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_GET_CAPTURE_RATE:
      handle_cmd_get_capture_rate(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_GET_RATE_ADJUST:
      handle_cmd_get_rate_adjust(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_GET_BUFFER_LEVEL:
      handle_cmd_get_buffer_level(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_GET_CLIPPED_SAMPLES:
      handle_cmd_get_clipped_samples(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_RESET_CLIPPED_SAMPLES:
      handle_cmd_reset_clipped_samples(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_GET_PROCESSING_LOAD:
      handle_cmd_get_processing_load(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_GET_RESAMPLER_LOAD:
      handle_cmd_get_resampler_load(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_GET_SUPPORTED_DEVICE_TYPES:
      handle_cmd_get_supported_device_types(server, client_idx, simple, root,
                                            ds);
      break;
    case WS_CMD_GET_UPDATE_INTERVAL:
      handle_cmd_get_update_interval(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_SET_UPDATE_INTERVAL:
      handle_cmd_set_update_interval(server, client_idx, simple, root, ds);
      break;

    case WS_CMD_GET_VOLUME:
      handle_cmd_get_volume(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_SET_VOLUME:
      handle_cmd_set_volume(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_GET_MUTE:
      handle_cmd_get_mute(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_SET_MUTE:
      handle_cmd_set_mute(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_TOGGLE_MUTE:
      handle_cmd_toggle_mute(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_GET_FADERS:
      handle_cmd_get_faders(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_GET_FADER_VOLUME:
      handle_cmd_get_fader_volume(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_SET_FADER_VOLUME:
      handle_cmd_set_fader_volume(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_SET_FADER_EXTERNAL_VOLUME:
      handle_cmd_set_fader_external_volume(server, client_idx, simple, root,
                                           ds);
      break;
    case WS_CMD_GET_FADER_MUTE:
      handle_cmd_get_fader_mute(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_SET_FADER_MUTE:
      handle_cmd_set_fader_mute(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_TOGGLE_FADER_MUTE:
      handle_cmd_toggle_fader_mute(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_ADJUST_VOLUME:
      handle_cmd_adjust_volume(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_ADJUST_FADER_VOLUME:
      handle_cmd_adjust_fader_volume(server, client_idx, simple, root, ds);
      break;

    case WS_CMD_GET_SPECTRUM:
      handle_cmd_get_spectrum(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_GET_AVAILABLE_CAPTURE_DEVICES:
      handle_get_available_devices(server, simple, root, true, ds);
      break;
    case WS_CMD_GET_AVAILABLE_PLAYBACK_DEVICES:
      handle_get_available_devices(server, simple, root, false, ds);
      break;
    case WS_CMD_GET_CAPTURE_DEVICE_CAPABILITIES:
      handle_get_device_capabilities(server, simple, root, true, ds);
      break;
    case WS_CMD_GET_PLAYBACK_DEVICE_CAPABILITIES:
      handle_get_device_capabilities(server, simple, root, false, ds);
      break;

    case WS_CMD_GET_CAPTURE_SIGNAL_RMS:
      handle_get_signal_single(server, simple, true, true, ds);
      break;
    case WS_CMD_GET_CAPTURE_SIGNAL_PEAK:
      handle_get_signal_single(server, simple, true, false, ds);
      break;
    case WS_CMD_GET_PLAYBACK_SIGNAL_RMS:
      handle_get_signal_single(server, simple, false, true, ds);
      break;
    case WS_CMD_GET_PLAYBACK_SIGNAL_PEAK:
      handle_get_signal_single(server, simple, false, false, ds);
      break;
    case WS_CMD_GET_CAPTURE_SIGNAL_RMS_SINCE_LAST:
      handle_get_signal_since_last(server, client_idx, simple, true, true, ds);
      break;
    case WS_CMD_GET_CAPTURE_SIGNAL_PEAK_SINCE_LAST:
      handle_get_signal_since_last(server, client_idx, simple, true, false, ds);
      break;
    case WS_CMD_GET_PLAYBACK_SIGNAL_RMS_SINCE_LAST:
      handle_get_signal_since_last(server, client_idx, simple, false, true, ds);
      break;
    case WS_CMD_GET_PLAYBACK_SIGNAL_PEAK_SINCE_LAST:
      handle_get_signal_since_last(server, client_idx, simple, false, false,
                                   ds);
      break;
    case WS_CMD_GET_CAPTURE_SIGNAL_RMS_SINCE:
      handle_get_signal_since(server, simple, root, true, true, ds);
      break;
    case WS_CMD_GET_CAPTURE_SIGNAL_PEAK_SINCE:
      handle_get_signal_since(server, simple, root, true, false, ds);
      break;
    case WS_CMD_GET_PLAYBACK_SIGNAL_RMS_SINCE:
      handle_get_signal_since(server, simple, root, false, true, ds);
      break;
    case WS_CMD_GET_PLAYBACK_SIGNAL_PEAK_SINCE:
      handle_get_signal_since(server, simple, root, false, false, ds);
      break;
    case WS_CMD_GET_SIGNAL_LEVELS:
      handle_cmd_get_signal_levels(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_GET_SIGNAL_LEVELS_SINCE_LAST:
      handle_cmd_get_signal_levels_since_last(server, client_idx, simple, root,
                                              ds);
      break;
    case WS_CMD_GET_SIGNAL_LEVELS_SINCE:
      handle_cmd_get_signal_levels_since(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_GET_SIGNAL_PEAKS_SINCE_START:
      handle_cmd_get_signal_peaks_since_start(server, client_idx, simple, root,
                                              ds);
      break;
    case WS_CMD_RESET_SIGNAL_PEAKS_SINCE_START:
      handle_cmd_reset_signal_peaks_since_start(server, client_idx, simple,
                                                root, ds);
      break;
    case WS_CMD_GET_CHANNEL_LABELS:
      handle_cmd_get_channel_labels(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_GET_SIGNAL_RANGE:
      handle_cmd_get_signal_range(server, client_idx, simple, root, ds);
      break;

    case WS_CMD_GET_CONFIG_FILE_PATH:
      handle_cmd_get_config_file_path(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_GET_PREVIOUS_CONFIG:
      handle_cmd_get_previous_config(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_GET_STATE_FILE_PATH:
      handle_cmd_get_state_file_path(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_GET_STATE_FILE_UPDATED:
      handle_cmd_get_state_file_updated(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_GET_CONFIG:
    case WS_CMD_GET_CONFIG_JSON:
      handle_cmd_get_config(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_GET_CONFIG_TITLE:
      handle_cmd_get_config_title(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_GET_CONFIG_DESCRIPTION:
      handle_cmd_get_config_description(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_RELOAD:
      handle_cmd_reload(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_STOP:
      handle_cmd_stop(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_EXIT:
      handle_cmd_exit(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_SET_CONFIG_FILE_PATH:
      handle_cmd_set_config_file_path(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_SET_CONFIG:
      handle_cmd_set_config_yaml(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_SET_CONFIG_JSON:
      handle_cmd_set_config_json(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_GET_CONFIG_VALUE:
      handle_cmd_get_config_value(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_SET_CONFIG_VALUE:
      handle_cmd_set_config_value(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_PATCH_CONFIG:
      handle_cmd_patch_config(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_READ_CONFIG:
    case WS_CMD_VALIDATE_CONFIG:
      handle_cmd_read_config_yaml(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_READ_CONFIG_JSON:
    case WS_CMD_VALIDATE_CONFIG_JSON:
      handle_cmd_read_config_json(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_READ_CONFIG_FILE:
    case WS_CMD_VALIDATE_CONFIG_FILE:
      handle_cmd_read_config_file(server, client_idx, simple, root, ds);
      break;

    case WS_CMD_SUBSCRIBE_STATE:
      handle_cmd_subscribe_state(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_SUBSCRIBE_VU_LEVELS:
      handle_cmd_subscribe_vu_levels(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_SUBSCRIBE_SIGNAL_LEVELS:
      handle_cmd_subscribe_signal_levels(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_SUBSCRIBE_SPECTRUM:
      handle_cmd_subscribe_spectrum(server, client_idx, simple, root, ds);
      break;
    case WS_CMD_STOP_SUBSCRIPTION:
      handle_cmd_stop_subscription(server, client_idx, simple, root, ds);
      break;

    default: {
      reply_invalid("Unsupported command", ds);
      break;
    }
  }
  pthread_mutex_unlock(&server->sessions_mutex);
  cJSON_Delete(root);
}
