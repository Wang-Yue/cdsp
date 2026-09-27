// WebSocket control server using libwebsockets
// Provides runtime control API compatible with the CamillaDSP monitor control
// protocol

#include "server/websocket_server.h"

#include <cjson/cJSON.h>
#include <libwebsockets.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cdsp/processing.h"
#include "cdsp/signal_levels.h"
#include "cdsp/spectrum.h"
#include "logging/app_logger.h"
#include "server/websocket_server_internal.h"
#include "server/ws_rpc_dispatcher.h"
#include "utils/cdsp_time.h"

static const logger_t server_logger = {"dsp.server.websocket"};

static inline cJSON *safe_create_float_array(const float *numbers, int count) {
  if (count <= 0 || !numbers) {
    return cJSON_CreateArray();
  }
  cJSON *array = cJSON_CreateArray();
  if (!array)
    return NULL;
  for (int i = 0; i < count; i++) {
    float val = numbers[i];
    if (isnan(val)) {
      val = -200.0f;
    } else if (isinf(val)) {
      val = (val < 0.0f) ? -200.0f : 0.0f;
    }
    cJSON_AddItemToArray(array, cJSON_CreateNumber((double)val));
  }
  return array;
}

uint64_t get_time_ms(void) { return cdsp_time_now_ns() / 1000000ULL; }

void dyn_string_init(dyn_string_t *ds, size_t initial_cap) {
  ds->data = (char *)calloc(initial_cap, sizeof(char));
  if (ds->data) {
    ds->data[0] = '\0';
    ds->capacity = initial_cap;
  } else {
    ds->capacity = 0;
  }
  ds->length = 0;
}

void dyn_string_free(dyn_string_t *ds) {
  if (ds->data)
    free(ds->data);
  ds->data = NULL;
  ds->capacity = 0;
  ds->length = 0;
}

void dyn_string_printf(dyn_string_t *ds, const char *fmt, ...) {
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

float db_to_amplitude(float db) {
  if (!isfinite(db) || db <= -200.0f)
    return 0.0f;
  return powf(10.0f, db / 20.0f);
}

float amplitude_to_db(float amp) {
  if (amp <= 0.0f || !isfinite(amp))
    return -INFINITY;
  return 20.0f * log10f(amp);
}

void client_session_clear(client_session_t *session) {
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

static float smoothing_alpha(float delta_ms, float time_constant_ms) {
  if (time_constant_ms <= 0.0f)
    return 1.0f;
  float delta_sec = delta_ms / 1000.0f;
  float time_constant_sec = time_constant_ms / 1000.0f;
  return 1.0f - expf(-delta_sec / time_constant_sec);
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

typedef struct {
  struct lws *wsi;
  websocket_server_t *server;
  int client_idx;
} lws_pss_t;

static int cdsp_lws_callback(struct lws *wsi, enum lws_callback_reasons reason,
                             void *user, void *in, size_t len) {
  lws_pss_t *pss = (lws_pss_t *)user;
  websocket_server_t *server = NULL;
  if (wsi) {
    server = (websocket_server_t *)lws_context_user(lws_get_context(wsi));
  }

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
          free(node->data);
          free(node);
          return -1;
        }
      }
      free(node->data);
      free(node);

      pthread_mutex_lock(&server->sessions_mutex);
      bool more_pending = (session->out_queue_head != NULL);
      pthread_mutex_unlock(&server->sessions_mutex);

      if (more_pending) {
        lws_callback_on_writable(wsi);
      }
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

static void websocket_server_process_periodic(websocket_server_t *server) {
  if (!server)
    return;

  uint64_t now = get_time_ms();

  ws_state_update_t status = {0};
  bool has_status = false;
  if (server->engine) {
    status.state = cdsp_get_state(server->engine);
    cdsp_get_stop_reason(server->engine, &status.stop_reason);
    has_status = true;
  }

  const char *state_str = "Inactive";
  if (has_status) {
    state_str = ws_processing_state_to_string(status.state);
  }

  uint64_t cap_gen = cdsp_get_chunk_generation(server->engine, true);
  uint64_t pb_gen = cdsp_get_chunk_generation(server->engine, false);

  float *current_cap_peak = NULL;
  float *current_cap_rms = NULL;
  float *current_pb_peak = NULL;
  float *current_pb_rms = NULL;
  size_t cap_channels = 0;
  size_t pb_channels = 0;

  float *cap_pk_buf = NULL;
  float *cap_rms_buf = NULL;
  float *pb_pk_buf = NULL;
  float *pb_rms_buf = NULL;

  cdsp_vu_levels_t vu_query = {0};
  if (server->engine && cdsp_get_vu_levels(server->engine, &vu_query)) {
    cap_channels = vu_query.capture_channels;
    pb_channels = vu_query.playback_channels;
    if (cap_channels > 0) {
      cap_pk_buf = (float *)malloc(cap_channels * sizeof(float));
      cap_rms_buf = (float *)malloc(cap_channels * sizeof(float));
    }
    if (pb_channels > 0) {
      pb_pk_buf = (float *)malloc(pb_channels * sizeof(float));
      pb_rms_buf = (float *)malloc(pb_channels * sizeof(float));
    }
    cdsp_vu_levels_t vu = {
        .playback_rms = pb_rms_buf,
        .playback_peak = pb_pk_buf,
        .capture_rms = cap_rms_buf,
        .capture_peak = cap_pk_buf,
    };
    if (cdsp_get_vu_levels(server->engine, &vu)) {
      current_cap_peak = vu.capture_peak;
      current_cap_rms = vu.capture_rms;
      current_pb_peak = vu.playback_peak;
      current_pb_rms = vu.playback_rms;
    }

    if (cap_channels > 0 && current_cap_peak && current_cap_rms) {
      if (server->capture_global_peaks_count != cap_channels) {
        float *new_peaks = (float *)realloc(server->capture_global_peaks,
                                            cap_channels * sizeof(float));
        if (new_peaks) {
          server->capture_global_peaks = new_peaks;
          memset(server->capture_global_peaks, 0, cap_channels * sizeof(float));
          server->capture_global_peaks_count = cap_channels;
        }
      }
      size_t limit = cap_channels < server->capture_global_peaks_count
                         ? cap_channels
                         : server->capture_global_peaks_count;
      for (size_t k = 0; k < limit; k++) {
        float lin = db_to_amplitude(current_cap_peak[k]);
        if (server->capture_global_peaks &&
            lin > server->capture_global_peaks[k]) {
          server->capture_global_peaks[k] = lin;
        }
      }
    }

    if (pb_channels > 0 && current_pb_peak && current_pb_rms) {
      if (server->playback_global_peaks_count != pb_channels) {
        float *new_peaks = (float *)realloc(server->playback_global_peaks,
                                            pb_channels * sizeof(float));
        if (new_peaks) {
          server->playback_global_peaks = new_peaks;
          memset(server->playback_global_peaks, 0, pb_channels * sizeof(float));
          server->playback_global_peaks_count = pb_channels;
        }
      }
      size_t limit = pb_channels < server->playback_global_peaks_count
                         ? pb_channels
                         : server->playback_global_peaks_count;
      for (size_t k = 0; k < limit; k++) {
        float lin = db_to_amplitude(current_pb_peak[k]);
        if (server->playback_global_peaks &&
            lin > server->playback_global_peaks[k]) {
          server->playback_global_peaks[k] = lin;
        }
      }
    }
  }

  pthread_mutex_lock(&server->sessions_mutex);
  for (int i = 0; i < 32; i++) {
    client_session_t *session = &server->client_sessions[i];
    if (!session->in_use || !session->wsi)
      continue;

    // State events
    if (session->state_subscribed &&
        strcmp(session->last_state, state_str) != 0) {
      strncpy(session->last_state, state_str, sizeof(session->last_state) - 1);
      cJSON *root = cJSON_CreateObject();
      cJSON_AddStringToObject(root, "reply", "StateEvent");
      cJSON_AddStringToObject(root, "result", "Ok");
      cJSON_AddItemToObject(
          root, "value",
          create_state_event_value(status.state, &status.stop_reason));
      char *json = cJSON_PrintUnformatted(root);
      cJSON_Delete(root);
      if (json) {
        session_enqueue_message(session, json, strlen(json));
        free(json);
        lws_callback_on_writable(session->wsi);
      }
    }

    // VU Levels events
    if (session->vu_subscribed && (pb_channels > 0 || cap_channels > 0)) {
      bool has_gens = (pb_gen > 0 || cap_gen > 0);
      bool new_chunk = has_gens ? (pb_gen != session->last_pb_generation ||
                                   cap_gen != session->last_cap_generation)
                                : true;

      if (new_chunk) {
        session->last_pb_generation = pb_gen;
        session->last_cap_generation = cap_gen;

        float dt = session->last_vu_update_time == 0
                       ? 100.0f
                       : (float)(now - session->last_vu_update_time);
        session->last_vu_update_time = now;
        float attack = smoothing_alpha(dt, session->vu_attack);
        float release = smoothing_alpha(dt, session->vu_release);

        if (pb_channels > 0 && current_pb_peak && current_pb_rms) {
          if (session->vu_pb_channels != pb_channels) {
            float *new_rms = (float *)calloc(pb_channels, sizeof(float));
            float *new_peak = (float *)calloc(pb_channels, sizeof(float));
            if (new_rms && new_peak) {
              size_t copy_count = session->vu_pb_channels < pb_channels
                                      ? session->vu_pb_channels
                                      : pb_channels;
              if (session->vu_pb_rms) {
                memcpy(new_rms, session->vu_pb_rms, copy_count * sizeof(float));
                free(session->vu_pb_rms);
              }
              if (session->vu_pb_peak) {
                memcpy(new_peak, session->vu_pb_peak,
                       copy_count * sizeof(float));
                free(session->vu_pb_peak);
              }
              for (size_t k = copy_count; k < pb_channels; k++) {
                new_rms[k] = current_pb_rms[k];
                new_peak[k] = current_pb_peak[k];
              }
              session->vu_pb_rms = new_rms;
              session->vu_pb_peak = new_peak;
              session->vu_pb_channels = pb_channels;
            } else {
              if (new_rms)
                free(new_rms);
              if (new_peak)
                free(new_peak);
            }
          } else {
            for (size_t k = 0; k < pb_channels; k++) {
              float prev_amp = db_to_amplitude(session->vu_pb_rms[k]);
              float curr_amp = db_to_amplitude(current_pb_rms[k]);
              float diff = curr_amp - prev_amp;
              if (diff > 0.0f)
                prev_amp += attack * diff;
              else
                prev_amp += release * diff;
              session->vu_pb_rms[k] = amplitude_to_db(prev_amp);
            }
            for (size_t k = 0; k < pb_channels; k++) {
              float prev_amp = db_to_amplitude(session->vu_pb_peak[k]);
              float curr_amp = db_to_amplitude(current_pb_peak[k]);
              float diff = curr_amp - prev_amp;
              if (diff > 0.0f)
                prev_amp += 1.0f * diff;
              else
                prev_amp += release * diff;
              session->vu_pb_peak[k] = amplitude_to_db(prev_amp);
            }
          }
        }

        if (cap_channels > 0 && current_cap_peak && current_cap_rms) {
          if (session->vu_cap_channels != cap_channels) {
            float *new_rms = (float *)calloc(cap_channels, sizeof(float));
            float *new_peak = (float *)calloc(cap_channels, sizeof(float));
            if (new_rms && new_peak) {
              size_t copy_count = session->vu_cap_channels < cap_channels
                                      ? session->vu_cap_channels
                                      : cap_channels;
              if (session->vu_cap_rms) {
                memcpy(new_rms, session->vu_cap_rms,
                       copy_count * sizeof(float));
                free(session->vu_cap_rms);
              }
              if (session->vu_cap_peak) {
                memcpy(new_peak, session->vu_cap_peak,
                       copy_count * sizeof(float));
                free(session->vu_cap_peak);
              }
              for (size_t k = copy_count; k < cap_channels; k++) {
                new_rms[k] = current_cap_rms[k];
                new_peak[k] = current_cap_peak[k];
              }
              session->vu_cap_rms = new_rms;
              session->vu_cap_peak = new_peak;
              session->vu_cap_channels = cap_channels;
            } else {
              if (new_rms)
                free(new_rms);
              if (new_peak)
                free(new_peak);
            }
          } else {
            for (size_t k = 0; k < cap_channels; k++) {
              float prev_amp = db_to_amplitude(session->vu_cap_rms[k]);
              float curr_amp = db_to_amplitude(current_cap_rms[k]);
              float diff = curr_amp - prev_amp;
              if (diff > 0.0f)
                prev_amp += attack * diff;
              else
                prev_amp += release * diff;
              session->vu_cap_rms[k] = amplitude_to_db(prev_amp);
            }
            for (size_t k = 0; k < cap_channels; k++) {
              float prev_amp = db_to_amplitude(session->vu_cap_peak[k]);
              float curr_amp = db_to_amplitude(current_cap_peak[k]);
              float diff = curr_amp - prev_amp;
              if (diff > 0.0f)
                prev_amp += 1.0f * diff;
              else
                prev_amp += release * diff;
              session->vu_cap_peak[k] = amplitude_to_db(prev_amp);
            }
          }
        }

        session->vu_pending_publish = true;
      }

      float interval =
          session->vu_max_rate > 0.0f ? 1000.0f / session->vu_max_rate : 0.0f;
      if (session->vu_pending_publish &&
          (now - session->last_vu_push_time >= interval)) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "reply", "VuLevelsEvent");
        cJSON_AddStringToObject(root, "result", "Ok");
        cJSON *val_value = cJSON_CreateObject();
        cJSON_AddItemToObject(root, "value", val_value);
        cJSON_AddItemToObject(
            val_value, "playback_rms",
            safe_create_float_array(session->vu_pb_rms,
                                    (int)session->vu_pb_channels));
        cJSON_AddItemToObject(
            val_value, "playback_peak",
            safe_create_float_array(session->vu_pb_peak,
                                    (int)session->vu_pb_channels));
        cJSON_AddItemToObject(
            val_value, "capture_rms",
            safe_create_float_array(session->vu_cap_rms,
                                    (int)session->vu_cap_channels));
        cJSON_AddItemToObject(
            val_value, "capture_peak",
            safe_create_float_array(session->vu_cap_peak,
                                    (int)session->vu_cap_channels));
        char *json = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (json) {
          session_enqueue_message(session, json, strlen(json));
          free(json);
          lws_callback_on_writable(session->wsi);
        }
        session->vu_pending_publish = false;
        session->last_vu_push_time = now;
      }
    }

    // Signal Levels events
    if (session->signal_levels_subscribed) {
      bool send_pb = strcmp(session->signal_levels_side, "playback") == 0 ||
                     strcmp(session->signal_levels_side, "both") == 0;
      bool send_cap = strcmp(session->signal_levels_side, "capture") == 0 ||
                      strcmp(session->signal_levels_side, "both") == 0;

      bool has_gens = (pb_gen > 0 || cap_gen > 0);
      bool pb_changed =
          has_gens ? (pb_gen != session->last_sig_pb_generation) : true;
      bool cap_changed =
          has_gens ? (cap_gen != session->last_sig_cap_generation) : true;

      if (send_pb && pb_channels > 0 && pb_changed) {
        session->last_sig_pb_generation = pb_gen;
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "reply", "SignalLevelsEvent");
        cJSON_AddStringToObject(root, "result", "Ok");
        cJSON *val_value = cJSON_CreateObject();
        cJSON_AddItemToObject(root, "value", val_value);
        cJSON_AddStringToObject(val_value, "side", "playback");
        cJSON_AddItemToObject(
            val_value, "rms",
            safe_create_float_array(current_pb_rms, (int)pb_channels));
        cJSON_AddItemToObject(
            val_value, "peak",
            safe_create_float_array(current_pb_peak, (int)pb_channels));
        char *json = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (json) {
          session_enqueue_message(session, json, strlen(json));
          free(json);
          lws_callback_on_writable(session->wsi);
        }
      }
      if (send_cap && cap_channels > 0 && cap_changed) {
        session->last_sig_cap_generation = cap_gen;
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "reply", "SignalLevelsEvent");
        cJSON_AddStringToObject(root, "result", "Ok");
        cJSON *val_value = cJSON_CreateObject();
        cJSON_AddItemToObject(root, "value", val_value);
        cJSON_AddStringToObject(val_value, "side", "capture");
        cJSON_AddItemToObject(
            val_value, "rms",
            safe_create_float_array(current_cap_rms, (int)cap_channels));
        cJSON_AddItemToObject(
            val_value, "peak",
            safe_create_float_array(current_cap_peak, (int)cap_channels));
        char *json = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (json) {
          session_enqueue_message(session, json, strlen(json));
          free(json);
          lws_callback_on_writable(session->wsi);
        }
      }
    }

    // Spectrum events
    if (session->spectrum_subscribed) {
      if (!server || !server->engine ||
          cdsp_get_state(server->engine) == CDSP_PROCESSING_STATE_INACTIVE) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "reply", "SpectrumEvent");
        cJSON_AddStringToObject(root, "result", "ProcessingStopped");
        char *json = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (json) {
          session_enqueue_message(session, json, strlen(json));
          free(json);
          lws_callback_on_writable(session->wsi);
        }
        session->spectrum_subscribed = false;
      } else {
        int cap_rate = (server && server->engine)
                           ? cdsp_get_capture_rate(server->engine)
                           : 44100;
        if (cap_rate <= 0)
          cap_rate = 44100;
        float min_freq = session->spectrum_min_freq > 0.0f
                             ? session->spectrum_min_freq
                             : 20.0f;
        size_t min_len = (size_t)ceilf((float)cap_rate / min_freq);
        size_t fft_len = 1024;
        while (fft_len < min_len && fft_len < 65536) {
          fft_len <<= 1;
        }
        float hop_interval_ms = (float)fft_len * 500.0f / (float)cap_rate;
        float rate_interval_ms = session->spectrum_max_rate > 0.0f
                                     ? 1000.0f / session->spectrum_max_rate
                                     : 0.0f;
        float interval = rate_interval_ms > hop_interval_ms ? rate_interval_ms
                                                            : hop_interval_ms;
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
          bool spec_ok =
              (p_freqs && p_mags && server && server->engine) &&
              cdsp_get_spectrum(server->engine, side_val, chan_ptr,
                                session->spectrum_min_freq,
                                session->spectrum_max_freq, n_bins, &spec);
          if (spec_ok) {
            cJSON *root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "reply", "SpectrumEvent");
            cJSON_AddStringToObject(root, "result", "Ok");
            cJSON_AddItemToObject(root, "value", serialize_spectrum(&spec));
            char *json = cJSON_PrintUnformatted(root);
            cJSON_Delete(root);
            if (json) {
              session_enqueue_message(session, json, strlen(json));
              free(json);
              lws_callback_on_writable(session->wsi);
            }
            session->last_spectrum_push_time = now;
          }
          if (p_freqs)
            free(p_freqs);
          if (p_mags)
            free(p_mags);
        }
      }
    }
  }
  pthread_mutex_unlock(&server->sessions_mutex);

  if (cap_pk_buf)
    free(cap_pk_buf);
  if (cap_rms_buf)
    free(cap_rms_buf);
  if (pb_pk_buf)
    free(pb_pk_buf);
  if (pb_rms_buf)
    free(pb_rms_buf);
}

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

websocket_server_t *websocket_server_create(uint16_t port, const char *host) {
  websocket_server_t *server =
      (websocket_server_t *)calloc(1, sizeof(websocket_server_t));
  if (!server)
    return NULL;
  server->port = port;
  if (host && host[0]) {
    strncpy(server->host, host, sizeof(server->host) - 1);
  } else {
    strncpy(server->host, "127.0.0.1", sizeof(server->host) - 1);
  }
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
  if (server) {
    server->engine = engine;
  }
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
  if (!server)
    return;
  if (!atomic_load(&server->running))
    return;

  atomic_store(&server->running, false);
  if (server->context) {
    lws_cancel_service(server->context);
  }
  pthread_join(server->thread, NULL);
}

void websocket_server_free(websocket_server_t *server) {
  if (!server)
    return;
  websocket_server_stop(server);

  if (server->capture_global_peaks)
    free(server->capture_global_peaks);
  if (server->playback_global_peaks)
    free(server->playback_global_peaks);

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
