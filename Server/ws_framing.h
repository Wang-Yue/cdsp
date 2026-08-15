#ifndef CLIB_SERVER_WS_FRAMING_H
#define CLIB_SERVER_WS_FRAMING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "Server/websocket_server_internal.h"

/**
 * @brief Parses a WebSocket frame from a buffer.
 *
 * @param buf Input buffer.
 * @param buf_len Total available bytes in buffer.
 * @param out_payload_len Output for decoded payload length.
 * @param out_header_len Output for total length of header + mask.
 * @param out_mask Output pointer pointing inside buf to the 4-byte masking key
 * (NULL if unmasked).
 * @param out_opcode Output for the 4-bit opcode (e.g. 0x01 for text, 0x08 for
 * close).
 * @return true if a complete and valid frame header is present in the buffer,
 * false otherwise.
 */
bool ws_parse_frame_header(const unsigned char* buf, size_t buf_len,
                           size_t* out_payload_len, size_t* out_header_len,
                           unsigned char** out_mask, uint8_t* out_opcode);

/**
 * @brief Sends a WebSocket text frame to a client file descriptor.
 *
 * @param fd The client socket file descriptor.
 * @param response The null-terminated payload string to send.
 */
void ws_send_frame(socket_t fd, const char* response);

/**
 * @brief Sends a WebSocket Pong control frame (Opcode 0x8A) to a client.
 *
 * @param fd The client socket file descriptor.
 * @param payload The binary payload received in the Ping frame.
 * @param len Length of the payload (<= 125 bytes).
 */
void ws_send_pong_frame(socket_t fd, const char* payload, size_t len);

/**
 * @brief Sends a WebSocket Close control frame (Opcode 0x88) to a client.
 *
 * @param fd The client socket file descriptor.
 * @param code 2-byte status code (e.g. 1000 for normal closure).
 */
void ws_send_close_frame(socket_t fd, uint16_t code);

#endif  // CLIB_SERVER_WS_FRAMING_H
