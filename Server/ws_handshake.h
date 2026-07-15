#ifndef CLIB_SERVER_WS_HANDSHAKE_H
#define CLIB_SERVER_WS_HANDSHAKE_H

#include <stdbool.h>
#include "websocket_server_internal.h"

/**
 * @brief Checks if a request is a WebSocket upgrade request and handles it.
 * @param request The raw HTTP request string.
 * @param client_fd The client socket to send the response to.
 * @return true if it was an upgrade request (even if invalid/handled), false if not a handshake.
 */
bool ws_handle_handshake(const char* request, socket_t client_fd);

#endif // CLIB_SERVER_WS_HANDSHAKE_H
