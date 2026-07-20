// WebSocket control server
// Provides runtime control API compatible with the CamillaDSP monitor control
// protocol

#include "websocket_server.h"

#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "Config/cJSON.h"
#include "Logging/app_logger.h"
#include "Public/general.h"
#include "Public/processing.h"
#include "Public/signal_levels.h"
#include "Public/spectrum.h"
#include "Utils/cdsp_time.h"
#include "websocket_server_internal.h"
#include "ws_framing.h"
#include "ws_handshake.h"
#include "ws_rpc_dispatcher.h"

static const logger_t server_logger = {"dsp.server.websocket"};

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

double db_to_amplitude(double db) {
  if (db <= -1000.0) return 0.0;
  return pow(10.0, db / 20.0);
}

double amplitude_to_db(double amp) {
  if (amp <= 0.0) return -1000.0;
  double db = 20.0 * log10(amp);
  return db < -1000.0 ? -1000.0 : db;
}

void level_history_clear(level_history_t* history) {
  if (!history) return;
  for (size_t i = 0; i < 300; i++) {
    if (history->samples[i].levels) {
      free(history->samples[i].levels);
      history->samples[i].levels = NULL;
    }
  }
  history->head = 0;
  history->size = 0;
  history->channels = 0;
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
}

static void level_history_append(level_history_t* history, const double* levels,
                                 size_t channels, uint64_t now_ms) {
  if (history->channels != channels) {
    level_history_clear(history);
    history->channels = channels;
  }
  if (channels == 0) return;
  level_sample_t* sample = &history->samples[history->head];
  if (sample->levels) {
    free(sample->levels);
  }
  sample->levels = (double*)calloc(channels, sizeof(double));
  if (sample->levels) {
    memcpy(sample->levels, levels, channels * sizeof(double));
    sample->timestamp_ms = now_ms;
    history->head = (history->head + 1) % 300;
    if (history->size < 300) {
      history->size++;
    }
  }
}

void level_history_get_max_since(const level_history_t* history,
                                 uint64_t since_ms, double* out_levels) {
  size_t channels = history->channels;
  for (size_t c = 0; c < channels; c++) {
    out_levels[c] = -1000.0;
  }
  if (history->size == 0 || channels == 0) return;
  size_t idx = (history->head + 300 - 1) % 300;
  for (size_t i = 0; i < history->size; i++) {
    const level_sample_t* sample = &history->samples[idx];
    if (sample->timestamp_ms < since_ms) break;
    for (size_t c = 0; c < channels; c++) {
      if (sample->levels && sample->levels[c] > out_levels[c]) {
        out_levels[c] = sample->levels[c];
      }
    }
    idx = (idx + 300 - 1) % 300;
  }
}

void level_history_get_rms_since(const level_history_t* history,
                                 uint64_t since_ms, double* out_levels) {
  size_t channels = history->channels;
  for (size_t c = 0; c < channels; c++) {
    out_levels[c] = -1000.0;
  }
  if (history->size == 0 || channels == 0) return;
  double* sums = (double*)calloc(channels, sizeof(double));
  size_t count = 0;
  size_t idx = (history->head + 300 - 1) % 300;
  for (size_t i = 0; i < history->size; i++) {
    const level_sample_t* sample = &history->samples[idx];
    if (sample->timestamp_ms < since_ms) break;
    for (size_t c = 0; c < channels; c++) {
      if (sample->levels) {
        double amp = db_to_amplitude(sample->levels[c]);
        sums[c] += amp * amp;
      }
    }
    count++;
    idx = (idx + 300 - 1) % 300;
  }
  if (count > 0) {
    for (size_t c = 0; c < channels; c++) {
      double mean_square = sums[c] / (double)count;
      out_levels[c] = amplitude_to_db(sqrt(mean_square));
    }
  }
  free(sums);
}

static double smoothing_alpha(double delta_ms, double time_constant_ms) {
  if (time_constant_ms <= 0.0) return 1.0;
  double delta_sec = delta_ms / 1000.0;
  double time_constant_sec = time_constant_ms / 1000.0;
  return 1.0 - exp(-delta_sec / time_constant_sec);
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
  server->update_interval = 100;
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

  uint64_t last_broadcast_time_ms = 0;

  while (atomic_load_explicit(&server->running, memory_order_acquire)) {
    struct pollfd fds[33];
    fds[0].fd = server->server_fd;
    fds[0].events = POLLIN;
    for (int i = 0; i < num_clients; i++) {
      fds[i + 1].fd = client_fds[i];
      fds[i + 1].events = POLLIN;
    }
    int ret = poll_sockets(fds, num_clients + 1, 50);

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

    // Periodic broadcast tick
    uint64_t now = get_time_ms();
    if (now - last_broadcast_time_ms >= server->update_interval) {
      last_broadcast_time_ms = now;

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

      double* current_cap_peak = NULL;
      double* current_cap_rms = NULL;
      double* current_pb_peak = NULL;
      double* current_pb_rms = NULL;
      size_t cap_channels = 0;
      size_t pb_channels = 0;

      cdsp_vu_levels_t vu = {0};
      if (server->engine && cdsp_get_vu_levels(server->engine, &vu)) {
        cap_channels = vu.capture_channels;
        pb_channels = vu.playback_channels;
        current_cap_peak = vu.capture_peak;
        current_cap_rms = vu.capture_rms;
        current_pb_peak = vu.playback_peak;
        current_pb_rms = vu.playback_rms;

        if (cap_channels > 0 && current_cap_peak && current_cap_rms) {
          level_history_append(&server->capture_peak_history, current_cap_peak,
                               cap_channels, now);
          level_history_append(&server->capture_rms_history, current_cap_rms,
                               cap_channels, now);

          if (server->capture_global_peaks_count != cap_channels) {
            double* new_peaks = (double*)realloc(server->capture_global_peaks,
                                                 cap_channels * sizeof(double));
            if (new_peaks) {
              server->capture_global_peaks = new_peaks;
              for (size_t k = server->capture_global_peaks_count;
                   k < cap_channels; k++) {
                server->capture_global_peaks[k] = -1000.0;
              }
              server->capture_global_peaks_count = cap_channels;
            }
          }
          size_t limit = cap_channels < server->capture_global_peaks_count
                             ? cap_channels
                             : server->capture_global_peaks_count;
          for (size_t k = 0; k < limit; k++) {
            if (server->capture_global_peaks &&
                current_cap_peak[k] > server->capture_global_peaks[k]) {
              server->capture_global_peaks[k] = current_cap_peak[k];
            }
          }
        }

        if (pb_channels > 0 && current_pb_peak && current_pb_rms) {
          level_history_append(&server->playback_peak_history, current_pb_peak,
                               pb_channels, now);
          level_history_append(&server->playback_rms_history, current_pb_rms,
                               pb_channels, now);

          if (server->playback_global_peaks_count != pb_channels) {
            double* new_peaks = (double*)realloc(server->playback_global_peaks,
                                                 pb_channels * sizeof(double));
            if (new_peaks) {
              server->playback_global_peaks = new_peaks;
              for (size_t k = server->playback_global_peaks_count;
                   k < pb_channels; k++) {
                server->playback_global_peaks[k] = -1000.0;
              }
              server->playback_global_peaks_count = pb_channels;
            }
          }
          size_t limit = pb_channels < server->playback_global_peaks_count
                             ? pb_channels
                             : server->playback_global_peaks_count;
          for (size_t k = 0; k < limit; k++) {
            if (server->playback_global_peaks &&
                current_pb_peak[k] > server->playback_global_peaks[k]) {
              server->playback_global_peaks[k] = current_pb_peak[k];
            }
          }
        }
      }

      for (int i = 0; i < num_clients; i++) {
        client_session_t* session = &server->client_sessions[i];

        if (session->state_subscribed &&
            strcmp(last_state[i], state_str) != 0) {
          strncpy(last_state[i], state_str, sizeof(last_state[i]) - 1);
          cJSON* root = cJSON_CreateObject();
          cJSON* inner = cJSON_CreateObject();
          cJSON_AddItemToObject(root, "StateEvent", inner);
          cJSON_AddStringToObject(inner, "result", "Ok");
          cJSON_AddItemToObject(
              inner, "value",
              create_state_event_value(status.state, &status.stop_reason));
          QUEUE_PENDING(client_fds[i], cJSON_PrintUnformatted(root));
          cJSON_Delete(root);
        }

        if (session->vu_subscribed && pb_channels > 0) {
          double interval =
              session->vu_max_rate > 0.0 ? 1000.0 / session->vu_max_rate : 0.0;
          if (now - session->last_vu_push_time >= interval) {
            double dt = session->last_vu_push_time == 0
                            ? 100.0
                            : (double)(now - session->last_vu_push_time);
            double attack = smoothing_alpha(dt, session->vu_attack);
            double release = smoothing_alpha(dt, session->vu_release);

            if (session->vu_pb_channels != pb_channels) {
              double* new_rms = (double*)calloc(pb_channels, sizeof(double));
              double* new_peak = (double*)calloc(pb_channels, sizeof(double));
              if (new_rms && new_peak) {
                size_t copy_count = session->vu_pb_channels < pb_channels
                                        ? session->vu_pb_channels
                                        : pb_channels;
                if (session->vu_pb_rms) {
                  memcpy(new_rms, session->vu_pb_rms,
                         copy_count * sizeof(double));
                  free(session->vu_pb_rms);
                }
                if (session->vu_pb_peak) {
                  memcpy(new_peak, session->vu_pb_peak,
                         copy_count * sizeof(double));
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
                double prev_amp = db_to_amplitude(session->vu_pb_rms[k]);
                double curr_amp = db_to_amplitude(current_pb_rms[k]);
                double diff = curr_amp - prev_amp;
                if (diff > 0.0)
                  prev_amp += attack * diff;
                else
                  prev_amp += release * diff;
                session->vu_pb_rms[k] = amplitude_to_db(prev_amp);
              }
              for (size_t k = 0; k < pb_channels; k++) {
                double prev_amp = db_to_amplitude(session->vu_pb_peak[k]);
                double curr_amp = db_to_amplitude(current_pb_peak[k]);
                double diff = curr_amp - prev_amp;
                if (diff > 0.0)
                  prev_amp += 1.0 * diff;
                else
                  prev_amp += release * diff;
                session->vu_pb_peak[k] = amplitude_to_db(prev_amp);
              }
            }

            if (cap_channels > 0) {
              if (session->vu_cap_channels != cap_channels) {
                double* new_rms = (double*)calloc(cap_channels, sizeof(double));
                double* new_peak =
                    (double*)calloc(cap_channels, sizeof(double));
                if (new_rms && new_peak) {
                  size_t copy_count = session->vu_cap_channels < cap_channels
                                          ? session->vu_cap_channels
                                          : cap_channels;
                  if (session->vu_cap_rms) {
                    memcpy(new_rms, session->vu_cap_rms,
                           copy_count * sizeof(double));
                    free(session->vu_cap_rms);
                  }
                  if (session->vu_cap_peak) {
                    memcpy(new_peak, session->vu_cap_peak,
                           copy_count * sizeof(double));
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
                  double prev_amp = db_to_amplitude(session->vu_cap_rms[k]);
                  double curr_amp = db_to_amplitude(current_cap_rms[k]);
                  double diff = curr_amp - prev_amp;
                  if (diff > 0.0)
                    prev_amp += attack * diff;
                  else
                    prev_amp += release * diff;
                  session->vu_cap_rms[k] = amplitude_to_db(prev_amp);
                }
                for (size_t k = 0; k < cap_channels; k++) {
                  double prev_amp = db_to_amplitude(session->vu_cap_peak[k]);
                  double curr_amp = db_to_amplitude(current_cap_peak[k]);
                  double diff = curr_amp - prev_amp;
                  if (diff > 0.0)
                    prev_amp += 1.0 * diff;
                  else
                    prev_amp += release * diff;
                  session->vu_cap_peak[k] = amplitude_to_db(prev_amp);
                }
              }
            }

            cJSON* root = cJSON_CreateObject();
            cJSON* val = cJSON_CreateObject();
            cJSON_AddItemToObject(root, "VuLevelsEvent", val);
            cJSON_AddStringToObject(val, "result", "Ok");
            cJSON* val_value = cJSON_CreateObject();
            cJSON_AddItemToObject(val, "value", val_value);
            cJSON_AddItemToObject(
                val_value, "playback_rms",
                cJSON_CreateDoubleArray(session->vu_pb_rms, (int)pb_channels));
            cJSON_AddItemToObject(
                val_value, "playback_peak",
                cJSON_CreateDoubleArray(session->vu_pb_peak, (int)pb_channels));
            cJSON_AddItemToObject(val_value, "capture_rms",
                                  cJSON_CreateDoubleArray(session->vu_cap_rms,
                                                          (int)cap_channels));
            cJSON_AddItemToObject(val_value, "capture_peak",
                                  cJSON_CreateDoubleArray(session->vu_cap_peak,
                                                          (int)cap_channels));
            QUEUE_PENDING(client_fds[i], cJSON_PrintUnformatted(root));
            cJSON_Delete(root);
            session->last_vu_push_time = now;
          }
        }

        if (session->signal_levels_subscribed) {
          bool send_pb = strcmp(session->signal_levels_side, "playback") == 0 ||
                         strcmp(session->signal_levels_side, "both") == 0;
          bool send_cap = strcmp(session->signal_levels_side, "capture") == 0 ||
                          strcmp(session->signal_levels_side, "both") == 0;

          if (send_pb && pb_channels > 0) {
            cJSON* root = cJSON_CreateObject();
            cJSON* val = cJSON_CreateObject();
            cJSON_AddItemToObject(root, "SignalLevelsEvent", val);
            cJSON_AddStringToObject(val, "result", "Ok");
            cJSON* val_value = cJSON_CreateObject();
            cJSON_AddItemToObject(val, "value", val_value);
            cJSON_AddStringToObject(val_value, "side", "playback");
            cJSON_AddItemToObject(
                val_value, "rms",
                cJSON_CreateDoubleArray(current_pb_rms, (int)pb_channels));
            cJSON_AddItemToObject(
                val_value, "peak",
                cJSON_CreateDoubleArray(current_pb_peak, (int)pb_channels));
            QUEUE_PENDING(client_fds[i], cJSON_PrintUnformatted(root));
            cJSON_Delete(root);
          }
          if (send_cap && cap_channels > 0) {
            cJSON* root = cJSON_CreateObject();
            cJSON* val = cJSON_CreateObject();
            cJSON_AddItemToObject(root, "SignalLevelsEvent", val);
            cJSON_AddStringToObject(val, "result", "Ok");
            cJSON* val_value = cJSON_CreateObject();
            cJSON_AddItemToObject(val, "value", val_value);
            cJSON_AddStringToObject(val_value, "side", "capture");
            cJSON_AddItemToObject(
                val_value, "rms",
                cJSON_CreateDoubleArray(current_cap_rms, (int)cap_channels));
            cJSON_AddItemToObject(
                val_value, "peak",
                cJSON_CreateDoubleArray(current_cap_peak, (int)cap_channels));
            QUEUE_PENDING(client_fds[i], cJSON_PrintUnformatted(root));
            cJSON_Delete(root);
          }
        }

        if (session->spectrum_subscribed) {
          double interval = session->spectrum_max_rate > 0.0
                                ? 1000.0 / session->spectrum_max_rate
                                : 0.0;
          if (now - session->last_spectrum_push_time >= interval) {
            cdsp_spectrum_t spec = {0};
            cdsp_spectrum_side_t side_val =
                session->spectrum_is_capture ? CDSP_SPECTRUM_SIDE_CAPTURE
                                             : CDSP_SPECTRUM_SIDE_PLAYBACK;
            const uint32_t* chan_ptr = (session->spectrum_channel == (uint32_t)-1)
                                           ? NULL
                                           : &session->spectrum_channel;
            bool spec_ok =
                server && server->engine &&
                cdsp_get_spectrum(server->engine, side_val, chan_ptr,
                                  session->spectrum_min_freq,
                                  session->spectrum_max_freq,
                                  session->spectrum_n_bins, &spec);
            if (spec_ok) {
              cJSON* root = cJSON_CreateObject();
              cJSON* val = cJSON_CreateObject();
              cJSON_AddItemToObject(root, "SpectrumEvent", val);
              cJSON_AddStringToObject(val, "result", "Ok");
              cJSON_AddItemToObject(val, "value", serialize_spectrum(&spec));
              QUEUE_PENDING(client_fds[i], cJSON_PrintUnformatted(root));
              cJSON_Delete(root);
              cdsp_free_spectrum(&spec);
              session->last_spectrum_push_time = now;
            }
          }
        }
      }

      cdsp_free_vu_levels(&vu);
    }
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
      for (int i = 0; i < num_clients; i++) {
        if (fds[i + 1].revents & (POLLIN | POLLERR | POLLHUP)) {
          char buf[4096];
          int n = recv(client_fds[i], buf, sizeof(buf) - 1, 0);

          if (n <= 0) {
            logger_info(&server_logger, "Client disconnected on slot %d", i);
            remove_client_session(server, fds, client_fds, last_state,
                                  &num_clients, i);
            i--;
          } else {
            buf[n] = '\0';
            if (ws_handle_handshake(buf, client_fds[i])) {
              continue;
            }

            int offset = 0;
            while (offset < n) {
              size_t payload_len = 0;
              size_t header_len = 0;
              unsigned char* mask = NULL;
              uint8_t opcode = 0;
              if (ws_parse_frame_header((const unsigned char*)&buf[offset],
                                        (size_t)(n - offset), &payload_len,
                                        &header_len, &mask, &opcode)) {
                if (opcode == 0x08) {
                  remove_client_session(server, fds, client_fds, last_state,
                                        &num_clients, i);
                  i--;
                  break;
                }

                if (payload_len > 128 * 1024) {
                  remove_client_session(server, fds, client_fds, last_state,
                                        &num_clients, i);
                  i--;
                  break;
                }

                size_t to_copy = (size_t)(n - offset - header_len);
                if (to_copy > payload_len) to_copy = payload_len;

                char* payload = (char*)malloc(payload_len + 1);
                if (!payload) {
                  remove_client_session(server, fds, client_fds, last_state,
                                        &num_clients, i);
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
                  i--;
                  break;
                }

                if (mask) {
                  for (size_t p = 0; p < payload_len; p++) {
                    payload[p] ^= mask[p % 4];
                  }
                }
                payload[payload_len] = '\0';

                logger_debug(&server_logger, "Received WS frame: %s", payload);

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

                offset += header_len + payload_len;
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

  level_history_clear(&server->capture_peak_history);
  level_history_clear(&server->capture_rms_history);
  level_history_clear(&server->playback_peak_history);
  level_history_clear(&server->playback_rms_history);

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

double websocket_server_get_client_vu_max_rate(const websocket_server_t* server,
                                               int client_idx) {
  if (!server || client_idx < 0 || client_idx >= 32) return 0.0;
  pthread_mutex_lock((pthread_mutex_t*)&server->sessions_mutex);
  double res = server->client_sessions[client_idx].vu_max_rate;
  pthread_mutex_unlock((pthread_mutex_t*)&server->sessions_mutex);
  return res;
}

double websocket_server_get_client_vu_attack(const websocket_server_t* server,
                                             int client_idx) {
  if (!server || client_idx < 0 || client_idx >= 32) return 0.0;
  pthread_mutex_lock((pthread_mutex_t*)&server->sessions_mutex);
  double res = server->client_sessions[client_idx].vu_attack;
  pthread_mutex_unlock((pthread_mutex_t*)&server->sessions_mutex);
  return res;
}

double websocket_server_get_client_vu_release(const websocket_server_t* server,
                                              int client_idx) {
  if (!server || client_idx < 0 || client_idx >= 32) return 0.0;
  pthread_mutex_lock((pthread_mutex_t*)&server->sessions_mutex);
  double res = server->client_sessions[client_idx].vu_release;
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
