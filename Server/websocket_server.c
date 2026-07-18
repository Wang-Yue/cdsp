// WebSocket control server using libwebsockets
// Provides runtime control API compatible with the CamillaDSP monitor control
// protocol

#include "websocket_server.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Logging/app_logger.h"
#include "Utils/cdsp_time.h"
#include "ws_rpc_dispatcher.h"

static const logger_t server_logger = {"dsp.server.websocket"};

void client_session_clear(client_session_t* session) {
  if (!session) return;
  if (session->metrics.vu_pb_rms) {
    free(session->metrics.vu_pb_rms);
    session->metrics.vu_pb_rms = NULL;
  }
  if (session->metrics.vu_pb_peak) {
    free(session->metrics.vu_pb_peak);
    session->metrics.vu_pb_peak = NULL;
  }
  if (session->metrics.vu_cap_rms) {
    free(session->metrics.vu_cap_rms);
    session->metrics.vu_cap_rms = NULL;
  }
  if (session->metrics.vu_cap_peak) {
    free(session->metrics.vu_cap_peak);
    session->metrics.vu_cap_peak = NULL;
  }
  session->metrics.vu_pb_channels = 0;
  session->metrics.vu_cap_channels = 0;
}

void websocket_server_queue_message(client_session_t* session,
                                    const char* msg) {
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
          websocket_server_queue_message(session, ds.data);
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
  while (atomic_load_explicit(&server->running, memory_order_acquire)) {
    cdsp_sleep_ms(50);
    ws_metrics_broadcast_tick(server);
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

  level_history_clear(&server->metrics.capture_peak_history);
  level_history_clear(&server->metrics.capture_rms_history);
  level_history_clear(&server->metrics.playback_peak_history);
  level_history_clear(&server->metrics.playback_rms_history);

  if (server->metrics.capture_global_peaks)
    free(server->metrics.capture_global_peaks);
  if (server->metrics.playback_global_peaks)
    free(server->metrics.playback_global_peaks);

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
