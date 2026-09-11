// WebSocket control server
// Provides runtime control API compatible with the CamillaDSP monitor control
// protocol

#include "Server/websocket_server.h"

#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Config/cJSON.h"
#include "Logging/app_logger.h"
#include "Public/processing.h"
#include "Public/signal_levels.h"
#include "Public/spectrum.h"
#include "Server/websocket_server_internal.h"
#include "Server/ws_framing.h"
#include "Server/ws_handshake.h"
#include "Server/ws_rpc_dispatcher.h"
#include "Utils/cdsp_time.h"

static const logger_t server_logger = {"dsp.server.websocket"};

static inline cJSON* safe_create_float_array(const float* numbers, int count) {
  if (count <= 0 || !numbers) {
    return cJSON_CreateArray();
  }
  cJSON* array = cJSON_CreateArray();
  if (!array) return NULL;
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

#ifdef _WIN32
#include <ws2tcpip.h>

#define CLOSE_SOCKET(s) closesocket(s)
#define INVALID_SOCKET_VAL INVALID_SOCKET
#define IS_INVALID_SOCKET(s) ((s) == INVALID_SOCKET)
#define IS_SOCKET_ERROR(r) ((r) == SOCKET_ERROR)
#define GET_SOCKET_ERROR() WSAGetLastError()
#define poll_sockets WSAPoll
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#define CLOSE_SOCKET(s) close(s)
#define INVALID_SOCKET_VAL (-1)
#define IS_INVALID_SOCKET(s) ((s) < 0)
#define IS_SOCKET_ERROR(r) ((r) < 0)
#define GET_SOCKET_ERROR() errno
#define poll_sockets poll
#endif

static bool is_valid_utf8(const unsigned char* s, size_t len) {
  size_t i = 0;
  while (i < len) {
    if (s[i] <= 0x7F) {
      i++;
    } else if ((s[i] & 0xE0) == 0xC0) {
      if (i + 1 >= len || (s[i + 1] & 0xC0) != 0x80) return false;
      if (s[i] < 0xC2) return false;
      i += 2;
    } else if ((s[i] & 0xF0) == 0xE0) {
      if (i + 2 >= len || (s[i + 1] & 0xC0) != 0x80 ||
          (s[i + 2] & 0xC0) != 0x80)
        return false;
      if (s[i] == 0xE0 && (s[i + 1] < 0xA0)) return false;
      if (s[i] == 0xED && (s[i + 1] >= 0xA0)) return false;
      i += 3;
    } else if ((s[i] & 0xF8) == 0xF0) {
      if (i + 3 >= len || (s[i + 1] & 0xC0) != 0x80 ||
          (s[i + 2] & 0xC0) != 0x80 || (s[i + 3] & 0xC0) != 0x80)
        return false;
      if (s[i] == 0xF0 && (s[i + 1] < 0x90)) return false;
      if (s[i] == 0xF4 && (s[i + 1] > 0x8F)) return false;
      i += 4;
    } else {
      return false;
    }
  }
  return true;
}

static bool is_valid_close_code(uint16_t code) {
  // RFC 6455 §7.4.1 & §7.4.2:
  // Defined valid status codes on wire:
  // 1000..1003, 1007..1011
  // 3000..4999 (registered / private use)
  // Reserved/forbidden to appear on wire: < 1000, 1004, 1005, 1006, 1015, >=
  // 5000.
  if (code < 1000 || code >= 5000) return false;
  if (code == 1004 || code == 1005 || code == 1006 || code == 1015)
    return false;
  if (code >= 1000 && code <= 1011) return true;
  if (code >= 3000 && code <= 4999) return true;
  return false;
}

void dyn_string_init(dyn_string_t* ds, size_t initial_cap) {
  ds->data = (char*)calloc(initial_cap, sizeof(char));
  if (ds->data) {
    ds->data[0] = '\0';
    ds->capacity = initial_cap;
  } else {
    ds->capacity = 0;
  }
  ds->length = 0;
}

void dyn_string_free(dyn_string_t* ds) {
  if (ds->data) free(ds->data);
  ds->data = NULL;
  ds->capacity = 0;
  ds->length = 0;
}

void dyn_string_printf(dyn_string_t* ds, const char* fmt, ...) {
  if (!ds->data || ds->capacity == 0) return;
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
    if (new_cap <= (size_t)needed) new_cap = (size_t)needed + 1;
    char* new_data = (char*)realloc(ds->data, new_cap);
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
  if (!isfinite(db) || db <= -200.0f) return 0.0f;
  return powf(10.0f, db / 20.0f);
}

float amplitude_to_db(float amp) {
  if (amp <= 0.0f || !isfinite(amp)) return -INFINITY;
  return 20.0f * log10f(amp);
}

void client_session_clear(client_session_t* session) {
  if (!session) return;
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
  if (session->frag_buf) {
    free(session->frag_buf);
    session->frag_buf = NULL;
  }
  session->frag_len = 0;
  session->frag_cap = 0;
  session->frag_opcode = 0;
  session->last_pb_generation = 0;
  session->last_cap_generation = 0;
  session->vu_pending_publish = false;
  session->last_sig_pb_generation = 0;
  session->last_sig_cap_generation = 0;
}

static float smoothing_alpha(float delta_ms, float time_constant_ms) {
  if (time_constant_ms <= 0.0f) return 1.0f;
  float delta_sec = delta_ms / 1000.0f;
  float time_constant_sec = time_constant_ms / 1000.0f;
  return 1.0f - expf(-delta_sec / time_constant_sec);
}

websocket_server_t* websocket_server_create(uint16_t port, const char* host) {
  websocket_server_t* server =
      (websocket_server_t*)calloc(1, sizeof(websocket_server_t));
  if (!server) return NULL;
  server->port = port;
  if (host && host[0]) {
    strncpy(server->host, host, sizeof(server->host) - 1);
  } else {
    strncpy(server->host, "127.0.0.1", sizeof(server->host) - 1);
  }
  server->server_fd = INVALID_SOCKET_VAL;
  server->update_interval = 1000;
  atomic_init(&server->running, false);

  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&server->sessions_mutex, &attr);
  pthread_mutexattr_destroy(&attr);

  return server;
}

void websocket_server_set_engine(websocket_server_t* server,
                                 dsp_engine_t* engine) {
  if (server) {
    server->engine = engine;
  }
}

uint64_t get_time_ms(void) { return cdsp_time_now_ns() / 1000000ULL; }

static void remove_client_session(websocket_server_t* server,
                                  struct pollfd* fds, socket_t* client_fds,
                                  char (*last_state)[64], int* num_clients,
                                  int index) {
  CLOSE_SOCKET(client_fds[index]);
  pthread_mutex_lock(&server->sessions_mutex);
  client_session_clear(&server->client_sessions[index]);

  for (int j = index; j < *num_clients - 1; j++) {
    fds[j + 1] = fds[j + 2];
    client_fds[j] = client_fds[j + 1];
    strcpy(last_state[j], last_state[j + 1]);
    server->client_sessions[j] = server->client_sessions[j + 1];
    memset(&server->client_sessions[j + 1], 0, sizeof(client_session_t));
  }
  server->client_sessions[*num_clients - 1] = (client_session_t){0};
  pthread_mutex_unlock(&server->sessions_mutex);
  (*num_clients)--;
}

static void* server_thread_func(void* arg) {
  websocket_server_t* server = (websocket_server_t*)arg;
  socket_t client_fds[32];
  char last_state[32][64];
  int num_clients = 0;

  while (atomic_load_explicit(&server->running, memory_order_acquire)) {
    struct pollfd fds[33];
    memset(fds, 0, sizeof(fds));
    int polled_clients = num_clients;
    fds[0].fd = server->server_fd;
    fds[0].events = POLLIN;
    for (int i = 0; i < polled_clients; i++) {
      fds[i + 1].fd = client_fds[i];
      fds[i + 1].events = POLLIN;
    }

    bool any_subscribed = false;
    for (int i = 0; i < polled_clients; i++) {
      client_session_t* s = &server->client_sessions[i];
      if (s->state_subscribed || s->vu_subscribed ||
          s->signal_levels_subscribed || s->spectrum_subscribed) {
        any_subscribed = true;
        break;
      }
    }
    int poll_timeout = any_subscribed ? 10 : 50;
    int ret = poll_sockets(fds, polled_clients + 1, poll_timeout);

    pthread_mutex_lock(&server->sessions_mutex);

    typedef struct {
      socket_t fd;
      char* msg;
    } pending_send_t;

    pending_send_t pending[128];
    size_t pending_count = 0;

#define QUEUE_PENDING(target_fd, str)            \
  do {                                           \
    char* m_ = (str);                            \
    if (m_) {                                    \
      if (pending_count < 128) {                 \
        pending[pending_count].fd = (target_fd); \
        pending[pending_count].msg = m_;         \
        pending_count++;                         \
      } else {                                   \
        free(m_);                                \
      }                                          \
    }                                            \
  } while (0)

    uint64_t now = get_time_ms();

    ws_state_update_t status = {0};
    bool has_status = false;
    if (server->engine) {
      status.state = cdsp_get_state(server->engine);
      cdsp_get_stop_reason(server->engine, &status.stop_reason);
      has_status = true;
    }

    const char* state_str = "Inactive";
    if (has_status) {
      state_str = ws_processing_state_to_string(status.state);
    }

    uint64_t cap_gen = cdsp_get_chunk_generation(server->engine, true);
    uint64_t pb_gen = cdsp_get_chunk_generation(server->engine, false);

    float* current_cap_peak = NULL;
    float* current_cap_rms = NULL;
    float* current_pb_peak = NULL;
    float* current_pb_rms = NULL;
    size_t cap_channels = 0;
    size_t pb_channels = 0;

    float* cap_pk_buf = NULL;
    float* cap_rms_buf = NULL;
    float* pb_pk_buf = NULL;
    float* pb_rms_buf = NULL;

    cdsp_vu_levels_t vu_query = {0};
    if (server->engine && cdsp_get_vu_levels(server->engine, &vu_query)) {
      cap_channels = vu_query.capture_channels;
      pb_channels = vu_query.playback_channels;
      if (cap_channels > 0) {
        cap_pk_buf = (float*)malloc(cap_channels * sizeof(float));
        cap_rms_buf = (float*)malloc(cap_channels * sizeof(float));
      }
      if (pb_channels > 0) {
        pb_pk_buf = (float*)malloc(pb_channels * sizeof(float));
        pb_rms_buf = (float*)malloc(pb_channels * sizeof(float));
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
          float* new_peaks = (float*)realloc(server->capture_global_peaks,
                                             cap_channels * sizeof(float));
          if (new_peaks) {
            server->capture_global_peaks = new_peaks;
            for (size_t k = server->capture_global_peaks_count;
                 k < cap_channels; k++) {
              server->capture_global_peaks[k] = 0.0f;
            }
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
          float* new_peaks = (float*)realloc(server->playback_global_peaks,
                                             pb_channels * sizeof(float));
          if (new_peaks) {
            server->playback_global_peaks = new_peaks;
            for (size_t k = server->playback_global_peaks_count;
                 k < pb_channels; k++) {
              server->playback_global_peaks[k] = 0.0f;
            }
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

    for (int i = 0; i < num_clients; i++) {
      client_session_t* session = &server->client_sessions[i];

      if (session->state_subscribed &&
          strcmp(session->last_state, state_str) != 0) {
        strncpy(session->last_state, state_str,
                sizeof(session->last_state) - 1);
        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "reply", "StateEvent");
        cJSON_AddStringToObject(root, "result", "Ok");
        cJSON_AddItemToObject(
            root, "value",
            create_state_event_value(status.state, &status.stop_reason));
        QUEUE_PENDING(client_fds[i], cJSON_PrintUnformatted(root));
        cJSON_Delete(root);
      }

      if (session->vu_subscribed && (pb_channels > 0 || cap_channels > 0)) {
        bool has_gens = (pb_gen > 0 || cap_gen > 0);
        bool new_chunk = has_gens ? (pb_gen != session->last_pb_generation ||
                                     cap_gen != session->last_cap_generation)
                                  : true;

        if (new_chunk) {
          session->last_pb_generation = pb_gen;
          session->last_cap_generation = cap_gen;

          float dt = session->last_vu_push_time == 0
                         ? 100.0f
                         : (float)(now - session->last_vu_push_time);
          float attack = smoothing_alpha(dt, session->vu_attack);
          float release = smoothing_alpha(dt, session->vu_release);

          if (pb_channels > 0 && current_pb_peak && current_pb_rms) {
            if (session->vu_pb_channels != pb_channels) {
              float* new_rms = (float*)calloc(pb_channels, sizeof(float));
              float* new_peak = (float*)calloc(pb_channels, sizeof(float));
              if (new_rms && new_peak) {
                size_t copy_count = session->vu_pb_channels < pb_channels
                                        ? session->vu_pb_channels
                                        : pb_channels;
                if (session->vu_pb_rms) {
                  memcpy(new_rms, session->vu_pb_rms,
                         copy_count * sizeof(float));
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
                if (new_rms) free(new_rms);
                if (new_peak) free(new_peak);
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
              float* new_rms = (float*)calloc(cap_channels, sizeof(float));
              float* new_peak = (float*)calloc(cap_channels, sizeof(float));
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
                if (new_rms) free(new_rms);
                if (new_peak) free(new_peak);
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
          cJSON* root = cJSON_CreateObject();
          cJSON_AddStringToObject(root, "reply", "VuLevelsEvent");
          cJSON_AddStringToObject(root, "result", "Ok");
          cJSON* val_value = cJSON_CreateObject();
          cJSON_AddItemToObject(root, "value", val_value);
          cJSON_AddItemToObject(
              val_value, "playback_rms",
              safe_create_float_array(session->vu_pb_rms, (int)pb_channels));
          cJSON_AddItemToObject(
              val_value, "playback_peak",
              safe_create_float_array(session->vu_pb_peak, (int)pb_channels));
          cJSON_AddItemToObject(
              val_value, "capture_rms",
              safe_create_float_array(session->vu_cap_rms, (int)cap_channels));
          cJSON_AddItemToObject(
              val_value, "capture_peak",
              safe_create_float_array(session->vu_cap_peak, (int)cap_channels));
          QUEUE_PENDING(client_fds[i], cJSON_PrintUnformatted(root));
          cJSON_Delete(root);
          session->vu_pending_publish = false;
          session->last_vu_push_time = now;
        }
      }

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
          cJSON* root = cJSON_CreateObject();
          cJSON_AddStringToObject(root, "reply", "SignalLevelsEvent");
          cJSON_AddStringToObject(root, "result", "Ok");
          cJSON* val_value = cJSON_CreateObject();
          cJSON_AddItemToObject(root, "value", val_value);
          cJSON_AddStringToObject(val_value, "side", "playback");
          cJSON_AddItemToObject(
              val_value, "rms",
              safe_create_float_array(current_pb_rms, (int)pb_channels));
          cJSON_AddItemToObject(
              val_value, "peak",
              safe_create_float_array(current_pb_peak, (int)pb_channels));
          QUEUE_PENDING(client_fds[i], cJSON_PrintUnformatted(root));
          cJSON_Delete(root);
        }
        if (send_cap && cap_channels > 0 && cap_changed) {
          session->last_sig_cap_generation = cap_gen;
          cJSON* root = cJSON_CreateObject();
          cJSON_AddStringToObject(root, "reply", "SignalLevelsEvent");
          cJSON_AddStringToObject(root, "result", "Ok");
          cJSON* val_value = cJSON_CreateObject();
          cJSON_AddItemToObject(root, "value", val_value);
          cJSON_AddStringToObject(val_value, "side", "capture");
          cJSON_AddItemToObject(
              val_value, "rms",
              safe_create_float_array(current_cap_rms, (int)cap_channels));
          cJSON_AddItemToObject(
              val_value, "peak",
              safe_create_float_array(current_cap_peak, (int)cap_channels));
          QUEUE_PENDING(client_fds[i], cJSON_PrintUnformatted(root));
          cJSON_Delete(root);
        }
      }

      if (session->spectrum_subscribed) {
        if (!server || !server->engine ||
            cdsp_get_state(server->engine) == CDSP_PROCESSING_STATE_INACTIVE) {
          cJSON* root = cJSON_CreateObject();
          cJSON_AddStringToObject(root, "reply", "SpectrumEvent");
          cJSON_AddStringToObject(root, "result", "ProcessingStopped");
          QUEUE_PENDING(client_fds[i], cJSON_PrintUnformatted(root));
          cJSON_Delete(root);
          session->spectrum_subscribed = false;
        } else {
          int cap_rate = (server && server->engine)
                             ? cdsp_get_capture_rate(server->engine)
                             : 44100;
          if (cap_rate <= 0) cap_rate = 44100;
          float hop_interval_ms = 1024.0f * 500.0f / (float)cap_rate;
          float rate_interval_ms = session->spectrum_max_rate > 0.0f
                                       ? 1000.0f / session->spectrum_max_rate
                                       : 0.0f;
          float interval = rate_interval_ms > hop_interval_ms ? rate_interval_ms
                                                              : hop_interval_ms;
          if (now - session->last_spectrum_push_time >= interval) {
            size_t n_bins = session->spectrum_n_bins;
            float* p_freqs = (float*)malloc(n_bins * sizeof(float));
            float* p_mags = (float*)malloc(n_bins * sizeof(float));
            cdsp_spectrum_t spec = {
                .frequencies = p_freqs,
                .magnitudes = p_mags,
                .error_message = {0},
            };
            cdsp_spectrum_side_t side_val = session->spectrum_is_capture
                                                ? CDSP_SPECTRUM_SIDE_CAPTURE
                                                : CDSP_SPECTRUM_SIDE_PLAYBACK;
            const size_t* chan_ptr = (session->spectrum_channel == (size_t)-1)
                                         ? NULL
                                         : &session->spectrum_channel;
            bool spec_ok =
                (p_freqs && p_mags && server && server->engine) &&
                cdsp_get_spectrum(server->engine, side_val, chan_ptr,
                                  session->spectrum_min_freq,
                                  session->spectrum_max_freq, n_bins, &spec);
            if (spec_ok) {
              cJSON* root = cJSON_CreateObject();
              cJSON_AddStringToObject(root, "reply", "SpectrumEvent");
              cJSON_AddStringToObject(root, "result", "Ok");
              cJSON_AddItemToObject(root, "value", serialize_spectrum(&spec));
              QUEUE_PENDING(client_fds[i], cJSON_PrintUnformatted(root));
              cJSON_Delete(root);
              session->last_spectrum_push_time = now;
            }
            if (p_freqs) free(p_freqs);
            if (p_mags) free(p_mags);
          }
        }
      }
    }
    if (cap_pk_buf) free(cap_pk_buf);
    if (cap_rms_buf) free(cap_rms_buf);
    if (pb_pk_buf) free(pb_pk_buf);
    if (pb_rms_buf) free(pb_rms_buf);
    pthread_mutex_unlock(&server->sessions_mutex);

    for (size_t k = 0; k < pending_count; k++) {
      ws_send_frame(pending[k].fd, pending[k].msg);
      free(pending[k].msg);
    }

    if (ret > 0) {
      if (fds[0].revents & POLLIN) {
        socket_t cfd = accept(server->server_fd, NULL, NULL);
        if (!IS_INVALID_SOCKET(cfd) && num_clients < 32) {
          logger_info(&server_logger, "Accepted client connection on slot %d",
                      num_clients);
          client_fds[num_clients] = cfd;
          last_state[num_clients][0] = '\0';

          pthread_mutex_lock(&server->sessions_mutex);
          client_session_t* session = &server->client_sessions[num_clients];
          *session = (client_session_t){0};
          uint64_t now_ms = get_time_ms();
          session->last_cap_peak_time = now_ms;
          session->last_cap_rms_time = now_ms;
          session->last_pb_peak_time = now_ms;
          session->last_pb_rms_time = now_ms;
          pthread_mutex_unlock(&server->sessions_mutex);

          num_clients++;
        } else if (!IS_INVALID_SOCKET(cfd)) {
          logger_warn(&server_logger,
                      "Max clients (32) reached, rejecting new connection");
          CLOSE_SOCKET(cfd);
        }
      }
      for (int i = 0; i < polled_clients; i++) {
        if (fds[i + 1].revents & (POLLIN | POLLERR | POLLHUP)) {
          char buf[4096];
          int n = recv(client_fds[i], buf, sizeof(buf) - 1, 0);

          if (n <= 0) {
            logger_info(&server_logger, "Client disconnected on slot %d", i);
            remove_client_session(server, fds, client_fds, last_state,
                                  &num_clients, i);
            polled_clients--;
            i--;
          } else {
            buf[n] = '\0';
            if (ws_handle_handshake(buf, client_fds[i])) {
              pthread_mutex_lock(&server->sessions_mutex);
              server->client_sessions[i].is_websocket = true;
              pthread_mutex_unlock(&server->sessions_mutex);
              continue;
            }

            int offset = 0;
            while (offset < n) {
              size_t payload_len = 0;
              size_t header_len = 0;
              unsigned char* mask = NULL;
              uint8_t opcode = 0;
              bool fin = false;
              if (ws_parse_frame_header_ext(
                      (const unsigned char*)&buf[offset], (size_t)(n - offset),
                      &payload_len, &header_len, &mask, &opcode, &fin)) {
                if (server->client_sessions[i].is_websocket && !mask) {
                  ws_send_close_frame(client_fds[i], 1002);
                  remove_client_session(server, fds, client_fds, last_state,
                                        &num_clients, i);
                  polled_clients--;
                  i--;
                  break;
                }
                // RFC 6455 §5.5: Control frames (opcodes 0x08, 0x09, 0x0A) MUST
                // have a payload length of 125 bytes or less and MUST NOT be
                // fragmented.
                if ((opcode & 0x08) != 0 && (payload_len > 125 || !fin)) {
                  ws_send_close_frame(client_fds[i], 1002);
                  remove_client_session(server, fds, client_fds, last_state,
                                        &num_clients, i);
                  polled_clients--;
                  i--;
                  break;
                }

                if (opcode == 0x08) {
                  uint16_t close_code = 1000;
                  // RFC 6455 §5.5.1: If there is a body, the first two bytes
                  // must be a 2-byte unsigned int code. A close frame cannot
                  // have a payload length of 1.
                  if (payload_len == 1) {
                    ws_send_close_frame(client_fds[i], 1002);
                    remove_client_session(server, fds, client_fds, last_state,
                                          &num_clients, i);
                    polled_clients--;
                    i--;
                    break;
                  }
                  if (payload_len >= 2 &&
                      (size_t)(n - offset) >= header_len + 2) {
                    const unsigned char* p =
                        (const unsigned char*)&buf[offset + header_len];
                    unsigned char b0 = mask ? (p[0] ^ mask[0]) : p[0];
                    unsigned char b1 = mask ? (p[1] ^ mask[1]) : p[1];
                    close_code = ((uint16_t)b0 << 8) | (uint16_t)b1;
                    if (!is_valid_close_code(close_code)) {
                      ws_send_close_frame(client_fds[i], 1002);
                      remove_client_session(server, fds, client_fds, last_state,
                                            &num_clients, i);
                      polled_clients--;
                      i--;
                      break;
                    }
                  }
                  ws_send_close_frame(client_fds[i], close_code);
                  remove_client_session(server, fds, client_fds, last_state,
                                        &num_clients, i);
                  polled_clients--;
                  i--;
                  break;
                }

                if (payload_len > 16 * 1024 * 1024) {
                  ws_send_close_frame(client_fds[i], 1009);
                  remove_client_session(server, fds, client_fds, last_state,
                                        &num_clients, i);
                  polled_clients--;
                  i--;
                  break;
                }

                size_t to_copy = (size_t)(n - offset - header_len);
                if (to_copy > payload_len) to_copy = payload_len;

                char* payload = (char*)malloc(payload_len + 1);
                if (!payload) {
                  ws_send_close_frame(client_fds[i], 1011);
                  remove_client_session(server, fds, client_fds, last_state,
                                        &num_clients, i);
                  polled_clients--;
                  i--;
                  break;
                }

                memcpy(payload, &buf[offset + header_len], to_copy);
                size_t total_read = to_copy;
                bool read_ok = true;
                while (total_read < payload_len) {
                  int r = recv(client_fds[i], payload + total_read,
                               (int)(payload_len - total_read), 0);
                  if (r <= 0) {
                    read_ok = false;
                    break;
                  }
                  total_read += (size_t)r;
                }

                if (!read_ok) {
                  free(payload);
                  remove_client_session(server, fds, client_fds, last_state,
                                        &num_clients, i);
                  polled_clients--;
                  i--;
                  break;
                }

                if (mask) {
                  for (size_t p = 0; p < payload_len; p++) {
                    payload[p] ^= mask[p % 4];
                  }
                }
                payload[payload_len] = '\0';

                if (to_copy < payload_len) {
                  offset = n;
                } else {
                  offset += (int)(header_len + payload_len);
                }

                if (opcode == 0x09) {
                  ws_send_pong_frame(client_fds[i], payload, payload_len);
                  free(payload);
                  continue;
                }
                if (opcode == 0x0A) {
                  free(payload);
                  continue;
                }
                if (opcode == 0x02) {
                  // Upstream ignores binary frames without responding
                  // (WsCommand::None)
                  free(payload);
                  continue;
                }

                pthread_mutex_lock(&server->sessions_mutex);
                client_session_t* session = &server->client_sessions[i];
                if (!fin || opcode == 0x00 || session->frag_len > 0) {
                  if (opcode != 0x00) {
                    session->frag_len = 0;
                    session->frag_opcode = opcode;
                  }
                  if (session->frag_len + payload_len > 64 * 1024 * 1024) {
                    pthread_mutex_unlock(&server->sessions_mutex);
                    free(payload);
                    ws_send_close_frame(client_fds[i], 1009);
                    remove_client_session(server, fds, client_fds, last_state,
                                          &num_clients, i);
                    polled_clients--;
                    i--;
                    break;
                  }

                  size_t needed = session->frag_len + payload_len + 1;
                  if (needed > session->frag_cap) {
                    size_t new_cap = needed < 65536 ? 65536 : needed * 2;
                    char* new_buf = (char*)realloc(session->frag_buf, new_cap);
                    if (!new_buf) {
                      pthread_mutex_unlock(&server->sessions_mutex);
                      free(payload);
                      ws_send_close_frame(client_fds[i], 1011);
                      remove_client_session(server, fds, client_fds, last_state,
                                            &num_clients, i);
                      polled_clients--;
                      i--;
                      break;
                    }
                    session->frag_buf = new_buf;
                    session->frag_cap = new_cap;
                  }
                  memcpy(session->frag_buf + session->frag_len, payload,
                         payload_len);
                  session->frag_len += payload_len;
                  session->frag_buf[session->frag_len] = '\0';
                  free(payload);

                  if (!fin) {
                    pthread_mutex_unlock(&server->sessions_mutex);
                    continue;
                  }

                  char* full_msg = session->frag_buf;
                  uint8_t orig_op = session->frag_opcode;
                  session->frag_buf = NULL;
                  session->frag_cap = 0;
                  session->frag_len = 0;
                  session->frag_opcode = 0;
                  pthread_mutex_unlock(&server->sessions_mutex);

                  if (orig_op == 0x01 &&
                      !is_valid_utf8((const unsigned char*)full_msg,
                                     strlen(full_msg))) {
                    free(full_msg);
                    ws_send_close_frame(client_fds[i], 1007);
                    remove_client_session(server, fds, client_fds, last_state,
                                          &num_clients, i);
                    polled_clients--;
                    i--;
                    break;
                  }

                  logger_debug(&server_logger,
                               "Received fragmented WS message: %s", full_msg);
                  dyn_string_t ds;
                  dyn_string_init(&ds, 4096);
                  websocket_server_handle_command(server, i, full_msg, &ds);
                  if (ds.data && ds.data[0] != '\0') {
                    logger_debug(&server_logger, "Sending WS response: %s",
                                 ds.data);
                    ws_send_frame(client_fds[i], ds.data);
                  }
                  dyn_string_free(&ds);
                  free(full_msg);
                } else {
                  pthread_mutex_unlock(&server->sessions_mutex);
                  if (opcode == 0x01 &&
                      !is_valid_utf8((const unsigned char*)payload,
                                     payload_len)) {
                    free(payload);
                    ws_send_close_frame(client_fds[i], 1007);
                    remove_client_session(server, fds, client_fds, last_state,
                                          &num_clients, i);
                    polled_clients--;
                    i--;
                    break;
                  }
                  logger_debug(&server_logger, "Received WS frame: %s",
                               payload);
                  dyn_string_t ds;
                  dyn_string_init(&ds, 4096);
                  websocket_server_handle_command(server, i, payload, &ds);
                  if (ds.data && ds.data[0] != '\0') {
                    logger_debug(&server_logger, "Sending WS response: %s",
                                 ds.data);
                    ws_send_frame(client_fds[i], ds.data);
                  }
                  dyn_string_free(&ds);
                  free(payload);
                }
              } else {
                logger_debug(&server_logger, "Received raw TCP: %s",
                             &buf[offset]);

                dyn_string_t ds;
                dyn_string_init(&ds, 4096);
                websocket_server_handle_command(server, i, &buf[offset], &ds);
                if (ds.data && ds.data[0] != '\0') {
                  logger_debug(&server_logger, "Sending raw TCP response: %s",
                               ds.data);
                  send(client_fds[i], ds.data, (int)strlen(ds.data), 0);
                }
                dyn_string_free(&ds);
                break;
              }
            }
          }
        }
      }
    }
  }
  for (int i = 0; i < num_clients; i++) {
    CLOSE_SOCKET(client_fds[i]);
  }
  return NULL;
}

bool websocket_server_start(websocket_server_t* server) {
  if (!server || atomic_load_explicit(&server->running, memory_order_acquire))
    return false;

#ifdef _WIN32
  WSADATA wsaData;
  if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
    logger_error(&server_logger, "WSAStartup failed");
    return false;
  }
#endif

  server->server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (IS_INVALID_SOCKET(server->server_fd)) {
    logger_error(&server_logger,
                 "Failed to create server socket: %s (errno=%d)",
                 strerror(errno), errno);
#ifdef _WIN32
    WSACleanup();
#endif
    return false;
  }

  int opt = 1;
#ifdef _WIN32
  setsockopt(server->server_fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt,
             sizeof(opt));
#else
  setsockopt(server->server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(server->port);
  inet_pton(AF_INET, server->host, &addr.sin_addr);

  if (IS_SOCKET_ERROR(
          bind(server->server_fd, (struct sockaddr*)&addr, sizeof(addr)))) {
    logger_error(&server_logger, "Failed to bind WebSocket server on %s:%d",
                 server->host, server->port);
    CLOSE_SOCKET(server->server_fd);
    server->server_fd = INVALID_SOCKET_VAL;
#ifdef _WIN32
    WSACleanup();
#endif
    return false;
  }

  if (IS_SOCKET_ERROR(listen(server->server_fd, 10))) {
    logger_error(
        &server_logger,
        "Failed to listen on WebSocket server socket on %s:%d: %s (errno=%d)",
        server->host, server->port, strerror(errno), errno);
    CLOSE_SOCKET(server->server_fd);
    server->server_fd = INVALID_SOCKET_VAL;
#ifdef _WIN32
    WSACleanup();
#endif
    return false;
  }

  atomic_store_explicit(&server->running, true, memory_order_release);
  if (pthread_create(&server->thread, NULL, server_thread_func, server) != 0) {
    logger_error(&server_logger,
                 "Failed to create WebSocket server thread: %s (errno=%d)",
                 strerror(errno), errno);
    atomic_store_explicit(&server->running, false, memory_order_release);
    CLOSE_SOCKET(server->server_fd);
    server->server_fd = INVALID_SOCKET_VAL;
#ifdef _WIN32
    WSACleanup();
#endif
    return false;
  }

  logger_info(&server_logger, "WebSocket control server listening on %s:%d",
              server->host, server->port);
  return true;
}

void websocket_server_stop(websocket_server_t* server) {
  if (!server || !atomic_load_explicit(&server->running, memory_order_acquire))
    return;
  logger_info(&server_logger, "Stopping WebSocket control server");
  atomic_store_explicit(&server->running, false, memory_order_release);
  pthread_join(server->thread, NULL);
  if (!IS_INVALID_SOCKET(server->server_fd)) {
    CLOSE_SOCKET(server->server_fd);
    server->server_fd = INVALID_SOCKET_VAL;
  }
#ifdef _WIN32
  WSACleanup();
#endif
}

void websocket_server_free(websocket_server_t* server) {
  if (!server) return;
  websocket_server_stop(server);

  if (server->capture_global_peaks) free(server->capture_global_peaks);
  if (server->playback_global_peaks) free(server->playback_global_peaks);

  for (size_t i = 0; i < 32; i++) {
    client_session_clear(&server->client_sessions[i]);
  }

  pthread_mutex_destroy(&server->sessions_mutex);
  free(server);
}

bool websocket_server_get_client_vu_subscribed(const websocket_server_t* server,
                                               int client_idx) {
  if (!server || client_idx < 0 || client_idx >= 32) return false;
  pthread_mutex_lock((pthread_mutex_t*)&server->sessions_mutex);
  bool res = server->client_sessions[client_idx].vu_subscribed;
  pthread_mutex_unlock((pthread_mutex_t*)&server->sessions_mutex);
  return res;
}

float websocket_server_get_client_vu_max_rate(const websocket_server_t* server,
                                              int client_idx) {
  if (!server || client_idx < 0 || client_idx >= 32) return 0.0f;
  pthread_mutex_lock((pthread_mutex_t*)&server->sessions_mutex);
  float res = server->client_sessions[client_idx].vu_max_rate;
  pthread_mutex_unlock((pthread_mutex_t*)&server->sessions_mutex);
  return res;
}

float websocket_server_get_client_vu_attack(const websocket_server_t* server,
                                            int client_idx) {
  if (!server || client_idx < 0 || client_idx >= 32) return 0.0f;
  pthread_mutex_lock((pthread_mutex_t*)&server->sessions_mutex);
  float res = server->client_sessions[client_idx].vu_attack;
  pthread_mutex_unlock((pthread_mutex_t*)&server->sessions_mutex);
  return res;
}

float websocket_server_get_client_vu_release(const websocket_server_t* server,
                                             int client_idx) {
  if (!server || client_idx < 0 || client_idx >= 32) return 0.0f;
  pthread_mutex_lock((pthread_mutex_t*)&server->sessions_mutex);
  float res = server->client_sessions[client_idx].vu_release;
  pthread_mutex_unlock((pthread_mutex_t*)&server->sessions_mutex);
  return res;
}

void websocket_server_set_client_vu_subscribed(websocket_server_t* server,
                                               int client_idx,
                                               bool subscribed) {
  if (!server || client_idx < 0 || client_idx >= 32) return;
  pthread_mutex_lock(&server->sessions_mutex);
  server->client_sessions[client_idx].vu_subscribed = subscribed;
  pthread_mutex_unlock(&server->sessions_mutex);
}
