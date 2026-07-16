/**
 * @file websocket_server.h
 * @brief WebSocket control server for CamillaDSP monitor.
 *
 * Provides a runtime control API compatible with the control protocol.
 */

#ifndef CLIB_SERVER_WEBSOCKET_SERVER_H
#define CLIB_SERVER_WEBSOCKET_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "Backend/backend_error.h"
#include "Config/configuration.h"
#include "Public/cdsp_pub_types.h"

/**
 * @brief Opaque structure representing a WebSocket server.
 */
typedef struct websocket_server websocket_server_t;

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

// MARK: - Command Handler

typedef struct dyn_string_s {
  char* data;
  size_t capacity;
  size_t length;
} dyn_string_t;

void dyn_string_init(dyn_string_t* ds, size_t initial_cap);
void dyn_string_free(dyn_string_t* ds);

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

// MARK: - Testing Helpers

bool websocket_server_get_client_vu_subscribed(const websocket_server_t* server,
                                               int client_idx);
double websocket_server_get_client_vu_max_rate(const websocket_server_t* server,
                                               int client_idx);
double websocket_server_get_client_vu_attack(const websocket_server_t* server,
                                             int client_idx);
double websocket_server_get_client_vu_release(const websocket_server_t* server,
                                              int client_idx);
void websocket_server_set_client_vu_subscribed(websocket_server_t* server,
                                               int client_idx, bool subscribed);

#endif  // CLIB_SERVER_WEBSOCKET_SERVER_H
