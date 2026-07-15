#include "ws_handshake.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <stdint.h>
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

static void CC_SHA1(const void* data, CC_LONG len, unsigned char* digest) {
  uint32_t state[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476,
                       0xC3D2C1F0};
  unsigned char buffer[64];
  uint64_t total_bits = (uint64_t)len * 8;
  const unsigned char* d = (const unsigned char*)data;
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

bool ws_handle_handshake(const char* request, socket_t client_fd) {
  if (strncmp(request, "GET ", 4) == 0 && strstr(request, "Upgrade: ")) {
    const char* key_ptr = strstr(request, "Sec-WebSocket-Key: ");
    if (key_ptr) {
      key_ptr += 19;
      char key[64];
      int k = 0;
      while (*key_ptr && *key_ptr != '\r' && *key_ptr != '\n' && k < 63) {
        key[k++] = *key_ptr++;
      }
      key[k] = '\0';
      char concat[128];
      snprintf(concat, sizeof(concat), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11",
               key);
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
    }
    return true;
  }
  return false;
}
