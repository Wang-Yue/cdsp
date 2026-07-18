// WebSocket control server using libwebsockets
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
#include "ws_rpc_dispatcher.h"

static const logger_t server_logger = {"dsp.server.websocket"};

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

uint64_t get_time_ms(void) { return cdsp_time_now_ns() / 1000000; }

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
      if (sample->levels && sums) {
        double val = db_to_amplitude(sample->levels[c]);
        sums[c] += val * val;
      }
    }
    count++;
    idx = (idx + 300 - 1) % 300;
  }
  if (count > 0 && sums) {
    for (size_t c = 0; c < channels; c++) {
      out_levels[c] = amplitude_to_db(sqrt(sums[c] / (double)count));
    }
  }
  if (sums) free(sums);
}

static double smoothing_alpha(double dt_ms, double tc_ms) {
  if (tc_ms <= 0.0) return 1.0;
  return 1.0 - exp(-dt_ms / tc_ms);
}

static void queue_message(client_session_t* session, const char* msg) {
  if (!session || !msg) return;
  pthread_mutex_lock(&session->write_mutex);
  if (session->pending_write) {
    free(session->pending_write);
  }
  session->pending_write = strdup(msg);
  session->pending_write_len = strlen(msg);
  pthread_mutex_unlock(&session->write_mutex);
  lws_callback_on_writable(session->wsi);
}

// libwebsockets callback protocol handler
static int callback_camilladsp(struct lws* wsi,
                               enum lws_callback_reasons reason, void* user,
                               void* in, size_t len) {
  struct lws_context* ctx = lws_get_context(wsi);
  websocket_server_t* server = (websocket_server_t*)lws_context_user(ctx);
  client_session_t* session = (client_session_t*)user;

  switch (reason) {
    case LWS_CALLBACK_ESTABLISHED: {
      pthread_mutex_init(&session->write_mutex, NULL);
      session->wsi = wsi;
      session->pending_write = NULL;
      session->pending_write_len = 0;

      pthread_mutex_lock(&server->sessions_mutex);
      for (int i = 0; i < 32; i++) {
        if (!server->client_sessions[i]) {
          server->client_sessions[i] = session;
          break;
        }
      }
      pthread_mutex_unlock(&server->sessions_mutex);
      break;
    }

    case LWS_CALLBACK_CLOSED: {
      pthread_mutex_lock(&server->sessions_mutex);
      for (int i = 0; i < 32; i++) {
        if (server->client_sessions[i] == session) {
          server->client_sessions[i] = NULL;
          break;
        }
      }
      pthread_mutex_unlock(&server->sessions_mutex);

      pthread_mutex_lock(&session->write_mutex);
      if (session->pending_write) {
        free(session->pending_write);
        session->pending_write = NULL;
      }
      pthread_mutex_unlock(&session->write_mutex);
      pthread_mutex_destroy(&session->write_mutex);

      client_session_clear(session);
      break;
    }

    case LWS_CALLBACK_RECEIVE: {
      char* in_str = malloc(len + 1);
      if (in_str) {
        memcpy(in_str, in, len);
        in_str[len] = '\0';

        dyn_string_t ds;
        dyn_string_init(&ds, 1024);

        int client_idx = -1;
        pthread_mutex_lock(&server->sessions_mutex);
        for (int i = 0; i < 32; i++) {
          if (server->client_sessions[i] == session) {
            client_idx = i;
            break;
          }
        }
        pthread_mutex_unlock(&server->sessions_mutex);

        websocket_server_handle_command(server, client_idx, in_str, &ds);
        free(in_str);

        if (ds.data && ds.length > 0) {
          queue_message(session, ds.data);
        }
        dyn_string_free(&ds);
      }
      break;
    }

    case LWS_CALLBACK_SERVER_WRITEABLE: {
      pthread_mutex_lock(&session->write_mutex);
      if (session->pending_write) {
        unsigned char* buf = malloc(LWS_PRE + session->pending_write_len + 1);
        if (buf) {
          memcpy(buf + LWS_PRE, session->pending_write,
                 session->pending_write_len);
          lws_write(wsi, buf + LWS_PRE, session->pending_write_len,
                    LWS_WRITE_TEXT);
          free(buf);
        }
        free(session->pending_write);
        session->pending_write = NULL;
        session->pending_write_len = 0;
      }
      pthread_mutex_unlock(&session->write_mutex);
      break;
    }

    default:
      break;
  }

  return 0;
}

static struct lws_protocols protocols[] = {
    {"camilladsp-protocol", callback_camilladsp, sizeof(client_session_t),
     1024 * 1024, 0, NULL, 0},
    LWS_PROTOCOL_LIST_TERM};

static void* lws_service_thread(void* arg) {
  websocket_server_t* server = (websocket_server_t*)arg;
  while (atomic_load_explicit(&server->running, memory_order_acquire)) {
    lws_service(server->context, 50);
  }
  return NULL;
}

static void* metrics_thread_func(void* arg) {
  websocket_server_t* server = (websocket_server_t*)arg;
  char last_state[32][64];
  for (int i = 0; i < 32; i++) {
    last_state[i][0] = '\0';
  }
  uint64_t last_broadcast_time_ms = 0;

  while (atomic_load_explicit(&server->running, memory_order_acquire)) {
    cdsp_sleep_ms(50);
    pthread_mutex_lock(&server->sessions_mutex);

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

      for (int i = 0; i < 32; i++) {
        client_session_t* session = server->client_sessions[i];
        if (!session) continue;

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
          char* p_str = cJSON_PrintUnformatted(root);
          queue_message(session, p_str);
          free(p_str);
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

            session->last_vu_push_time = now;

            cJSON* root = cJSON_CreateObject();
            cJSON* inner = cJSON_CreateObject();
            cJSON_AddItemToObject(root, "VuEvent", inner);
            cJSON_AddStringToObject(inner, "result", "Ok");

            cJSON* val = cJSON_CreateObject();
            cJSON* rms_arr = cJSON_CreateDoubleArray(session->vu_pb_rms,
                                                     session->vu_pb_channels);
            cJSON* peak_arr = cJSON_CreateDoubleArray(session->vu_pb_peak,
                                                      session->vu_pb_channels);
            cJSON_AddItemToObject(val, "playback_rms", rms_arr);
            cJSON_AddItemToObject(val, "playback_peak", peak_arr);

            if (cap_channels > 0 && session->vu_cap_rms &&
                session->vu_cap_peak) {
              cJSON* cap_rms_arr = cJSON_CreateDoubleArray(
                  session->vu_cap_rms, session->vu_cap_channels);
              cJSON* cap_peak_arr = cJSON_CreateDoubleArray(
                  session->vu_cap_peak, session->vu_cap_channels);
              cJSON_AddItemToObject(val, "capture_rms", cap_rms_arr);
              cJSON_AddItemToObject(val, "capture_peak", cap_peak_arr);
            }

            cJSON_AddItemToObject(inner, "value", val);
            char* p_str = cJSON_PrintUnformatted(root);
            queue_message(session, p_str);
            free(p_str);
            cJSON_Delete(root);
          }
        }

        if (session->signal_levels_subscribed) {
          cJSON* root = cJSON_CreateObject();
          cJSON* inner = cJSON_CreateObject();
          cJSON_AddItemToObject(root, "SignalLevelsEvent", inner);
          cJSON_AddStringToObject(inner, "result", "Ok");

          cJSON* val = cJSON_CreateObject();
          cJSON* rms_arr = NULL;
          cJSON* peak_arr = NULL;

          if (strcmp(session->signal_levels_side, "capture") == 0) {
            rms_arr = cJSON_CreateDoubleArray(current_cap_rms, cap_channels);
            peak_arr = cJSON_CreateDoubleArray(current_cap_peak, cap_channels);
          } else {
            rms_arr = cJSON_CreateDoubleArray(current_pb_rms, pb_channels);
            peak_arr = cJSON_CreateDoubleArray(current_pb_peak, pb_channels);
          }
          cJSON_AddItemToObject(val, "rms", rms_arr);
          cJSON_AddItemToObject(val, "peak", peak_arr);
          cJSON_AddItemToObject(inner, "value", val);

          char* p_str = cJSON_PrintUnformatted(root);
          queue_message(session, p_str);
          free(p_str);
          cJSON_Delete(root);
        }

        if (session->spectrum_subscribed) {
          double interval = session->spectrum_max_rate > 0.0
                                ? 1000.0 / session->spectrum_max_rate
                                : 0.0;
          if (now - session->last_spectrum_push_time >= interval) {
            cJSON* root = cJSON_CreateObject();
            cJSON* inner = cJSON_CreateObject();
            cJSON_AddItemToObject(root, "SpectrumEvent", inner);
            cJSON_AddStringToObject(inner, "result", "Ok");

            cJSON* val = cJSON_CreateObject();
            cdsp_spectrum_t spec = {0};
            if (cdsp_get_spectrum(server->engine, session->spectrum_is_capture,
                                  session->spectrum_channel,
                                  session->spectrum_min_freq,
                                  session->spectrum_max_freq,
                                  session->spectrum_n_bins, &spec)) {
              cJSON* bins_arr =
                  cJSON_CreateDoubleArray(spec.magnitudes, (int)spec.count);
              cJSON_AddItemToObject(val, "bins", bins_arr);
              cJSON_AddItemToObject(inner, "value", val);
              cdsp_free_spectrum(&spec);
            }

            char* p_str = cJSON_PrintUnformatted(root);
            queue_message(session, p_str);
            free(p_str);
            cJSON_Delete(root);

            session->last_spectrum_push_time = now;
          }
        }
      }
    }
    pthread_mutex_unlock(&server->sessions_mutex);
  }
  return NULL;
}

websocket_server_t* websocket_server_create(uint16_t port, const char* host) {
  websocket_server_t* server =
      (websocket_server_t*)calloc(1, sizeof(websocket_server_t));
  if (!server) return NULL;

  server->port = port;
  if (host) {
    strncpy(server->host, host, sizeof(server->host) - 1);
  } else {
    strcpy(server->host, "127.0.0.1");
  }
  server->update_interval = 100;
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&server->sessions_mutex, &attr);
  pthread_mutexattr_destroy(&attr);

  return server;
}

void websocket_server_set_engine(websocket_server_t* server,
                                 dsp_engine_t* engine) {
  if (server) server->engine = engine;
}

bool websocket_server_start(websocket_server_t* server) {
  if (!server || atomic_load_explicit(&server->running, memory_order_acquire))
    return false;

  struct lws_context_creation_info info;
  memset(&info, 0, sizeof(info));
  info.port = server->port;
  info.iface = server->host[0] != '\0' ? server->host : NULL;
  info.protocols = protocols;
  info.user = server;
  info.gid = -1;
  info.uid = -1;
  info.options = LWS_SERVER_OPTION_DISABLE_IPV6;

  lws_set_log_level(LLL_ERR | LLL_WARN, NULL);

  server->context = lws_create_context(&info);
  if (!server->context) {
    logger_error(&server_logger, "Failed to create libwebsockets context");
    return false;
  }

  atomic_store_explicit(&server->running, true, memory_order_release);

  if (pthread_create(&server->thread, NULL, lws_service_thread, server) != 0) {
    logger_error(&server_logger, "Failed to create LWS service thread");
    lws_context_destroy(server->context);
    server->context = NULL;
    atomic_store_explicit(&server->running, false, memory_order_release);
    return false;
  }

  if (pthread_create(&server->metrics_thread, NULL, metrics_thread_func,
                     server) != 0) {
    logger_error(&server_logger, "Failed to create metrics broadcast thread");
    atomic_store_explicit(&server->running, false, memory_order_release);
    pthread_join(server->thread, NULL);
    lws_context_destroy(server->context);
    server->context = NULL;
    return false;
  }

  logger_info(&server_logger,
              "WebSocket control server listening on %s:%d using libwebsockets",
              server->host, server->port);
  return true;
}

void websocket_server_stop(websocket_server_t* server) {
  if (!server || !atomic_load_explicit(&server->running, memory_order_acquire))
    return;
  logger_info(&server_logger, "Stopping WebSocket control server");
  atomic_store_explicit(&server->running, false, memory_order_release);

  pthread_join(server->thread, NULL);
  pthread_join(server->metrics_thread, NULL);

  if (server->context) {
    lws_context_destroy(server->context);
    server->context = NULL;
  }
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

  pthread_mutex_lock(&server->sessions_mutex);
  for (size_t i = 0; i < 32; i++) {
    if (server->client_sessions[i]) {
      client_session_clear(server->client_sessions[i]);
      pthread_mutex_destroy(&server->client_sessions[i]->write_mutex);
      free(server->client_sessions[i]);
      server->client_sessions[i] = NULL;
    }
  }
  pthread_mutex_unlock(&server->sessions_mutex);

  pthread_mutex_destroy(&server->sessions_mutex);
  free(server);
}

bool websocket_server_get_client_vu_subscribed(const websocket_server_t* server,
                                               int client_idx) {
  if (!server || client_idx < 0 || client_idx >= 32) return false;
  pthread_mutex_lock((pthread_mutex_t*)&server->sessions_mutex);
  bool res = false;
  if (server->client_sessions[client_idx]) {
    res = server->client_sessions[client_idx]->vu_subscribed;
  }
  pthread_mutex_unlock((pthread_mutex_t*)&server->sessions_mutex);
  return res;
}

double websocket_server_get_client_vu_max_rate(const websocket_server_t* server,
                                               int client_idx) {
  if (!server || client_idx < 0 || client_idx >= 32) return 0.0;
  pthread_mutex_lock((pthread_mutex_t*)&server->sessions_mutex);
  double res = 0.0;
  if (server->client_sessions[client_idx]) {
    res = server->client_sessions[client_idx]->vu_max_rate;
  }
  pthread_mutex_unlock((pthread_mutex_t*)&server->sessions_mutex);
  return res;
}

double websocket_server_get_client_vu_attack(const websocket_server_t* server,
                                             int client_idx) {
  if (!server || client_idx < 0 || client_idx >= 32) return 0.0;
  pthread_mutex_lock((pthread_mutex_t*)&server->sessions_mutex);
  double res = 0.0;
  if (server->client_sessions[client_idx]) {
    res = server->client_sessions[client_idx]->vu_attack;
  }
  pthread_mutex_unlock((pthread_mutex_t*)&server->sessions_mutex);
  return res;
}

double websocket_server_get_client_vu_release(const websocket_server_t* server,
                                              int client_idx) {
  if (!server || client_idx < 0 || client_idx >= 32) return 0.0;
  pthread_mutex_lock((pthread_mutex_t*)&server->sessions_mutex);
  double res = 0.0;
  if (server->client_sessions[client_idx]) {
    res = server->client_sessions[client_idx]->vu_release;
  }
  pthread_mutex_unlock((pthread_mutex_t*)&server->sessions_mutex);
  return res;
}

void websocket_server_set_client_vu_subscribed(websocket_server_t* server,
                                               int client_idx,
                                               bool subscribed) {
  if (!server || client_idx < 0 || client_idx >= 32) return;
  pthread_mutex_lock(&server->sessions_mutex);
  if (server->client_sessions[client_idx]) {
    server->client_sessions[client_idx]->vu_subscribed = subscribed;
  }
  pthread_mutex_unlock(&server->sessions_mutex);
}
