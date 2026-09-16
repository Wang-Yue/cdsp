#include "server/ws_handshake.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "server/websocket_server_internal.h"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else

#define CC_SHA1_DIGEST_LENGTH 20
typedef uint32_t CC_LONG;

#define SHA1_ROL(value, bits) (((value) << (bits)) | ((value) >> (32 - (bits))))

static void sha1_transform(uint32_t state[5], const unsigned char buffer[64]) {
  uint32_t block[80];
  for (int i = 0; i < 16; i++) {
    block[i] =
        ((uint32_t)buffer[i * 4] << 24) | ((uint32_t)buffer[i * 4 + 1] << 16) |
        ((uint32_t)buffer[i * 4 + 2] << 8) | ((uint32_t)buffer[i * 4 + 3]);
  }
  for (int i = 16; i < 80; i++) {
    block[i] = SHA1_ROL(
        block[i - 3] ^ block[i - 8] ^ block[i - 14] ^ block[i - 16], 1);
  }
  uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
  for (int i = 0; i < 80; i++) {
    uint32_t f, k;
    if (i < 20) {
      f = (b & c) | (~b & d);
      k = 0x5A827999;
    } else if (i < 40) {
      f = b ^ c ^ d;
      k = 0x6ED9EBA1;
    } else if (i < 60) {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8F1BBCDC;
    } else {
      f = b ^ c ^ d;
      k = 0xCA62C1D6;
    }
    uint32_t temp = SHA1_ROL(a, 5) + f + e + k + block[i];
    e = d;
    d = c;
    c = SHA1_ROL(b, 30);
    b = a;
    a = temp;
  }
  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
}

static void CC_SHA1(const void *data, CC_LONG len, unsigned char *digest) {
  uint32_t state[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476,
                       0xC3D2E1F0};
  unsigned char buffer[64];
  uint64_t total_bits = (uint64_t)len * 8;
  const unsigned char *d = (const unsigned char *)data;
  CC_LONG offset = 0;
  CC_LONG remaining_len = len;
  while (remaining_len >= 64) {
    sha1_transform(state, d + offset);
    offset += 64;
    remaining_len -= 64;
  }
  memcpy(buffer, d + offset, remaining_len);
  buffer[remaining_len] = 0x80;
  if (remaining_len >= 56) {
    memset(buffer + remaining_len + 1, 0, 63 - remaining_len);
    sha1_transform(state, buffer);
    memset(buffer, 0, 56);
  } else {
    memset(buffer + remaining_len + 1, 0, 55 - remaining_len);
  }
  for (int i = 0; i < 8; i++) {
    buffer[56 + i] = (unsigned char)(total_bits >> ((7 - i) * 8));
  }
  sha1_transform(state, buffer);
  for (int i = 0; i < 5; i++) {
    digest[i * 4] = (unsigned char)(state[i] >> 24);
    digest[i * 4 + 1] = (unsigned char)(state[i] >> 16);
    digest[i * 4 + 2] = (unsigned char)(state[i] >> 8);
    digest[i * 4 + 3] = (unsigned char)(state[i]);
  }
}
#endif

#ifdef _WIN32
#define cdsp_strncasecmp _strnicmp
#else
#define cdsp_strncasecmp strncasecmp
#endif

#include <ctype.h>

static const char *strcasestr_custom(const char *haystack, const char *needle) {
  if (!haystack || !needle)
    return NULL;
  size_t nlen = strlen(needle);
  if (nlen == 0)
    return haystack;
  while (*haystack) {
    if (cdsp_strncasecmp(haystack, needle, nlen) == 0) {
      return haystack;
    }
    haystack++;
  }
  return NULL;
}

bool ws_handle_handshake(const char *request, socket_t client_fd) {
  if (!request || IS_INVALID_SOCKET(client_fd))
    return false;

  // RFC 6455 §4.2.1: Request must be GET and HTTP/1.1 or higher
  const char *line_end = strstr(request, "\r\n");
  if (!line_end)
    line_end = strchr(request, '\n');
  if (!line_end)
    return false;

  if (strncmp(request, "GET ", 4) != 0 ||
      !strcasestr_custom(request, "HTTP/1.1")) {
    const char *bad_request =
        "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n";
    send(client_fd, bad_request, (int)strlen(bad_request), 0);
    return false;
  }

  bool has_upgrade_websocket = false;
  bool has_connection_upgrade = false;
  char ws_key[64] = "";
  char ws_version[32] = "";

  const char *p = line_end;
  while (*p == '\r' || *p == '\n')
    p++;

  while (*p) {
    if (*p == '\r' || *p == '\n')
      break;

    const char *next_line = strstr(p, "\r\n");
    size_t line_len = next_line ? (size_t)(next_line - p) : strlen(p);
    const char *colon = (const char *)memchr(p, ':', line_len);
    if (colon) {
      size_t name_len = (size_t)(colon - p);
      const char *val_start = colon + 1;
      while (val_start < p + line_len &&
             (*val_start == ' ' || *val_start == '\t')) {
        val_start++;
      }
      const char *val_end = p + line_len;
      while (val_end > val_start &&
             (val_end[-1] == ' ' || val_end[-1] == '\t')) {
        val_end--;
      }
      size_t val_len = (size_t)(val_end - val_start);

      if (name_len == 7 && cdsp_strncasecmp(p, "Upgrade", 7) == 0) {
        char val[128];
        if (val_len < sizeof(val)) {
          memcpy(val, val_start, val_len);
          val[val_len] = '\0';
          if (strcasestr_custom(val, "websocket")) {
            has_upgrade_websocket = true;
          }
        }
      } else if (name_len == 10 && cdsp_strncasecmp(p, "Connection", 10) == 0) {
        char val[128];
        if (val_len < sizeof(val)) {
          memcpy(val, val_start, val_len);
          val[val_len] = '\0';
          if (strcasestr_custom(val, "upgrade")) {
            has_connection_upgrade = true;
          }
        }
      } else if (name_len == 17 &&
                 cdsp_strncasecmp(p, "Sec-WebSocket-Key", 17) == 0) {
        if (val_len < sizeof(ws_key)) {
          memcpy(ws_key, val_start, val_len);
          ws_key[val_len] = '\0';
        }
      } else if (name_len == 21 &&
                 cdsp_strncasecmp(p, "Sec-WebSocket-Version", 21) == 0) {
        if (val_len < sizeof(ws_version)) {
          memcpy(ws_version, val_start, val_len);
          ws_version[val_len] = '\0';
        }
      }
    }

    if (!next_line)
      break;
    p = next_line + 2;
  }

  // RFC 6455 §4.4: Sec-WebSocket-Version must be 13; if not, reject with
  // version 13
  if (strcmp(ws_version, "13") != 0) {
    const char *bad_version = "HTTP/1.1 400 Bad Request\r\n"
                              "Sec-WebSocket-Version: 13\r\n"
                              "Connection: close\r\n\r\n";
    send(client_fd, bad_version, (int)strlen(bad_version), 0);
    return false;
  }

  // Validate Sec-WebSocket-Key is 16 bytes base64-encoded (24 characters, '=='
  // padding)
  bool valid_key =
      (strlen(ws_key) == 24 && ws_key[22] == '=' && ws_key[23] == '=');
  if (valid_key) {
    for (int i = 0; i < 22; i++) {
      char c = ws_key[i];
      if (!isalnum((unsigned char)c) && c != '+' && c != '/') {
        valid_key = false;
        break;
      }
    }
  }

  if (!has_upgrade_websocket || !has_connection_upgrade || !valid_key) {
    const char *bad_request =
        "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n";
    send(client_fd, bad_request, (int)strlen(bad_request), 0);
    return false;
  }

  char concat[128];
  snprintf(concat, sizeof(concat), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11",
           ws_key);
  unsigned char hash[CC_SHA1_DIGEST_LENGTH];
  CC_SHA1(concat, (CC_LONG)strlen(concat), hash);

  static const char b64[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  char b64_hash[32];
  int b_idx = 0;
  for (int idx = 0; idx < 20; idx += 3) {
    uint32_t val = (hash[idx] << 16) |
                   ((idx + 1 < 20 ? hash[idx + 1] : 0) << 8) |
                   (idx + 2 < 20 ? hash[idx + 2] : 0);
    b64_hash[b_idx++] = b64[(val >> 18) & 63];
    b64_hash[b_idx++] = b64[(val >> 12) & 63];
    b64_hash[b_idx++] = (idx + 1 < 20) ? b64[(val >> 6) & 63] : '=';
    b64_hash[b_idx++] = (idx + 2 < 20) ? b64[val & 63] : '=';
  }
  b64_hash[b_idx] = '\0';

  char reply[512];
  snprintf(reply, sizeof(reply),
           "HTTP/1.1 101 Switching Protocols\r\nUpgrade: "
           "websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: "
           "%s\r\n\r\n",
           b64_hash);
  send(client_fd, reply, (int)strlen(reply), 0);
  return true;
}
