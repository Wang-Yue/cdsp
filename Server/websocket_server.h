/**
 * @file websocket_server.h
 * @brief WebSocket control server for CamillaDSP monitor.
 *
 * Provides a runtime control API compatible with the control protocol.
 */

#ifndef CLIB_SERVER_WEBSOCKET_SERVER_H
#define CLIB_SERVER_WEBSOCKET_SERVER_H

#include <libwebsockets.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "Public/cdsp_pub_types.h"
#include "ws_metrics.h"

typedef struct cJSON cJSON;

typedef struct {
  cdsp_processing_state_t state;
  cdsp_stop_reason_t stop_reason;
} ws_state_update_t;

static inline const char* ws_processing_state_to_string(
    cdsp_processing_state_t state) {
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
    default:
      return "Inactive";
  }
}

typedef struct client_session_s {
  session_metrics_t metrics;

  struct lws* wsi;
  char* pending_write;
  size_t pending_write_len;
  pthread_mutex_t write_mutex;
} client_session_t;

struct websocket_server {
  uint16_t port;
  char host[128];
  dsp_engine_t* engine;

  struct lws_context* context;
  _Atomic bool running;
  pthread_t thread;
  pthread_t metrics_thread;

  uint32_t update_interval;

  ws_metrics_state_t metrics;

  pthread_mutex_t sessions_mutex;
  client_session_t* client_sessions[32];
};

typedef struct websocket_server websocket_server_t;

void websocket_server_queue_message(client_session_t* session, const char* msg);
void client_session_clear(client_session_t* session);
cJSON* serialize_stop_reason(const cdsp_stop_reason_t* reason);
cJSON* create_state_event_value(cdsp_processing_state_t state,
                                const cdsp_stop_reason_t* reason);

/**
 * @brief Create a new WebSocket control server on the specified port and host.
 *
 * @param port Port number to listen on.
 * @param host Hostname or IP address to bind to.
 * @return A pointer to the created websocket_server_t, or NULL on failure.
 */
websocket_server_t* websocket_server_create(uint16_t port, const char* host);

/**
 * @brief Set the DSP engine for the WebSocket server to interact with.
 *
 * @param server Pointer to the WebSocket server.
 * @param engine Pointer to the DSP engine.
 */
void websocket_server_set_engine(websocket_server_t* server,
                                 dsp_engine_t* engine);

/**
 * @brief Start the WebSocket server listening and processing connections in a
 * background thread.
 *
 * @param server Pointer to the WebSocket server.
 * @return true if the server started successfully, false otherwise.
 */
bool websocket_server_start(websocket_server_t* server);

/**
 * @brief Stop the WebSocket server, disconnect all clients, and join the server
 * thread.
 *
 * @param server Pointer to the WebSocket server.
 */
void websocket_server_stop(websocket_server_t* server);

/**
 * @brief Destroy and free the WebSocket server.
 *
 * @param server Pointer to the WebSocket server to free.
 */
void websocket_server_free(websocket_server_t* server);

#include "dyn_string.h"

/**
 * @brief Handle a control command text (either simple quoted string or JSON
 * object) and populate ds.
 *
 * @param server Pointer to the WebSocket server.
 * @param client_idx The index of the client session that sent the command.
 * @param command_text The raw command text received.
 * @param ds Dynamic string to write the response to.
 */
void websocket_server_handle_command(websocket_server_t* server, int client_idx,
                                     const char* command_text,
                                     dyn_string_t* ds);

#endif  // CLIB_SERVER_WEBSOCKET_SERVER_H
