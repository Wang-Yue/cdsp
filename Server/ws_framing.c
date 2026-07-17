#include "ws_framing.h"

#include <stdlib.h>
#include <string.h>

#include "websocket_server_internal.h"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

bool ws_parse_frame_header(const unsigned char* buf, size_t buf_len,
                           size_t* out_payload_len, size_t* out_header_len,
                           unsigned char** out_mask, uint8_t* out_opcode) {
  if (buf_len < 2) return false;

  uint8_t first_byte = buf[0];
  if (!(first_byte & 0x80)) return false;

  uint8_t opcode = first_byte & 0x0F;
  if (opcode != 0x01 && opcode != 0x02 && opcode != 0x08) return false;
  *out_opcode = opcode;

  uint8_t len_byte = buf[1];
  bool masked = (len_byte & 0x80) != 0;
  size_t payload_len = len_byte & 0x7F;

  size_t header_len = 2;
  if (payload_len == 126) {
    if (buf_len < 4) return false;
    payload_len = ((size_t)buf[2] << 8) | buf[3];
    header_len = 4;
  } else if (payload_len == 127) {
    if (buf_len < 10) return false;
    payload_len = 0;
    for (int i = 0; i < 8; i++) {
      payload_len = (payload_len << 8) | buf[2 + i];
    }
    header_len = 10;
  }

  if (masked) {
    if (buf_len < header_len + 4) return false;
    *out_mask = (unsigned char*)&buf[header_len];
    header_len += 4;
  } else {
    *out_mask = NULL;
  }

  *out_payload_len = payload_len;
  *out_header_len = header_len;
  return true;
}

void ws_send_frame(socket_t fd, const char* response) {
  if (fd < 0 || !response) return;
  size_t resp_len = strlen(response);
  char frame[16384];
  frame[0] = (char)0x81;
  int header_len = 2;
  if (resp_len < 126) {
    frame[1] = (char)resp_len;
  } else if (resp_len <= 65535) {
    frame[1] = (char)126;
    frame[2] = (char)((resp_len >> 8) & 0xFF);
    frame[3] = (char)(resp_len & 0xFF);
    header_len = 4;
  } else {
    frame[1] = (char)127;
    for (int i = 0; i < 8; i++) {
      frame[2 + i] = (char)((resp_len >> ((7 - i) * 8)) & 0xFF);
    }
    header_len = 10;
  }
  if (header_len + resp_len <= sizeof(frame)) {
    memcpy(&frame[header_len], response, resp_len);
    send(fd, frame, (int)(header_len + resp_len), 0);
  } else {
    char* dyn_frame = (char*)malloc(header_len + resp_len);
    if (dyn_frame) {
      dyn_frame[0] = (char)0x81;
      dyn_frame[1] = frame[1];
      memcpy(&dyn_frame[2], &frame[2], header_len - 2);
      memcpy(&dyn_frame[header_len], response, resp_len);
      send(fd, dyn_frame, (int)(header_len + resp_len), 0);
      free(dyn_frame);
    }
  }
}
