// WebSocket control server using libwebsockets
// Provides runtime control API compatible with the CamillaDSP monitor control
// protocol

#include "server/websocket_server.h"

#include <cjson/cJSON.h>
#include <ctype.h>
#include <libwebsockets.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cdsp/cdsp_pub_types.h"
#include "cdsp/config.h"
#include "cdsp/devices.h"
#include "cdsp/fader.h"
#include "cdsp/general.h"
#include "cdsp/processing.h"
#include "cdsp/signal_levels.h"
#include "cdsp/spectrum.h"
#include "logging/app_logger.h"
#include "utils/cdsp_macros.h"
#include "utils/cdsp_time.h"

static const logger_t server_logger = {"dsp.server.websocket"};

// MARK: - Internal Types & Structs

typedef struct {
  cdsp_processing_state_t state;
  cdsp_stop_reason_t stop_reason;
} ws_state_update_t;

static inline const char *
ws_processing_state_to_string(cdsp_processing_state_t state) {
  switch (state) {
  case CDSP_PROCESSING_STATE_INACTIVE:
    return "Inactive";
  case CDSP_PROCESSING_STATE_STARTING:
    return "Starting";
  case CDSP_PROCESSING_STATE_RUNNING:
    return "Running";
  case CDSP_PROCESSING_STATE_PAUSED:
    return "Paused";
  case CDSP_PROCESSING_STATE_STALLED:
    return "Stalled";
  }
  CDSP_UNREACHABLE();
  return "Inactive";
}

typedef struct ws_msg_node_s {
  char *data;
  size_t len;
  struct ws_msg_node_s *next;
} ws_msg_node_t;

typedef struct {
  uint64_t last_cap_peak_time;
  uint64_t last_cap_rms_time;
  uint64_t last_pb_peak_time;
  uint64_t last_pb_rms_time;

  bool in_use;
  bool is_websocket;
  bool state_subscribed;
  char last_state[64];
  bool vu_subscribed;
  bool signal_levels_subscribed;
  char signal_levels_side[16];
  bool spectrum_subscribed;
  bool spectrum_is_capture;
  size_t spectrum_channel;
  float spectrum_min_freq;
  float spectrum_max_freq;
  uint32_t spectrum_n_bins;
  float spectrum_max_rate;
  uint64_t last_spectrum_push_time;

  float vu_max_rate;
  float vu_attack;
  float vu_release;
  uint64_t last_vu_push_time;
  uint64_t last_vu_update_time;

  float *vu_pb_rms;
  float *vu_pb_peak;
  float *vu_cap_rms;
  float *vu_cap_peak;
  size_t vu_pb_channels;
  size_t vu_cap_channels;

  uint64_t last_pb_generation;
  uint64_t last_cap_generation;
  bool vu_pending_publish;
  uint64_t last_sig_pb_generation;
  uint64_t last_sig_cap_generation;

  struct lws *wsi;
  void *pss;

  char *rx_buf;
  size_t rx_len;
  size_t rx_cap;

  ws_msg_node_t *out_queue_head;
  ws_msg_node_t *out_queue_tail;
} client_session_t;

struct websocket_server {
  uint16_t port;
  char host[128];
  dsp_engine_t *engine;

  struct lws_context *context;
  lws_sorted_usec_list_t sul;
  _Atomic bool running;
  _Atomic bool exit_requested;
  pthread_t thread;

  pthread_mutex_t start_mutex;
  pthread_cond_t start_cond;
  _Atomic int start_status;

  uint32_t update_interval;

  float *capture_global_peaks;
  float *playback_global_peaks;
  size_t capture_global_peaks_count;
  size_t playback_global_peaks_count;

  pthread_mutex_t sessions_mutex;
  client_session_t client_sessions[32];
};

typedef struct {
  struct lws *wsi;
  websocket_server_t *server;
  int client_idx;
} lws_pss_t;

// MARK: - Dynamic String Helpers

void dyn_string_init(dyn_string_t *ds, size_t initial_cap) {
  ds->data = (char *)calloc(initial_cap, sizeof(char));
  ds->capacity = ds->data ? initial_cap : 0;
  ds->length = 0;
}

void dyn_string_free(dyn_string_t *ds) {
  if (ds->data) {
    free(ds->data);
    ds->data = NULL;
  }
  ds->capacity = 0;
  ds->length = 0;
}

static void dyn_string_printf(dyn_string_t *ds, const char *fmt, ...) {
  if (!ds->data || ds->capacity == 0)
    return;
  va_list args;
  va_start(args, fmt);
  va_list args_copy;
  va_copy(args_copy, args);
  int needed = vsnprintf(ds->data, ds->capacity, fmt, args_copy);
  va_end(args_copy);

  if (needed < 0) {
    va_end(args);
    return;
  }
  if ((size_t)needed >= ds->capacity) {
    size_t new_cap = ds->capacity * 2;
    if (new_cap <= (size_t)needed)
      new_cap = (size_t)needed + 1;
    char *new_data = (char *)realloc(ds->data, new_cap);
    if (!new_data) {
      va_end(args);
      return;
    }
    ds->data = new_data;
    ds->capacity = new_cap;
    vsnprintf(ds->data, ds->capacity, fmt, args);
  }
  ds->length = (size_t)needed;
  va_end(args);
}

// MARK: - Numeric & JSON Serialization Helpers

static inline uint64_t get_time_ms(void) {
  return cdsp_time_now_ns() / 1000000ULL;
}

static inline float db_to_amplitude(float db) {
  if (!isfinite(db) || db <= -200.0f)
    return 0.0f;
  return powf(10.0f, db / 20.0f);
}

static inline float amplitude_to_db(float amp) {
  if (amp <= 0.0f || !isfinite(amp))
    return -INFINITY;
  return 20.0f * log10f(amp);
}

static inline float smoothing_alpha(float delta_ms, float time_constant_ms) {
  if (time_constant_ms <= 0.0f)
    return 1.0f;
  return 1.0f - expf(-(delta_ms / 1000.0f) / (time_constant_ms / 1000.0f));
}

static inline cJSON *safe_create_float_array(const float *numbers, int count) {
  if (count <= 0 || !numbers)
    return cJSON_CreateArray();
  cJSON *array = cJSON_CreateArray();
  if (!array)
    return NULL;
  for (int i = 0; i < count; i++) {
    float val = numbers[i];
    if (isnan(val))
      val = -200.0f;
    else if (isinf(val))
      val = (val < 0.0f) ? -200.0f : 0.0f;
    cJSON_AddItemToArray(array, cJSON_CreateNumber((double)val));
  }
  return array;
}

static inline cJSON *safe_create_float_number(double val) {
  if (isnan(val))
    val = -200.0;
  else if (isinf(val))
    val = (val < 0.0) ? -200.0 : 0.0;
  return cJSON_CreateNumber(val);
}

static void reply_json_root(cJSON *root, dyn_string_t *ds) {
  if (!root)
    return;
  char *str = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (str) {
    dyn_string_printf(ds, "%s", str);
    free(str);
  }
}

static void reply_ok(const char *cmd, cJSON *value_json, dyn_string_t *ds) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "reply", cmd);
  cJSON_AddStringToObject(root, "result", "Ok");
  if (value_json)
    cJSON_AddItemToObject(root, "value", value_json);
  reply_json_root(root, ds);
}

static void reply_error(const char *cmd, const char *error_name,
                        const char *message, dyn_string_t *ds) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "reply", cmd);
  cJSON_AddStringToObject(root, "result",
                          error_name ? error_name : "ProcessingError");
  if (message && message[0] != '\0')
    cJSON_AddStringToObject(root, "message", message);
  reply_json_root(root, ds);
}

static void reply_error_with_value(const char *cmd, const char *error_name,
                                   const char *message, cJSON *val,
                                   dyn_string_t *ds) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "reply", cmd);
  cJSON_AddStringToObject(root, "result",
                          error_name ? error_name : "ProcessingError");
  if (message && message[0] != '\0')
    cJSON_AddStringToObject(root, "message", message);
  if (val)
    cJSON_AddItemToObject(root, "value", val);
  reply_json_root(root, ds);
}

static void reply_invalid(const char *error_message, dyn_string_t *ds) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "reply", "Invalid");
  cJSON_AddStringToObject(root, "error",
                          error_message ? error_message : "Invalid JSON");
  reply_json_root(root, ds);
}

static cJSON *serialize_stop_reason(const cdsp_stop_reason_t *reason) {
  if (!reason)
    return cJSON_CreateString("None");
  cJSON *root = NULL;
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
  }
  CDSP_UNREACHABLE();
  return cJSON_CreateString("None");
}

static cJSON *create_state_event_value(cdsp_processing_state_t state,
                                       const cdsp_stop_reason_t *reason) {
  cJSON *val = cJSON_CreateObject();
  cJSON_AddStringToObject(val, "state", ws_processing_state_to_string(state));
  if (state == CDSP_PROCESSING_STATE_INACTIVE) {
    cJSON_AddItemToObject(val, "stop_reason", serialize_stop_reason(reason));
  }
  return val;
}

static cJSON *serialize_spectrum(const cdsp_spectrum_t *spec) {
  if (!spec || spec->count == 0)
    return cJSON_CreateNull();
  cJSON *root = cJSON_CreateObject();
  cJSON_AddItemToObject(
      root, "frequencies",
      safe_create_float_array(spec->frequencies, (int)spec->count));
  cJSON_AddItemToObject(
      root, "magnitudes",
      safe_create_float_array(spec->magnitudes, (int)spec->count));
  return root;
}

static cJSON *
serialize_device_descriptor(const cdsp_device_descriptor_t *desc) {
  if (!desc)
    return cJSON_CreateNull();
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "name", desc->name);
  cJSON_AddStringToObject(root, "description", desc->description);

  cJSON *cs_arr = cJSON_CreateArray();
  cJSON_AddItemToObject(root, "capability_sets", cs_arr);

  for (size_t cs_idx = 0; cs_idx < desc->capability_sets_count; cs_idx++) {
    const cdsp_device_capability_set_t *cs = &desc->capability_sets[cs_idx];
    cJSON *cs_obj = cJSON_CreateObject();
    cJSON_AddItemToArray(cs_arr, cs_obj);
    cJSON_AddStringToObject(cs_obj, "mode", cs->mode);

    cJSON *caps_arr = cJSON_CreateArray();
    cJSON_AddItemToObject(cs_obj, "capabilities", caps_arr);

    for (size_t c_idx = 0; c_idx < cs->capabilities_count; c_idx++) {
      const cdsp_channel_capability_t *cap = &cs->capabilities[c_idx];
      cJSON *cap_obj = cJSON_CreateObject();
      cJSON_AddItemToArray(caps_arr, cap_obj);
      cJSON_AddNumberToObject(cap_obj, "channels", cap->channels);

      cJSON *sr_arr = cJSON_CreateArray();
      cJSON_AddItemToObject(cap_obj, "samplerates", sr_arr);

      for (size_t s_idx = 0; s_idx < cap->samplerates_count; s_idx++) {
        const cdsp_samplerate_capability_t *sr = &cap->samplerates[s_idx];
        cJSON *sr_obj = cJSON_CreateObject();
        cJSON_AddItemToArray(sr_arr, sr_obj);
        cJSON_AddNumberToObject(sr_obj, "samplerate", sr->samplerate);

        cJSON *formats_arr = cJSON_CreateArray();
        cJSON_AddItemToObject(sr_obj, "formats", formats_arr);

        for (size_t f_idx = 0; f_idx < sr->formats_count; f_idx++) {
          cJSON_AddItemToArray(formats_arr,
                               cJSON_CreateString(sr->formats[f_idx]));
        }
      }
    }
  }
  return root;
}

static const char *get_websocket_error_key(cdsp_backend_error_type_t type) {
  switch (type) {
  case CDSP_BACKEND_ERR_SUCCESS:
    return "ProcessingError";
  case CDSP_BACKEND_ERR_CONFIG_PARSE:
    return "ConfigValidationError";
  case CDSP_BACKEND_ERR_CONFIG_READ:
    return "ConfigReadError";
  case CDSP_BACKEND_ERR_DEVICE_NOT_FOUND:
    return "DeviceNotFoundError";
  case CDSP_BACKEND_ERR_DEVICE_BUSY:
    return "DeviceBusyError";
  case CDSP_BACKEND_ERR_UNKNOWN:
  default:
    return "ProcessingError";
  }
}

static const char *
get_websocket_device_error_key(cdsp_device_error_type_t type) {
  switch (type) {
  case CDSP_DEVICE_ERROR_NOT_FOUND:
    return "DeviceNotFoundError";
  case CDSP_DEVICE_ERROR_BUSY:
    return "DeviceBusyError";
  case CDSP_DEVICE_ERROR_NONE:
  case CDSP_DEVICE_ERROR_UNKNOWN:
  default:
    return "DeviceError";
  }
}

// MARK: - Session & Queue Management

static void client_session_clear(client_session_t *session) {
  if (!session)
    return;
  if (session->vu_pb_rms) {
    free(session->vu_pb_rms);
    session->vu_pb_rms = NULL;
  }
  if (session->vu_pb_peak) {
    free(session->vu_pb_peak);
    session->vu_pb_peak = NULL;
  }
  if (session->vu_cap_rms) {
    free(session->vu_cap_rms);
    session->vu_cap_rms = NULL;
  }
  if (session->vu_cap_peak) {
    free(session->vu_cap_peak);
    session->vu_cap_peak = NULL;
  }
  session->vu_pb_channels = 0;
  session->vu_cap_channels = 0;

  if (session->rx_buf) {
    free(session->rx_buf);
    session->rx_buf = NULL;
  }
  session->rx_len = 0;
  session->rx_cap = 0;

  ws_msg_node_t *node = session->out_queue_head;
  while (node) {
    ws_msg_node_t *next = node->next;
    if (node->data)
      free(node->data);
    free(node);
    node = next;
  }
  session->out_queue_head = NULL;
  session->out_queue_tail = NULL;

  session->last_vu_update_time = 0;
  session->last_pb_generation = 0;
  session->last_cap_generation = 0;
  session->vu_pending_publish = false;
  session->last_sig_pb_generation = 0;
  session->last_sig_cap_generation = 0;
  session->in_use = false;
  session->wsi = NULL;
  session->pss = NULL;
}

static void session_enqueue_message(client_session_t *session, const char *data,
                                    size_t len) {
  if (!session || !data || len == 0)
    return;
  ws_msg_node_t *node = (ws_msg_node_t *)malloc(sizeof(ws_msg_node_t));
  if (!node)
    return;
  node->data = (char *)malloc(len + 1);
  if (!node->data) {
    free(node);
    return;
  }
  memcpy(node->data, data, len);
  node->data[len] = '\0';
  node->len = len;
  node->next = NULL;

  if (session->out_queue_tail) {
    session->out_queue_tail->next = node;
    session->out_queue_tail = node;
  } else {
    session->out_queue_head = node;
    session->out_queue_tail = node;
  }
}

static void session_send_json(client_session_t *session, cJSON *root) {
  char *json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (json) {
    session_enqueue_message(session, json, strlen(json));
    free(json);
    if (session->wsi)
      lws_callback_on_writable(session->wsi);
  }
}

// MARK: - Metering & Smoothing Math

static void update_global_peaks(float **peaks, size_t *peaks_count,
                                const float *curr_peaks, size_t count) {
  if (count == 0 || !curr_peaks)
    return;
  if (*peaks_count != count || !*peaks) {
    float *new_peaks = (float *)realloc(*peaks, count * sizeof(float));
    if (!new_peaks)
      return;
    *peaks = new_peaks;
    memset(*peaks, 0, count * sizeof(float));
    *peaks_count = count;
  }
  for (size_t k = 0; k < count; k++) {
    float lin = db_to_amplitude(curr_peaks[k]);
    if (lin > (*peaks)[k])
      (*peaks)[k] = lin;
  }
}

static void smooth_vu_buffers(float **p_rms, float **p_peak, size_t *p_channels,
                              const float *curr_rms, const float *curr_peak,
                              size_t channels, float attack, float release) {
  if (channels == 0 || !curr_rms || !curr_peak)
    return;
  if (*p_channels != channels || !*p_rms || !*p_peak) {
    float *new_rms = (float *)calloc(channels, sizeof(float));
    float *new_peak = (float *)calloc(channels, sizeof(float));
    if (!new_rms || !new_peak) {
      if (new_rms)
        free(new_rms);
      if (new_peak)
        free(new_peak);
      return;
    }
    size_t copy_count = (*p_channels < channels) ? *p_channels : channels;
    if (*p_rms) {
      memcpy(new_rms, *p_rms, copy_count * sizeof(float));
      free(*p_rms);
    }
    if (*p_peak) {
      memcpy(new_peak, *p_peak, copy_count * sizeof(float));
      free(*p_peak);
    }
    for (size_t k = copy_count; k < channels; k++) {
      new_rms[k] = curr_rms[k];
      new_peak[k] = curr_peak[k];
    }
    *p_rms = new_rms;
    *p_peak = new_peak;
    *p_channels = channels;
    return;
  }

  for (size_t k = 0; k < channels; k++) {
    float prev_amp = db_to_amplitude((*p_rms)[k]);
    float curr_amp = db_to_amplitude(curr_rms[k]);
    float diff = curr_amp - prev_amp;
    prev_amp += (diff > 0.0f ? attack : release) * diff;
    (*p_rms)[k] = amplitude_to_db(prev_amp);

    prev_amp = db_to_amplitude((*p_peak)[k]);
    curr_amp = db_to_amplitude(curr_peak[k]);
    diff = curr_amp - prev_amp;
    prev_amp += (diff > 0.0f ? 1.0f : release) * diff;
    (*p_peak)[k] = amplitude_to_db(prev_amp);
  }
}

// MARK: - Periodic Push Events

static void push_state_event(client_session_t *session, const char *state_str,
                             const ws_state_update_t *status) {
  if (!session->state_subscribed || strcmp(session->last_state, state_str) == 0)
    return;

  strncpy(session->last_state, state_str, sizeof(session->last_state) - 1);
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "reply", "StateEvent");
  cJSON_AddStringToObject(root, "result", "Ok");
  cJSON_AddItemToObject(
      root, "value",
      create_state_event_value(status->state, &status->stop_reason));
  session_send_json(session, root);
}

static void push_vu_levels_event(client_session_t *session, uint64_t now,
                                 uint64_t pb_gen, uint64_t cap_gen,
                                 size_t pb_ch, size_t cap_ch,
                                 const float *pb_pk, const float *pb_rms,
                                 const float *cap_pk, const float *cap_rms) {
  if (!session->vu_subscribed || (pb_ch == 0 && cap_ch == 0))
    return;

  bool has_gens = (pb_gen > 0 || cap_gen > 0);
  bool new_chunk = has_gens ? (pb_gen != session->last_pb_generation ||
                               cap_gen != session->last_cap_generation)
                            : true;

  if (new_chunk) {
    session->last_pb_generation = pb_gen;
    session->last_cap_generation = cap_gen;
    float dt = (session->last_vu_update_time == 0)
                   ? 100.0f
                   : (float)(now - session->last_vu_update_time);
    session->last_vu_update_time = now;
    float attack = smoothing_alpha(dt, session->vu_attack);
    float release = smoothing_alpha(dt, session->vu_release);

    smooth_vu_buffers(&session->vu_pb_rms, &session->vu_pb_peak,
                      &session->vu_pb_channels, pb_rms, pb_pk, pb_ch, attack,
                      release);
    smooth_vu_buffers(&session->vu_cap_rms, &session->vu_cap_peak,
                      &session->vu_cap_channels, cap_rms, cap_pk, cap_ch,
                      attack, release);
    session->vu_pending_publish = true;
  }

  float interval =
      session->vu_max_rate > 0.0f ? 1000.0f / session->vu_max_rate : 0.0f;
  if (session->vu_pending_publish &&
      (now - session->last_vu_push_time >= interval)) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "reply", "VuLevelsEvent");
    cJSON_AddStringToObject(root, "result", "Ok");
    cJSON *val = cJSON_CreateObject();
    cJSON_AddItemToObject(
        val, "playback_rms",
        safe_create_float_array(session->vu_pb_rms,
                                (int)session->vu_pb_channels));
    cJSON_AddItemToObject(
        val, "playback_peak",
        safe_create_float_array(session->vu_pb_peak,
                                (int)session->vu_pb_channels));
    cJSON_AddItemToObject(
        val, "capture_rms",
        safe_create_float_array(session->vu_cap_rms,
                                (int)session->vu_cap_channels));
    cJSON_AddItemToObject(
        val, "capture_peak",
        safe_create_float_array(session->vu_cap_peak,
                                (int)session->vu_cap_channels));
    cJSON_AddItemToObject(root, "value", val);
    session_send_json(session, root);
    session->vu_pending_publish = false;
    session->last_vu_push_time = now;
  }
}

static void push_signal_side_event(client_session_t *session, const char *side,
                                   const float *rms, const float *peak,
                                   size_t channels) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "reply", "SignalLevelsEvent");
  cJSON_AddStringToObject(root, "result", "Ok");
  cJSON *val = cJSON_CreateObject();
  cJSON_AddStringToObject(val, "side", side);
  cJSON_AddItemToObject(val, "rms",
                        safe_create_float_array(rms, (int)channels));
  cJSON_AddItemToObject(val, "peak",
                        safe_create_float_array(peak, (int)channels));
  cJSON_AddItemToObject(root, "value", val);
  session_send_json(session, root);
}

static void push_signal_levels_event(client_session_t *session, uint64_t pb_gen,
                                     uint64_t cap_gen, size_t pb_ch,
                                     size_t cap_ch, const float *pb_pk,
                                     const float *pb_rms, const float *cap_pk,
                                     const float *cap_rms) {
  if (!session->signal_levels_subscribed)
    return;

  bool send_pb = strcmp(session->signal_levels_side, "playback") == 0 ||
                 strcmp(session->signal_levels_side, "both") == 0;
  bool send_cap = strcmp(session->signal_levels_side, "capture") == 0 ||
                  strcmp(session->signal_levels_side, "both") == 0;

  bool has_gens = (pb_gen > 0 || cap_gen > 0);
  bool pb_changed =
      has_gens ? (pb_gen != session->last_sig_pb_generation) : true;
  bool cap_changed =
      has_gens ? (cap_gen != session->last_sig_cap_generation) : true;

  if (send_pb && pb_ch > 0 && pb_changed) {
    session->last_sig_pb_generation = pb_gen;
    push_signal_side_event(session, "playback", pb_rms, pb_pk, pb_ch);
  }
  if (send_cap && cap_ch > 0 && cap_changed) {
    session->last_sig_cap_generation = cap_gen;
    push_signal_side_event(session, "capture", cap_rms, cap_pk, cap_ch);
  }
}

static void push_spectrum_event(websocket_server_t *server,
                                client_session_t *session, uint64_t now) {
  if (!session->spectrum_subscribed)
    return;

  if (!server || !server->engine ||
      cdsp_get_state(server->engine) == CDSP_PROCESSING_STATE_INACTIVE) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "reply", "SpectrumEvent");
    cJSON_AddStringToObject(root, "result", "ProcessingStopped");
    session_send_json(session, root);
    session->spectrum_subscribed = false;
    return;
  }

  int cap_rate = cdsp_get_capture_rate(server->engine);
  if (cap_rate <= 0)
    cap_rate = 44100;
  float min_freq =
      (session->spectrum_min_freq > 0.0f) ? session->spectrum_min_freq : 20.0f;
  size_t min_len = (size_t)ceilf((float)cap_rate / min_freq);
  size_t fft_len = 1024;
  while (fft_len < min_len && fft_len < 65536)
    fft_len <<= 1;

  float hop_interval_ms = (float)fft_len * 500.0f / (float)cap_rate;
  float rate_interval_ms = (session->spectrum_max_rate > 0.0f)
                               ? 1000.0f / session->spectrum_max_rate
                               : 0.0f;
  float interval =
      rate_interval_ms > hop_interval_ms ? rate_interval_ms : hop_interval_ms;

  if (now - session->last_spectrum_push_time >= interval) {
    size_t n_bins = session->spectrum_n_bins;
    float *p_freqs = (float *)malloc(n_bins * sizeof(float));
    float *p_mags = (float *)malloc(n_bins * sizeof(float));
    cdsp_spectrum_t spec = {
        .frequencies = p_freqs,
        .magnitudes = p_mags,
        .error_message = {0},
    };
    cdsp_spectrum_side_t side_val = session->spectrum_is_capture
                                        ? CDSP_SPECTRUM_SIDE_CAPTURE
                                        : CDSP_SPECTRUM_SIDE_PLAYBACK;
    const size_t *chan_ptr = (session->spectrum_channel == (size_t)-1)
                                 ? NULL
                                 : &session->spectrum_channel;
    bool spec_ok = (p_freqs && p_mags) &&
                   cdsp_get_spectrum(server->engine, side_val, chan_ptr,
                                     session->spectrum_min_freq,
                                     session->spectrum_max_freq, n_bins, &spec);
    if (spec_ok) {
      cJSON *root = cJSON_CreateObject();
      cJSON_AddStringToObject(root, "reply", "SpectrumEvent");
      cJSON_AddStringToObject(root, "result", "Ok");
      cJSON_AddItemToObject(root, "value", serialize_spectrum(&spec));
      session_send_json(session, root);
      session->last_spectrum_push_time = now;
    }
    if (p_freqs)
      free(p_freqs);
    if (p_mags)
      free(p_mags);
  }
}

static void websocket_server_process_periodic(websocket_server_t *server) {
  if (!server)
    return;

  uint64_t now = get_time_ms();
  ws_state_update_t status = {0};
  if (server->engine) {
    status.state = cdsp_get_state(server->engine);
    cdsp_get_stop_reason(server->engine, &status.stop_reason);
  }
  const char *state_str = ws_processing_state_to_string(status.state);

  uint64_t cap_gen = cdsp_get_chunk_generation(server->engine, true);
  uint64_t pb_gen = cdsp_get_chunk_generation(server->engine, false);

  size_t cap_ch = 0, pb_ch = 0;
  float *cap_pk = NULL, *cap_rms = NULL, *pb_pk = NULL, *pb_rms = NULL;

  cdsp_vu_levels_t vu_query = {0};
  if (server->engine && cdsp_get_vu_levels(server->engine, &vu_query)) {
    cap_ch = vu_query.capture_channels;
    pb_ch = vu_query.playback_channels;
    if (cap_ch > 0) {
      cap_pk = (float *)malloc(cap_ch * sizeof(float));
      cap_rms = (float *)malloc(cap_ch * sizeof(float));
    }
    if (pb_ch > 0) {
      pb_pk = (float *)malloc(pb_ch * sizeof(float));
      pb_rms = (float *)malloc(pb_ch * sizeof(float));
    }
    cdsp_vu_levels_t vu = {
        .playback_rms = pb_rms,
        .playback_peak = pb_pk,
        .capture_rms = cap_rms,
        .capture_peak = cap_pk,
    };
    if (cdsp_get_vu_levels(server->engine, &vu)) {
      update_global_peaks(&server->capture_global_peaks,
                          &server->capture_global_peaks_count, cap_pk, cap_ch);
      update_global_peaks(&server->playback_global_peaks,
                          &server->playback_global_peaks_count, pb_pk, pb_ch);
    }
  }

  pthread_mutex_lock(&server->sessions_mutex);
  for (int i = 0; i < 32; i++) {
    client_session_t *sess = &server->client_sessions[i];
    if (!sess->in_use || !sess->wsi)
      continue;

    push_state_event(sess, state_str, &status);
    push_vu_levels_event(sess, now, pb_gen, cap_gen, pb_ch, cap_ch, pb_pk,
                         pb_rms, cap_pk, cap_rms);
    push_signal_levels_event(sess, pb_gen, cap_gen, pb_ch, cap_ch, pb_pk,
                             pb_rms, cap_pk, cap_rms);
    push_spectrum_event(server, sess, now);
  }
  pthread_mutex_unlock(&server->sessions_mutex);

  if (cap_pk)
    free(cap_pk);
  if (cap_rms)
    free(cap_rms);
  if (pb_pk)
    free(pb_pk);
  if (pb_rms)
    free(pb_rms);
}

// MARK: - RPC Command Dispatcher Helpers

#define WS_COMMAND_LIST(X)                                                     \
  X(GET_VERSION, "GetVersion")                                                 \
  X(GET_STATE, "GetState")                                                     \
  X(GET_STOP_REASON, "GetStopReason")                                          \
  X(GET_CAPTURE_RATE, "GetCaptureRate")                                        \
  X(GET_RATE_ADJUST, "GetRateAdjust")                                          \
  X(GET_BUFFER_LEVEL, "GetBufferLevel")                                        \
  X(GET_CLIPPED_SAMPLES, "GetClippedSamples")                                  \
  X(RESET_CLIPPED_SAMPLES, "ResetClippedSamples")                              \
  X(GET_PROCESSING_LOAD, "GetProcessingLoad")                                  \
  X(GET_RESAMPLER_LOAD, "GetResamplerLoad")                                    \
  X(GET_SUPPORTED_DEVICE_TYPES, "GetSupportedDeviceTypes")                     \
  X(GET_UPDATE_INTERVAL, "GetUpdateInterval")                                  \
  X(SET_UPDATE_INTERVAL, "SetUpdateInterval")                                  \
  X(GET_VOLUME, "GetVolume")                                                   \
  X(SET_VOLUME, "SetVolume")                                                   \
  X(GET_MUTE, "GetMute")                                                       \
  X(SET_MUTE, "SetMute")                                                       \
  X(TOGGLE_MUTE, "ToggleMute")                                                 \
  X(GET_FADERS, "GetFaders")                                                   \
  X(GET_FADER_VOLUME, "GetFaderVolume")                                        \
  X(SET_FADER_VOLUME, "SetFaderVolume")                                        \
  X(SET_FADER_EXTERNAL_VOLUME, "SetFaderExternalVolume")                       \
  X(GET_FADER_MUTE, "GetFaderMute")                                            \
  X(SET_FADER_MUTE, "SetFaderMute")                                            \
  X(TOGGLE_FADER_MUTE, "ToggleFaderMute")                                      \
  X(ADJUST_VOLUME, "AdjustVolume")                                             \
  X(ADJUST_FADER_VOLUME, "AdjustFaderVolume")                                  \
  X(GET_SPECTRUM, "GetSpectrum")                                               \
  X(GET_AVAILABLE_CAPTURE_DEVICES, "GetAvailableCaptureDevices")               \
  X(GET_AVAILABLE_PLAYBACK_DEVICES, "GetAvailablePlaybackDevices")             \
  X(GET_CAPTURE_DEVICE_CAPABILITIES, "GetCaptureDeviceCapabilities")           \
  X(GET_PLAYBACK_DEVICE_CAPABILITIES, "GetPlaybackDeviceCapabilities")         \
  X(GET_CAPTURE_SIGNAL_RMS, "GetCaptureSignalRms")                             \
  X(GET_CAPTURE_SIGNAL_PEAK, "GetCaptureSignalPeak")                           \
  X(GET_PLAYBACK_SIGNAL_RMS, "GetPlaybackSignalRms")                           \
  X(GET_PLAYBACK_SIGNAL_PEAK, "GetPlaybackSignalPeak")                         \
  X(GET_CAPTURE_SIGNAL_RMS_SINCE_LAST, "GetCaptureSignalRmsSinceLast")         \
  X(GET_CAPTURE_SIGNAL_PEAK_SINCE_LAST, "GetCaptureSignalPeakSinceLast")       \
  X(GET_PLAYBACK_SIGNAL_RMS_SINCE_LAST, "GetPlaybackSignalRmsSinceLast")       \
  X(GET_PLAYBACK_SIGNAL_PEAK_SINCE_LAST, "GetPlaybackSignalPeakSinceLast")     \
  X(GET_CAPTURE_SIGNAL_RMS_SINCE, "GetCaptureSignalRmsSince")                  \
  X(GET_CAPTURE_SIGNAL_PEAK_SINCE, "GetCaptureSignalPeakSince")                \
  X(GET_PLAYBACK_SIGNAL_RMS_SINCE, "GetPlaybackSignalRmsSince")                \
  X(GET_PLAYBACK_SIGNAL_PEAK_SINCE, "GetPlaybackSignalPeakSince")              \
  X(GET_SIGNAL_LEVELS, "GetSignalLevels")                                      \
  X(GET_SIGNAL_LEVELS_SINCE_LAST, "GetSignalLevelsSinceLast")                  \
  X(GET_SIGNAL_LEVELS_SINCE, "GetSignalLevelsSince")                           \
  X(GET_SIGNAL_PEAKS_SINCE_START, "GetSignalPeaksSinceStart")                  \
  X(RESET_SIGNAL_PEAKS_SINCE_START, "ResetSignalPeaksSinceStart")              \
  X(GET_CHANNEL_LABELS, "GetChannelLabels")                                    \
  X(GET_SIGNAL_RANGE, "GetSignalRange")                                        \
  X(GET_CONFIG_FILE_PATH, "GetConfigFilePath")                                 \
  X(GET_PREVIOUS_CONFIG, "GetPreviousConfig")                                  \
  X(GET_STATE_FILE_PATH, "GetStateFilePath")                                   \
  X(GET_STATE_FILE_UPDATED, "GetStateFileUpdated")                             \
  X(GET_CONFIG, "GetConfig")                                                   \
  X(GET_CONFIG_JSON, "GetConfigJson")                                          \
  X(GET_CONFIG_TITLE, "GetConfigTitle")                                        \
  X(GET_CONFIG_DESCRIPTION, "GetConfigDescription")                            \
  X(RELOAD, "Reload")                                                          \
  X(STOP, "Stop")                                                              \
  X(EXIT, "Exit")                                                              \
  X(SET_CONFIG_FILE_PATH, "SetConfigFilePath")                                 \
  X(SET_CONFIG, "SetConfig")                                                   \
  X(SET_CONFIG_JSON, "SetConfigJson")                                          \
  X(GET_CONFIG_VALUE, "GetConfigValue")                                        \
  X(SET_CONFIG_VALUE, "SetConfigValue")                                        \
  X(PATCH_CONFIG, "PatchConfig")                                               \
  X(READ_CONFIG, "ReadConfig")                                                 \
  X(READ_CONFIG_JSON, "ReadConfigJson")                                        \
  X(READ_CONFIG_FILE, "ReadConfigFile")                                        \
  X(VALIDATE_CONFIG, "ValidateConfig")                                         \
  X(VALIDATE_CONFIG_JSON, "ValidateConfigJson")                                \
  X(VALIDATE_CONFIG_FILE, "ValidateConfigFile")                                \
  X(SUBSCRIBE_STATE, "SubscribeState")                                         \
  X(SUBSCRIBE_VU_LEVELS, "SubscribeVuLevels")                                  \
  X(SUBSCRIBE_SIGNAL_LEVELS, "SubscribeSignalLevels")                          \
  X(SUBSCRIBE_SPECTRUM, "SubscribeSpectrum")                                   \
  X(STOP_SUBSCRIPTION, "StopSubscription")

typedef enum {
  WS_CMD_UNKNOWN = 0,
#define X(id, name) WS_CMD_##id,
  WS_COMMAND_LIST(X)
#undef X
} websocket_command_t;

typedef struct {
  const char *name;
  websocket_command_t type;
} command_map_t;

static const command_map_t kCommandMap[] = {
#define X(id, name) {name, WS_CMD_##id},
    WS_COMMAND_LIST(X)
#undef X
};

static websocket_command_t lookup_command(const char *name) {
  if (!name)
    return WS_CMD_UNKNOWN;
  for (size_t i = 0; i < sizeof(kCommandMap) / sizeof(kCommandMap[0]); i++) {
    if (strcmp(kCommandMap[i].name, name) == 0)
      return kCommandMap[i].type;
  }
  return WS_CMD_UNKNOWN;
}

static inline bool validate_and_clamp_volume(float *inout_vol) {
  if (isnan(*inout_vol))
    return false;
  if (*inout_vol > 50.0f)
    *inout_vol = 50.0f;
  if (*inout_vol < -150.0f)
    *inout_vol = -150.0f;
  return true;
}

static bool server_handle_adjust_volume_fader(websocket_server_t *server,
                                              cdsp_fader_t fader, float delta,
                                              float min_vol, float max_vol,
                                              dyn_string_t *ds,
                                              const char *cmd_name) {
  if (!server || !server->engine) {
    reply_error(cmd_name, "InvalidRequestError", "Server or engine unavailable",
                ds);
    return true;
  }
  if (max_vol < min_vol) {
    float current = cdsp_get_fader_volume(server->engine, fader);
    cJSON *val = (strcmp(cmd_name, "AdjustVolume") == 0)
                     ? safe_create_float_number(current)
                     : cJSON_CreateArray();
    if (val && strcmp(cmd_name, "AdjustVolume") != 0) {
      cJSON_AddItemToArray(val, cJSON_CreateNumber((double)fader));
      cJSON_AddItemToArray(val, safe_create_float_number(current));
    }
    reply_error_with_value(cmd_name, "InvalidValueError",
                           "Max volume must be bigger than min volume", val,
                           ds);
    return true;
  }

  float current = cdsp_get_fader_volume(server->engine, fader);
  float new_vol = current + delta;
  if (new_vol < min_vol)
    new_vol = min_vol;
  if (new_vol > max_vol)
    new_vol = max_vol;

  cdsp_set_fader_volume(server->engine, fader, new_vol, false);

  if (strcmp(cmd_name, "AdjustVolume") == 0) {
    reply_ok(cmd_name, safe_create_float_number(new_vol), ds);
  } else {
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)fader));
    cJSON_AddItemToArray(arr, safe_create_float_number(new_vol));
    reply_ok(cmd_name, arr, ds);
  }
  return true;
}

static cJSON *string_array_to_json(char **items, size_t count) {
  cJSON *arr = cJSON_CreateArray();
  if (items) {
    for (size_t i = 0; i < count; i++) {
      if (items[i])
        cJSON_AddItemToArray(arr, cJSON_CreateString(items[i]));
      else
        cJSON_AddItemToArray(arr, cJSON_CreateNull());
    }
  }
  return arr;
}

static bool get_fader_arg(cJSON *root, int *out_idx, dyn_string_t *ds) {
  cJSON *fader_node = cJSON_GetObjectItemCaseSensitive(root, "fader");
  if (!fader_node || !cJSON_IsNumber(fader_node) ||
      fader_node->valuedouble < 0.0) {
    reply_invalid("invalid type or missing field `fader`", ds);
    return false;
  }
  *out_idx = fader_node->valueint;
  return true;
}

static void parse_volume_limits(cJSON *root, float *min_vol, float *max_vol) {
  *min_vol = -150.0f;
  *max_vol = 50.0f;
  cJSON *min_node = cJSON_GetObjectItemCaseSensitive(root, "min");
  if (min_node && cJSON_IsNumber(min_node))
    *min_vol = (float)min_node->valuedouble;
  cJSON *max_node = cJSON_GetObjectItemCaseSensitive(root, "max");
  if (max_node && cJSON_IsNumber(max_node))
    *max_vol = (float)max_node->valuedouble;
}

static void handle_set_volume_common(websocket_server_t *server,
                                     const char *cmd_name, cJSON *root,
                                     bool is_fader, bool external,
                                     dyn_string_t *ds) {
  int idx = CDSP_FADER_MAIN;
  if (is_fader) {
    cJSON *fader_node = cJSON_GetObjectItemCaseSensitive(root, "fader");
    if (!fader_node || !cJSON_IsNumber(fader_node) ||
        fader_node->valuedouble < 0.0) {
      reply_invalid("invalid type or missing field `fader` or `value`", ds);
      return;
    }
    idx = fader_node->valueint;
  }
  cJSON *val_node = cJSON_GetObjectItemCaseSensitive(root, "value");
  if (!val_node || !cJSON_IsNumber(val_node)) {
    reply_invalid(is_fader ? "invalid type or missing field `fader` or `value`"
                           : "invalid type or missing field `value`",
                  ds);
    return;
  }
  float vol = (float)val_node->valuedouble;
  if (!validate_and_clamp_volume(&vol)) {
    reply_error(cmd_name, "InvalidValueError", "Volume must be a finite number",
                ds);
    return;
  }
  if (!server || !server->engine) {
    reply_error(cmd_name, "InvalidRequestError", "Server or engine unavailable",
                ds);
    return;
  }
  if (idx < 0 || idx >= CDSP_FADER_COUNT) {
    reply_error(cmd_name, "InvalidFaderError", NULL, ds);
    return;
  }
  cdsp_set_fader_volume(server->engine, (cdsp_fader_t)idx, vol, external);
  reply_ok(cmd_name, NULL, ds);
}

static void handle_set_mute_common(websocket_server_t *server,
                                   const char *cmd_name, cJSON *root,
                                   bool is_fader, dyn_string_t *ds) {
  int idx = CDSP_FADER_MAIN;
  if (is_fader) {
    cJSON *fader_node = cJSON_GetObjectItemCaseSensitive(root, "fader");
    if (!fader_node || !cJSON_IsNumber(fader_node) ||
        fader_node->valuedouble < 0.0) {
      reply_invalid("invalid type or missing field `fader` or `value`", ds);
      return;
    }
    idx = fader_node->valueint;
  }
  cJSON *val_node = cJSON_GetObjectItemCaseSensitive(root, "value");
  if (!val_node || !cJSON_IsBool(val_node)) {
    reply_invalid(is_fader ? "invalid type or missing field `fader` or `value`"
                           : "invalid type or missing field `value`",
                  ds);
    return;
  }
  if (!server || !server->engine) {
    reply_error(cmd_name, "InvalidRequestError", "Server or engine unavailable",
                ds);
    return;
  }
  if (idx < 0 || idx >= CDSP_FADER_COUNT) {
    reply_error(cmd_name, "InvalidFaderError", NULL, ds);
    return;
  }
  cdsp_set_fader_mute(server->engine, (cdsp_fader_t)idx,
                      cJSON_IsTrue(val_node));
  reply_ok(cmd_name, NULL, ds);
}

static cJSON *get_signal_vector(websocket_server_t *server, int client_idx,
                                bool is_capture, bool is_rms, int mode,
                                float secs) {
  if (!server)
    return safe_create_float_array(NULL, 0);

  if (mode == 0) {
    if (!server->engine)
      return safe_create_float_array(NULL, 0);
    cdsp_vu_levels_t vu = {0};
    if (cdsp_get_vu_levels(server->engine, &vu)) {
      size_t count = is_capture ? vu.capture_channels : vu.playback_channels;
      if (count > 0) {
        float *buf = (float *)malloc(count * sizeof(float));
        if (buf) {
          if (is_capture)
            *(is_rms ? &vu.capture_rms : &vu.capture_peak) = buf;
          else
            *(is_rms ? &vu.playback_rms : &vu.playback_peak) = buf;
          cdsp_get_vu_levels(server->engine, &vu);
          cJSON *arr = safe_create_float_array(buf, (int)count);
          free(buf);
          return arr;
        }
      }
    }
    return safe_create_float_array(NULL, 0);
  }

  uint64_t now = get_time_ms();
  uint64_t since = 0;
  if (mode == 1) {
    if (client_idx < 0 || client_idx >= 32)
      return safe_create_float_array(NULL, 0);
    client_session_t *sess = &server->client_sessions[client_idx];
    if (is_capture)
      since = is_rms ? sess->last_cap_rms_time : sess->last_cap_peak_time;
    else
      since = is_rms ? sess->last_pb_rms_time : sess->last_pb_peak_time;
  } else if (mode == 2) {
    if (secs < 0.0f || !isfinite(secs))
      secs = 0.0f;
    else if (secs > 600.0f)
      secs = 600.0f;
    since = now - (uint64_t)(secs * 1000.0f);
  }

  size_t ch = 0;
  if (server->engine &&
      cdsp_get_signal_levels_since(server->engine, is_capture, is_rms, since,
                                   NULL, &ch) &&
      ch > 0) {
    float *buf = (float *)malloc(ch * sizeof(float));
    if (buf) {
      cdsp_get_signal_levels_since(server->engine, is_capture, is_rms, since,
                                   buf, &ch);
      if (mode == 1 && client_idx >= 0 && client_idx < 32) {
        client_session_t *sess = &server->client_sessions[client_idx];
        if (is_capture) {
          if (is_rms)
            sess->last_cap_rms_time = now;
          else
            sess->last_cap_peak_time = now;
        } else {
          if (is_rms)
            sess->last_pb_rms_time = now;
          else
            sess->last_pb_peak_time = now;
        }
      }
      cJSON *arr = safe_create_float_array(buf, (int)ch);
      free(buf);
      return arr;
    }
  }
  return safe_create_float_array(NULL, 0);
}

static void handle_cmd_get_signal_levels_combined(websocket_server_t *server,
                                                  int client_idx,
                                                  const char *cmd_name,
                                                  int mode, float secs,
                                                  dyn_string_t *ds) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddItemToObject(
      root, "playback_rms",
      get_signal_vector(server, client_idx, false, true, mode, secs));
  cJSON_AddItemToObject(
      root, "playback_peak",
      get_signal_vector(server, client_idx, false, false, mode, secs));
  cJSON_AddItemToObject(
      root, "capture_rms",
      get_signal_vector(server, client_idx, true, true, mode, secs));
  cJSON_AddItemToObject(
      root, "capture_peak",
      get_signal_vector(server, client_idx, true, false, mode, secs));
  reply_ok(cmd_name, root, ds);
}

static void handle_config_check(const char *cmd_name, cJSON *root,
                                bool is_validate, int kind, dyn_string_t *ds) {
  cJSON *arg = cJSON_GetObjectItemCaseSensitive(root, "value");
  if (!arg || !cJSON_IsString(arg) || !arg->valuestring) {
    char err_msg[128];
    snprintf(err_msg, sizeof(err_msg),
             "missing field `value` or invalid type for %s", cmd_name);
    reply_invalid(err_msg, ds);
    return;
  }
  const char *input = arg->valuestring;
  char *result = NULL;
  cdsp_config_error_type_t err_type = CDSP_CONFIG_ERR_NONE;
  bool ok = false;

  if (is_validate) {
    if (kind == 0)
      ok = cdsp_validate_config_json(input, &result, &err_type);
    else if (kind == 1)
      ok = cdsp_validate_config_yaml(input, &result, &err_type);
    else
      ok = cdsp_validate_config_file(input, &result, &err_type);
  } else {
    if (kind == 0)
      ok = cdsp_read_config_json(input, &result, &err_type);
    else if (kind == 1)
      ok = cdsp_read_config_yaml(input, &result, &err_type);
    else
      ok = cdsp_read_config_file(input, &result, &err_type);
  }

  if (ok && err_type == CDSP_CONFIG_ERR_NONE) {
    reply_ok(cmd_name, cJSON_CreateString(result ? result : input), ds);
  } else {
    const char *err_key = (is_validate && err_type != CDSP_CONFIG_ERR_PARSE)
                              ? "ConfigValidationError"
                              : "ConfigReadError";
    const char *fallback_msg =
        (kind == 2) ? "Invalid config file" : "Invalid config";
    const char *err_msg = (result && result[0]) ? result : fallback_msg;
    reply_error_with_value(cmd_name, err_key, err_msg,
                           cJSON_CreateString(err_msg), ds);
  }
  if (result)
    free(result);
}

static bool parse_spectrum_args(cJSON *root, const char *cmd_name,
                                bool *out_is_capture, size_t *out_channel,
                                float *out_min_freq, float *out_max_freq,
                                uint32_t *out_n_bins, dyn_string_t *ds) {
  cJSON *arg = cJSON_GetObjectItemCaseSensitive(root, "value");
  if (!arg || !cJSON_IsObject(arg)) {
    char err_msg[128];
    snprintf(
        err_msg, sizeof(err_msg),
        "%s requires a JSON object with side, min_freq, max_freq, and n_bins",
        cmd_name);
    reply_invalid(err_msg, ds);
    return false;
  }

  cJSON *item_bins = cJSON_GetObjectItemCaseSensitive(arg, "n_bins");
  if (!item_bins || !cJSON_IsNumber(item_bins) || item_bins->valueint < 2 ||
      item_bins->valuedouble != (double)item_bins->valueint) {
    if (item_bins && cJSON_IsNumber(item_bins) &&
        (item_bins->valueint < 2 || item_bins->valuedouble < 2.0)) {
      reply_error(cmd_name, "InvalidRequestError", "n_bins must be at least 2",
                  ds);
    } else {
      char err_msg[128];
      snprintf(err_msg, sizeof(err_msg), "%s requires integer n_bins >= 2",
               cmd_name);
      reply_invalid(err_msg, ds);
    }
    return false;
  }
  *out_n_bins = (uint32_t)item_bins->valueint;

  cJSON *item_min = cJSON_GetObjectItemCaseSensitive(arg, "min_freq");
  cJSON *item_max = cJSON_GetObjectItemCaseSensitive(arg, "max_freq");
  if (!item_min || !cJSON_IsNumber(item_min) || !item_max ||
      !cJSON_IsNumber(item_max)) {
    char err_msg[128];
    snprintf(err_msg, sizeof(err_msg),
             "%s requires numeric min_freq and max_freq", cmd_name);
    reply_invalid(err_msg, ds);
    return false;
  }
  *out_min_freq = (float)item_min->valuedouble;
  *out_max_freq = (float)item_max->valuedouble;

  if (*out_min_freq <= 0.0f || *out_min_freq >= *out_max_freq) {
    reply_error(cmd_name, "InvalidRequestError",
                "Invalid frequency range: min_freq must be > 0 and < max_freq",
                ds);
    return false;
  }

  cJSON *item_side = cJSON_GetObjectItemCaseSensitive(arg, "side");
  if (!item_side || !cJSON_IsString(item_side) || !item_side->valuestring) {
    reply_invalid("Missing or invalid 'side' parameter", ds);
    return false;
  }
  if (strcmp(item_side->valuestring, "capture") == 0)
    *out_is_capture = true;
  else if (strcmp(item_side->valuestring, "playback") == 0)
    *out_is_capture = false;
  else {
    reply_invalid("side must be 'capture' or 'playback'", ds);
    return false;
  }

  *out_channel = (size_t)-1;
  cJSON *item_chan = cJSON_GetObjectItemCaseSensitive(arg, "channel");
  if (item_chan && !cJSON_IsNull(item_chan)) {
    if (cJSON_IsNumber(item_chan)) {
      if (item_chan->valueint < 0) {
        reply_invalid("channel must be non-negative", ds);
        return false;
      }
      *out_channel = (size_t)item_chan->valueint;
    } else {
      reply_invalid("channel must be an integer or null", ds);
      return false;
    }
  }
  return true;
}

// MARK: - Command Dispatcher Entrypoint

void websocket_server_handle_command(websocket_server_t *server, int client_idx,
                                     const char *command_text,
                                     dyn_string_t *ds) {
  if (!server || !ds || !command_text || client_idx < 0 || client_idx >= 32)
    return;

  cJSON *root = cJSON_Parse(command_text);
  if (!root) {
    reply_invalid("Invalid JSON", ds);
    return;
  }
  if (!cJSON_IsObject(root)) {
    reply_invalid("Command must be a JSON object", ds);
    cJSON_Delete(root);
    return;
  }

  cJSON *cmd_node = cJSON_GetObjectItemCaseSensitive(root, "command");
  if (!cmd_node || !cJSON_IsString(cmd_node) || !cmd_node->valuestring) {
    reply_invalid("Missing or invalid 'command' field", ds);
    cJSON_Delete(root);
    return;
  }

  char cmd_name[128] = "";
  strncpy(cmd_name, cmd_node->valuestring, sizeof(cmd_name) - 1);
  const char *simple = cmd_name;

  pthread_mutex_lock(&server->sessions_mutex);
  client_session_t *sess = &server->client_sessions[client_idx];
  bool is_streaming = sess->state_subscribed || sess->vu_subscribed ||
                      sess->signal_levels_subscribed ||
                      sess->spectrum_subscribed;

  websocket_command_t cmd_type = lookup_command(simple);
  if (is_streaming && cmd_type != WS_CMD_STOP_SUBSCRIPTION) {
    reply_invalid("Only StopSubscription is accepted while streaming is active",
                  ds);
    pthread_mutex_unlock(&server->sessions_mutex);
    cJSON_Delete(root);
    return;
  }

  switch (cmd_type) {
  case WS_CMD_GET_VERSION:
    reply_ok(simple, cJSON_CreateString(cdsp_get_version()), ds);
    break;

  case WS_CMD_GET_STATE: {
    cdsp_processing_state_t state = CDSP_PROCESSING_STATE_INACTIVE;
    if (server->engine) {
      ws_state_update_t status;
      status.state = cdsp_get_state(server->engine);
      cdsp_get_stop_reason(server->engine, &status.stop_reason);
      state = status.state;
    }
    reply_ok(simple, cJSON_CreateString(ws_processing_state_to_string(state)),
             ds);
    break;
  }

  case WS_CMD_GET_STOP_REASON: {
    cJSON *val = NULL;
    if (server->engine) {
      cdsp_stop_reason_t stop_reason;
      cdsp_get_stop_reason(server->engine, &stop_reason);
      val = serialize_stop_reason(&stop_reason);
    }
    reply_ok(simple, val ? val : cJSON_CreateString("None"), ds);
    break;
  }

  case WS_CMD_GET_CAPTURE_RATE: {
    int sr = server->engine ? cdsp_get_capture_rate(server->engine) : 0;
    reply_ok(simple, cJSON_CreateNumber(sr), ds);
    break;
  }

  case WS_CMD_GET_RATE_ADJUST: {
    double rate = 0.0;
    if (server->engine)
      cdsp_get_processing_status(server->engine, &rate, NULL, NULL, NULL, NULL);
    reply_ok(simple, safe_create_float_number(rate), ds);
    break;
  }

  case WS_CMD_GET_BUFFER_LEVEL: {
    double lvl = 0.0;
    if (server->engine)
      cdsp_get_processing_status(server->engine, NULL, &lvl, NULL, NULL, NULL);
    reply_ok(simple, cJSON_CreateNumber((int)lvl), ds);
    break;
  }

  case WS_CMD_GET_CLIPPED_SAMPLES: {
    uint64_t clips = 0;
    if (server->engine)
      cdsp_get_processing_status(server->engine, NULL, NULL, &clips, NULL,
                                 NULL);
    reply_ok(simple, cJSON_CreateNumber((double)clips), ds);
    break;
  }

  case WS_CMD_RESET_CLIPPED_SAMPLES:
    if (server->engine)
      cdsp_reset_clipped_samples(server->engine);
    reply_ok(simple, NULL, ds);
    break;

  case WS_CMD_GET_PROCESSING_LOAD:
  case WS_CMD_GET_RESAMPLER_LOAD: {
    double load = 0.0;
    if (server->engine) {
      if (cmd_type == WS_CMD_GET_PROCESSING_LOAD)
        cdsp_get_processing_status(server->engine, NULL, NULL, NULL, &load,
                                   NULL);
      else
        cdsp_get_processing_status(server->engine, NULL, NULL, NULL, NULL,
                                   &load);
    }
    reply_ok(simple, safe_create_float_number(load), ds);
    break;
  }

  case WS_CMD_GET_SUPPORTED_DEVICE_TYPES: {
    char **play_types = NULL, **cap_types = NULL;
    size_t play_count = 0, cap_count = 0;
    cdsp_get_supported_device_types(&play_types, &play_count, &cap_types,
                                    &cap_count);
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, string_array_to_json(play_types, play_count));
    cJSON_AddItemToArray(arr, string_array_to_json(cap_types, cap_count));
    if (play_types)
      cdsp_free_device_types(play_types, play_count);
    if (cap_types)
      cdsp_free_device_types(cap_types, cap_count);
    reply_ok(simple, arr, ds);
    break;
  }

  case WS_CMD_GET_UPDATE_INTERVAL:
    reply_ok(simple,
             cJSON_CreateNumber(server ? (int)server->update_interval : 1000),
             ds);
    break;

  case WS_CMD_SET_UPDATE_INTERVAL: {
    cJSON *arg = cJSON_GetObjectItemCaseSensitive(root, "value");
    if (arg && cJSON_IsNumber(arg) && arg->valuedouble >= 0.0 &&
        arg->valuedouble == floor(arg->valuedouble)) {
      server->update_interval = (uint32_t)arg->valuedouble;
      reply_ok(simple, NULL, ds);
    } else {
      reply_invalid(
          "invalid value for SetUpdateInterval: expected non-negative integer",
          ds);
    }
    break;
  }

  case WS_CMD_GET_VOLUME:
    if (server->engine)
      reply_ok(simple,
               safe_create_float_number(
                   cdsp_get_fader_volume(server->engine, CDSP_FADER_MAIN)),
               ds);
    else
      reply_error(simple, "InvalidRequestError", "Server or engine unavailable",
                  ds);
    break;

  case WS_CMD_SET_VOLUME:
    handle_set_volume_common(server, simple, root, false, false, ds);
    break;

  case WS_CMD_GET_MUTE:
    if (server->engine)
      reply_ok(simple,
               cJSON_CreateBool(
                   cdsp_get_fader_mute(server->engine, CDSP_FADER_MAIN)),
               ds);
    else
      reply_error(simple, "InvalidRequestError", "Server or engine unavailable",
                  ds);
    break;

  case WS_CMD_SET_MUTE:
    handle_set_mute_common(server, simple, root, false, ds);
    break;

  case WS_CMD_TOGGLE_MUTE:
    if (server->engine) {
      bool was = cdsp_get_fader_mute(server->engine, CDSP_FADER_MAIN);
      cdsp_set_fader_mute(server->engine, CDSP_FADER_MAIN, !was);
      reply_ok(simple, cJSON_CreateBool(!was), ds);
    } else {
      reply_error(simple, "InvalidRequestError", "Server or engine unavailable",
                  ds);
    }
    break;

  case WS_CMD_GET_FADERS: {
    if (!server->engine) {
      reply_error(simple, "InvalidRequestError", "Server or engine unavailable",
                  ds);
      break;
    }
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < CDSP_FADER_COUNT; i++) {
      cJSON *obj = cJSON_CreateObject();
      float vol = cdsp_get_fader_volume(server->engine, (cdsp_fader_t)i);
      bool mute = cdsp_get_fader_mute(server->engine, (cdsp_fader_t)i);
      cJSON_AddItemToObject(obj, "volume", safe_create_float_number(vol));
      cJSON_AddBoolToObject(obj, "mute", mute);
      cJSON_AddItemToArray(arr, obj);
    }
    reply_ok(simple, arr, ds);
    break;
  }

  case WS_CMD_GET_FADER_VOLUME:
  case WS_CMD_GET_FADER_MUTE: {
    int idx = 0;
    if (!get_fader_arg(root, &idx, ds))
      break;
    if (!server->engine) {
      reply_error(simple, "InvalidRequestError", "Server or engine unavailable",
                  ds);
      break;
    }
    bool is_vol = (cmd_type == WS_CMD_GET_FADER_VOLUME);
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, cJSON_CreateNumber(idx));
    if (idx < CDSP_FADER_COUNT) {
      if (is_vol) {
        float vol = cdsp_get_fader_volume(server->engine, (cdsp_fader_t)idx);
        cJSON_AddItemToArray(arr, safe_create_float_number(vol));
      } else {
        bool mute = cdsp_get_fader_mute(server->engine, (cdsp_fader_t)idx);
        cJSON_AddItemToArray(arr, cJSON_CreateBool(mute));
      }
      reply_ok(simple, arr, ds);
    } else {
      if (is_vol)
        cJSON_AddItemToArray(arr, safe_create_float_number(0.0));
      else
        cJSON_AddItemToArray(arr, cJSON_CreateBool(false));
      reply_error_with_value(simple, "InvalidFaderError", NULL, arr, ds);
    }
    break;
  }

  case WS_CMD_SET_FADER_VOLUME:
    handle_set_volume_common(server, simple, root, true, false, ds);
    break;

  case WS_CMD_SET_FADER_EXTERNAL_VOLUME:
    handle_set_volume_common(server, simple, root, true, true, ds);
    break;

  case WS_CMD_SET_FADER_MUTE:
    handle_set_mute_common(server, simple, root, true, ds);
    break;

  case WS_CMD_TOGGLE_FADER_MUTE: {
    int idx = 0;
    if (!get_fader_arg(root, &idx, ds))
      break;
    if (!server->engine) {
      reply_error(simple, "InvalidRequestError", "Server or engine unavailable",
                  ds);
      break;
    }
    if (idx < 0 || idx >= CDSP_FADER_COUNT) {
      cJSON *arr = cJSON_CreateArray();
      cJSON_AddItemToArray(arr, cJSON_CreateNumber(idx));
      cJSON_AddItemToArray(arr, cJSON_CreateBool(false));
      reply_error_with_value(simple, "InvalidFaderError", NULL, arr, ds);
      break;
    }
    bool was_muted = cdsp_get_fader_mute(server->engine, (cdsp_fader_t)idx);
    cdsp_set_fader_mute(server->engine, (cdsp_fader_t)idx, !was_muted);
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, cJSON_CreateNumber(idx));
    cJSON_AddItemToArray(arr, cJSON_CreateBool(!was_muted));
    reply_ok(simple, arr, ds);
    break;
  }

  case WS_CMD_ADJUST_VOLUME: {
    cJSON *val_node = cJSON_GetObjectItemCaseSensitive(root, "value");
    if (!val_node || !cJSON_IsNumber(val_node)) {
      reply_invalid("invalid type or missing field `value`", ds);
      break;
    }
    float min_vol, max_vol;
    parse_volume_limits(root, &min_vol, &max_vol);
    server_handle_adjust_volume_fader(server, CDSP_FADER_MAIN,
                                      (float)val_node->valuedouble, min_vol,
                                      max_vol, ds, simple);
    break;
  }

  case WS_CMD_ADJUST_FADER_VOLUME: {
    cJSON *fader_node = cJSON_GetObjectItemCaseSensitive(root, "fader");
    cJSON *val_node = cJSON_GetObjectItemCaseSensitive(root, "value");
    if (!fader_node || !val_node || !cJSON_IsNumber(fader_node) ||
        !cJSON_IsNumber(val_node) || fader_node->valuedouble < 0.0) {
      reply_invalid("invalid type or missing field `fader` or `value`", ds);
      break;
    }
    int fader = fader_node->valueint;
    float val = (float)val_node->valuedouble;
    if (fader < 0 || fader >= CDSP_FADER_COUNT) {
      cJSON *arr = cJSON_CreateArray();
      cJSON_AddItemToArray(arr, cJSON_CreateNumber(fader));
      cJSON_AddItemToArray(arr, safe_create_float_number(val));
      reply_error_with_value(simple, "InvalidFaderError", NULL, arr, ds);
      break;
    }
    float min_vol, max_vol;
    parse_volume_limits(root, &min_vol, &max_vol);
    server_handle_adjust_volume_fader(server, (cdsp_fader_t)fader, val, min_vol,
                                      max_vol, ds, simple);
    break;
  }

  case WS_CMD_GET_SPECTRUM: {
    bool is_capture = true;
    size_t channel = (size_t)-1;
    float min_freq = 20.0f, max_freq = 20000.0f;
    uint32_t n_bins = 1024;
    if (!parse_spectrum_args(root, simple, &is_capture, &channel, &min_freq,
                             &max_freq, &n_bins, ds))
      break;
    if (!server->engine ||
        cdsp_get_state(server->engine) == CDSP_PROCESSING_STATE_INACTIVE) {
      reply_error(simple, "ProcessingNotRunningError", NULL, ds);
      break;
    }
    cdsp_spectrum_side_t side_val =
        is_capture ? CDSP_SPECTRUM_SIDE_CAPTURE : CDSP_SPECTRUM_SIDE_PLAYBACK;
    const size_t *chan_ptr = (channel == (size_t)-1) ? NULL : &channel;
    float *p_freqs = (float *)malloc(n_bins * sizeof(float));
    float *p_mags = (float *)malloc(n_bins * sizeof(float));
    cdsp_spectrum_t spec = {
        .frequencies = p_freqs, .magnitudes = p_mags, .error_message = {0}};
    bool spec_ok = (p_freqs && p_mags) &&
                   cdsp_get_spectrum(server->engine, side_val, chan_ptr,
                                     min_freq, max_freq, n_bins, &spec);
    if (spec_ok) {
      cJSON *spec_json = serialize_spectrum(&spec);
      if (spec_json)
        reply_ok(simple, spec_json, ds);
      else
        reply_error(simple, "UnknownError", NULL, ds);
    } else {
      reply_error(simple, "InvalidRequestError",
                  spec.error_message[0] ? spec.error_message
                                        : "No audio data available",
                  ds);
    }
    if (p_freqs)
      free(p_freqs);
    if (p_mags)
      free(p_mags);
    break;
  }

  case WS_CMD_GET_AVAILABLE_CAPTURE_DEVICES:
  case WS_CMD_GET_AVAILABLE_PLAYBACK_DEVICES: {
    bool is_capture = (cmd_type == WS_CMD_GET_AVAILABLE_CAPTURE_DEVICES);
    cJSON *arg = cJSON_GetObjectItemCaseSensitive(root, "backend");
    if (!arg)
      arg = cJSON_GetObjectItemCaseSensitive(root, "value");
    if (arg && cJSON_IsString(arg) && arg->valuestring) {
      char lower_backend[64];
      size_t i = 0;
      for (; arg->valuestring[i] && i < sizeof(lower_backend) - 1; i++)
        lower_backend[i] = (char)tolower((unsigned char)arg->valuestring[i]);
      lower_backend[i] = '\0';
      cdsp_device_info_t *devs = NULL;
      size_t count = 0;
      bool ok =
          cdsp_get_available_devices(lower_backend, is_capture, &devs, &count);
      cJSON *arr = cJSON_CreateArray();
      if (ok && devs) {
        for (size_t j = 0; j < count; j++) {
          cJSON *tuple = cJSON_CreateArray();
          const char *id = (devs[j].identifier[0] != '\0') ? devs[j].identifier
                                                           : devs[j].name;
          cJSON_AddItemToArray(tuple, cJSON_CreateString(id));
          cJSON_AddItemToArray(tuple, cJSON_CreateString(devs[j].name));
          cJSON_AddItemToArray(arr, tuple);
        }
        free(devs);
      }
      reply_ok(simple, arr, ds);
    } else {
      reply_invalid(
          "missing field `backend` or invalid type for GetAvailableDevices",
          ds);
    }
    break;
  }

  case WS_CMD_GET_CAPTURE_DEVICE_CAPABILITIES:
  case WS_CMD_GET_PLAYBACK_DEVICE_CAPABILITIES: {
    bool is_capture = (cmd_type == WS_CMD_GET_CAPTURE_DEVICE_CAPABILITIES);
    char backend[128] = "", device[256] = "";
    bool ok = false;
    cJSON *b_top = cJSON_GetObjectItemCaseSensitive(root, "backend");
    cJSON *d_top = cJSON_GetObjectItemCaseSensitive(root, "device");
    if (b_top && d_top && cJSON_IsString(b_top) && cJSON_IsString(d_top)) {
      strncpy(backend, b_top->valuestring, sizeof(backend) - 1);
      strncpy(device, d_top->valuestring, sizeof(device) - 1);
      ok = true;
    } else {
      cJSON *arg = cJSON_GetObjectItemCaseSensitive(root, "value");
      if (arg && cJSON_IsArray(arg) && cJSON_GetArraySize(arg) >= 2) {
        cJSON *b_node = cJSON_GetArrayItem(arg, 0);
        cJSON *d_node = cJSON_GetArrayItem(arg, 1);
        if (b_node && d_node && cJSON_IsString(b_node) &&
            cJSON_IsString(d_node)) {
          strncpy(backend, b_node->valuestring, sizeof(backend) - 1);
          strncpy(device, d_node->valuestring, sizeof(device) - 1);
          ok = true;
        }
      }
    }
    if (!ok) {
      reply_invalid(
          "Could not parse backend/device arguments for device capabilities",
          ds);
      break;
    }
    for (size_t i = 0; backend[i]; i++)
      backend[i] = (char)tolower((unsigned char)backend[i]);
    cdsp_device_descriptor_t *desc = NULL;
    cdsp_device_error_t d_err = {0};
    if (cdsp_get_device_capabilities(backend, device, is_capture, &desc,
                                     &d_err) &&
        desc) {
      cJSON *desc_obj = serialize_device_descriptor(desc);
      if (desc_obj)
        reply_ok(simple, desc_obj, ds);
      else
        reply_error(simple, "UnknownError", NULL, ds);
      cdsp_free_device_capabilities(desc);
    } else {
      cJSON *fallback = cJSON_CreateObject();
      cJSON_AddItemToObject(fallback, "name", cJSON_CreateString(device));
      cJSON_AddItemToObject(fallback, "description", cJSON_CreateString(""));
      cJSON_AddItemToObject(fallback, "capability_sets", cJSON_CreateArray());
      reply_error_with_value(simple, get_websocket_device_error_key(d_err.type),
                             d_err.message, fallback, ds);
    }
    break;
  }

  case WS_CMD_GET_CAPTURE_SIGNAL_RMS:
  case WS_CMD_GET_CAPTURE_SIGNAL_PEAK:
  case WS_CMD_GET_PLAYBACK_SIGNAL_RMS:
  case WS_CMD_GET_PLAYBACK_SIGNAL_PEAK: {
    bool is_cap = (cmd_type == WS_CMD_GET_CAPTURE_SIGNAL_RMS ||
                   cmd_type == WS_CMD_GET_CAPTURE_SIGNAL_PEAK);
    bool is_rms = (cmd_type == WS_CMD_GET_CAPTURE_SIGNAL_RMS ||
                   cmd_type == WS_CMD_GET_PLAYBACK_SIGNAL_RMS);
    reply_ok(simple,
             get_signal_vector(server, client_idx, is_cap, is_rms, 0, 0), ds);
    break;
  }

  case WS_CMD_GET_CAPTURE_SIGNAL_RMS_SINCE_LAST:
  case WS_CMD_GET_CAPTURE_SIGNAL_PEAK_SINCE_LAST:
  case WS_CMD_GET_PLAYBACK_SIGNAL_RMS_SINCE_LAST:
  case WS_CMD_GET_PLAYBACK_SIGNAL_PEAK_SINCE_LAST: {
    bool is_cap = (cmd_type == WS_CMD_GET_CAPTURE_SIGNAL_RMS_SINCE_LAST ||
                   cmd_type == WS_CMD_GET_CAPTURE_SIGNAL_PEAK_SINCE_LAST);
    bool is_rms = (cmd_type == WS_CMD_GET_CAPTURE_SIGNAL_RMS_SINCE_LAST ||
                   cmd_type == WS_CMD_GET_PLAYBACK_SIGNAL_RMS_SINCE_LAST);
    reply_ok(simple,
             get_signal_vector(server, client_idx, is_cap, is_rms, 1, 0), ds);
    break;
  }

  case WS_CMD_GET_CAPTURE_SIGNAL_RMS_SINCE:
  case WS_CMD_GET_CAPTURE_SIGNAL_PEAK_SINCE:
  case WS_CMD_GET_PLAYBACK_SIGNAL_RMS_SINCE:
  case WS_CMD_GET_PLAYBACK_SIGNAL_PEAK_SINCE: {
    cJSON *arg = cJSON_GetObjectItemCaseSensitive(root, "value");
    if (arg && cJSON_IsNumber(arg)) {
      float secs = (float)arg->valuedouble;
      bool is_cap = (cmd_type == WS_CMD_GET_CAPTURE_SIGNAL_RMS_SINCE ||
                     cmd_type == WS_CMD_GET_CAPTURE_SIGNAL_PEAK_SINCE);
      bool is_rms = (cmd_type == WS_CMD_GET_CAPTURE_SIGNAL_RMS_SINCE ||
                     cmd_type == WS_CMD_GET_PLAYBACK_SIGNAL_RMS_SINCE);
      reply_ok(simple,
               get_signal_vector(server, client_idx, is_cap, is_rms, 2, secs),
               ds);
    } else {
      reply_invalid("Could not parse seconds: expected number", ds);
    }
    break;
  }

  case WS_CMD_GET_SIGNAL_LEVELS:
    handle_cmd_get_signal_levels_combined(server, client_idx, simple, 0, 0.0f,
                                          ds);
    break;

  case WS_CMD_GET_SIGNAL_LEVELS_SINCE_LAST:
    handle_cmd_get_signal_levels_combined(server, client_idx, simple, 1, 0.0f,
                                          ds);
    break;

  case WS_CMD_GET_SIGNAL_LEVELS_SINCE: {
    cJSON *arg = cJSON_GetObjectItemCaseSensitive(root, "value");
    if (arg && cJSON_IsNumber(arg)) {
      handle_cmd_get_signal_levels_combined(server, client_idx, simple, 2,
                                            (float)arg->valuedouble, ds);
    } else {
      reply_invalid("Could not parse seconds: expected number", ds);
    }
    break;
  }

  case WS_CMD_GET_SIGNAL_PEAKS_SINCE_START: {
    if (server && server->engine) {
      cdsp_vu_levels_t vu_query = {0};
      if (cdsp_get_vu_levels(server->engine, &vu_query)) {
        size_t cap_channels = vu_query.capture_channels;
        size_t pb_channels = vu_query.playback_channels;
        float *cap_pk = cap_channels > 0
                            ? (float *)malloc(cap_channels * sizeof(float))
                            : NULL;
        float *pb_pk = pb_channels > 0
                           ? (float *)malloc(pb_channels * sizeof(float))
                           : NULL;
        cdsp_vu_levels_t vu = {
            .playback_peak = pb_pk,
            .capture_peak = cap_pk,
        };
        if (cdsp_get_vu_levels(server->engine, &vu)) {
          update_global_peaks(&server->capture_global_peaks,
                              &server->capture_global_peaks_count, cap_pk,
                              cap_channels);
          update_global_peaks(&server->playback_global_peaks,
                              &server->playback_global_peaks_count, pb_pk,
                              pb_channels);
        }
        if (cap_pk) {
          free(cap_pk);
          cap_pk = NULL;
        }
        if (pb_pk) {
          free(pb_pk);
          pb_pk = NULL;
        }
      }
    }
    cJSON *peaks_obj = cJSON_CreateObject();
    cJSON_AddItemToObject(
        peaks_obj, "capture",
        safe_create_float_array(server ? server->capture_global_peaks : NULL,
                                server ? (int)server->capture_global_peaks_count
                                       : 0));
    cJSON_AddItemToObject(
        peaks_obj, "playback",
        safe_create_float_array(
            server ? server->playback_global_peaks : NULL,
            server ? (int)server->playback_global_peaks_count : 0));
    reply_ok(simple, peaks_obj, ds);
    break;
  }

  case WS_CMD_RESET_SIGNAL_PEAKS_SINCE_START:
    if (server) {
      for (size_t i = 0; i < server->capture_global_peaks_count; i++)
        server->capture_global_peaks[i] = 0.0f;
      for (size_t i = 0; i < server->playback_global_peaks_count; i++)
        server->playback_global_peaks[i] = 0.0f;
    }
    reply_ok(simple, NULL, ds);
    break;

  case WS_CMD_GET_CHANNEL_LABELS: {
    char **play_labels = NULL, **cap_labels = NULL;
    size_t play_count = 0, cap_count = 0;
    bool ok = server->engine &&
              cdsp_get_channel_labels(server->engine, &play_labels, &play_count,
                                      &cap_labels, &cap_count);
    cJSON *labels_obj = cJSON_CreateObject();
    cJSON_AddItemToObject(labels_obj, "playback",
                          (ok && play_labels)
                              ? string_array_to_json(play_labels, play_count)
                              : cJSON_CreateNull());
    cJSON_AddItemToObject(labels_obj, "capture",
                          (ok && cap_labels)
                              ? string_array_to_json(cap_labels, cap_count)
                              : cJSON_CreateNull());
    reply_ok(simple, labels_obj, ds);
    if (play_labels)
      cdsp_free_channel_labels(play_labels, play_count);
    if (cap_labels)
      cdsp_free_channel_labels(cap_labels, cap_count);
    break;
  }

  case WS_CMD_GET_SIGNAL_RANGE: {
    double range = server->engine ? cdsp_get_signal_range(server->engine) : 0.0;
    reply_ok(simple, safe_create_float_number(range), ds);
    break;
  }

  case WS_CMD_GET_CONFIG_FILE_PATH: {
    char *path =
        server->engine ? cdsp_get_config_file_path(server->engine) : NULL;
    reply_ok(simple, path ? cJSON_CreateString(path) : cJSON_CreateNull(), ds);
    if (path)
      free(path);
    break;
  }

  case WS_CMD_GET_PREVIOUS_CONFIG: {
    char *prev = NULL;
    if (server->engine)
      cdsp_get_previous_config_yaml(server->engine, &prev);
    reply_ok(simple, cJSON_CreateString(prev ? prev : "null\n"), ds);
    if (prev)
      free(prev);
    break;
  }

  case WS_CMD_GET_STATE_FILE_PATH: {
    const char *path =
        server->engine ? cdsp_get_state_file_path(server->engine) : NULL;
    reply_ok(simple, path ? cJSON_CreateString(path) : cJSON_CreateNull(), ds);
    break;
  }

  case WS_CMD_GET_STATE_FILE_UPDATED: {
    bool updated =
        server->engine ? cdsp_get_state_file_updated(server->engine) : true;
    reply_ok(simple, cJSON_CreateBool(updated), ds);
    break;
  }

  case WS_CMD_GET_CONFIG:
  case WS_CMD_GET_CONFIG_JSON: {
    char *config_str = NULL;
    bool ok = false;
    if (server->engine) {
      if (cmd_type == WS_CMD_GET_CONFIG)
        ok = cdsp_get_active_config_yaml(server->engine, &config_str);
      else
        ok = cdsp_get_active_config_json(server->engine, &config_str);
    }
    if (ok && config_str) {
      reply_ok(simple, cJSON_CreateString(config_str), ds);
      free(config_str);
    } else {
      reply_ok(
          simple,
          cJSON_CreateString(cmd_type == WS_CMD_GET_CONFIG ? "null\n" : "null"),
          ds);
    }
    break;
  }

  case WS_CMD_GET_CONFIG_TITLE:
  case WS_CMD_GET_CONFIG_DESCRIPTION: {
    char *text = NULL;
    if (server->engine) {
      text = (cmd_type == WS_CMD_GET_CONFIG_TITLE)
                 ? cdsp_get_config_title(server->engine)
                 : cdsp_get_config_description(server->engine);
    }
    reply_ok(simple, cJSON_CreateString(text ? text : ""), ds);
    if (text) {
      free(text);
      text = NULL;
    }
    break;
  }

  case WS_CMD_RELOAD: {
    if (!server->engine) {
      reply_error(simple, "InvalidRequestError",
                  "Config path not given, cannot reload", ds);
      break;
    }
    char *path = cdsp_get_config_file_path(server->engine);
    if (!path || path[0] == '\0') {
      if (path)
        free(path);
      reply_error(simple, "InvalidRequestError",
                  "Config path not given, cannot reload", ds);
      break;
    }
    free(path);
    cdsp_backend_error_t err = {0};
    if (cdsp_reload_config(server->engine, &err)) {
      reply_ok(simple, NULL, ds);
    } else {
      const char *err_type = get_websocket_error_key(err.type);
      if (err.type == CDSP_BACKEND_ERR_CONFIG_READ)
        err_type = "ConfigValidationError";
      else if (err.type == CDSP_BACKEND_ERR_CONFIG_PARSE)
        err_type = "ConfigReadError";
      reply_error(simple, err_type,
                  err.message[0] ? err.message : "Failed to reload config", ds);
    }
    break;
  }

  case WS_CMD_STOP:
    if (server->engine)
      cdsp_stop(server->engine);
    reply_ok(simple, NULL, ds);
    break;

  case WS_CMD_EXIT:
    websocket_server_request_exit(server);
    if (server->engine)
      cdsp_stop(server->engine);
    reply_ok(simple, NULL, ds);
    break;

  case WS_CMD_SET_CONFIG_FILE_PATH: {
    cJSON *arg = cJSON_GetObjectItemCaseSensitive(root, "value");
    if (arg && cJSON_IsString(arg) && arg->valuestring) {
      const char *path = arg->valuestring;
      char *err_msg = NULL;
      cdsp_config_error_type_t err_type = CDSP_CONFIG_ERR_NONE;
      if (!cdsp_validate_config_file(path, &err_msg, &err_type)) {
        reply_error(simple, "InvalidValueError",
                    err_msg ? err_msg : "Could not read file", ds);
        if (err_msg)
          free(err_msg);
        break;
      }
      if (err_msg)
        free(err_msg);
      if (server->engine)
        cdsp_set_config_file_path(server->engine, path);
      reply_ok(simple, NULL, ds);
    } else {
      reply_invalid(
          "missing field `value` or invalid type for SetConfigFilePath", ds);
    }
    break;
  }

  case WS_CMD_SET_CONFIG:
  case WS_CMD_SET_CONFIG_JSON: {
    bool is_json = (cmd_type == WS_CMD_SET_CONFIG_JSON);
    cJSON *arg = cJSON_GetObjectItemCaseSensitive(root, "value");
    if (!arg || !cJSON_IsString(arg) || !arg->valuestring) {
      char err_msg[128];
      snprintf(err_msg, sizeof(err_msg),
               "missing field `value` or invalid type for %s", simple);
      reply_invalid(err_msg, ds);
      break;
    }
    const char *new_cfg = arg->valuestring;
    cdsp_backend_error_t err = {0};
    bool ok = server->engine &&
              (is_json ? cdsp_set_config_json(server->engine, new_cfg, &err)
                       : cdsp_set_config_yaml(server->engine, new_cfg, &err));
    if (ok) {
      reply_ok(simple, NULL, ds);
    } else {
      const char *err_key = get_websocket_error_key(err.type);
      if (!is_json && (strncmp(err.message, "YAML parse error", 16) == 0 ||
                       err.type == CDSP_BACKEND_ERR_CONFIG_PARSE))
        err_key = "ConfigReadError";
      reply_error(simple, err_key, err.message, ds);
    }
    break;
  }

  case WS_CMD_GET_CONFIG_VALUE: {
    cJSON *arg = cJSON_GetObjectItemCaseSensitive(root, "value");
    if (arg && cJSON_IsString(arg) && arg->valuestring) {
      const char *pointer = arg->valuestring;
      char *val = server->engine
                      ? cdsp_get_config_value(server->engine, pointer)
                      : NULL;
      if (val) {
        cJSON *parsed_val = cJSON_Parse(val);
        reply_ok(simple, parsed_val ? parsed_val : cJSON_CreateString(val), ds);
        free(val);
      } else {
        char msg[256];
        snprintf(msg, sizeof(msg), "The path '%s' does not exit in the config",
                 pointer);
        reply_error_with_value(simple, "InvalidRequestError", msg,
                               cJSON_CreateNull(), ds);
      }
    } else {
      reply_invalid("missing field `value` or invalid type for GetConfigValue",
                    ds);
    }
    break;
  }

  case WS_CMD_SET_CONFIG_VALUE: {
    char pointer[256] = "";
    char *val_json = NULL;
    if (cJSON_IsArray(root) && cJSON_GetArraySize(root) >= 2) {
      cJSON *p_node = cJSON_GetArrayItem(root, 0);
      cJSON *v_node = cJSON_GetArrayItem(root, 1);
      if (p_node && cJSON_IsString(p_node))
        strncpy(pointer, p_node->valuestring, sizeof(pointer) - 1);
      if (v_node)
        val_json = cJSON_PrintUnformatted(v_node);
    } else if (cJSON_IsObject(root)) {
      cJSON *p_node = cJSON_GetObjectItemCaseSensitive(root, "pointer");
      cJSON *v_node = cJSON_GetObjectItemCaseSensitive(root, "value");
      if (p_node && cJSON_IsString(p_node))
        strncpy(pointer, p_node->valuestring, sizeof(pointer) - 1);
      if (v_node)
        val_json = cJSON_PrintUnformatted(v_node);
    }
    if (pointer[0] == '\0' || !val_json) {
      if (val_json)
        free(val_json);
      reply_invalid(
          "Could not parse SetConfigValue command: expected pointer and value",
          ds);
      break;
    }
    char *cur_val =
        server->engine ? cdsp_get_config_value(server->engine, pointer) : NULL;
    if (!cur_val) {
      char msg[300];
      snprintf(msg, sizeof(msg),
               "The active config does not contain the path '%s'", pointer);
      reply_error(simple, "InvalidRequestError", msg, ds);
      free(val_json);
      break;
    }
    free(cur_val);
    cdsp_backend_error_t err = {0};
    bool ok = server->engine &&
              cdsp_set_config_value(server->engine, pointer, val_json, &err);
    if (ok) {
      reply_ok(simple, NULL, ds);
    } else {
      const char *err_key = get_websocket_error_key(err.type);
      if (err.type == CDSP_BACKEND_ERR_CONFIG_PARSE ||
          strncmp(err.message, "YAML parse error", 16) == 0)
        err_key = "ConfigReadError";
      reply_error(simple, err_key,
                  err.message[0]
                      ? err.message
                      : "The active config does not contain the path",
                  ds);
    }
    free(val_json);
    break;
  }

  case WS_CMD_PATCH_CONFIG: {
    cJSON *arg = cJSON_GetObjectItemCaseSensitive(root, "value");
    if (!arg || !cJSON_IsObject(arg)) {
      reply_invalid(
          "Could not parse PatchConfig command: expected object value", ds);
      break;
    }
    char *active_cfg = NULL;
    bool has_cfg = server->engine &&
                   cdsp_get_active_config_json(server->engine, &active_cfg) &&
                   active_cfg != NULL;
    if (active_cfg)
      free(active_cfg);
    if (!has_cfg) {
      reply_error(simple, "InvalidRequestError", "No active config to patch",
                  ds);
      break;
    }
    char *patch_str = cJSON_PrintUnformatted(arg);
    if (patch_str) {
      cdsp_backend_error_t err = {0};
      bool ok =
          server->engine && cdsp_patch_config(server->engine, patch_str, &err);
      if (ok) {
        reply_ok(simple, NULL, ds);
      } else {
        const char *err_key = get_websocket_error_key(err.type);
        if (err.type == CDSP_BACKEND_ERR_CONFIG_PARSE)
          err_key = "ConfigReadError";
        reply_error(simple, err_key,
                    err.message[0] ? err.message : "Invalid patch", ds);
      }
      free(patch_str);
    } else {
      reply_error(simple, "ConfigReadError", "Invalid patch", ds);
    }
    break;
  }

  case WS_CMD_READ_CONFIG:
    handle_config_check(simple, root, false, 1, ds);
    break;
  case WS_CMD_READ_CONFIG_JSON:
    handle_config_check(simple, root, false, 0, ds);
    break;
  case WS_CMD_READ_CONFIG_FILE:
    handle_config_check(simple, root, false, 2, ds);
    break;
  case WS_CMD_VALIDATE_CONFIG:
    handle_config_check(simple, root, true, 1, ds);
    break;
  case WS_CMD_VALIDATE_CONFIG_JSON:
    handle_config_check(simple, root, true, 0, ds);
    break;
  case WS_CMD_VALIDATE_CONFIG_FILE:
    handle_config_check(simple, root, true, 2, ds);
    break;

  case WS_CMD_SUBSCRIBE_STATE: {
    sess->state_subscribed = true;
    cdsp_processing_state_t state = server->engine
                                        ? cdsp_get_state(server->engine)
                                        : CDSP_PROCESSING_STATE_INACTIVE;
    strncpy(sess->last_state, ws_processing_state_to_string(state),
            sizeof(sess->last_state) - 1);
    reply_ok(simple, NULL, ds);
    break;
  }

  case WS_CMD_SUBSCRIBE_VU_LEVELS: {
    float max_rate = 0.0f, attack = 0.0f, release = 0.0f;
    cJSON *arg = cJSON_GetObjectItemCaseSensitive(root, "value");
    if (arg && !cJSON_IsNull(arg)) {
      if (!cJSON_IsObject(arg)) {
        reply_invalid("SubscribeVuLevels requires an object with max_rate, "
                      "attack, and release",
                      ds);
        break;
      }
      cJSON *item_rate = cJSON_GetObjectItemCaseSensitive(arg, "max_rate");
      cJSON *item_attack = cJSON_GetObjectItemCaseSensitive(arg, "attack");
      cJSON *item_release = cJSON_GetObjectItemCaseSensitive(arg, "release");
      if (item_rate) {
        if (!cJSON_IsNumber(item_rate)) {
          reply_invalid("SubscribeVuLevels requires numeric max_rate", ds);
          break;
        }
        max_rate = (float)item_rate->valuedouble;
      }
      if (item_attack) {
        if (!cJSON_IsNumber(item_attack)) {
          reply_invalid("SubscribeVuLevels requires numeric attack", ds);
          break;
        }
        attack = (float)item_attack->valuedouble;
      }
      if (item_release) {
        if (!cJSON_IsNumber(item_release)) {
          reply_invalid("SubscribeVuLevels requires numeric release", ds);
          break;
        }
        release = (float)item_release->valuedouble;
      }
    }
    if (!isfinite(attack) || attack < 0.0f || attack > 60000.0f) {
      reply_error(simple, "InvalidValueError",
                  "attack must be between 0 and 60000 ms", ds);
      break;
    }
    if (!isfinite(release) || release < 0.0f || release > 60000.0f) {
      reply_error(simple, "InvalidValueError",
                  "release must be between 0 and 60000 ms", ds);
      break;
    }
    sess->vu_subscribed = true;
    sess->vu_pending_publish = false;
    sess->last_pb_generation = cdsp_get_chunk_generation(server->engine, false);
    sess->last_cap_generation = cdsp_get_chunk_generation(server->engine, true);
    sess->vu_max_rate = max_rate;
    sess->vu_attack = attack;
    sess->vu_release = release;
    sess->last_vu_push_time = 0;
    reply_ok(simple, NULL, ds);
    break;
  }

  case WS_CMD_SUBSCRIBE_SIGNAL_LEVELS: {
    char side[16] = "";
    cJSON *arg = cJSON_GetObjectItemCaseSensitive(root, "value");
    if (arg && cJSON_IsString(arg) && arg->valuestring)
      strncpy(side, arg->valuestring, sizeof(side) - 1);
    if (strcmp(side, "playback") == 0 || strcmp(side, "capture") == 0 ||
        strcmp(side, "both") == 0) {
      sess->signal_levels_subscribed = true;
      sess->last_sig_pb_generation =
          cdsp_get_chunk_generation(server->engine, false);
      sess->last_sig_cap_generation =
          cdsp_get_chunk_generation(server->engine, true);
      snprintf(sess->signal_levels_side, sizeof(sess->signal_levels_side), "%s",
               side);
      reply_ok(simple, NULL, ds);
    } else {
      reply_invalid("side must be playback, capture, or both", ds);
    }
    break;
  }

  case WS_CMD_SUBSCRIBE_SPECTRUM: {
    bool is_capture = true;
    size_t channel = (size_t)-1;
    float min_freq = 20.0f, max_freq = 20000.0f;
    uint32_t n_bins = 1024;
    if (!parse_spectrum_args(root, simple, &is_capture, &channel, &min_freq,
                             &max_freq, &n_bins, ds))
      break;
    float max_rate = 0.0f;
    bool has_max_rate = false;
    cJSON *arg = cJSON_GetObjectItemCaseSensitive(root, "value");
    cJSON *item_rate = cJSON_GetObjectItemCaseSensitive(arg, "max_rate");
    if (item_rate && !cJSON_IsNull(item_rate)) {
      if (cJSON_IsNumber(item_rate)) {
        max_rate = (float)item_rate->valuedouble;
        has_max_rate = true;
        if (max_rate <= 0.0f) {
          reply_error(simple, "InvalidRequestError", "max_rate must be > 0",
                      ds);
          break;
        }
      } else {
        reply_invalid("max_rate must be a number", ds);
        break;
      }
    }
    if (!server->engine ||
        cdsp_get_state(server->engine) == CDSP_PROCESSING_STATE_INACTIVE) {
      reply_error(simple, "ProcessingNotRunningError", NULL, ds);
      break;
    }
    sess->spectrum_subscribed = true;
    sess->spectrum_is_capture = is_capture;
    sess->spectrum_channel = channel;
    sess->spectrum_min_freq = min_freq;
    sess->spectrum_max_freq = max_freq;
    sess->spectrum_n_bins = n_bins;
    sess->spectrum_max_rate = has_max_rate ? max_rate : 0.0f;
    sess->last_spectrum_push_time = 0;
    reply_ok(simple, NULL, ds);
    break;
  }

  case WS_CMD_STOP_SUBSCRIPTION:
    if (!sess->state_subscribed && !sess->vu_subscribed &&
        !sess->signal_levels_subscribed && !sess->spectrum_subscribed) {
      reply_invalid("No active subscription", ds);
    } else {
      sess->state_subscribed = false;
      sess->vu_subscribed = false;
      sess->signal_levels_subscribed = false;
      sess->spectrum_subscribed = false;
      reply_ok(simple, NULL, ds);
    }
    break;

  case WS_CMD_UNKNOWN:
  default:
    reply_invalid("Unsupported command", ds);
    break;
  }

  pthread_mutex_unlock(&server->sessions_mutex);
  cJSON_Delete(root);
}

// MARK: - Libwebsockets Callback

static int cdsp_lws_callback(struct lws *wsi, enum lws_callback_reasons reason,
                             void *user, void *in, size_t len) {
  lws_pss_t *pss = (lws_pss_t *)user;
  websocket_server_t *server =
      wsi ? (websocket_server_t *)lws_context_user(lws_get_context(wsi)) : NULL;

  switch (reason) {
  case LWS_CALLBACK_ESTABLISHED: {
    if (!server || !pss)
      return -1;
    pthread_mutex_lock(&server->sessions_mutex);
    int slot = -1;
    for (int i = 0; i < 32; i++) {
      if (!server->client_sessions[i].in_use) {
        slot = i;
        break;
      }
    }
    if (slot < 0) {
      pthread_mutex_unlock(&server->sessions_mutex);
      logger_warn(&server_logger,
                  "Max clients (32) reached, rejecting connection");
      return -1;
    }

    client_session_t *session = &server->client_sessions[slot];
    client_session_clear(session);
    session->in_use = true;
    session->is_websocket = true;
    session->wsi = wsi;
    session->pss = pss;

    uint64_t now_ms = get_time_ms();
    session->last_cap_peak_time = now_ms;
    session->last_cap_rms_time = now_ms;
    session->last_pb_peak_time = now_ms;
    session->last_pb_rms_time = now_ms;

    pss->client_idx = slot;
    pss->wsi = wsi;
    pss->server = server;
    pthread_mutex_unlock(&server->sessions_mutex);
    logger_info(&server_logger, "WebSocket client connected on slot %d", slot);
    break;
  }

  case LWS_CALLBACK_RECEIVE: {
    if (!server || !pss || pss->client_idx < 0 || pss->client_idx >= 32)
      return -1;

    pthread_mutex_lock(&server->sessions_mutex);
    client_session_t *session = &server->client_sessions[pss->client_idx];

    if (session->rx_len + len > 16 * 1024 * 1024) {
      pthread_mutex_unlock(&server->sessions_mutex);
      logger_warn(&server_logger,
                  "Client message exceeded 16MB limit, closing (1009)");
      lws_close_reason(wsi, LWS_CLOSE_STATUS_MESSAGE_TOO_LARGE, NULL, 0);
      return -1;
    }

    if (session->rx_len + len + 1 > session->rx_cap) {
      size_t new_cap = session->rx_len + len + 4096;
      if (new_cap < 8192)
        new_cap = 8192;
      char *new_buf = (char *)realloc(session->rx_buf, new_cap);
      if (!new_buf) {
        pthread_mutex_unlock(&server->sessions_mutex);
        return -1;
      }
      session->rx_buf = new_buf;
      session->rx_cap = new_cap;
    }

    memcpy(session->rx_buf + session->rx_len, in, len);
    session->rx_len += len;
    session->rx_buf[session->rx_len] = '\0';

    if (lws_is_final_fragment(wsi) && lws_remaining_packet_payload(wsi) == 0) {
      char *cmd_text = strdup(session->rx_buf);
      session->rx_len = 0;
      pthread_mutex_unlock(&server->sessions_mutex);

      if (cmd_text) {
        logger_debug(&server_logger, "Handling command: %s", cmd_text);
        dyn_string_t ds;
        dyn_string_init(&ds, 4096);
        websocket_server_handle_command(server, pss->client_idx, cmd_text, &ds);
        free(cmd_text);

        if (ds.data && ds.data[0] != '\0') {
          pthread_mutex_lock(&server->sessions_mutex);
          session_enqueue_message(session, ds.data, ds.length);
          pthread_mutex_unlock(&server->sessions_mutex);
          lws_callback_on_writable(wsi);
        }
        dyn_string_free(&ds);
      }
    } else {
      pthread_mutex_unlock(&server->sessions_mutex);
    }
    break;
  }

  case LWS_CALLBACK_SERVER_WRITEABLE: {
    if (!server || !pss || pss->client_idx < 0 || pss->client_idx >= 32)
      return 0;

    pthread_mutex_lock(&server->sessions_mutex);
    client_session_t *session = &server->client_sessions[pss->client_idx];
    if (session->out_queue_head) {
      ws_msg_node_t *node = session->out_queue_head;
      session->out_queue_head = node->next;
      if (!session->out_queue_head)
        session->out_queue_tail = NULL;
      pthread_mutex_unlock(&server->sessions_mutex);

      unsigned char *buf = (unsigned char *)malloc(LWS_PRE + node->len);
      if (buf) {
        memcpy(buf + LWS_PRE, node->data, node->len);
        int written = lws_write(wsi, buf + LWS_PRE, node->len, LWS_WRITE_TEXT);
        free(buf);
        if (written < 0) {
          logger_warn(&server_logger, "Error writing to WebSocket client");
          if (node->data)
            free(node->data);
          free(node);
          return -1;
        }
      }
      if (node->data)
        free(node->data);
      free(node);

      pthread_mutex_lock(&server->sessions_mutex);
      bool more_pending = (session->out_queue_head != NULL);
      pthread_mutex_unlock(&server->sessions_mutex);

      if (more_pending)
        lws_callback_on_writable(wsi);
    } else {
      pthread_mutex_unlock(&server->sessions_mutex);
    }
    break;
  }

  case LWS_CALLBACK_CLOSED: {
    if (server && pss && pss->client_idx >= 0 && pss->client_idx < 32) {
      logger_info(&server_logger, "WebSocket client slot %d disconnected",
                  pss->client_idx);
      pthread_mutex_lock(&server->sessions_mutex);
      client_session_clear(&server->client_sessions[pss->client_idx]);
      pthread_mutex_unlock(&server->sessions_mutex);
      pss->client_idx = -1;
    }
    break;
  }

  default:
    break;
  }
  return 0;
}

static const struct lws_protocols s_protocols[] = {
    {
        .name = "cdsp",
        .callback = cdsp_lws_callback,
        .per_session_data_size = sizeof(lws_pss_t),
        .rx_buffer_size = 65536,
        .id = 0,
        .user = NULL,
        .tx_packet_size = 0,
    },
    {
        .name = "camilladsp",
        .callback = cdsp_lws_callback,
        .per_session_data_size = sizeof(lws_pss_t),
        .rx_buffer_size = 65536,
        .id = 1,
        .user = NULL,
        .tx_packet_size = 0,
    },
    LWS_PROTOCOL_LIST_TERM};

static void periodic_sul_cb(struct lws_sorted_usec_list *sul) {
  websocket_server_t *server = lws_container_of(sul, websocket_server_t, sul);
  if (!server || !atomic_load(&server->running))
    return;
  websocket_server_process_periodic(server);
  if (server->context && atomic_load(&server->running)) {
    lws_sul_schedule(server->context, 0, &server->sul, periodic_sul_cb,
                     20 * LWS_US_PER_MS);
  }
}

static void *server_thread_func(void *arg) {
  websocket_server_t *server = (websocket_server_t *)arg;

  struct lws_context_creation_info info;
  memset(&info, 0, sizeof(info));
  info.port = (int)server->port;
  info.iface = (server->host[0] != '\0') ? server->host : NULL;
  info.protocols = s_protocols;
  info.gid = -1;
  info.uid = -1;
  info.options = LWS_SERVER_OPTION_VALIDATE_UTF8;
  info.user = server;
  info.vhost_name = "cdsp";

  server->context = lws_create_context(&info);

  pthread_mutex_lock(&server->start_mutex);
  if (!server->context) {
    server->start_status = -1;
    pthread_cond_signal(&server->start_cond);
    pthread_mutex_unlock(&server->start_mutex);
    logger_error(&server_logger,
                 "Failed to create libwebsockets context on %s:%u",
                 server->host, server->port);
    return NULL;
  }
  server->start_status = 1;
  pthread_cond_signal(&server->start_cond);
  pthread_mutex_unlock(&server->start_mutex);

  logger_info(&server_logger, "WebSocket server listening on %s:%u",
              server->host, server->port);

  lws_sul_schedule(server->context, 0, &server->sul, periodic_sul_cb,
                   20 * LWS_US_PER_MS);

  while (atomic_load(&server->running)) {
    int ret = lws_service(server->context, 0);
    if (ret < 0 || !atomic_load(&server->running))
      break;
  }

  logger_info(&server_logger, "WebSocket server event loop stopping");
  lws_sul_cancel(&server->sul);
  lws_context_destroy(server->context);
  server->context = NULL;
  return NULL;
}

// MARK: - Server Public Lifecycle & API

websocket_server_t *websocket_server_create(uint16_t port, const char *host) {
  websocket_server_t *server =
      (websocket_server_t *)calloc(1, sizeof(websocket_server_t));
  if (!server)
    return NULL;
  server->port = port;
  strncpy(server->host, (host && host[0]) ? host : "127.0.0.1",
          sizeof(server->host) - 1);
  server->update_interval = 1000;
  atomic_init(&server->running, false);
  atomic_init(&server->exit_requested, false);
  pthread_mutex_init(&server->sessions_mutex, NULL);
  pthread_mutex_init(&server->start_mutex, NULL);
  pthread_cond_init(&server->start_cond, NULL);
  return server;
}

void websocket_server_set_engine(websocket_server_t *server,
                                 dsp_engine_t *engine) {
  if (server)
    server->engine = engine;
}

bool websocket_server_start(websocket_server_t *server) {
  if (!server)
    return false;
  if (atomic_load(&server->running))
    return true;

  atomic_store(&server->running, true);
  server->start_status = 0;

  if (pthread_create(&server->thread, NULL, server_thread_func, server) != 0) {
    atomic_store(&server->running, false);
    return false;
  }

  pthread_mutex_lock(&server->start_mutex);
  while (server->start_status == 0) {
    pthread_cond_wait(&server->start_cond, &server->start_mutex);
  }
  bool started = (server->start_status == 1);
  pthread_mutex_unlock(&server->start_mutex);

  if (!started) {
    pthread_join(server->thread, NULL);
    atomic_store(&server->running, false);
    return false;
  }
  return true;
}

void websocket_server_stop(websocket_server_t *server) {
  if (!server || !atomic_load(&server->running))
    return;
  atomic_store(&server->running, false);
  if (server->context)
    lws_cancel_service(server->context);
  pthread_join(server->thread, NULL);
}

void websocket_server_free(websocket_server_t *server) {
  if (!server)
    return;
  websocket_server_stop(server);

  if (server->capture_global_peaks) {
    free(server->capture_global_peaks);
    server->capture_global_peaks = NULL;
  }
  if (server->playback_global_peaks) {
    free(server->playback_global_peaks);
    server->playback_global_peaks = NULL;
  }

  pthread_mutex_lock(&server->sessions_mutex);
  for (size_t i = 0; i < 32; i++) {
    client_session_clear(&server->client_sessions[i]);
  }
  pthread_mutex_unlock(&server->sessions_mutex);

  pthread_mutex_destroy(&server->sessions_mutex);
  pthread_mutex_destroy(&server->start_mutex);
  pthread_cond_destroy(&server->start_cond);
  free(server);
}

bool websocket_server_get_client_vu_subscribed(const websocket_server_t *server,
                                               int client_idx) {
  if (!server || client_idx < 0 || client_idx >= 32)
    return false;
  pthread_mutex_lock((pthread_mutex_t *)&server->sessions_mutex);
  bool res = server->client_sessions[client_idx].vu_subscribed;
  pthread_mutex_unlock((pthread_mutex_t *)&server->sessions_mutex);
  return res;
}

float websocket_server_get_client_vu_max_rate(const websocket_server_t *server,
                                              int client_idx) {
  if (!server || client_idx < 0 || client_idx >= 32)
    return 0.0f;
  pthread_mutex_lock((pthread_mutex_t *)&server->sessions_mutex);
  float res = server->client_sessions[client_idx].vu_max_rate;
  pthread_mutex_unlock((pthread_mutex_t *)&server->sessions_mutex);
  return res;
}

float websocket_server_get_client_vu_attack(const websocket_server_t *server,
                                            int client_idx) {
  if (!server || client_idx < 0 || client_idx >= 32)
    return 0.0f;
  pthread_mutex_lock((pthread_mutex_t *)&server->sessions_mutex);
  float res = server->client_sessions[client_idx].vu_attack;
  pthread_mutex_unlock((pthread_mutex_t *)&server->sessions_mutex);
  return res;
}

float websocket_server_get_client_vu_release(const websocket_server_t *server,
                                             int client_idx) {
  if (!server || client_idx < 0 || client_idx >= 32)
    return 0.0f;
  pthread_mutex_lock((pthread_mutex_t *)&server->sessions_mutex);
  float res = server->client_sessions[client_idx].vu_release;
  pthread_mutex_unlock((pthread_mutex_t *)&server->sessions_mutex);
  return res;
}

void websocket_server_set_client_vu_subscribed(websocket_server_t *server,
                                               int client_idx,
                                               bool subscribed) {
  if (!server || client_idx < 0 || client_idx >= 32)
    return;
  pthread_mutex_lock(&server->sessions_mutex);
  server->client_sessions[client_idx].vu_subscribed = subscribed;
  pthread_mutex_unlock(&server->sessions_mutex);
}

bool websocket_server_is_exit_requested(const websocket_server_t *server) {
  if (!server)
    return false;
  return atomic_load(&server->exit_requested);
}

void websocket_server_request_exit(websocket_server_t *server) {
  if (!server)
    return;
  atomic_store(&server->exit_requested, true);
}
