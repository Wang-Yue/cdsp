#ifndef CLIB_SERVER_WS_RPC_DISPATCHER_H
#define CLIB_SERVER_WS_RPC_DISPATCHER_H

#include "websocket_server_internal.h"
#include "Public/spectrum.h"

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

cJSON* serialize_spectrum(const cdsp_spectrum_t* spec);
cJSON* serialize_stop_reason(const cdsp_stop_reason_t* reason);

#endif  // CLIB_SERVER_WS_RPC_DISPATCHER_H
