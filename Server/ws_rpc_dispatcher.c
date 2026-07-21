#include "ws_rpc_dispatcher.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Config/cJSON.h"
#include "Logging/app_logger.h"
#include "Public/cdsp_pub_types.h"
#include "Public/config.h"
#include "Public/devices.h"
#include "Public/general.h"
#include "Public/processing.h"
#include "Public/signal_levels.h"
#include "Public/spectrum.h"
#include "Public/volume.h"
#include "websocket_server_internal.h"

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
  cJSON* inner = cJSON_CreateObject();
  cJSON_AddItemToObject(root, cmd, inner);
  cJSON_AddStringToObject(inner, "result", "Ok");
  if (value_json) {
    cJSON_AddItemToObject(inner, "value", value_json);
  }
  char* str = cJSON_PrintUnformatted(root);
  if (str) {
    dyn_string_printf(ds, "%s", str);
    free(str);
  }
  cJSON_Delete(root);
}

static void reply_error(const char* cmd, const char* error_name,
                        cJSON* error_value, dyn_string_t* ds) {
  cJSON* root = cJSON_CreateObject();
  cJSON* inner = cJSON_CreateObject();
  cJSON_AddItemToObject(root, cmd, inner);
  if (error_value) {
    cJSON_AddItemToObject(inner, "result", error_value);
  } else {
    cJSON_AddStringToObject(inner, "result", error_name);
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
  cJSON* inner = cJSON_CreateObject();
  cJSON_AddItemToObject(root, "Invalid", inner);
  cJSON_AddStringToObject(inner, "error", error_message);
  char* str = cJSON_PrintUnformatted(root);
  if (str) {
    dyn_string_printf(ds, "%s", str);
    free(str);
  }
  cJSON_Delete(root);
}

static bool parse_value_with_optional_limits(cJSON* node, double* out_delta,
                                             double* out_min, double* out_max) {
  if (!node) return false;
  if (cJSON_IsNumber(node)) {
    *out_delta = node->valuedouble;
    return true;
  }
  if (cJSON_IsArray(node) && cJSON_GetArraySize(node) >= 3) {
    cJSON* d = cJSON_GetArrayItem(node, 0);
    cJSON* mn = cJSON_GetArrayItem(node, 1);
    cJSON* mx = cJSON_GetArrayItem(node, 2);
    if (d && mn && mx && cJSON_IsNumber(d) && cJSON_IsNumber(mn) &&
        cJSON_IsNumber(mx)) {
      *out_delta = d->valuedouble;
      *out_min = mn->valuedouble;
      *out_max = mx->valuedouble;
      return true;
    }
  }
  return false;
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
                                              cdsp_fader_t fader, double delta,
                                              double min_vol, double max_vol,
                                              dyn_string_t* ds,
                                              const char* cmd_name) {
  ws_state_update_t status;
  if (!server || !server->engine ||
      !ws_engine_get_status(server->engine, &status) ||
      status.state != CDSP_PROCESSING_STATE_RUNNING) {
    reply_error(cmd_name, "ProcessingNotRunningError", NULL, ds);
    return true;
  }

  double current = cdsp_get_fader_volume(server->engine, fader);
  double new_vol = current + delta;
  if (new_vol < min_vol) new_vol = min_vol;
  if (new_vol > max_vol) new_vol = max_vol;

  cdsp_set_fader_volume(server->engine, fader, (float)new_vol, false);

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
  ws_state_update_t status;
  if (server && server->engine &&
      ws_engine_get_status(server->engine, &status) &&
      status.state == CDSP_PROCESSING_STATE_RUNNING) {
    double vol = cdsp_get_fader_volume(server->engine, CDSP_FADER_MAIN);
    reply_ok(cmd_name, cJSON_CreateNumber(vol), ds);
  } else {
    reply_error(cmd_name, "ProcessingNotRunningError", NULL, ds);
  }
}

static void handle_cmd_set_volume(websocket_server_t* server, int client_idx,
                                  const char* cmd_name, cJSON* arg,
                                  dyn_string_t* ds) {
  (void)client_idx;
  double vol = 0.0;
  if (arg && cJSON_IsNumber(arg)) {
    vol = arg->valuedouble;
    ws_state_update_t status;
    if (server && server->engine &&
        ws_engine_get_status(server->engine, &status) &&
        status.state == CDSP_PROCESSING_STATE_RUNNING) {
      cdsp_set_fader_volume(server->engine, CDSP_FADER_MAIN, (float)vol, false);
      reply_ok(cmd_name, NULL, ds);
    } else {
      reply_error(cmd_name, "ProcessingNotRunningError", NULL, ds);
    }
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "Could not parse volume");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_cmd_get_mute(websocket_server_t* server, int client_idx,
                                const char* cmd_name, cJSON* arg,
                                dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  ws_state_update_t status;
  if (server && server->engine &&
      ws_engine_get_status(server->engine, &status) &&
      status.state == CDSP_PROCESSING_STATE_RUNNING) {
    bool mute = cdsp_get_fader_mute(server->engine, CDSP_FADER_MAIN);
    reply_ok(cmd_name, cJSON_CreateBool(mute), ds);
  } else {
    reply_error(cmd_name, "ProcessingNotRunningError", NULL, ds);
  }
}

static void handle_cmd_set_mute(websocket_server_t* server, int client_idx,
                                const char* cmd_name, cJSON* arg,
                                dyn_string_t* ds) {
  (void)client_idx;
  bool mute = false;
  if (arg && cJSON_IsBool(arg)) {
    mute = cJSON_IsTrue(arg);
    ws_state_update_t status;
    if (server && server->engine &&
        ws_engine_get_status(server->engine, &status) &&
        status.state == CDSP_PROCESSING_STATE_RUNNING) {
      cdsp_set_fader_mute(server->engine, CDSP_FADER_MAIN, mute);
      reply_ok(cmd_name, NULL, ds);
    } else {
      reply_error(cmd_name, "ProcessingNotRunningError", NULL, ds);
    }
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError", "Could not parse mute");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_cmd_toggle_mute(websocket_server_t* server, int client_idx,
                                   const char* cmd_name, cJSON* arg,
                                   dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  ws_state_update_t status;
  if (server && server->engine &&
      ws_engine_get_status(server->engine, &status) &&
      status.state == CDSP_PROCESSING_STATE_RUNNING) {
    bool was_muted = cdsp_get_fader_mute(server->engine, CDSP_FADER_MAIN);
    cdsp_set_fader_mute(server->engine, CDSP_FADER_MAIN, !was_muted);
    reply_ok(cmd_name, cJSON_CreateBool(!was_muted), ds);
  } else {
    reply_error(cmd_name, "ProcessingNotRunningError", NULL, ds);
  }
}

static void handle_cmd_get_faders(websocket_server_t* server, int client_idx,
                                  const char* cmd_name, cJSON* arg,
                                  dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  ws_state_update_t status;
  if (server && server->engine &&
      ws_engine_get_status(server->engine, &status) &&
      status.state == CDSP_PROCESSING_STATE_RUNNING) {
    cJSON* arr = cJSON_CreateArray();
    for (int i = 0; i < CDSP_FADER_COUNT; i++) {
      cJSON* obj = cJSON_CreateObject();
      double vol = cdsp_get_fader_volume(server->engine, (cdsp_fader_t)i);
      bool mute = cdsp_get_fader_mute(server->engine, (cdsp_fader_t)i);
      cJSON_AddNumberToObject(obj, "volume", vol);
      cJSON_AddBoolToObject(obj, "mute", mute);
      cJSON_AddItemToArray(arr, obj);
    }
    reply_ok(cmd_name, arr, ds);
  } else {
    reply_error(cmd_name, "ProcessingNotRunningError", NULL, ds);
  }
}

static void handle_cmd_get_fader_volume(websocket_server_t* server,
                                        int client_idx, const char* cmd_name,
                                        cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  if (arg && cJSON_IsNumber(arg)) {
    int idx = arg->valueint;
    ws_state_update_t status;
    if (server && server->engine &&
        ws_engine_get_status(server->engine, &status) &&
        status.state == CDSP_PROCESSING_STATE_RUNNING) {
      if (idx >= 0 && idx < CDSP_FADER_COUNT) {
        double vol = cdsp_get_fader_volume(server->engine, (cdsp_fader_t)idx);
        cJSON* arr = cJSON_CreateArray();
        cJSON_AddItemToArray(arr, cJSON_CreateNumber(idx));
        cJSON_AddItemToArray(arr, cJSON_CreateNumber(vol));
        reply_ok(cmd_name, arr, ds);
      } else {
        reply_error(cmd_name, "InvalidFaderError", NULL, ds);
      }
    } else {
      reply_error(cmd_name, "ProcessingNotRunningError", NULL, ds);
    }
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "Could not parse fader index");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_cmd_set_fader_volume(websocket_server_t* server,
                                        int client_idx, const char* cmd_name,
                                        cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  int idx = -1;
  double vol = 0.0;
  bool ok = false;
  if (arg && cJSON_IsArray(arg) && cJSON_GetArraySize(arg) == 2) {
    cJSON* idx_node = cJSON_GetArrayItem(arg, 0);
    cJSON* vol_node = cJSON_GetArrayItem(arg, 1);
    if (idx_node && vol_node && cJSON_IsNumber(idx_node) &&
        cJSON_IsNumber(vol_node)) {
      idx = idx_node->valueint;
      vol = vol_node->valuedouble;
      ok = true;
    }
  }
  if (ok) {
    ws_state_update_t status;
    if (server && server->engine &&
        ws_engine_get_status(server->engine, &status) &&
        status.state == CDSP_PROCESSING_STATE_RUNNING) {
      if (idx >= 0 && idx < CDSP_FADER_COUNT) {
        cdsp_set_fader_volume(server->engine, (cdsp_fader_t)idx, (float)vol,
                              false);
        reply_ok(cmd_name, NULL, ds);
      } else {
        reply_error(cmd_name, "InvalidFaderError", NULL, ds);
      }
    } else {
      reply_error(cmd_name, "ProcessingNotRunningError", NULL, ds);
    }
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "Could not parse fader index/volume array");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_cmd_set_fader_external_volume(websocket_server_t* server,
                                                 int client_idx,
                                                 const char* cmd_name,
                                                 cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  int idx = -1;
  double vol = 0.0;
  bool ok = false;
  if (arg && cJSON_IsArray(arg) && cJSON_GetArraySize(arg) == 2) {
    cJSON* idx_node = cJSON_GetArrayItem(arg, 0);
    cJSON* vol_node = cJSON_GetArrayItem(arg, 1);
    if (idx_node && vol_node && cJSON_IsNumber(idx_node) &&
        cJSON_IsNumber(vol_node)) {
      idx = idx_node->valueint;
      vol = vol_node->valuedouble;
      ok = true;
    }
  }
  if (ok) {
    ws_state_update_t status;
    if (server && server->engine &&
        ws_engine_get_status(server->engine, &status) &&
        status.state == CDSP_PROCESSING_STATE_RUNNING) {
      if (idx >= 0 && idx < CDSP_FADER_COUNT) {
        cdsp_set_fader_volume(server->engine, (cdsp_fader_t)idx, (float)vol,
                              true);
        reply_ok(cmd_name, NULL, ds);
      } else {
        reply_error(cmd_name, "InvalidFaderError", NULL, ds);
      }
    } else {
      reply_error(cmd_name, "ProcessingNotRunningError", NULL, ds);
    }
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(
        err, "InvalidRequestError",
        "Could not parse fader index/external volume array");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_cmd_get_fader_mute(websocket_server_t* server,
                                      int client_idx, const char* cmd_name,
                                      cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  if (arg && cJSON_IsNumber(arg)) {
    int idx = arg->valueint;
    ws_state_update_t status;
    if (server && server->engine &&
        ws_engine_get_status(server->engine, &status) &&
        status.state == CDSP_PROCESSING_STATE_RUNNING) {
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
      reply_error(cmd_name, "ProcessingNotRunningError", NULL, ds);
    }
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "Could not parse fader index");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_cmd_set_fader_mute(websocket_server_t* server,
                                      int client_idx, const char* cmd_name,
                                      cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  int idx = -1;
  bool mute = false;
  bool ok = false;
  if (arg && cJSON_IsArray(arg) && cJSON_GetArraySize(arg) == 2) {
    cJSON* idx_node = cJSON_GetArrayItem(arg, 0);
    cJSON* mute_node = cJSON_GetArrayItem(arg, 1);
    if (idx_node && mute_node && cJSON_IsNumber(idx_node) &&
        cJSON_IsBool(mute_node)) {
      idx = idx_node->valueint;
      mute = cJSON_IsTrue(mute_node);
      ok = true;
    }
  }
  if (ok) {
    ws_state_update_t status;
    if (server && server->engine &&
        ws_engine_get_status(server->engine, &status) &&
        status.state == CDSP_PROCESSING_STATE_RUNNING) {
      if (idx >= 0 && idx < CDSP_FADER_COUNT) {
        cdsp_set_fader_mute(server->engine, (cdsp_fader_t)idx, mute);
        reply_ok(cmd_name, NULL, ds);
      } else {
        reply_error(cmd_name, "InvalidFaderError", NULL, ds);
      }
    } else {
      reply_error(cmd_name, "ProcessingNotRunningError", NULL, ds);
    }
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "Could not parse fader index/mute array");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_cmd_toggle_fader_mute(websocket_server_t* server,
                                         int client_idx, const char* cmd_name,
                                         cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  if (arg && cJSON_IsNumber(arg)) {
    int idx = arg->valueint;
    ws_state_update_t status;
    if (server && server->engine &&
        ws_engine_get_status(server->engine, &status) &&
        status.state == CDSP_PROCESSING_STATE_RUNNING) {
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
      reply_error(cmd_name, "ProcessingNotRunningError", NULL, ds);
    }
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "Could not parse fader index");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_cmd_adjust_volume(websocket_server_t* server, int client_idx,
                                     const char* cmd_name, cJSON* arg,
                                     dyn_string_t* ds) {
  (void)client_idx;
  double delta = 0.0;
  double min_vol = -150.0;
  double max_vol = 50.0;
  if (parse_value_with_optional_limits(arg, &delta, &min_vol, &max_vol)) {
    server_handle_adjust_volume_fader(server, CDSP_FADER_MAIN, delta, min_vol,
                                      max_vol, ds, cmd_name);
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "Could not parse AdjustVolume argument");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_cmd_adjust_fader_volume(websocket_server_t* server,
                                           int client_idx, const char* cmd_name,
                                           cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  int idx = -1;
  double delta = 0.0;
  double min_vol = -150.0;
  double max_vol = 50.0;
  bool ok = false;
  if (arg && cJSON_IsArray(arg) && cJSON_GetArraySize(arg) == 2) {
    cJSON* idx_node = cJSON_GetArrayItem(arg, 0);
    cJSON* val_limits = cJSON_GetArrayItem(arg, 1);
    if (idx_node && cJSON_IsNumber(idx_node)) {
      idx = idx_node->valueint;
      ok = parse_value_with_optional_limits(val_limits, &delta, &min_vol,
                                            &max_vol);
    }
  }
  if (ok) {
    if (idx >= 0 && idx < CDSP_FADER_COUNT) {
      server_handle_adjust_volume_fader(server, (cdsp_fader_t)idx, delta,
                                        min_vol, max_vol, ds, cmd_name);
    } else {
      reply_error(cmd_name, "InvalidFaderError", NULL, ds);
    }
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "Could not parse AdjustFaderVolume array");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_cmd_subscribe_state(websocket_server_t* server,
                                       int client_idx, const char* cmd_name,
                                       cJSON* arg, dyn_string_t* ds) {
  (void)arg;
  if (server) {
    pthread_mutex_lock(&server->sessions_mutex);
    server->client_sessions[client_idx].state_subscribed = true;
    pthread_mutex_unlock(&server->sessions_mutex);
  }
  reply_ok(cmd_name, NULL, ds);
}

static void handle_cmd_subscribe_vu_levels(websocket_server_t* server,
                                           int client_idx, const char* cmd_name,
                                           cJSON* arg, dyn_string_t* ds) {
  double max_rate = 0.0;
  double attack = 0.0;
  double release = 0.0;
  if (arg && cJSON_IsObject(arg)) {
    cJSON* item;
    item = cJSON_GetObjectItemCaseSensitive(arg, "max_rate");
    if (item && cJSON_IsNumber(item)) max_rate = item->valuedouble;
    item = cJSON_GetObjectItemCaseSensitive(arg, "attack");
    if (item && cJSON_IsNumber(item)) attack = item->valuedouble;
    item = cJSON_GetObjectItemCaseSensitive(arg, "release");
    if (item && cJSON_IsNumber(item)) release = item->valuedouble;
  }
  if (attack < 0.0 || attack > 60000.0 || release < 0.0 || release > 60000.0) {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(
        err, "InvalidValueError",
        "attack and release must be between 0 and 60000 ms");
    reply_error(cmd_name, NULL, err, ds);
  } else {
    if (server) {
      pthread_mutex_lock(&server->sessions_mutex);
      server->client_sessions[client_idx].vu_subscribed = true;
      server->client_sessions[client_idx].vu_max_rate = max_rate;
      server->client_sessions[client_idx].vu_attack = attack;
      server->client_sessions[client_idx].vu_release = release;
      server->client_sessions[client_idx].last_vu_push_time = 0;
      pthread_mutex_unlock(&server->sessions_mutex);
    }
    reply_ok(cmd_name, NULL, ds);
  }
}

static void handle_cmd_subscribe_signal_levels(websocket_server_t* server,
                                               int client_idx,
                                               const char* cmd_name, cJSON* arg,
                                               dyn_string_t* ds) {
  char side[16] = "";
  if (arg && cJSON_IsString(arg) && arg->valuestring) {
    strncpy(side, arg->valuestring, sizeof(side) - 1);
  }
  if (strcmp(side, "playback") == 0 || strcmp(side, "capture") == 0 ||
      strcmp(side, "both") == 0) {
    if (server) {
      pthread_mutex_lock(&server->sessions_mutex);
      server->client_sessions[client_idx].signal_levels_subscribed = true;
      snprintf(server->client_sessions[client_idx].signal_levels_side,
               sizeof(server->client_sessions[client_idx].signal_levels_side),
               "%s", side);
      pthread_mutex_unlock(&server->sessions_mutex);
    }
    reply_ok(cmd_name, NULL, ds);
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidValueError",
                            "side must be playback, capture, or both");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_cmd_subscribe_spectrum(websocket_server_t* server,
                                          int client_idx, const char* cmd_name,
                                          cJSON* arg, dyn_string_t* ds) {
  bool is_capture = true;
  uint32_t channel = (uint32_t)-1;
  double min_freq = 20.0;
  double max_freq = 20000.0;
  uint32_t n_bins = 1024;
  double max_rate = 0.0;

  if (!arg || !cJSON_IsObject(arg)) {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "Arguments must be a JSON object");
    reply_error(cmd_name, NULL, err, ds);
    return;
  }

  cJSON* item_side = cJSON_GetObjectItemCaseSensitive(arg, "side");
  if (!item_side || !cJSON_IsString(item_side)) {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "Missing or invalid 'side' parameter");
    reply_error(cmd_name, NULL, err, ds);
    return;
  }
  if (strcmp(item_side->valuestring, "capture") == 0) {
    is_capture = true;
  } else if (strcmp(item_side->valuestring, "playback") == 0) {
    is_capture = false;
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidValueError",
                            "side must be 'capture' or 'playback'");
    reply_error(cmd_name, NULL, err, ds);
    return;
  }

  cJSON* item_chan = cJSON_GetObjectItemCaseSensitive(arg, "channel");
  if (item_chan && !cJSON_IsNull(item_chan)) {
    if (cJSON_IsNumber(item_chan)) {
      if (item_chan->valueint < 0) {
        cJSON* err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "InvalidValueError",
                                "channel must be non-negative");
        reply_error(cmd_name, NULL, err, ds);
        return;
      }
      channel = (uint32_t)item_chan->valueint;
    } else {
      cJSON* err = cJSON_CreateObject();
      cJSON_AddStringToObject(err, "InvalidValueError",
                              "channel must be an integer or null");
      reply_error(cmd_name, NULL, err, ds);
      return;
    }
  }

  cJSON* item_min = cJSON_GetObjectItemCaseSensitive(arg, "min_freq");
  if (item_min && cJSON_IsNumber(item_min)) {
    min_freq = item_min->valuedouble;
  }
  if (min_freq <= 0.0) {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidValueError",
                            "min_freq must be greater than 0");
    reply_error(cmd_name, NULL, err, ds);
    return;
  }

  cJSON* item_max = cJSON_GetObjectItemCaseSensitive(arg, "max_freq");
  if (item_max && cJSON_IsNumber(item_max)) {
    max_freq = item_max->valuedouble;
  }
  if (max_freq <= min_freq) {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidValueError",
                            "max_freq must be greater than min_freq");
    reply_error(cmd_name, NULL, err, ds);
    return;
  }

  cJSON* item_bins = cJSON_GetObjectItemCaseSensitive(arg, "n_bins");
  if (item_bins && cJSON_IsNumber(item_bins)) {
    n_bins = (uint32_t)item_bins->valueint;
  }
  if (n_bins < 2) {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "n_bins must be at least 2");
    reply_error(cmd_name, NULL, err, ds);
    return;
  }

  cJSON* item_rate = cJSON_GetObjectItemCaseSensitive(arg, "max_rate");
  if (item_rate && cJSON_IsNumber(item_rate)) {
    max_rate = item_rate->valuedouble;
  }

  if (server) {
    pthread_mutex_lock(&server->sessions_mutex);
    server->client_sessions[client_idx].spectrum_subscribed = true;
    server->client_sessions[client_idx].spectrum_is_capture = is_capture;
    server->client_sessions[client_idx].spectrum_channel = channel;
    server->client_sessions[client_idx].spectrum_min_freq = min_freq;
    server->client_sessions[client_idx].spectrum_max_freq = max_freq;
    server->client_sessions[client_idx].spectrum_n_bins = n_bins;
    server->client_sessions[client_idx].spectrum_max_rate = max_rate;
    server->client_sessions[client_idx].last_spectrum_push_time = 0;
    pthread_mutex_unlock(&server->sessions_mutex);
  }
  reply_ok(cmd_name, NULL, ds);
}

static void handle_cmd_stop_subscription(websocket_server_t* server,
                                         int client_idx, const char* cmd_name,
                                         cJSON* arg, dyn_string_t* ds) {
  (void)arg;
  if (server) {
    pthread_mutex_lock(&server->sessions_mutex);
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
      pthread_mutex_unlock(&server->sessions_mutex);
      reply_ok(cmd_name, NULL, ds);
    } else {
      pthread_mutex_unlock(&server->sessions_mutex);
      cJSON* err = cJSON_CreateObject();
      cJSON_AddStringToObject(err, "InvalidRequestError",
                              "No active subscription");
      reply_error(cmd_name, NULL, err, ds);
    }
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "No active subscription");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_cmd_get_config_file_path(websocket_server_t* server,
                                            int client_idx,
                                            const char* cmd_name, cJSON* arg,
                                            dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
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
                                           cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
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
                                           cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
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
                                              const char* cmd_name, cJSON* arg,
                                              dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  bool updated = (server && server->engine)
                     ? cdsp_get_state_file_updated(server->engine)
                     : true;
  reply_ok(cmd_name, cJSON_CreateBool(updated), ds);
}

static void handle_cmd_get_config(websocket_server_t* server, int client_idx,
                                  const char* cmd_name, cJSON* arg,
                                  dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
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
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError", "No active config");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_cmd_get_config_title(websocket_server_t* server,
                                        int client_idx, const char* cmd_name,
                                        cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  char* title =
      (server && server->engine) ? cdsp_get_config_title(server->engine) : NULL;
  if (title) {
    reply_ok(cmd_name, cJSON_CreateString(title), ds);
    free(title);
  } else {
    reply_ok(cmd_name, cJSON_CreateNull(), ds);
  }
}

static void handle_cmd_get_config_description(websocket_server_t* server,
                                              int client_idx,
                                              const char* cmd_name, cJSON* arg,
                                              dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  char* desc = (server && server->engine)
                   ? cdsp_get_config_description(server->engine)
                   : NULL;
  if (desc) {
    reply_ok(cmd_name, cJSON_CreateString(desc), ds);
    free(desc);
  } else {
    reply_ok(cmd_name, cJSON_CreateNull(), ds);
  }
}

static void handle_cmd_reload(websocket_server_t* server, int client_idx,
                              const char* cmd_name, cJSON* arg,
                              dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  cdsp_backend_error_t err = {0};
  if (server && server->engine && cdsp_reload_config(server->engine, &err)) {
    reply_ok(cmd_name, NULL, ds);
  } else {
    cJSON* err_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(
        err_obj, get_websocket_error_key(err.type),
        err.message[0] ? err.message : "Failed to reload config");
    reply_error(cmd_name, NULL, err_obj, ds);
  }
}

static void handle_cmd_stop(websocket_server_t* server, int client_idx,
                            const char* cmd_name, cJSON* arg,
                            dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  if (server && server->engine) {
    cdsp_stop(server->engine);
  }
  reply_ok(cmd_name, NULL, ds);
}

static void handle_cmd_exit(websocket_server_t* server, int client_idx,
                            const char* cmd_name, cJSON* arg,
                            dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  if (server && server->engine) {
    cdsp_stop(server->engine);
  }
  reply_ok(cmd_name, NULL, ds);
}

static void handle_cmd_set_config_file_path(websocket_server_t* server,
                                            int client_idx,
                                            const char* cmd_name, cJSON* arg,
                                            dyn_string_t* ds) {
  (void)client_idx;
  if (arg && cJSON_IsString(arg) && arg->valuestring) {
    const char* path = arg->valuestring;
    if (server && server->engine) {
      cdsp_set_config_file_path(server->engine, path);
    }
    reply_ok(cmd_name, NULL, ds);
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "Could not parse Config File Path");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_cmd_set_config_json(websocket_server_t* server,
                                       int client_idx, const char* cmd_name,
                                       cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  if (arg && cJSON_IsString(arg) && arg->valuestring) {
    const char* new_json = arg->valuestring;
    cdsp_backend_error_t err = {0};
    bool ok = server && server->engine &&
              cdsp_set_config_json(server->engine, new_json, &err);
    if (ok) {
      reply_ok(cmd_name, NULL, ds);
    } else {
      cJSON* err_obj = cJSON_CreateObject();
      cJSON_AddStringToObject(err_obj, get_websocket_error_key(err.type),
                              err.message);
      reply_error(cmd_name, NULL, err_obj, ds);
    }
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "Could not parse Config JSON");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_cmd_set_config_yaml(websocket_server_t* server,
                                       int client_idx, const char* cmd_name,
                                       cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  if (arg && cJSON_IsString(arg) && arg->valuestring) {
    const char* new_yaml = arg->valuestring;
    cdsp_backend_error_t err = {0};
    bool ok = server && server->engine &&
              cdsp_set_config_yaml(server->engine, new_yaml, &err);
    if (ok) {
      reply_ok(cmd_name, NULL, ds);
    } else {
      cJSON* err_obj = cJSON_CreateObject();
      cJSON_AddStringToObject(err_obj, get_websocket_error_key(err.type),
                              err.message);
      reply_error(cmd_name, NULL, err_obj, ds);
    }
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "Could not parse Config YAML");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_cmd_get_config_value(websocket_server_t* server,
                                        int client_idx, const char* cmd_name,
                                        cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
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
      cJSON* err = cJSON_CreateObject();
      char msg[256];
      snprintf(msg, sizeof(msg), "Path not found: %s", pointer);
      cJSON_AddStringToObject(err, "InvalidRequestError", msg);
      reply_error(cmd_name, NULL, err, ds);
    }
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "Could not parse pointer");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_cmd_set_config_value(websocket_server_t* server,
                                        int client_idx, const char* cmd_name,
                                        cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  char pointer[256] = "";
  char* val_json = NULL;
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
      cJSON* err_obj = cJSON_CreateObject();
      cJSON_AddStringToObject(err_obj, get_websocket_error_key(err.type),
                              err.message[0] ? err.message : "Path not found");
      reply_error(cmd_name, NULL, err_obj, ds);
    }
    free(val_json);
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "Could not parse SetConfigValue command");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_cmd_patch_config(websocket_server_t* server, int client_idx,
                                    const char* cmd_name, cJSON* arg,
                                    dyn_string_t* ds) {
  (void)client_idx;
  if (arg && cJSON_IsObject(arg)) {
    char* patch_str = cJSON_PrintUnformatted(arg);
    if (patch_str) {
      cdsp_backend_error_t err = {0};
      bool ok = server && server->engine &&
                cdsp_patch_config(server->engine, patch_str, &err);
      if (ok) {
        reply_ok(cmd_name, NULL, ds);
      } else {
        cJSON* err_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(err_obj, get_websocket_error_key(err.type),
                                err.message[0] ? err.message : "Invalid patch");
        reply_error(cmd_name, NULL, err_obj, ds);
      }
      free(patch_str);
    } else {
      cJSON* err = cJSON_CreateObject();
      cJSON_AddStringToObject(err, "InvalidRequestError",
                              "Could not format patch JSON");
      reply_error(cmd_name, NULL, err, ds);
    }
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "Could not parse PatchConfig command");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_cmd_read_config_json(websocket_server_t* server,
                                        int client_idx, const char* cmd_name,
                                        cJSON* arg, dyn_string_t* ds) {
  (void)server;
  (void)client_idx;
  if (arg && cJSON_IsString(arg) && arg->valuestring) {
    const char* config_json = arg->valuestring;
    char* result = NULL;
    cdsp_config_error_type_t err_type = CDSP_CONFIG_ERR_NONE;
    if (cdsp_validate_config_json(config_json, &result, &err_type) &&
        err_type == CDSP_CONFIG_ERR_NONE) {
      reply_ok(cmd_name, cJSON_CreateString(result ? result : config_json), ds);
    } else {
      cJSON* err = cJSON_CreateObject();
      const char* err_key = (err_type == CDSP_CONFIG_ERR_PARSE)
                                ? "ConfigReadError"
                                : "ConfigValidationError";
      cJSON_AddStringToObject(err, err_key, result ? result : "Invalid config");
      reply_error(cmd_name, NULL, err, ds);
    }
    if (result) free(result);
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "Could not parse input config JSON");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_cmd_read_config_yaml(websocket_server_t* server,
                                        int client_idx, const char* cmd_name,
                                        cJSON* arg, dyn_string_t* ds) {
  (void)server;
  (void)client_idx;
  if (arg && cJSON_IsString(arg) && arg->valuestring) {
    const char* config_yaml = arg->valuestring;
    char* result = NULL;
    cdsp_config_error_type_t err_type = CDSP_CONFIG_ERR_NONE;
    if (cdsp_validate_config_yaml(config_yaml, &result, &err_type) &&
        err_type == CDSP_CONFIG_ERR_NONE) {
      reply_ok(cmd_name, cJSON_CreateString(result ? result : config_yaml), ds);
    } else {
      cJSON* err = cJSON_CreateObject();
      const char* err_key = (err_type == CDSP_CONFIG_ERR_PARSE)
                                ? "ConfigReadError"
                                : "ConfigValidationError";
      cJSON_AddStringToObject(err, err_key, result ? result : "Invalid config");
      reply_error(cmd_name, NULL, err, ds);
    }
    if (result) free(result);
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "Could not parse input config YAML");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_cmd_read_config_file(websocket_server_t* server,
                                        int client_idx, const char* cmd_name,
                                        cJSON* arg, dyn_string_t* ds) {
  (void)server;
  (void)client_idx;
  if (arg && cJSON_IsString(arg) && arg->valuestring) {
    const char* path = arg->valuestring;
    char* result = NULL;
    cdsp_config_error_type_t err_type = CDSP_CONFIG_ERR_NONE;
    if (cdsp_validate_config_file(path, &result, &err_type) &&
        err_type == CDSP_CONFIG_ERR_NONE) {
      reply_ok(cmd_name, cJSON_CreateString(result), ds);
    } else {
      cJSON* err = cJSON_CreateObject();
      const char* err_key = (err_type == CDSP_CONFIG_ERR_PARSE)
                                ? "ConfigReadError"
                                : "ConfigValidationError";
      cJSON_AddStringToObject(err, err_key,
                              result ? result : "Invalid config file");
      reply_error(cmd_name, NULL, err, ds);
    }
    if (result) free(result);
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "Could not parse input config file path");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_get_signal_single_helper(websocket_server_t* server,
                                            const char* cmd_name,
                                            bool is_capture, bool is_rms,
                                            dyn_string_t* ds) {
  cdsp_vu_levels_t vu = {0};
  if (server && server->engine && cdsp_get_vu_levels(server->engine, &vu)) {
    double* arr = NULL;
    size_t count = 0;
    if (is_capture) {
      arr = is_rms ? vu.capture_rms : vu.capture_peak;
      count = vu.capture_channels;
    } else {
      arr = is_rms ? vu.playback_rms : vu.playback_peak;
      count = vu.playback_channels;
    }
    reply_ok(cmd_name, cJSON_CreateDoubleArray(arr, (int)count), ds);
    cdsp_free_vu_levels(&vu);
  } else {
    reply_error(cmd_name, "ProcessingNotRunningError", NULL, ds);
  }
}

static void handle_cmd_get_capture_signal_rms(websocket_server_t* server,
                                              int client_idx,
                                              const char* cmd_name, cJSON* arg,
                                              dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  handle_get_signal_single_helper(server, cmd_name, true, true, ds);
}

static void handle_cmd_get_capture_signal_peak(websocket_server_t* server,
                                               int client_idx,
                                               const char* cmd_name, cJSON* arg,
                                               dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  handle_get_signal_single_helper(server, cmd_name, true, false, ds);
}

static void handle_cmd_get_playback_signal_rms(websocket_server_t* server,
                                               int client_idx,
                                               const char* cmd_name, cJSON* arg,
                                               dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  handle_get_signal_single_helper(server, cmd_name, false, true, ds);
}

static void handle_cmd_get_playback_signal_peak(websocket_server_t* server,
                                                int client_idx,
                                                const char* cmd_name,
                                                cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  handle_get_signal_single_helper(server, cmd_name, false, false, ds);
}

static void handle_get_signal_since_last_helper(websocket_server_t* server,
                                                int client_idx,
                                                const char* cmd_name,
                                                bool is_capture, bool is_rms,
                                                dyn_string_t* ds) {
  ws_state_update_t status;
  if (server && server->engine &&
      ws_engine_get_status(server->engine, &status) &&
      status.state == CDSP_PROCESSING_STATE_RUNNING) {
    uint64_t since = 0;
    uint64_t now = get_time_ms();
    level_history_t* hist = NULL;
    pthread_mutex_lock(&server->sessions_mutex);
    if (is_capture) {
      if (is_rms) {
        since = server->client_sessions[client_idx].last_cap_rms_time;
        server->client_sessions[client_idx].last_cap_rms_time = now;
        hist = &server->capture_rms_history;
      } else {
        since = server->client_sessions[client_idx].last_cap_peak_time;
        server->client_sessions[client_idx].last_cap_peak_time = now;
        hist = &server->capture_peak_history;
      }
    } else {
      if (is_rms) {
        since = server->client_sessions[client_idx].last_pb_rms_time;
        server->client_sessions[client_idx].last_pb_rms_time = now;
        hist = &server->playback_rms_history;
      } else {
        since = server->client_sessions[client_idx].last_pb_peak_time;
        server->client_sessions[client_idx].last_pb_peak_time = now;
        hist = &server->playback_peak_history;
      }
    }
    pthread_mutex_unlock(&server->sessions_mutex);
    size_t ch = hist->channels;
    double* vals = (double*)calloc(ch, sizeof(double));
    if (is_rms) {
      level_history_get_rms_since(hist, since, vals);
    } else {
      level_history_get_max_since(hist, since, vals);
    }
    reply_ok(cmd_name, cJSON_CreateDoubleArray(vals, (int)ch), ds);
    free(vals);
  } else {
    reply_error(cmd_name, "ProcessingNotRunningError", NULL, ds);
  }
}

static void handle_cmd_get_capture_signal_rms_since_last(
    websocket_server_t* server, int client_idx, const char* cmd_name,
    cJSON* arg, dyn_string_t* ds) {
  (void)arg;
  handle_get_signal_since_last_helper(server, client_idx, cmd_name, true, true,
                                      ds);
}

static void handle_cmd_get_capture_signal_peak_since_last(
    websocket_server_t* server, int client_idx, const char* cmd_name,
    cJSON* arg, dyn_string_t* ds) {
  (void)arg;
  handle_get_signal_since_last_helper(server, client_idx, cmd_name, true, false,
                                      ds);
}

static void handle_cmd_get_playback_signal_rms_since_last(
    websocket_server_t* server, int client_idx, const char* cmd_name,
    cJSON* arg, dyn_string_t* ds) {
  (void)arg;
  handle_get_signal_since_last_helper(server, client_idx, cmd_name, false, true,
                                      ds);
}

static void handle_cmd_get_playback_signal_peak_since_last(
    websocket_server_t* server, int client_idx, const char* cmd_name,
    cJSON* arg, dyn_string_t* ds) {
  (void)arg;
  handle_get_signal_since_last_helper(server, client_idx, cmd_name, false,
                                      false, ds);
}

static void handle_get_signal_since_helper(websocket_server_t* server,
                                           const char* cmd_name, cJSON* arg,
                                           bool is_capture, bool is_rms,
                                           dyn_string_t* ds) {
  double secs = 0;
  if (arg && cJSON_IsNumber(arg)) {
    secs = arg->valuedouble;
    ws_state_update_t status;
    if (server && server->engine &&
        ws_engine_get_status(server->engine, &status) &&
        status.state == CDSP_PROCESSING_STATE_RUNNING) {
      uint64_t now = get_time_ms();
      uint64_t since = now - (uint64_t)(secs * 1000.0);
      level_history_t* hist = NULL;
      if (is_capture) {
        hist = is_rms ? &server->capture_rms_history
                      : &server->capture_peak_history;
      } else {
        hist = is_rms ? &server->playback_rms_history
                      : &server->playback_peak_history;
      }
      size_t ch = hist->channels;
      double* vals = (double*)calloc(ch, sizeof(double));
      if (is_rms) {
        level_history_get_rms_since(hist, since, vals);
      } else {
        level_history_get_max_since(hist, since, vals);
      }
      reply_ok(cmd_name, cJSON_CreateDoubleArray(vals, (int)ch), ds);
      free(vals);
    } else {
      reply_error(cmd_name, "ProcessingNotRunningError", NULL, ds);
    }
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "Could not parse seconds");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_cmd_get_capture_signal_rms_since(websocket_server_t* server,
                                                    int client_idx,
                                                    const char* cmd_name,
                                                    cJSON* arg,
                                                    dyn_string_t* ds) {
  (void)client_idx;
  handle_get_signal_since_helper(server, cmd_name, arg, true, true, ds);
}

static void handle_cmd_get_capture_signal_peak_since(websocket_server_t* server,
                                                     int client_idx,
                                                     const char* cmd_name,
                                                     cJSON* arg,
                                                     dyn_string_t* ds) {
  (void)client_idx;
  handle_get_signal_since_helper(server, cmd_name, arg, true, false, ds);
}

static void handle_cmd_get_playback_signal_rms_since(websocket_server_t* server,
                                                     int client_idx,
                                                     const char* cmd_name,
                                                     cJSON* arg,
                                                     dyn_string_t* ds) {
  (void)client_idx;
  handle_get_signal_since_helper(server, cmd_name, arg, false, true, ds);
}

static void handle_cmd_get_playback_signal_peak_since(
    websocket_server_t* server, int client_idx, const char* cmd_name,
    cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  handle_get_signal_since_helper(server, cmd_name, arg, false, false, ds);
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
        cJSON_CreateDoubleArray(vu.playback_rms, (int)vu.playback_channels));
    cJSON_AddItemToObject(
        root, "playback_peak",
        cJSON_CreateDoubleArray(vu.playback_peak, (int)vu.playback_channels));
    cJSON_AddItemToObject(
        root, "capture_rms",
        cJSON_CreateDoubleArray(vu.capture_rms, (int)vu.capture_channels));
    cJSON_AddItemToObject(
        root, "capture_peak",
        cJSON_CreateDoubleArray(vu.capture_peak, (int)vu.capture_channels));
    reply_ok(cmd_name, root, ds);
    cdsp_free_vu_levels(&vu);
  } else {
    reply_error(cmd_name, "ProcessingNotRunningError", NULL, ds);
  }
}

static void handle_cmd_get_signal_levels_since_last(websocket_server_t* server,
                                                    int client_idx,
                                                    const char* cmd_name,
                                                    cJSON* arg,
                                                    dyn_string_t* ds) {
  (void)arg;
  ws_state_update_t status;
  if (server && server->engine &&
      ws_engine_get_status(server->engine, &status) &&
      status.state == CDSP_PROCESSING_STATE_RUNNING) {
    pthread_mutex_lock(&server->sessions_mutex);
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
    pthread_mutex_unlock(&server->sessions_mutex);

    size_t c_ch = server->capture_rms_history.channels;
    size_t p_ch = server->playback_rms_history.channels;
    double* c_rms = (double*)calloc(c_ch, sizeof(double));
    double* c_pk = (double*)calloc(c_ch, sizeof(double));
    double* p_rms = (double*)calloc(p_ch, sizeof(double));
    double* p_pk = (double*)calloc(p_ch, sizeof(double));

    level_history_get_rms_since(&server->capture_rms_history, cap_rms_since,
                                c_rms);
    level_history_get_max_since(&server->capture_peak_history, cap_pk_since,
                                c_pk);
    level_history_get_rms_since(&server->playback_rms_history, pb_rms_since,
                                p_rms);
    level_history_get_max_since(&server->playback_peak_history, pb_pk_since,
                                p_pk);

    cJSON* root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "playback_rms",
                          cJSON_CreateDoubleArray(p_rms, (int)p_ch));
    cJSON_AddItemToObject(root, "playback_peak",
                          cJSON_CreateDoubleArray(p_pk, (int)p_ch));
    cJSON_AddItemToObject(root, "capture_rms",
                          cJSON_CreateDoubleArray(c_rms, (int)c_ch));
    cJSON_AddItemToObject(root, "capture_peak",
                          cJSON_CreateDoubleArray(c_pk, (int)c_ch));

    reply_ok(cmd_name, root, ds);

    free(c_rms);
    free(c_pk);
    free(p_rms);
    free(p_pk);
  } else {
    reply_error(cmd_name, "ProcessingNotRunningError", NULL, ds);
  }
}

static void handle_cmd_get_signal_levels_since(websocket_server_t* server,
                                               int client_idx,
                                               const char* cmd_name, cJSON* arg,
                                               dyn_string_t* ds) {
  (void)client_idx;
  double secs = 0;
  if (arg && cJSON_IsNumber(arg)) {
    secs = arg->valuedouble;
    ws_state_update_t status;
    if (server && server->engine &&
        ws_engine_get_status(server->engine, &status) &&
        status.state == CDSP_PROCESSING_STATE_RUNNING) {
      uint64_t now = get_time_ms();
      uint64_t since = now - (uint64_t)(secs * 1000.0);

      size_t c_ch = server->capture_rms_history.channels;
      size_t p_ch = server->playback_rms_history.channels;
      double* c_rms = (double*)calloc(c_ch, sizeof(double));
      double* c_pk = (double*)calloc(c_ch, sizeof(double));
      double* p_rms = (double*)calloc(p_ch, sizeof(double));
      double* p_pk = (double*)calloc(p_ch, sizeof(double));

      level_history_get_rms_since(&server->capture_rms_history, since, c_rms);
      level_history_get_max_since(&server->capture_peak_history, since, c_pk);
      level_history_get_rms_since(&server->playback_rms_history, since, p_rms);
      level_history_get_max_since(&server->playback_peak_history, since, p_pk);

      cJSON* root = cJSON_CreateObject();
      cJSON_AddItemToObject(root, "playback_rms",
                            cJSON_CreateDoubleArray(p_rms, (int)p_ch));
      cJSON_AddItemToObject(root, "playback_peak",
                            cJSON_CreateDoubleArray(p_pk, (int)p_ch));
      cJSON_AddItemToObject(root, "capture_rms",
                            cJSON_CreateDoubleArray(c_rms, (int)c_ch));
      cJSON_AddItemToObject(root, "capture_peak",
                            cJSON_CreateDoubleArray(c_pk, (int)c_ch));

      reply_ok(cmd_name, root, ds);

      free(c_rms);
      free(c_pk);
      free(p_rms);
      free(p_pk);
    } else {
      reply_error(cmd_name, "ProcessingNotRunningError", NULL, ds);
    }
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "Could not parse seconds");
    reply_error(cmd_name, NULL, err, ds);
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
    double max_peak = -1000.0;
    for (size_t i = 0; i < count; i++) {
      double pk = vu.playback_peak[i];
      if (pk > max_peak) max_peak = pk;
    }
    double range = 2.0 * db_to_amplitude(max_peak);
    reply_ok(cmd_name, cJSON_CreateNumber(range), ds);
    cdsp_free_vu_levels(&vu);
  } else {
    reply_error(cmd_name, "ProcessingNotRunningError", NULL, ds);
  }
}

static void handle_cmd_get_spectrum(websocket_server_t* server, int client_idx,
                                    const char* cmd_name, cJSON* arg,
                                    dyn_string_t* ds) {
  (void)client_idx;
  bool is_capture = true;
  uint32_t channel = (uint32_t)-1;
  double min_freq = 20.0;
  double max_freq = 20000.0;
  uint32_t n_bins = 1024;

  if (!arg || !cJSON_IsObject(arg)) {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "Arguments must be a JSON object");
    reply_error(cmd_name, NULL, err, ds);
    return;
  }

  cJSON* item_side = cJSON_GetObjectItemCaseSensitive(arg, "side");
  if (!item_side || !cJSON_IsString(item_side)) {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "Missing or invalid 'side' parameter");
    reply_error(cmd_name, NULL, err, ds);
    return;
  }
  if (strcmp(item_side->valuestring, "capture") == 0) {
    is_capture = true;
  } else if (strcmp(item_side->valuestring, "playback") == 0) {
    is_capture = false;
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidValueError",
                            "side must be 'capture' or 'playback'");
    reply_error(cmd_name, NULL, err, ds);
    return;
  }

  cJSON* item_chan = cJSON_GetObjectItemCaseSensitive(arg, "channel");
  if (item_chan && !cJSON_IsNull(item_chan)) {
    if (cJSON_IsNumber(item_chan)) {
      if (item_chan->valueint < 0) {
        cJSON* err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "InvalidValueError",
                                "channel must be non-negative");
        reply_error(cmd_name, NULL, err, ds);
        return;
      }
      channel = (uint32_t)item_chan->valueint;
    } else {
      cJSON* err = cJSON_CreateObject();
      cJSON_AddStringToObject(err, "InvalidValueError",
                              "channel must be an integer or null");
      reply_error(cmd_name, NULL, err, ds);
      return;
    }
  }

  cJSON* item_min = cJSON_GetObjectItemCaseSensitive(arg, "min_freq");
  if (item_min && cJSON_IsNumber(item_min)) {
    min_freq = item_min->valuedouble;
  }
  if (min_freq <= 0.0) {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidValueError",
                            "min_freq must be greater than 0");
    reply_error(cmd_name, NULL, err, ds);
    return;
  }

  cJSON* item_max = cJSON_GetObjectItemCaseSensitive(arg, "max_freq");
  if (item_max && cJSON_IsNumber(item_max)) {
    max_freq = item_max->valuedouble;
  }
  if (max_freq <= min_freq) {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidValueError",
                            "max_freq must be greater than min_freq");
    reply_error(cmd_name, NULL, err, ds);
    return;
  }

  cJSON* item_bins = cJSON_GetObjectItemCaseSensitive(arg, "n_bins");
  if (item_bins && cJSON_IsNumber(item_bins)) {
    n_bins = (uint32_t)item_bins->valueint;
  }
  if (n_bins < 2) {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "n_bins must be at least 2");
    reply_error(cmd_name, NULL, err, ds);
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
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "DeviceError", "Failed to compute spectrum");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_get_available_devices_helper(websocket_server_t* server,
                                                const char* cmd_name,
                                                cJSON* arg, bool is_capture,
                                                dyn_string_t* ds) {
  if (arg && cJSON_IsString(arg) && arg->valuestring) {
    const char* backend = arg->valuestring;
    cdsp_device_info_t* devs = NULL;
    size_t count = 0;
    bool ok = server && server->engine &&
              cdsp_get_available_devices(backend, is_capture, &devs, &count);
    if (ok && devs) {
      cJSON* arr = cJSON_CreateArray();
      for (size_t i = 0; i < count; i++) {
        cJSON* tuple = cJSON_CreateArray();
        cJSON_AddItemToArray(tuple, cJSON_CreateString(devs[i].name));
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
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "Could not parse backend");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_cmd_get_available_capture_devices(websocket_server_t* server,
                                                     int client_idx,
                                                     const char* cmd_name,
                                                     cJSON* arg,
                                                     dyn_string_t* ds) {
  (void)client_idx;
  handle_get_available_devices_helper(server, cmd_name, arg, true, ds);
}

static void handle_cmd_get_available_playback_devices(
    websocket_server_t* server, int client_idx, const char* cmd_name,
    cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  handle_get_available_devices_helper(server, cmd_name, arg, false, ds);
}

static void handle_get_device_capabilities_helper(websocket_server_t* server,
                                                  const char* cmd_name,
                                                  cJSON* arg, bool is_capture,
                                                  dyn_string_t* ds) {
  (void)server;
  char backend[128] = "";
  char device[256] = "";
  bool ok = false;
  if (arg && cJSON_IsArray(arg) && cJSON_GetArraySize(arg) >= 2) {
    cJSON* b_node = cJSON_GetArrayItem(arg, 0);
    cJSON* d_node = cJSON_GetArrayItem(arg, 1);
    if (b_node && d_node && cJSON_IsString(b_node) && cJSON_IsString(d_node)) {
      strncpy(backend, b_node->valuestring, sizeof(backend) - 1);
      strncpy(device, d_node->valuestring, sizeof(device) - 1);
      ok = true;
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
        cJSON* err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "DeviceError", "Out of memory");
        reply_error(cmd_name, NULL, err, ds);
      }
      cdsp_free_device_capabilities(desc);
    } else {
      cJSON* err = cJSON_CreateObject();
      cJSON_AddStringToObject(err, get_websocket_device_error_key(d_err.type),
                              d_err.message);
      reply_error(cmd_name, NULL, err, ds);
    }
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "Could not parse backend/device array");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_cmd_get_capture_device_capabilities(
    websocket_server_t* server, int client_idx, const char* cmd_name,
    cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  handle_get_device_capabilities_helper(server, cmd_name, arg, true, ds);
}

static void handle_cmd_get_playback_device_capabilities(
    websocket_server_t* server, int client_idx, const char* cmd_name,
    cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  handle_get_device_capabilities_helper(server, cmd_name, arg, false, ds);
}

static void handle_cmd_get_version(websocket_server_t* server, int client_idx,
                                   const char* cmd_name, cJSON* arg,
                                   dyn_string_t* ds) {
  (void)server;
  (void)client_idx;
  (void)arg;
  reply_ok(cmd_name, cJSON_CreateString("2.0.0"), ds);
}

static void handle_cmd_get_state(websocket_server_t* server, int client_idx,
                                 const char* cmd_name, cJSON* arg,
                                 dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
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
                                       cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
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
                                        cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
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
                                       cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  double rate = 1.0;
  if (server && server->engine) {
    cdsp_get_processing_status(server->engine, &rate, NULL, NULL, NULL, NULL);
  }
  reply_ok(cmd_name, cJSON_CreateNumber(rate), ds);
}

static void handle_cmd_get_buffer_level(websocket_server_t* server,
                                        int client_idx, const char* cmd_name,
                                        cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  double lvl = 0.0;
  if (server && server->engine) {
    cdsp_get_processing_status(server->engine, NULL, &lvl, NULL, NULL, NULL);
  }
  reply_ok(cmd_name, cJSON_CreateNumber((int)lvl), ds);
}

static void handle_cmd_get_clipped_samples(websocket_server_t* server,
                                           int client_idx, const char* cmd_name,
                                           cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  uint64_t clips = 0;
  if (server && server->engine) {
    cdsp_get_processing_status(server->engine, NULL, NULL, &clips, NULL, NULL);
  }
  reply_ok(cmd_name, cJSON_CreateNumber((double)clips), ds);
}

static void handle_cmd_reset_clipped_samples(websocket_server_t* server,
                                             int client_idx,
                                             const char* cmd_name, cJSON* arg,
                                             dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  if (server && server->engine) {
    cdsp_reset_clipped_samples(server->engine);
  }
  reply_ok(cmd_name, NULL, ds);
}

static void handle_cmd_get_processing_load(websocket_server_t* server,
                                           int client_idx, const char* cmd_name,
                                           cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  double load = 0.0;
  if (server && server->engine) {
    cdsp_get_processing_status(server->engine, NULL, NULL, NULL, &load, NULL);
  }
  reply_ok(cmd_name, cJSON_CreateNumber(load), ds);
}

static void handle_cmd_get_resampler_load(websocket_server_t* server,
                                          int client_idx, const char* cmd_name,
                                          cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  double load = 0.0;
  if (server && server->engine) {
    cdsp_get_processing_status(server->engine, NULL, NULL, NULL, NULL, &load);
  }
  reply_ok(cmd_name, cJSON_CreateNumber(load), ds);
}

static void handle_cmd_get_supported_device_types(websocket_server_t* server,
                                                  int client_idx,
                                                  const char* cmd_name,
                                                  cJSON* arg,
                                                  dyn_string_t* ds) {
  (void)server;
  (void)client_idx;
  (void)arg;
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
                                           cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  int interval = server ? (int)server->update_interval : 100;
  reply_ok(cmd_name, cJSON_CreateNumber(interval), ds);
}

static void handle_cmd_set_update_interval(websocket_server_t* server,
                                           int client_idx, const char* cmd_name,
                                           cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  if (arg && cJSON_IsNumber(arg)) {
    double val = arg->valuedouble;
    if (val >= 0.0) {
      if (server) server->update_interval = (uint32_t)val;
      reply_ok(cmd_name, NULL, ds);
    } else {
      cJSON* err = cJSON_CreateObject();
      cJSON_AddStringToObject(err, "InvalidValueError", "Value must be >= 0");
      reply_error(cmd_name, NULL, err, ds);
    }
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "Could not parse SetUpdateInterval argument");
    reply_error(cmd_name, NULL, err, ds);
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

  pthread_mutex_lock(&server->sessions_mutex);

  char cmd_name[128] = "";
  cJSON* arg = NULL;

  if (cJSON_IsString(root)) {
    strncpy(cmd_name, root->valuestring, sizeof(cmd_name) - 1);
  } else if (cJSON_IsObject(root)) {
    cJSON* child = root->child;
    if (child && child->string) {
      strncpy(cmd_name, child->string, sizeof(cmd_name) - 1);
      arg = child;
    }
  }

  const char* simple = cmd_name;

  websocket_command_t cmd_type = lookup_command(simple);
  switch (cmd_type) {
    case WS_CMD_GET_VERSION:
      handle_cmd_get_version(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_STATE:
      handle_cmd_get_state(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_STOP_REASON:
      handle_cmd_get_stop_reason(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_CAPTURE_RATE:
      handle_cmd_get_capture_rate(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_RATE_ADJUST:
      handle_cmd_get_rate_adjust(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_BUFFER_LEVEL:
      handle_cmd_get_buffer_level(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_CLIPPED_SAMPLES:
      handle_cmd_get_clipped_samples(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_RESET_CLIPPED_SAMPLES:
      handle_cmd_reset_clipped_samples(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_PROCESSING_LOAD:
      handle_cmd_get_processing_load(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_RESAMPLER_LOAD:
      handle_cmd_get_resampler_load(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_SUPPORTED_DEVICE_TYPES:
      handle_cmd_get_supported_device_types(server, client_idx, simple, arg,
                                            ds);
      break;
    case WS_CMD_GET_UPDATE_INTERVAL:
      handle_cmd_get_update_interval(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_SET_UPDATE_INTERVAL:
      handle_cmd_set_update_interval(server, client_idx, simple, arg, ds);
      break;

    case WS_CMD_GET_VOLUME:
      handle_cmd_get_volume(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_SET_VOLUME:
      handle_cmd_set_volume(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_MUTE:
      handle_cmd_get_mute(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_SET_MUTE:
      handle_cmd_set_mute(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_TOGGLE_MUTE:
      handle_cmd_toggle_mute(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_FADERS:
      handle_cmd_get_faders(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_FADER_VOLUME:
      handle_cmd_get_fader_volume(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_SET_FADER_VOLUME:
      handle_cmd_set_fader_volume(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_SET_FADER_EXTERNAL_VOLUME:
      handle_cmd_set_fader_external_volume(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_FADER_MUTE:
      handle_cmd_get_fader_mute(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_SET_FADER_MUTE:
      handle_cmd_set_fader_mute(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_TOGGLE_FADER_MUTE:
      handle_cmd_toggle_fader_mute(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_ADJUST_VOLUME:
      handle_cmd_adjust_volume(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_ADJUST_FADER_VOLUME:
      handle_cmd_adjust_fader_volume(server, client_idx, simple, arg, ds);
      break;

    case WS_CMD_GET_SPECTRUM:
      handle_cmd_get_spectrum(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_AVAILABLE_CAPTURE_DEVICES:
      handle_cmd_get_available_capture_devices(server, client_idx, simple, arg,
                                               ds);
      break;
    case WS_CMD_GET_AVAILABLE_PLAYBACK_DEVICES:
      handle_cmd_get_available_playback_devices(server, client_idx, simple, arg,
                                                ds);
      break;
    case WS_CMD_GET_CAPTURE_DEVICE_CAPABILITIES:
      handle_cmd_get_capture_device_capabilities(server, client_idx, simple,
                                                 arg, ds);
      break;
    case WS_CMD_GET_PLAYBACK_DEVICE_CAPABILITIES:
      handle_cmd_get_playback_device_capabilities(server, client_idx, simple,
                                                  arg, ds);
      break;

    case WS_CMD_GET_CAPTURE_SIGNAL_RMS:
      handle_cmd_get_capture_signal_rms(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_CAPTURE_SIGNAL_PEAK:
      handle_cmd_get_capture_signal_peak(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_PLAYBACK_SIGNAL_RMS:
      handle_cmd_get_playback_signal_rms(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_PLAYBACK_SIGNAL_PEAK:
      handle_cmd_get_playback_signal_peak(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_CAPTURE_SIGNAL_RMS_SINCE_LAST:
      handle_cmd_get_capture_signal_rms_since_last(server, client_idx, simple,
                                                   arg, ds);
      break;
    case WS_CMD_GET_CAPTURE_SIGNAL_PEAK_SINCE_LAST:
      handle_cmd_get_capture_signal_peak_since_last(server, client_idx, simple,
                                                    arg, ds);
      break;
    case WS_CMD_GET_PLAYBACK_SIGNAL_RMS_SINCE_LAST:
      handle_cmd_get_playback_signal_rms_since_last(server, client_idx, simple,
                                                    arg, ds);
      break;
    case WS_CMD_GET_PLAYBACK_SIGNAL_PEAK_SINCE_LAST:
      handle_cmd_get_playback_signal_peak_since_last(server, client_idx, simple,
                                                     arg, ds);
      break;
    case WS_CMD_GET_CAPTURE_SIGNAL_RMS_SINCE:
      handle_cmd_get_capture_signal_rms_since(server, client_idx, simple, arg,
                                              ds);
      break;
    case WS_CMD_GET_CAPTURE_SIGNAL_PEAK_SINCE:
      handle_cmd_get_capture_signal_peak_since(server, client_idx, simple, arg,
                                               ds);
      break;
    case WS_CMD_GET_PLAYBACK_SIGNAL_RMS_SINCE:
      handle_cmd_get_playback_signal_rms_since(server, client_idx, simple, arg,
                                               ds);
      break;
    case WS_CMD_GET_PLAYBACK_SIGNAL_PEAK_SINCE:
      handle_cmd_get_playback_signal_peak_since(server, client_idx, simple, arg,
                                                ds);
      break;
    case WS_CMD_GET_SIGNAL_LEVELS:
      handle_cmd_get_signal_levels(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_SIGNAL_LEVELS_SINCE_LAST:
      handle_cmd_get_signal_levels_since_last(server, client_idx, simple, arg,
                                              ds);
      break;
    case WS_CMD_GET_SIGNAL_LEVELS_SINCE:
      handle_cmd_get_signal_levels_since(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_SIGNAL_PEAKS_SINCE_START:
      handle_cmd_get_signal_peaks_since_start(server, client_idx, simple, arg,
                                              ds);
      break;
    case WS_CMD_RESET_SIGNAL_PEAKS_SINCE_START:
      handle_cmd_reset_signal_peaks_since_start(server, client_idx, simple, arg,
                                                ds);
      break;
    case WS_CMD_GET_CHANNEL_LABELS:
      handle_cmd_get_channel_labels(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_SIGNAL_RANGE:
      handle_cmd_get_signal_range(server, client_idx, simple, arg, ds);
      break;

    case WS_CMD_GET_CONFIG_FILE_PATH:
      handle_cmd_get_config_file_path(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_PREVIOUS_CONFIG:
      handle_cmd_get_previous_config(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_STATE_FILE_PATH:
      handle_cmd_get_state_file_path(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_STATE_FILE_UPDATED:
      handle_cmd_get_state_file_updated(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_CONFIG:
    case WS_CMD_GET_CONFIG_JSON:
      handle_cmd_get_config(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_CONFIG_TITLE:
      handle_cmd_get_config_title(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_CONFIG_DESCRIPTION:
      handle_cmd_get_config_description(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_RELOAD:
      handle_cmd_reload(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_STOP:
      handle_cmd_stop(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_EXIT:
      handle_cmd_exit(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_SET_CONFIG_FILE_PATH:
      handle_cmd_set_config_file_path(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_SET_CONFIG:
      handle_cmd_set_config_yaml(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_SET_CONFIG_JSON:
      handle_cmd_set_config_json(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_CONFIG_VALUE:
      handle_cmd_get_config_value(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_SET_CONFIG_VALUE:
      handle_cmd_set_config_value(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_PATCH_CONFIG:
      handle_cmd_patch_config(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_READ_CONFIG:
    case WS_CMD_VALIDATE_CONFIG:
      handle_cmd_read_config_yaml(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_READ_CONFIG_JSON:
    case WS_CMD_VALIDATE_CONFIG_JSON:
      handle_cmd_read_config_json(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_READ_CONFIG_FILE:
    case WS_CMD_VALIDATE_CONFIG_FILE:
      handle_cmd_read_config_file(server, client_idx, simple, arg, ds);
      break;

    case WS_CMD_SUBSCRIBE_STATE:
      handle_cmd_subscribe_state(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_SUBSCRIBE_VU_LEVELS:
      handle_cmd_subscribe_vu_levels(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_SUBSCRIBE_SIGNAL_LEVELS:
      handle_cmd_subscribe_signal_levels(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_SUBSCRIBE_SPECTRUM:
      handle_cmd_subscribe_spectrum(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_STOP_SUBSCRIPTION:
      handle_cmd_stop_subscription(server, client_idx, simple, arg, ds);
      break;

    default: {
      reply_invalid("Unsupported command", ds);
      break;
    }
  }
  pthread_mutex_unlock(&server->sessions_mutex);
  cJSON_Delete(root);
}
