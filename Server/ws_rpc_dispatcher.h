#ifndef CLIB_SERVER_WS_RPC_DISPATCHER_H
#define CLIB_SERVER_WS_RPC_DISPATCHER_H

#include "Public/spectrum.h"
#include "websocket_server.h"

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

void ws_rpc_emit_state_event(client_session_t* session,
                             cdsp_processing_state_t state,
                             const cdsp_stop_reason_t* reason);
void ws_rpc_emit_vu_event(client_session_t* session, const double* pb_rms,
                          const double* pb_peak, size_t pb_ch,
                          const double* cap_rms, const double* cap_peak,
                          size_t cap_ch);
void ws_rpc_emit_signal_levels_event(client_session_t* session,
                                     const double* rms, const double* peak,
                                     size_t ch);
void ws_rpc_emit_spectrum_event(websocket_server_t* server,
                                client_session_t* session);

#endif  // CLIB_SERVER_WS_RPC_DISPATCHER_H
