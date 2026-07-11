// WebSocket control server
// Provides runtime control API compatible with the control protocol

#include "websocket_server.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void dyn_string_init(dyn_string_t* ds, size_t initial_cap) {
  ds->data = (char*)calloc(initial_cap, sizeof(char));
  if (ds->data) {
    ds->data[0] = '\0';
    ds->capacity = initial_cap;
  } else {
    ds->capacity = 0;
  }
  ds->length = 0;
}

void dyn_string_free(dyn_string_t* ds) {
  if (ds->data) free(ds->data);
  ds->data = NULL;
  ds->capacity = 0;
  ds->length = 0;
}

static void dyn_string_printf(dyn_string_t* ds, const char* fmt, ...) {
  if (!ds->data || ds->capacity == 0) return;
  va_list args;
  va_start(args, fmt);

  va_list args_copy;
  va_copy(args_copy, args);
  int needed = vsnprintf(ds->data, ds->capacity, fmt, args_copy);
  va_end(args_copy);

  if (needed < 0) {
    va_end(args);
    return;
  }

  if ((size_t)needed >= ds->capacity) {
    size_t new_cap = ds->capacity * 2;
    if (new_cap <= (size_t)needed) new_cap = (size_t)needed + 1;
    char* new_data = (char*)realloc(ds->data, new_cap);
    if (!new_data) {
      va_end(args);
      return;
    }
    ds->data = new_data;
    ds->capacity = new_cap;

    vsnprintf(ds->data, ds->capacity, fmt, args);
  }
  ds->length = (size_t)needed;
  va_end(args);
}

static void free_vu_levels_arrays(vu_levels_t* vu) {
  if (vu->playback_rms) free(vu->playback_rms);
  if (vu->playback_peak) free(vu->playback_peak);
  if (vu->capture_rms) free(vu->capture_rms);
  if (vu->capture_peak) free(vu->capture_peak);
  memset(vu, 0, sizeof(vu_levels_t));
}

#include <pthread.h>
#include <stdatomic.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
typedef SOCKET socket_t;
#else
typedef int socket_t;
#endif

/**
 * @brief Represents a single point in level history.
 */
typedef struct {
  /** Timestamp in milliseconds when the level was sampled. */
  uint64_t timestamp_ms;
  /** Array of levels (one per channel). */
  double* levels;
} level_sample_t;

/**
 * @brief Stores historical level samples.
 */
typedef struct {
  /** Array of historical level samples. */
  level_sample_t samples[300];
  /** Index of the head of the circular buffer. */
  size_t head;
  /** Number of elements in the buffer. */
  size_t size;
  /** Number of channels per sample. */
  size_t channels;
} level_history_t;

/**
 * @brief Represents a single client's WebSocket session state.
 */
typedef struct {
  /** Last time capture peak levels were pushed. */
  uint64_t last_cap_peak_time;
  /** Last time capture RMS levels were pushed. */
  uint64_t last_cap_rms_time;
  /** Last time playback peak levels were pushed. */
  uint64_t last_pb_peak_time;
  /** Last time playback RMS levels were pushed. */
  uint64_t last_pb_rms_time;

  /** True if client is subscribed to state updates. */
  bool state_subscribed;
  /** True if client is subscribed to VU level updates. */
  bool vu_subscribed;
  /** True if client is subscribed to signal level updates. */
  bool signal_levels_subscribed;
  /** Side to subscribe to for signal levels ("capture" or "playback"). */
  char signal_levels_side[16];
  /** True if client is subscribed to spectrum updates. */
  bool spectrum_subscribed;
  /** True if spectrum subscription is for capture channels. */
  bool spectrum_is_capture;
  /** Channel index for spectrum updates. */
  uint32_t spectrum_channel;
  /** Minimum frequency for spectrum updates. */
  double spectrum_min_freq;
  /** Maximum frequency for spectrum updates. */
  double spectrum_max_freq;
  /** Number of bins for spectrum updates. */
  uint32_t spectrum_n_bins;
  /** Maximum rate of spectrum updates (per second). */
  double spectrum_max_rate;
  /** Last time spectrum data was pushed to this client. */
  uint64_t last_spectrum_push_time;

  /** Maximum rate of VU updates (per second). */
  double vu_max_rate;
  /** Attack time for VU meters. */
  double vu_attack;
  /** Release time for VU meters. */
  double vu_release;

  /** Last time VU levels were pushed to this client. */
  uint64_t last_vu_push_time;

  /** Current playback RMS levels. */
  double* vu_pb_rms;
  /** Current playback peak levels. */
  double* vu_pb_peak;
  /** Current capture RMS levels. */
  double* vu_cap_rms;
  /** Current capture peak levels. */
  double* vu_cap_peak;
  /** Number of playback channels in the VU. */
  size_t vu_pb_channels;
  /** Number of capture channels in the VU. */
  size_t vu_cap_channels;
} client_session_t;

/**
 * @brief Structure containing the internal state of the WebSocket server.
 */
struct websocket_server {
  /** The port the server listens on. */
  uint16_t port;
  /** The host interface the server binds to. */
  char host[128];
  /** The interface to the DSP engine. */
  dsp_engine_interface_t* engine;

  /** Server socket file descriptor. */
  socket_t server_fd;
  /** Atomic flag indicating if the server is running. */
  _Atomic bool running;
  /** Thread handle for the server runloop. */
  pthread_t thread;

  /** Server update/tick interval in microseconds. */
  uint32_t update_interval;

  /** History of capture peak levels. */
  level_history_t capture_peak_history;
  /** History of capture RMS levels. */
  level_history_t capture_rms_history;
  /** History of playback peak levels. */
  level_history_t playback_peak_history;
  /** History of playback RMS levels. */
  level_history_t playback_rms_history;

  /** Array storing global peak capture levels per channel. */
  double* capture_global_peaks;
  /** Array storing global peak playback levels per channel. */
  double* playback_global_peaks;
  /** Number of capture channels for global peaks. */
  size_t capture_global_peaks_count;
  /** Number of playback channels for global peaks. */
  size_t playback_global_peaks_count;

  pthread_mutex_t sessions_mutex;
  /** Array of active client sessions. */
  client_session_t client_sessions[32];
};

#include "Logging/app_logger.h"

static const logger_t server_logger = {"dsp.server.websocket"};

#ifdef _WIN32
#include <ws2tcpip.h>
#define CLOSE_SOCKET(s) closesocket(s)
#define INVALID_SOCKET_VAL INVALID_SOCKET
#define IS_INVALID_SOCKET(s) ((s) == INVALID_SOCKET)
#define IS_SOCKET_ERROR(r) ((r) == SOCKET_ERROR)
#define GET_SOCKET_ERROR() WSAGetLastError()
#define poll_sockets WSAPoll
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#define CLOSE_SOCKET(s) close(s)
#define INVALID_SOCKET_VAL (-1)
#define IS_INVALID_SOCKET(s) ((s) < 0)
#define IS_SOCKET_ERROR(r) ((r) < 0)
#define GET_SOCKET_ERROR() errno
#define poll_sockets poll
#endif

#include "Audio/processing_parameters.h"
#include "Config/cJSON.h"
#include "Pipeline/config_loader.h"
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <stdint.h>
#define CC_SHA1_DIGEST_LENGTH 20
typedef uint32_t CC_LONG;

#define SHA1_ROL(value, bits) (((value) << (bits)) | ((value) >> (32 - (bits))))

/**
 * @brief Performs a SHA-1 block transformation.
 *
 * Core SHA-1 compression function. Processes a single 64-byte block to update
 * the 160-bit state. Used for WebSocket handshake key generation on non-macOS
 * systems.
 *
 * @param state SHA-1 state vector (5 words).
 * @param buffer 64-byte block buffer to process.
 */
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

/**
 * @brief Computes SHA-1 hash of the given data buffer.
 *
 * Provides a fallback SHA-1 implementation on systems that do not offer
 * CommonCrypto (like Linux). Outputs a 20-byte digest.
 *
 * @param data Pointer to the input data buffer.
 * @param len Length of input data in bytes.
 * @param digest Output buffer to store the 20-byte SHA-1 digest.
 */
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
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/**
 * @brief Converts a decibel (dB) value to a linear amplitude ratio.
 *
 * @param db Value in dB. Values <= -1000.0 are treated as silence (0.0
 * amplitude).
 * @return Linear amplitude ratio.
 */
static double db_to_amplitude(double db) {
  if (db <= -1000.0) return 0.0;
  return pow(10.0, db / 20.0);
}

/**
 * @brief Converts a linear amplitude ratio to a decibel (dB) value.
 *
 * @param amp Linear amplitude ratio. Values <= 0.0 return -1000.0 dB.
 * @return Value in dB. Min value is capped at -1000.0 dB.
 */
static double amplitude_to_db(double amp) {
  if (amp <= 0.0) return -1000.0;
  double db = 20.0 * log10(amp);
  return db < -1000.0 ? -1000.0 : db;
}

/**
 * @brief Appends a new level sample to the history ring buffer.
 *
 * If the channel count changes, the history is cleared and re-initialized.
 *
 * @param history Pointer to the level history structure.
 * @param levels Array of levels per channel.
 * @param channels Number of channels.
 * @param now_ms Current timestamp in milliseconds.
 */
static void level_history_clear(level_history_t* history) {
  if (!history) return;
  for (size_t i = 0; i < 300; i++) {
    if (history->samples[i].levels) {
      free(history->samples[i].levels);
      history->samples[i].levels = NULL;
    }
  }
  history->head = 0;
  history->size = 0;
  history->channels = 0;
}

static void client_session_clear(client_session_t* session) {
  if (!session) return;
  if (session->vu_pb_rms) {
    free(session->vu_pb_rms);
    session->vu_pb_rms = NULL;
  }
  if (session->vu_pb_peak) {
    free(session->vu_pb_peak);
    session->vu_pb_peak = NULL;
  }
  if (session->vu_cap_rms) {
    free(session->vu_cap_rms);
    session->vu_cap_rms = NULL;
  }
  if (session->vu_cap_peak) {
    free(session->vu_cap_peak);
    session->vu_cap_peak = NULL;
  }
  session->vu_pb_channels = 0;
  session->vu_cap_channels = 0;
}

/**
 * @brief Appends a new level sample to the history ring buffer.
 *
 * If the channel count changes, the history is cleared and re-initialized.
 *
 * @param history Pointer to the level history structure.
 * @param levels Array of levels per channel.
 * @param channels Number of channels.
 * @param now_ms Current timestamp in milliseconds.
 */
static void level_history_append(level_history_t* history, const double* levels,
                                 size_t channels, uint64_t now_ms) {
  if (history->channels != channels) {
    level_history_clear(history);
    history->channels = channels;
  }
  if (channels == 0) return;
  level_sample_t* sample = &history->samples[history->head];
  if (sample->levels) {
    free(sample->levels);
  }
  sample->levels = (double*)calloc(channels, sizeof(double));
  if (sample->levels) {
    memcpy(sample->levels, levels, channels * sizeof(double));
    sample->timestamp_ms = now_ms;
    history->head = (history->head + 1) % 300;
    if (history->size < 300) {
      history->size++;
    }
  }
}

/**
 * @brief Finds the peak (maximum) level for each channel since a given
 * timestamp.
 *
 * Searches the level history ring buffer backward starting from the head.
 *
 * @param history Pointer to the level history structure.
 * @param since_ms Start timestamp in milliseconds.
 * @param out_levels Output array to store the peak levels per channel
 * (initialized to -1000.0 dB).
 */
static void level_history_get_max_since(const level_history_t* history,
                                        uint64_t since_ms, double* out_levels) {
  size_t channels = history->channels;
  for (size_t c = 0; c < channels; c++) {
    out_levels[c] = -1000.0;
  }
  if (history->size == 0 || channels == 0) return;
  size_t idx = (history->head + 300 - 1) % 300;
  for (size_t i = 0; i < history->size; i++) {
    const level_sample_t* sample = &history->samples[idx];
    if (sample->timestamp_ms < since_ms) break;
    for (size_t c = 0; c < channels; c++) {
      if (sample->levels[c] > out_levels[c]) {
        out_levels[c] = sample->levels[c];
      }
    }
    idx = (idx + 300 - 1) % 300;
  }
}

/**
 * @brief Calculates the root-mean-square (RMS) level for each channel since a
 * given timestamp.
 *
 * Aggregates values in the level history ring buffer backward from the head.
 *
 * @param history Pointer to the level history structure.
 * @param since_ms Start timestamp in milliseconds.
 * @param out_levels Output array to store the RMS levels per channel in dB.
 */
static void level_history_get_rms_since(const level_history_t* history,
                                        uint64_t since_ms, double* out_levels) {
  size_t channels = history->channels;
  for (size_t c = 0; c < channels; c++) {
    out_levels[c] = -1000.0;
  }
  if (history->size == 0 || channels == 0) return;
  double* sums = (double*)calloc(channels, sizeof(double));
  size_t count = 0;
  size_t idx = (history->head + 300 - 1) % 300;
  for (size_t i = 0; i < history->size; i++) {
    const level_sample_t* sample = &history->samples[idx];
    if (sample->timestamp_ms < since_ms) break;
    for (size_t c = 0; c < channels; c++) {
      double amp = db_to_amplitude(sample->levels[c]);
      sums[c] += amp * amp;
    }
    count++;
    idx = (idx + 300 - 1) % 300;
  }
  if (count > 0) {
    for (size_t c = 0; c < channels; c++) {
      double mean_square = sums[c] / (double)count;
      out_levels[c] = amplitude_to_db(sqrt(mean_square));
    }
  }
  free(sums);
}

/**
 * @brief Calculates the smoothing factor (alpha) for an exponential moving
 * average.
 *
 * @param delta_ms Time elapsed since the last update in milliseconds.
 * @param time_constant_ms The response time constant in milliseconds.
 * @return The smoothing factor alpha (between 0.0 and 1.0). Returns 1.0 if time
 * constant is <= 0.
 */
static double smoothing_alpha(double delta_ms, double time_constant_ms) {
  if (time_constant_ms <= 0.0) return 1.0;
  double delta_sec = delta_ms / 1000.0;
  double time_constant_sec = time_constant_ms / 1000.0;
  return 1.0 - exp(-delta_sec / time_constant_sec);
}

websocket_server_t* websocket_server_create(uint16_t port, const char* host) {
  websocket_server_t* server =
      (websocket_server_t*)calloc(1, sizeof(websocket_server_t));
  if (!server) return NULL;
  server->port = port;
  if (host && host[0]) {
    strncpy(server->host, host, sizeof(server->host) - 1);
  } else {
    strncpy(server->host, "127.0.0.1", sizeof(server->host) - 1);
  }
  server->server_fd = INVALID_SOCKET_VAL;
  server->update_interval = 100;
  atomic_init(&server->running, false);

  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&server->sessions_mutex, &attr);
  pthread_mutexattr_destroy(&attr);

  return server;
}

void websocket_server_set_engine(websocket_server_t* server,
                                 dsp_engine_interface_t* engine) {
  if (server) {
    server->engine = engine;
  }
}

/**
 * @brief Gets the current system time in milliseconds.
 *
 * @return Current timestamp in milliseconds.
 */
static uint64_t get_time_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/**
 * @brief Formats a processing stop reason into a quoted JSON string.
 *
 * Writes a description of why processing stopped (e.g. format change, error)
 * into the output buffer.
 *
 * @param reason Pointer to the stop reason structure.
 * @param out Output buffer to write the string into.
 * @param max_len Maximum length of the output buffer.
 */
static void stop_reason_to_string(const processing_stop_reason_t* reason,
                                  char* out, size_t max_len) {
  if (!reason || !out || max_len == 0) return;
  switch (reason->type) {
    case STOP_REASON_NONE:
      snprintf(out, max_len, "\"None\"");
      break;
    case STOP_REASON_DONE:
      snprintf(out, max_len, "\"Done\"");
      break;
    case STOP_REASON_CAPTURE_ERROR:
      snprintf(out, max_len, "\"CaptureError: %s\"", reason->message);
      break;
    case STOP_REASON_PLAYBACK_ERROR:
      snprintf(out, max_len, "\"PlaybackError: %s\"", reason->message);
      break;
    case STOP_REASON_CAPTURE_FORMAT_CHANGE:
      snprintf(out, max_len, "\"CaptureFormatChange(%d)\"",
               reason->format_change_rate);
      break;
    case STOP_REASON_PLAYBACK_FORMAT_CHANGE:
      snprintf(out, max_len, "\"PlaybackFormatChange(%d)\"",
               reason->format_change_rate);
      break;
    case STOP_REASON_UNKNOWN_ERROR:
      snprintf(out, max_len, "\"UnknownError: %s\"", reason->message);
      break;
    default:
      snprintf(out, max_len, "\"None\"");
      break;
  }
}

/**
 * @brief Formats the payload for a state event into a JSON object string.
 *
 * Generates an object like: `{"state":"Running","stop_reason":"None"}`.
 *
 * @param state The current processing state.
 * @param reason The stop reason.
 * @param out Output buffer.
 * @param max_len Maximum length of the output buffer.
 */
static void format_state_event_payload(processing_state_t state,
                                       const processing_stop_reason_t* reason,
                                       char* out, size_t max_len) {
  char reason_str[512] = "\"None\"";
  stop_reason_to_string(reason, reason_str, sizeof(reason_str));
  snprintf(out, max_len, "{\"state\":\"%s\",\"stop_reason\":%s}",
           processing_state_to_string(state), reason_str);
}

static void reply_ok(const char* cmd, cJSON* value_json, dyn_string_t* ds) {
  cJSON* root = cJSON_CreateObject();
  cJSON* inner = cJSON_CreateObject();
  cJSON_AddItemToObject(root, cmd, inner);
  cJSON_AddStringToObject(inner, "result", "Ok");
  if (value_json) {
    cJSON_AddItemToObject(inner, "value", value_json);
  }
  char* str = cJSON_PrintUnformatted(root);
  if (str) {
    dyn_string_printf(ds, "%s", str);
    free(str);
  }
  cJSON_Delete(root);
}

static void reply_error(const char* cmd, const char* error_name,
                        cJSON* error_value, dyn_string_t* ds) {
  cJSON* root = cJSON_CreateObject();
  cJSON* inner = cJSON_CreateObject();
  cJSON_AddItemToObject(root, cmd, inner);
  if (error_value) {
    cJSON_AddItemToObject(inner, "result", error_value);
  } else {
    cJSON_AddStringToObject(inner, "result", error_name);
  }
  char* str = cJSON_PrintUnformatted(root);
  if (str) {
    dyn_string_printf(ds, "%s", str);
    free(str);
  }
  cJSON_Delete(root);
}

/**
 * @brief Parses a value node which may be a number or an array of three
 * numbers.
 *
 * If the node is a number, it is treated as the relative change (delta).
 * If the node is an array of three numbers `[delta, min, max]`, all three are
 * parsed.
 *
 * @param node cJSON node to parse.
 * @param out_delta Output for the delta value.
 * @param out_min Output for the optional minimum value.
 * @param out_max Output for the optional maximum value.
 * @return true if parsing succeeded, false otherwise.
 */
static bool parse_value_with_optional_limits(cJSON* node, double* out_delta,
                                             double* out_min, double* out_max) {
  if (!node) return false;
  if (cJSON_IsNumber(node)) {
    *out_delta = node->valuedouble;
    return true;
  }
  if (cJSON_IsArray(node) && cJSON_GetArraySize(node) >= 3) {
    cJSON* d = cJSON_GetArrayItem(node, 0);
    cJSON* mn = cJSON_GetArrayItem(node, 1);
    cJSON* mx = cJSON_GetArrayItem(node, 2);
    if (d && mn && mx && cJSON_IsNumber(d) && cJSON_IsNumber(mn) &&
        cJSON_IsNumber(mx)) {
      *out_delta = d->valuedouble;
      *out_min = mn->valuedouble;
      *out_max = mx->valuedouble;
      return true;
    }
  }
  return false;
}

typedef enum {
  WS_CMD_UNKNOWN = 0,
  WS_CMD_GET_VERSION,
  WS_CMD_GET_STATE,
  WS_CMD_GET_STOP_REASON,
  WS_CMD_GET_CAPTURE_RATE,
  WS_CMD_GET_RATE_ADJUST,
  WS_CMD_GET_BUFFER_LEVEL,
  WS_CMD_GET_CLIPPED_SAMPLES,
  WS_CMD_RESET_CLIPPED_SAMPLES,
  WS_CMD_GET_PROCESSING_LOAD,
  WS_CMD_GET_RESAMPLER_LOAD,
  WS_CMD_GET_SUPPORTED_DEVICE_TYPES,
  WS_CMD_GET_UPDATE_INTERVAL,
  WS_CMD_SET_UPDATE_INTERVAL,
  WS_CMD_GET_VOLUME,
  WS_CMD_SET_VOLUME,
  WS_CMD_GET_MUTE,
  WS_CMD_SET_MUTE,
  WS_CMD_TOGGLE_MUTE,
  WS_CMD_GET_FADERS,
  WS_CMD_GET_FADER_VOLUME,
  WS_CMD_SET_FADER_VOLUME,
  WS_CMD_SET_FADER_EXTERNAL_VOLUME,
  WS_CMD_GET_FADER_MUTE,
  WS_CMD_SET_FADER_MUTE,
  WS_CMD_TOGGLE_FADER_MUTE,
  WS_CMD_ADJUST_VOLUME,
  WS_CMD_ADJUST_FADER_VOLUME,
  WS_CMD_GET_SPECTRUM,
  WS_CMD_GET_AVAILABLE_CAPTURE_DEVICES,
  WS_CMD_GET_AVAILABLE_PLAYBACK_DEVICES,
  WS_CMD_GET_CAPTURE_DEVICE_CAPABILITIES,
  WS_CMD_GET_PLAYBACK_DEVICE_CAPABILITIES,
  WS_CMD_GET_CAPTURE_SIGNAL_RMS,
  WS_CMD_GET_CAPTURE_SIGNAL_PEAK,
  WS_CMD_GET_PLAYBACK_SIGNAL_RMS,
  WS_CMD_GET_PLAYBACK_SIGNAL_PEAK,
  WS_CMD_GET_CAPTURE_SIGNAL_RMS_SINCE_LAST,
  WS_CMD_GET_CAPTURE_SIGNAL_PEAK_SINCE_LAST,
  WS_CMD_GET_PLAYBACK_SIGNAL_RMS_SINCE_LAST,
  WS_CMD_GET_PLAYBACK_SIGNAL_PEAK_SINCE_LAST,
  WS_CMD_GET_CAPTURE_SIGNAL_RMS_SINCE,
  WS_CMD_GET_CAPTURE_SIGNAL_PEAK_SINCE,
  WS_CMD_GET_PLAYBACK_SIGNAL_RMS_SINCE,
  WS_CMD_GET_PLAYBACK_SIGNAL_PEAK_SINCE,
  WS_CMD_GET_SIGNAL_LEVELS,
  WS_CMD_GET_SIGNAL_LEVELS_SINCE_LAST,
  WS_CMD_GET_SIGNAL_LEVELS_SINCE,
  WS_CMD_GET_SIGNAL_PEAKS_SINCE_START,
  WS_CMD_RESET_SIGNAL_PEAKS_SINCE_START,
  WS_CMD_GET_CHANNEL_LABELS,
  WS_CMD_GET_SIGNAL_RANGE,
  WS_CMD_GET_CONFIG_FILE_PATH,
  WS_CMD_GET_PREVIOUS_CONFIG,
  WS_CMD_GET_STATE_FILE_PATH,
  WS_CMD_GET_STATE_FILE_UPDATED,
  WS_CMD_GET_CONFIG,
  WS_CMD_GET_CONFIG_JSON,
  WS_CMD_GET_CONFIG_TITLE,
  WS_CMD_GET_CONFIG_DESCRIPTION,
  WS_CMD_RELOAD,
  WS_CMD_STOP,
  WS_CMD_EXIT,
  WS_CMD_SET_CONFIG_FILE_PATH,
  WS_CMD_SET_CONFIG_JSON,
  WS_CMD_GET_CONFIG_VALUE,
  WS_CMD_SET_CONFIG_VALUE,
  WS_CMD_PATCH_CONFIG,
  WS_CMD_READ_CONFIG_JSON,
  WS_CMD_VALIDATE_CONFIG_JSON,
  WS_CMD_SUBSCRIBE_STATE,
  WS_CMD_SUBSCRIBE_VU_LEVELS,
  WS_CMD_SUBSCRIBE_SIGNAL_LEVELS,
  WS_CMD_SUBSCRIBE_SPECTRUM,
  WS_CMD_STOP_SUBSCRIPTION
} websocket_command_t;

typedef struct {
  const char* name;
  websocket_command_t type;
} command_map_t;

static const command_map_t kCommandMap[] = {
    {"GetVersion", WS_CMD_GET_VERSION},
    {"GetState", WS_CMD_GET_STATE},
    {"GetStopReason", WS_CMD_GET_STOP_REASON},
    {"GetCaptureRate", WS_CMD_GET_CAPTURE_RATE},
    {"GetRateAdjust", WS_CMD_GET_RATE_ADJUST},
    {"GetBufferLevel", WS_CMD_GET_BUFFER_LEVEL},
    {"GetClippedSamples", WS_CMD_GET_CLIPPED_SAMPLES},
    {"ResetClippedSamples", WS_CMD_RESET_CLIPPED_SAMPLES},
    {"GetProcessingLoad", WS_CMD_GET_PROCESSING_LOAD},
    {"GetResamplerLoad", WS_CMD_GET_RESAMPLER_LOAD},
    {"GetSupportedDeviceTypes", WS_CMD_GET_SUPPORTED_DEVICE_TYPES},
    {"GetUpdateInterval", WS_CMD_GET_UPDATE_INTERVAL},
    {"SetUpdateInterval", WS_CMD_SET_UPDATE_INTERVAL},
    {"GetVolume", WS_CMD_GET_VOLUME},
    {"SetVolume", WS_CMD_SET_VOLUME},
    {"GetMute", WS_CMD_GET_MUTE},
    {"SetMute", WS_CMD_SET_MUTE},
    {"ToggleMute", WS_CMD_TOGGLE_MUTE},
    {"GetFaders", WS_CMD_GET_FADERS},
    {"GetFaderVolume", WS_CMD_GET_FADER_VOLUME},
    {"SetFaderVolume", WS_CMD_SET_FADER_VOLUME},
    {"SetFaderExternalVolume", WS_CMD_SET_FADER_EXTERNAL_VOLUME},
    {"GetFaderMute", WS_CMD_GET_FADER_MUTE},
    {"SetFaderMute", WS_CMD_SET_FADER_MUTE},
    {"ToggleFaderMute", WS_CMD_TOGGLE_FADER_MUTE},
    {"AdjustVolume", WS_CMD_ADJUST_VOLUME},
    {"AdjustFaderVolume", WS_CMD_ADJUST_FADER_VOLUME},
    {"GetSpectrum", WS_CMD_GET_SPECTRUM},
    {"GetAvailableCaptureDevices", WS_CMD_GET_AVAILABLE_CAPTURE_DEVICES},
    {"GetAvailablePlaybackDevices", WS_CMD_GET_AVAILABLE_PLAYBACK_DEVICES},
    {"GetCaptureDeviceCapabilities", WS_CMD_GET_CAPTURE_DEVICE_CAPABILITIES},
    {"GetPlaybackDeviceCapabilities", WS_CMD_GET_PLAYBACK_DEVICE_CAPABILITIES},
    {"GetCaptureSignalRms", WS_CMD_GET_CAPTURE_SIGNAL_RMS},
    {"GetCaptureSignalPeak", WS_CMD_GET_CAPTURE_SIGNAL_PEAK},
    {"GetPlaybackSignalRms", WS_CMD_GET_PLAYBACK_SIGNAL_RMS},
    {"GetPlaybackSignalPeak", WS_CMD_GET_PLAYBACK_SIGNAL_PEAK},
    {"GetCaptureSignalRmsSinceLast", WS_CMD_GET_CAPTURE_SIGNAL_RMS_SINCE_LAST},
    {"GetCaptureSignalPeakSinceLast",
     WS_CMD_GET_CAPTURE_SIGNAL_PEAK_SINCE_LAST},
    {"GetPlaybackSignalRmsSinceLast",
     WS_CMD_GET_PLAYBACK_SIGNAL_RMS_SINCE_LAST},
    {"GetPlaybackSignalPeakSinceLast",
     WS_CMD_GET_PLAYBACK_SIGNAL_PEAK_SINCE_LAST},
    {"GetCaptureSignalRmsSince", WS_CMD_GET_CAPTURE_SIGNAL_RMS_SINCE},
    {"GetCaptureSignalPeakSince", WS_CMD_GET_CAPTURE_SIGNAL_PEAK_SINCE},
    {"GetPlaybackSignalRmsSince", WS_CMD_GET_PLAYBACK_SIGNAL_RMS_SINCE},
    {"GetPlaybackSignalPeakSince", WS_CMD_GET_PLAYBACK_SIGNAL_PEAK_SINCE},
    {"GetSignalLevels", WS_CMD_GET_SIGNAL_LEVELS},
    {"GetSignalLevelsSinceLast", WS_CMD_GET_SIGNAL_LEVELS_SINCE_LAST},
    {"GetSignalLevelsSince", WS_CMD_GET_SIGNAL_LEVELS_SINCE},
    {"GetSignalPeaksSinceStart", WS_CMD_GET_SIGNAL_PEAKS_SINCE_START},
    {"ResetSignalPeaksSinceStart", WS_CMD_RESET_SIGNAL_PEAKS_SINCE_START},
    {"GetChannelLabels", WS_CMD_GET_CHANNEL_LABELS},
    {"GetSignalRange", WS_CMD_GET_SIGNAL_RANGE},
    {"GetConfigFilePath", WS_CMD_GET_CONFIG_FILE_PATH},
    {"GetPreviousConfig", WS_CMD_GET_PREVIOUS_CONFIG},
    {"GetStateFilePath", WS_CMD_GET_STATE_FILE_PATH},
    {"GetStateFileUpdated", WS_CMD_GET_STATE_FILE_UPDATED},
    {"GetConfig", WS_CMD_GET_CONFIG},
    {"GetConfigJson", WS_CMD_GET_CONFIG_JSON},
    {"GetConfigTitle", WS_CMD_GET_CONFIG_TITLE},
    {"GetConfigDescription", WS_CMD_GET_CONFIG_DESCRIPTION},
    {"Reload", WS_CMD_RELOAD},
    {"Stop", WS_CMD_STOP},
    {"Exit", WS_CMD_EXIT},
    {"SetConfigFilePath", WS_CMD_SET_CONFIG_FILE_PATH},
    {"SetConfigJson", WS_CMD_SET_CONFIG_JSON},
    {"GetConfigValue", WS_CMD_GET_CONFIG_VALUE},
    {"SetConfigValue", WS_CMD_SET_CONFIG_VALUE},
    {"PatchConfig", WS_CMD_PATCH_CONFIG},
    {"ReadConfigJson", WS_CMD_READ_CONFIG_JSON},
    {"ValidateConfigJson", WS_CMD_VALIDATE_CONFIG_JSON},
    {"SubscribeState", WS_CMD_SUBSCRIBE_STATE},
    {"SubscribeVuLevels", WS_CMD_SUBSCRIBE_VU_LEVELS},
    {"SubscribeSignalLevels", WS_CMD_SUBSCRIBE_SIGNAL_LEVELS},
    {"SubscribeSpectrum", WS_CMD_SUBSCRIBE_SPECTRUM},
    {"StopSubscription", WS_CMD_STOP_SUBSCRIPTION}};

static websocket_command_t lookup_command(const char* name) {
  if (!name) return WS_CMD_UNKNOWN;
  size_t count = sizeof(kCommandMap) / sizeof(kCommandMap[0]);
  for (size_t i = 0; i < count; i++) {
    if (strcmp(kCommandMap[i].name, name) == 0) {
      return kCommandMap[i].type;
    }
  }
  return WS_CMD_UNKNOWN;
}

/**
 * @brief Maps an audio backend error type to a WebSocket control protocol error
 * key.
 *
 * @param type The audio backend error type.
 * @return The corresponding error name string.
 */
static const char* get_websocket_error_key(audio_backend_error_type_t type) {
  switch (type) {
    case AUDIO_BACKEND_ERR_CONFIG_PARSE:
      return "ConfigValidationError";
    case AUDIO_BACKEND_ERR_DEVICE_NOT_FOUND:
      return "DeviceNotFoundError";
    case AUDIO_BACKEND_ERR_DEVICE_BUSY:
      return "DeviceBusyError";
    default:
      return "DeviceError";
  }
}

static const char* get_websocket_device_error_key(device_error_type_t type) {
  switch (type) {
    case DEVICE_ERROR_NOT_FOUND:
      return "DeviceNotFoundError";
    case DEVICE_ERROR_BUSY:
      return "DeviceBusyError";
    default:
      return "DeviceError";
  }
}

/**
 * @brief Constructs a standard JSON reply string for the control protocol.
 *
 * Formats a reply of the form:
 * `{"Command":{"result":RESULT_STR,"value":VALUE_STR}}`.
 *
 * @param cmd The command name.
 * @param res_str The result status string (e.g. `"Ok"` or error string).
 * @param val_str Optional payload value string (can be NULL or empty).
 * @param out Output buffer.
 * @param max_len Maximum length of the output buffer.
 */
static void json_reply(const char* cmd, const char* res_str,
                       const char* val_str, dyn_string_t* ds) {
  if (val_str && val_str[0]) {
    dyn_string_printf(ds, "{\"%s\":{\"result\":%s,\"value\":%s}}", cmd, res_str,
                      val_str);
  } else {
    dyn_string_printf(ds, "{\"%s\":{\"result\":%s}}", cmd, res_str);
  }
}

/**
 * @brief Reads a file into a dynamically allocated string (server helper).
 *
 * @param path Path to the file.
 * @return Pointer to the allocated string containing the file contents, or NULL
 * if reading fails.
 */
static char* server_read_file_to_string(const char* path) {
  FILE* fp = fopen(path, "rb");
  if (!fp) return NULL;
  fseek(fp, 0, SEEK_END);
  long len = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  if (len < 0) {
    fclose(fp);
    return NULL;
  }
  char* buf = (char*)calloc((size_t)len + 1, sizeof(char));
  if (!buf) {
    fclose(fp);
    return NULL;
  }
  size_t read_bytes = fread(buf, 1, (size_t)len, fp);
  buf[read_bytes] = '\0';
  fclose(fp);
  return buf;
}

/**
 * @brief Extracts a string value from a flat JSON object by its key.
 *
 * Helper to quickly read simple string fields from JSON without manual cJSON
 * traversal.
 *
 * @param json The JSON string.
 * @param key The key to look up.
 * @param out_buf Output buffer to copy the string into.
 * @param max_len Size of the output buffer.
 * @return true if the key was found and value copied, false otherwise.
 */
static bool extract_json_string_value(const char* json, const char* key,
                                      char* out_buf, size_t max_len) {
  if (!json || !key || !out_buf || max_len == 0) return false;
  cJSON* root = cJSON_Parse(json);
  if (!root) return false;
  cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
  bool success = false;
  if (cJSON_IsString(item) && item->valuestring) {
    strncpy(out_buf, item->valuestring, max_len - 1);
    out_buf[max_len - 1] = '\0';
    success = true;
  }
  cJSON_Delete(root);
  return success;
}

/**
 * @brief Locates a cJSON node matching a RFC 6901 JSON pointer.
 *
 * Recursively navigates through JSON objects and arrays matching segments of
 * the pointer.
 *
 * @param root The root cJSON node.
 * @param pointer The JSON pointer string (e.g. "/devices/playback/device").
 * @param out_parent Optional output pointer to store the located node's parent.
 * @param out_key Optional output pointer to store the key matching the node in
 * its parent.
 * @param out_index Optional output pointer to store the index matching the node
 * in its parent array.
 * @return The located cJSON node, or NULL if not found.
 */
static cJSON* cjson_locate_pointer(cJSON* root, const char* pointer,
                                   cJSON** out_parent, const char** out_key,
                                   int* out_index) {
  if (!root || !pointer) return NULL;
  const char* ptr = pointer;
  if (*ptr == '/') ptr++;
  cJSON* curr = root;
  cJSON* parent = NULL;
  const char* last_key = NULL;
  int last_idx = -1;

  while (*ptr && curr) {
    char segment[128];
    size_t seg_len = 0;
    while (*ptr && *ptr != '/' && seg_len < sizeof(segment) - 1) {
      segment[seg_len++] = *ptr++;
    }
    segment[seg_len] = '\0';
    if (*ptr == '/') ptr++;

    parent = curr;
    if (cJSON_IsObject(curr)) {
      cJSON* child = curr->child;
      curr = NULL;
      last_key = NULL;
      while (child) {
        if (strcmp(child->string, segment) == 0) {
          curr = child;
          last_key = child->string;
          break;
        }
        child = child->next;
      }
      last_idx = -1;
    } else if (cJSON_IsArray(curr)) {
      char* endptr = NULL;
      int idx = (int)strtol(segment, &endptr, 10);
      if (endptr == segment || *endptr != '\0') return NULL;
      curr = cJSON_GetArrayItem(curr, idx);
      last_idx = idx;
      last_key = NULL;
    } else {
      return NULL;
    }
  }

  if (out_parent) *out_parent = parent;
  if (out_key) *out_key = last_key;
  if (out_index) *out_index = last_idx;
  return curr;
}

/**
 * @brief Extracts the JSON fragment at the specified JSON pointer as a string.
 *
 * @param json The source JSON string.
 * @param pointer JSON pointer.
 * @param out_val Output buffer to copy the printed JSON fragment.
 * @param max_len Size of the output buffer.
 * @return true if successful, false otherwise.
 */
static bool server_get_value_at_pointer(const char* json, const char* pointer,
                                        char* out_val, size_t max_len) {
  cJSON* root = cJSON_Parse(json);
  if (!root) return false;
  cJSON* node = cjson_locate_pointer(root, pointer, NULL, NULL, NULL);
  if (!node) {
    cJSON_Delete(root);
    return false;
  }
  char* printed = cJSON_PrintUnformatted(node);
  if (printed) {
    strncpy(out_val, printed, max_len - 1);
    out_val[max_len - 1] = '\0';
    free(printed);
    cJSON_Delete(root);
    return true;
  }
  cJSON_Delete(root);
  return false;
}

/**
 * @brief Replaces or inserts a JSON value at the specified JSON pointer
 * location.
 *
 * Parses the new value string, locates the parent at the pointer, and replaces
 * the element. Returns a newly allocated string containing the updated JSON.
 *
 * @param json The source JSON string.
 * @param pointer JSON pointer.
 * @param new_val_str The new value as a JSON fragment string.
 * @return A newly allocated string containing the serialized updated JSON, or
 * NULL on failure.
 */
static char* server_set_value_at_pointer_str(const char* json,
                                             const char* pointer,
                                             const char* new_val_str) {
  cJSON* root = cJSON_Parse(json);
  if (!root) return NULL;

  cJSON* parent = NULL;
  const char* key = NULL;
  int idx = -1;
  cJSON* target = cjson_locate_pointer(root, pointer, &parent, &key, &idx);
  (void)target;
  if (!parent) {
    cJSON_Delete(root);
    return NULL;
  }

  cJSON* new_node = cJSON_Parse(new_val_str);
  if (!new_node) {
    cJSON_Delete(root);
    return NULL;
  }

  if (key) {
    cJSON_ReplaceItemInObject(parent, key, new_node);
  } else if (idx != -1) {
    cJSON_ReplaceItemInArray(parent, idx, new_node);
  } else {
    cJSON_Delete(new_node);
    cJSON_Delete(root);
    return NULL;
  }

  char* updated_json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  return updated_json;
}

/**
 * @brief Applies a JSON Merge Patch (RFC 7396) to a target cJSON object.
 *
 * Modifies the target cJSON object in-place according to the rules of JSON
 * Merge Patch. Null values in the patch delete corresponding fields in the
 * target.
 *
 * @param target The target cJSON object to patch.
 * @param patch The patch cJSON object.
 */
static void cjson_merge_patch(cJSON* target, const cJSON* patch) {
  if (!target || !patch) return;
  if (!cJSON_IsObject(target) || !cJSON_IsObject(patch)) return;

  cJSON* patch_child = patch->child;
  while (patch_child) {
    cJSON* target_child =
        cJSON_GetObjectItemCaseSensitive(target, patch_child->string);
    if (cJSON_IsNull(patch_child)) {
      if (target_child) {
        cJSON_DeleteItemFromObject(target, patch_child->string);
      }
    } else if (cJSON_IsObject(patch_child)) {
      if (target_child && cJSON_IsObject(target_child)) {
        cjson_merge_patch(target_child, patch_child);
      } else {
        cJSON* duplicated = cJSON_Duplicate(patch_child, true);
        if (target_child) {
          cJSON_ReplaceItemInObject(target, patch_child->string, duplicated);
        } else {
          cJSON_AddItemToObject(target, patch_child->string, duplicated);
        }
      }
    } else {
      cJSON* duplicated = cJSON_Duplicate(patch_child, true);
      if (target_child) {
        cJSON_ReplaceItemInObject(target, patch_child->string, duplicated);
      } else {
        cJSON_AddItemToObject(target, patch_child->string, duplicated);
      }
    }
    patch_child = patch_child->next;
  }
}

/**
 * @brief Serializes an audio device descriptor struct into its JSON
 * representation.
 *
 * Formats details of the device's name, capabilities, sample rates, formats and
 * channels.
 *
 * @param desc Pointer to the device descriptor.
 * @param out Output buffer.
 * @param max_len Maximum length of the output buffer.
 */
static char* format_device_descriptor(const audio_device_descriptor_t* desc) {
  if (!desc) return strdup("null");
  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "name", desc->name);

  cJSON* cs_arr = cJSON_CreateArray();
  cJSON_AddItemToObject(root, "capability_sets", cs_arr);

  for (size_t cs_idx = 0; cs_idx < desc->capability_sets_count; cs_idx++) {
    const device_capability_set_t* cs = &desc->capability_sets[cs_idx];
    cJSON* cs_obj = cJSON_CreateObject();
    cJSON_AddItemToArray(cs_arr, cs_obj);

    cJSON* caps_arr = cJSON_CreateArray();
    cJSON_AddItemToObject(cs_obj, "capabilities", caps_arr);

    for (size_t c_idx = 0; c_idx < cs->capabilities_count; c_idx++) {
      const channel_capability_t* cap = &cs->capabilities[c_idx];
      cJSON* cap_obj = cJSON_CreateObject();
      cJSON_AddItemToArray(caps_arr, cap_obj);

      cJSON_AddNumberToObject(cap_obj, "channels", cap->channels);

      cJSON* sr_arr = cJSON_CreateArray();
      cJSON_AddItemToObject(cap_obj, "samplerates", sr_arr);

      for (size_t s_idx = 0; s_idx < cap->samplerates_count; s_idx++) {
        const samplerate_capability_t* sr = &cap->samplerates[s_idx];
        cJSON* sr_obj = cJSON_CreateObject();
        cJSON_AddItemToArray(sr_arr, sr_obj);

        cJSON_AddNumberToObject(sr_obj, "samplerate", sr->samplerate);

        cJSON* formats_arr = cJSON_CreateArray();
        cJSON_AddItemToObject(sr_obj, "formats", formats_arr);

        for (size_t f_idx = 0; f_idx < sr->formats_count; f_idx++) {
          cJSON_AddItemToArray(formats_arr,
                               cJSON_CreateString(sr->formats[f_idx]));
        }
      }
    }
  }
  char* str = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  return str;
}

/**
 * @brief Serializes a frequency spectrum structure into a JSON object string.
 *
 * Formats frequencies and magnitudes arrays.
 *
 * @param spec Pointer to the spectrum structure.
 * @param out Output buffer.
 * @param max_len Maximum length of the output buffer.
 */
static cJSON* serialize_spectrum(const spectrum_t* spec) {
  if (!spec || spec->count == 0) return cJSON_CreateNull();
  cJSON* root = cJSON_CreateObject();
  cJSON_AddItemToObject(
      root, "frequencies",
      cJSON_CreateDoubleArray(spec->frequencies, (int)spec->count));
  cJSON_AddItemToObject(
      root, "magnitudes",
      cJSON_CreateDoubleArray(spec->magnitudes, (int)spec->count));
  return root;
}

/**
 * @brief Helper to handle volume adjustments (relative change) on a specific
 * fader.
 *
 * Validates the current running status, fetches processing parameters, computes
 * the new volume clamped to the bounds, applies it, and formats the JSON
 * response.
 *
 * @param server Pointer to the WebSocket server.
 * @param fader The fader index to adjust.
 * @param delta The volume change in dB.
 * @param min_vol Minimum volume limit in dB.
 * @param max_vol Maximum volume limit in dB.
 * @param out_response Output buffer for the JSON reply.
 * @param max_len Maximum length of the response buffer.
 * @param cmd_name Command name used for the reply key.
 * @return true if handled, false otherwise.
 */
static bool server_handle_adjust_volume_fader(websocket_server_t* server,
                                              fader_t fader, double delta,
                                              double min_vol, double max_vol,
                                              dyn_string_t* ds,
                                              const char* cmd_name) {
  state_update_t status;
  if (!server || !server->engine || !server->engine->get_status ||
      !server->engine->get_status(server->engine->ctx, &status) ||
      status.state != PROCESSING_STATE_RUNNING) {
    reply_error(cmd_name, "ProcessingNotRunningError", NULL, ds);
    return true;
  }
  if (!server->engine->get_fader_volume) {
    reply_error(cmd_name, "UnknownError", NULL, ds);
    return true;
  }

  double current = server->engine->get_fader_volume(server->engine->ctx, fader);
  double new_vol = current + delta;
  if (new_vol < min_vol) new_vol = min_vol;
  if (new_vol > max_vol) new_vol = max_vol;

  if (server->engine->set_fader_volume) {
    server->engine->set_fader_volume(server->engine->ctx, fader, (float)new_vol,
                                     false);
  }

  if (strcmp(cmd_name, "AdjustVolume") == 0) {
    reply_ok(cmd_name, cJSON_CreateNumber(new_vol), ds);
  } else {
    cJSON* arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)fader));
    cJSON_AddItemToArray(arr, cJSON_CreateNumber(new_vol));
    reply_ok(cmd_name, arr, ds);
  }
  return true;
}

static void handle_cmd_get_volume(websocket_server_t* server, int client_idx,
                                  const char* cmd_name, cJSON* arg,
                                  dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  state_update_t status;
  if (server && server->engine && server->engine->get_status &&
      server->engine->get_status(server->engine->ctx, &status) &&
      status.state == PROCESSING_STATE_RUNNING) {
    double vol =
        (server->engine->get_fader_volume)
            ? server->engine->get_fader_volume(server->engine->ctx, FADER_MAIN)
            : 0.0;
    reply_ok(cmd_name, cJSON_CreateNumber(vol), ds);
  } else {
    reply_error(cmd_name, "ProcessingNotRunningError", NULL, ds);
  }
}

static void handle_cmd_set_volume(websocket_server_t* server, int client_idx,
                                  const char* cmd_name, cJSON* arg,
                                  dyn_string_t* ds) {
  (void)client_idx;
  double vol = 0.0;
  if (arg && cJSON_IsNumber(arg)) {
    vol = arg->valuedouble;
    state_update_t status;
    if (server && server->engine && server->engine->get_status &&
        server->engine->get_status(server->engine->ctx, &status) &&
        status.state == PROCESSING_STATE_RUNNING) {
      if (server->engine->set_fader_volume) {
        server->engine->set_fader_volume(server->engine->ctx, FADER_MAIN, vol,
                                         false);
      }
      reply_ok(cmd_name, NULL, ds);
    } else {
      reply_error(cmd_name, "ProcessingNotRunningError", NULL, ds);
    }
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "Could not parse volume");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_cmd_get_mute(websocket_server_t* server, int client_idx,
                                const char* cmd_name, cJSON* arg,
                                dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  state_update_t status;
  if (server && server->engine && server->engine->get_status &&
      server->engine->get_status(server->engine->ctx, &status) &&
      status.state == PROCESSING_STATE_RUNNING) {
    bool mute =
        (server->engine->is_fader_muted)
            ? server->engine->is_fader_muted(server->engine->ctx, FADER_MAIN)
            : false;
    reply_ok(cmd_name, cJSON_CreateBool(mute), ds);
  } else {
    reply_error(cmd_name, "ProcessingNotRunningError", NULL, ds);
  }
}

static void handle_cmd_set_mute(websocket_server_t* server, int client_idx,
                                const char* cmd_name, cJSON* arg,
                                dyn_string_t* ds) {
  (void)client_idx;
  bool mute = false;
  if (arg && cJSON_IsBool(arg)) {
    mute = cJSON_IsTrue(arg);
    state_update_t status;
    if (server && server->engine && server->engine->get_status &&
        server->engine->get_status(server->engine->ctx, &status) &&
        status.state == PROCESSING_STATE_RUNNING) {
      if (server->engine->set_fader_mute) {
        server->engine->set_fader_mute(server->engine->ctx, FADER_MAIN, mute);
      }
      reply_ok(cmd_name, NULL, ds);
    } else {
      reply_error(cmd_name, "ProcessingNotRunningError", NULL, ds);
    }
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError", "Could not parse mute");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_cmd_toggle_mute(websocket_server_t* server, int client_idx,
                                   const char* cmd_name, cJSON* arg,
                                   dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  state_update_t status;
  if (server && server->engine && server->engine->get_status &&
      server->engine->get_status(server->engine->ctx, &status) &&
      status.state == PROCESSING_STATE_RUNNING) {
    bool was_muted =
        (server->engine->is_fader_muted)
            ? server->engine->is_fader_muted(server->engine->ctx, FADER_MAIN)
            : false;
    if (server->engine->set_fader_mute) {
      server->engine->set_fader_mute(server->engine->ctx, FADER_MAIN,
                                     !was_muted);
    }
    reply_ok(cmd_name, cJSON_CreateBool(!was_muted), ds);
  } else {
    reply_error(cmd_name, "ProcessingNotRunningError", NULL, ds);
  }
}

static void handle_cmd_get_faders(websocket_server_t* server, int client_idx,
                                  const char* cmd_name, cJSON* arg,
                                  dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  state_update_t status;
  if (server && server->engine && server->engine->get_status &&
      server->engine->get_status(server->engine->ctx, &status) &&
      status.state == PROCESSING_STATE_RUNNING) {
    cJSON* arr = cJSON_CreateArray();
    for (int i = 0; i < FADER_COUNT; i++) {
      cJSON* obj = cJSON_CreateObject();
      double vol = (server->engine->get_fader_volume)
                       ? server->engine->get_fader_volume(server->engine->ctx,
                                                          (fader_t)i)
                       : 0.0;
      bool mute =
          (server->engine->is_fader_muted)
              ? server->engine->is_fader_muted(server->engine->ctx, (fader_t)i)
              : false;
      cJSON_AddNumberToObject(obj, "volume", vol);
      cJSON_AddBoolToObject(obj, "mute", mute);
      cJSON_AddItemToArray(arr, obj);
    }
    reply_ok(cmd_name, arr, ds);
  } else {
    reply_error(cmd_name, "ProcessingNotRunningError", NULL, ds);
  }
}

static void handle_cmd_get_fader_volume(websocket_server_t* server,
                                        int client_idx, const char* cmd_name,
                                        cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  if (arg && cJSON_IsNumber(arg)) {
    int idx = arg->valueint;
    state_update_t status;
    if (server && server->engine && server->engine->get_status &&
        server->engine->get_status(server->engine->ctx, &status) &&
        status.state == PROCESSING_STATE_RUNNING) {
      if (idx >= 0 && idx < FADER_COUNT) {
        double vol = (server->engine->get_fader_volume)
                         ? server->engine->get_fader_volume(server->engine->ctx,
                                                            (fader_t)idx)
                         : 0.0;
        cJSON* arr = cJSON_CreateArray();
        cJSON_AddItemToArray(arr, cJSON_CreateNumber(idx));
        cJSON_AddItemToArray(arr, cJSON_CreateNumber(vol));
        reply_ok(cmd_name, arr, ds);
      } else {
        reply_error(cmd_name, "InvalidFaderError", NULL, ds);
      }
    } else {
      reply_error(cmd_name, "ProcessingNotRunningError", NULL, ds);
    }
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "Could not parse fader index");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_cmd_set_fader_volume(websocket_server_t* server,
                                        int client_idx, const char* cmd_name,
                                        cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  int idx = -1;
  double vol = 0.0;
  bool ok = false;
  if (arg && cJSON_IsArray(arg) && cJSON_GetArraySize(arg) == 2) {
    cJSON* idx_node = cJSON_GetArrayItem(arg, 0);
    cJSON* vol_node = cJSON_GetArrayItem(arg, 1);
    if (idx_node && vol_node && cJSON_IsNumber(idx_node) &&
        cJSON_IsNumber(vol_node)) {
      idx = idx_node->valueint;
      vol = vol_node->valuedouble;
      ok = true;
    }
  }
  if (ok) {
    state_update_t status;
    if (server && server->engine && server->engine->get_status &&
        server->engine->get_status(server->engine->ctx, &status) &&
        status.state == PROCESSING_STATE_RUNNING) {
      if (idx >= 0 && idx < FADER_COUNT) {
        if (server->engine->set_fader_volume) {
          server->engine->set_fader_volume(server->engine->ctx, (fader_t)idx,
                                           vol, false);
        }
        reply_ok(cmd_name, NULL, ds);
      } else {
        reply_error(cmd_name, "InvalidFaderError", NULL, ds);
      }
    } else {
      reply_error(cmd_name, "ProcessingNotRunningError", NULL, ds);
    }
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "Could not parse fader index/volume array");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_cmd_set_fader_external_volume(websocket_server_t* server,
                                                 int client_idx,
                                                 const char* cmd_name,
                                                 cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  int idx = -1;
  double vol = 0.0;
  bool ok = false;
  if (arg && cJSON_IsArray(arg) && cJSON_GetArraySize(arg) == 2) {
    cJSON* idx_node = cJSON_GetArrayItem(arg, 0);
    cJSON* vol_node = cJSON_GetArrayItem(arg, 1);
    if (idx_node && vol_node && cJSON_IsNumber(idx_node) &&
        cJSON_IsNumber(vol_node)) {
      idx = idx_node->valueint;
      vol = vol_node->valuedouble;
      ok = true;
    }
  }
  if (ok) {
    state_update_t status;
    if (server && server->engine && server->engine->get_status &&
        server->engine->get_status(server->engine->ctx, &status) &&
        status.state == PROCESSING_STATE_RUNNING) {
      if (idx >= 0 && idx < FADER_COUNT) {
        if (server->engine->set_fader_volume) {
          server->engine->set_fader_volume(server->engine->ctx, (fader_t)idx,
                                           vol, true);
        }
        reply_ok(cmd_name, NULL, ds);
      } else {
        reply_error(cmd_name, "InvalidFaderError", NULL, ds);
      }
    } else {
      reply_error(cmd_name, "ProcessingNotRunningError", NULL, ds);
    }
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(
        err, "InvalidRequestError",
        "Could not parse fader index/external volume array");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_cmd_get_fader_mute(websocket_server_t* server,
                                      int client_idx, const char* cmd_name,
                                      cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  if (arg && cJSON_IsNumber(arg)) {
    int idx = arg->valueint;
    state_update_t status;
    if (server && server->engine && server->engine->get_status &&
        server->engine->get_status(server->engine->ctx, &status) &&
        status.state == PROCESSING_STATE_RUNNING) {
      if (idx >= 0 && idx < FADER_COUNT) {
        bool mute = (server->engine->is_fader_muted)
                        ? server->engine->is_fader_muted(server->engine->ctx,
                                                         (fader_t)idx)
                        : false;
        cJSON* arr = cJSON_CreateArray();
        cJSON_AddItemToArray(arr, cJSON_CreateNumber(idx));
        cJSON_AddItemToArray(arr, cJSON_CreateBool(mute));
        reply_ok(cmd_name, arr, ds);
      } else {
        reply_error(cmd_name, "InvalidFaderError", NULL, ds);
      }
    } else {
      reply_error(cmd_name, "ProcessingNotRunningError", NULL, ds);
    }
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "Could not parse fader index");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_cmd_set_fader_mute(websocket_server_t* server,
                                      int client_idx, const char* cmd_name,
                                      cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  int idx = -1;
  bool mute = false;
  bool ok = false;
  if (arg && cJSON_IsArray(arg) && cJSON_GetArraySize(arg) == 2) {
    cJSON* idx_node = cJSON_GetArrayItem(arg, 0);
    cJSON* mute_node = cJSON_GetArrayItem(arg, 1);
    if (idx_node && mute_node && cJSON_IsNumber(idx_node) &&
        cJSON_IsBool(mute_node)) {
      idx = idx_node->valueint;
      mute = cJSON_IsTrue(mute_node);
      ok = true;
    }
  }
  if (ok) {
    state_update_t status;
    if (server && server->engine && server->engine->get_status &&
        server->engine->get_status(server->engine->ctx, &status) &&
        status.state == PROCESSING_STATE_RUNNING) {
      if (idx >= 0 && idx < FADER_COUNT) {
        if (server->engine->set_fader_mute) {
          server->engine->set_fader_mute(server->engine->ctx, (fader_t)idx,
                                         mute);
        }
        reply_ok(cmd_name, NULL, ds);
      } else {
        reply_error(cmd_name, "InvalidFaderError", NULL, ds);
      }
    } else {
      reply_error(cmd_name, "ProcessingNotRunningError", NULL, ds);
    }
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "Could not parse fader index/mute array");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_cmd_toggle_fader_mute(websocket_server_t* server,
                                         int client_idx, const char* cmd_name,
                                         cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  if (arg && cJSON_IsNumber(arg)) {
    int idx = arg->valueint;
    state_update_t status;
    if (server && server->engine && server->engine->get_status &&
        server->engine->get_status(server->engine->ctx, &status) &&
        status.state == PROCESSING_STATE_RUNNING) {
      if (idx >= 0 && idx < FADER_COUNT) {
        bool was_muted = (server->engine->is_fader_muted)
                             ? server->engine->is_fader_muted(
                                   server->engine->ctx, (fader_t)idx)
                             : false;
        if (server->engine->set_fader_mute) {
          server->engine->set_fader_mute(server->engine->ctx, (fader_t)idx,
                                         !was_muted);
        }
        cJSON* arr = cJSON_CreateArray();
        cJSON_AddItemToArray(arr, cJSON_CreateNumber(idx));
        cJSON_AddItemToArray(arr, cJSON_CreateBool(!was_muted));
        reply_ok(cmd_name, arr, ds);
      } else {
        reply_error(cmd_name, "InvalidFaderError", NULL, ds);
      }
    } else {
      reply_error(cmd_name, "ProcessingNotRunningError", NULL, ds);
    }
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "Could not parse fader index");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_cmd_adjust_volume(websocket_server_t* server, int client_idx,
                                     const char* cmd_name, cJSON* arg,
                                     dyn_string_t* ds) {
  (void)client_idx;
  double delta = 0.0;
  double min_vol = -150.0;
  double max_vol = 50.0;
  if (parse_value_with_optional_limits(arg, &delta, &min_vol, &max_vol)) {
    server_handle_adjust_volume_fader(server, FADER_MAIN, delta, min_vol,
                                      max_vol, ds, cmd_name);
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "Could not parse AdjustVolume argument");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_cmd_adjust_fader_volume(websocket_server_t* server,
                                           int client_idx, const char* cmd_name,
                                           cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  int idx = -1;
  double delta = 0.0;
  double min_vol = -150.0;
  double max_vol = 50.0;
  bool ok = false;
  if (arg && cJSON_IsArray(arg) && cJSON_GetArraySize(arg) == 2) {
    cJSON* idx_node = cJSON_GetArrayItem(arg, 0);
    cJSON* val_limits = cJSON_GetArrayItem(arg, 1);
    if (idx_node && cJSON_IsNumber(idx_node)) {
      idx = idx_node->valueint;
      ok = parse_value_with_optional_limits(val_limits, &delta, &min_vol,
                                            &max_vol);
    }
  }
  if (ok) {
    if (idx >= 0 && idx < FADER_COUNT) {
      server_handle_adjust_volume_fader(server, (fader_t)idx, delta, min_vol,
                                        max_vol, ds, cmd_name);
    } else {
      reply_error(cmd_name, "InvalidFaderError", NULL, ds);
    }
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "Could not parse AdjustFaderVolume array");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_cmd_subscribe_state(websocket_server_t* server,
                                       int client_idx, const char* cmd_name,
                                       cJSON* arg, dyn_string_t* ds) {
  (void)arg;
  if (server) {
    server->client_sessions[client_idx].state_subscribed = true;
  }
  reply_ok(cmd_name, NULL, ds);
}

static void handle_cmd_subscribe_vu_levels(websocket_server_t* server,
                                           int client_idx, const char* cmd_name,
                                           cJSON* arg, dyn_string_t* ds) {
  double max_rate = 0.0;
  double attack = 0.0;
  double release = 0.0;
  if (arg && cJSON_IsObject(arg)) {
    cJSON* item;
    item = cJSON_GetObjectItemCaseSensitive(arg, "max_rate");
    if (item && cJSON_IsNumber(item)) max_rate = item->valuedouble;
    item = cJSON_GetObjectItemCaseSensitive(arg, "attack");
    if (item && cJSON_IsNumber(item)) attack = item->valuedouble;
    item = cJSON_GetObjectItemCaseSensitive(arg, "release");
    if (item && cJSON_IsNumber(item)) release = item->valuedouble;
  }
  if (attack < 0.0 || attack > 60000.0 || release < 0.0 || release > 60000.0) {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(
        err, "InvalidValueError",
        "attack and release must be between 0 and 60000 ms");
    reply_error(cmd_name, NULL, err, ds);
  } else {
    if (server) {
      server->client_sessions[client_idx].vu_subscribed = true;
      server->client_sessions[client_idx].vu_max_rate = max_rate;
      server->client_sessions[client_idx].vu_attack = attack;
      server->client_sessions[client_idx].vu_release = release;
      server->client_sessions[client_idx].last_vu_push_time = 0;
    }
    reply_ok(cmd_name, NULL, ds);
  }
}

static void handle_cmd_subscribe_signal_levels(websocket_server_t* server,
                                               int client_idx,
                                               const char* cmd_name, cJSON* arg,
                                               dyn_string_t* ds) {
  char side[16] = "";
  if (arg && cJSON_IsString(arg) && arg->valuestring) {
    strncpy(side, arg->valuestring, sizeof(side) - 1);
  }
  if (strcmp(side, "playback") == 0 || strcmp(side, "capture") == 0 ||
      strcmp(side, "both") == 0) {
    if (server) {
      server->client_sessions[client_idx].signal_levels_subscribed = true;
      snprintf(server->client_sessions[client_idx].signal_levels_side,
               sizeof(server->client_sessions[client_idx].signal_levels_side),
               "%s", side);
    }
    reply_ok(cmd_name, NULL, ds);
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidValueError",
                            "side must be playback, capture, or both");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_cmd_subscribe_spectrum(websocket_server_t* server,
                                          int client_idx, const char* cmd_name,
                                          cJSON* arg, dyn_string_t* ds) {
  bool is_capture = true;
  uint32_t channel = 0;
  double min_freq = 20.0;
  double max_freq = 20000.0;
  uint32_t n_bins = 1024;
  double max_rate = 0.0;
  bool ok = false;
  if (arg && cJSON_IsObject(arg)) {
    cJSON* item;
    item = cJSON_GetObjectItemCaseSensitive(arg, "is_capture");
    if (item && cJSON_IsBool(item)) is_capture = cJSON_IsTrue(item);
    item = cJSON_GetObjectItemCaseSensitive(arg, "channel");
    if (item && cJSON_IsNumber(item)) channel = (uint32_t)item->valueint;
    item = cJSON_GetObjectItemCaseSensitive(arg, "min_freq");
    if (item && cJSON_IsNumber(item)) min_freq = item->valuedouble;
    item = cJSON_GetObjectItemCaseSensitive(arg, "max_freq");
    if (item && cJSON_IsNumber(item)) max_freq = item->valuedouble;
    item = cJSON_GetObjectItemCaseSensitive(arg, "n_bins");
    if (item && cJSON_IsNumber(item)) n_bins = (uint32_t)item->valueint;
    item = cJSON_GetObjectItemCaseSensitive(arg, "max_rate");
    if (item && cJSON_IsNumber(item)) max_rate = item->valuedouble;
    ok = true;
  }
  if (ok) {
    if (server) {
      server->client_sessions[client_idx].spectrum_subscribed = true;
      server->client_sessions[client_idx].spectrum_is_capture = is_capture;
      server->client_sessions[client_idx].spectrum_channel = channel;
      server->client_sessions[client_idx].spectrum_min_freq = min_freq;
      server->client_sessions[client_idx].spectrum_max_freq = max_freq;
      server->client_sessions[client_idx].spectrum_n_bins = n_bins;
      server->client_sessions[client_idx].spectrum_max_rate = max_rate;
      server->client_sessions[client_idx].last_spectrum_push_time = 0;
    }
    reply_ok(cmd_name, NULL, ds);
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "Could not parse SubscribeSpectrum arguments");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_cmd_stop_subscription(websocket_server_t* server,
                                         int client_idx, const char* cmd_name,
                                         cJSON* arg, dyn_string_t* ds) {
  (void)arg;
  if (server) {
    bool active =
        server->client_sessions[client_idx].state_subscribed ||
        server->client_sessions[client_idx].vu_subscribed ||
        server->client_sessions[client_idx].signal_levels_subscribed ||
        server->client_sessions[client_idx].spectrum_subscribed;
    if (active) {
      server->client_sessions[client_idx].state_subscribed = false;
      server->client_sessions[client_idx].vu_subscribed = false;
      server->client_sessions[client_idx].signal_levels_subscribed = false;
      server->client_sessions[client_idx].spectrum_subscribed = false;
      reply_ok(cmd_name, NULL, ds);
    } else {
      cJSON* err = cJSON_CreateObject();
      cJSON_AddStringToObject(err, "InvalidRequestError",
                              "No active subscription");
      reply_error(cmd_name, NULL, err, ds);
    }
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "No active subscription");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_cmd_get_config_file_path(websocket_server_t* server,
                                            int client_idx,
                                            const char* cmd_name, cJSON* arg,
                                            dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  char* path = NULL;
  if (server && server->engine && server->engine->get_config_path) {
    path = server->engine->get_config_path(server->engine->ctx);
  }
  if (path) {
    reply_ok(cmd_name, cJSON_CreateString(path), ds);
    free(path);
  } else {
    reply_ok(cmd_name, cJSON_CreateNull(), ds);
  }
}

static void handle_cmd_get_previous_config(websocket_server_t* server,
                                           int client_idx, const char* cmd_name,
                                           cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  char* prev = NULL;
  if (server && server->engine && server->engine->get_previous_config_json) {
    server->engine->get_previous_config_json(server->engine->ctx, &prev);
  }
  if (prev) {
    reply_ok(cmd_name, cJSON_CreateString(prev), ds);
    free(prev);
  } else {
    reply_ok(cmd_name, cJSON_CreateNull(), ds);
  }
}

static void handle_cmd_get_state_file_path(websocket_server_t* server,
                                           int client_idx, const char* cmd_name,
                                           cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  const char* path = NULL;
  if (server && server->engine && server->engine->get_state_file) {
    path = server->engine->get_state_file(server->engine->ctx);
  }
  if (path) {
    reply_ok(cmd_name, cJSON_CreateString(path), ds);
  } else {
    reply_ok(cmd_name, cJSON_CreateNull(), ds);
  }
}

static void handle_cmd_get_state_file_updated(websocket_server_t* server,
                                              int client_idx,
                                              const char* cmd_name, cJSON* arg,
                                              dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  bool updated = server && server->engine && server->engine->is_state_dirty
                     ? !server->engine->is_state_dirty(server->engine->ctx)
                     : true;
  reply_ok(cmd_name, cJSON_CreateBool(updated), ds);
}

static void handle_cmd_get_config(websocket_server_t* server, int client_idx,
                                  const char* cmd_name, cJSON* arg,
                                  dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  char* json = NULL;
  if (server && server->engine && server->engine->get_active_config_json) {
    server->engine->get_active_config_json(server->engine->ctx, &json);
  }
  if (!json) {
    char* path = NULL;
    if (server && server->engine && server->engine->get_config_path) {
      path = server->engine->get_config_path(server->engine->ctx);
    }
    if (path) {
      json = server_read_file_to_string(path);
      free(path);
    }
  }
  if (json) {
    reply_ok(cmd_name, cJSON_CreateString(json), ds);
    free(json);
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError", "No active config");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_get_config_metadata_helper(websocket_server_t* server,
                                              const char* cmd_name,
                                              const char* key,
                                              dyn_string_t* ds) {
  char* json = NULL;
  if (server && server->engine && server->engine->get_active_config_json) {
    server->engine->get_active_config_json(server->engine->ctx, &json);
  }
  if (!json) {
    char* path = NULL;
    if (server && server->engine && server->engine->get_config_path) {
      path = server->engine->get_config_path(server->engine->ctx);
    }
    if (path) {
      json = server_read_file_to_string(path);
      free(path);
    }
  }
  char val_str[1024] = "";
  if (json && extract_json_string_value(json, key, val_str, sizeof(val_str))) {
    reply_ok(cmd_name, cJSON_CreateString(val_str), ds);
  } else {
    reply_ok(cmd_name, cJSON_CreateNull(), ds);
  }
  if (json) free(json);
}

static void handle_cmd_get_config_title(websocket_server_t* server,
                                        int client_idx, const char* cmd_name,
                                        cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  handle_get_config_metadata_helper(server, cmd_name, "title", ds);
}

static void handle_cmd_get_config_description(websocket_server_t* server,
                                              int client_idx,
                                              const char* cmd_name, cJSON* arg,
                                              dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  handle_get_config_metadata_helper(server, cmd_name, "description", ds);
}

static void handle_cmd_reload(websocket_server_t* server, int client_idx,
                              const char* cmd_name, cJSON* arg,
                              dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  char* path = NULL;
  if (server && server->engine && server->engine->get_config_path) {
    path = server->engine->get_config_path(server->engine->ctx);
  }
  if (path) {
    char* json = server_read_file_to_string(path);
    free(path);
    if (json) {
      audio_backend_error_t err;
      memset(&err, 0, sizeof(err));
      bool ok =
          server && server->engine && server->engine->set_config_json &&
          server->engine->set_config_json(server->engine->ctx, json, &err);
      if (ok) {
        reply_ok(cmd_name, NULL, ds);
      } else {
        cJSON* err_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(err_obj, get_websocket_error_key(err.type),
                                err.message);
        reply_error(cmd_name, NULL, err_obj, ds);
      }
      free(json);
    } else {
      cJSON* err_obj = cJSON_CreateObject();
      cJSON_AddStringToObject(err_obj, "ConfigReadError",
                              "Could not read config file");
      reply_error(cmd_name, NULL, err_obj, ds);
    }
  } else {
    cJSON* err_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(err_obj, "InvalidRequestError",
                            "No config file path set");
    reply_error(cmd_name, NULL, err_obj, ds);
  }
}

static void handle_cmd_stop(websocket_server_t* server, int client_idx,
                            const char* cmd_name, cJSON* arg,
                            dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  if (server && server->engine && server->engine->stop) {
    server->engine->stop(server->engine->ctx);
  }
  reply_ok(cmd_name, NULL, ds);
}

static void handle_cmd_exit(websocket_server_t* server, int client_idx,
                            const char* cmd_name, cJSON* arg,
                            dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  if (server && server->engine && server->engine->stop) {
    server->engine->stop(server->engine->ctx);
  }
  reply_ok(cmd_name, NULL, ds);
}

static void handle_cmd_set_config_file_path(websocket_server_t* server,
                                            int client_idx,
                                            const char* cmd_name, cJSON* arg,
                                            dyn_string_t* ds) {
  (void)client_idx;
  if (arg && cJSON_IsString(arg) && arg->valuestring) {
    const char* path = arg->valuestring;
    if (server && server->engine && server->engine->set_config_path) {
      server->engine->set_config_path(server->engine->ctx, path);
    }
    reply_ok(cmd_name, NULL, ds);
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "Could not parse Config File Path");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_cmd_set_config_json(websocket_server_t* server,
                                       int client_idx, const char* cmd_name,
                                       cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  if (arg && cJSON_IsString(arg) && arg->valuestring) {
    const char* new_json = arg->valuestring;
    audio_backend_error_t err;
    memset(&err, 0, sizeof(err));
    bool ok =
        server && server->engine && server->engine->set_config_json &&
        server->engine->set_config_json(server->engine->ctx, new_json, &err);
    if (ok) {
      reply_ok(cmd_name, NULL, ds);
    } else {
      cJSON* err_obj = cJSON_CreateObject();
      cJSON_AddStringToObject(err_obj, get_websocket_error_key(err.type),
                              err.message);
      reply_error(cmd_name, NULL, err_obj, ds);
    }
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "Could not parse Config JSON");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_cmd_get_config_value(websocket_server_t* server,
                                        int client_idx, const char* cmd_name,
                                        cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  if (arg && cJSON_IsString(arg) && arg->valuestring) {
    const char* pointer = arg->valuestring;
    char* json = NULL;
    if (server && server->engine && server->engine->get_active_config_json) {
      server->engine->get_active_config_json(server->engine->ctx, &json);
    }
    if (!json) {
      char* path = NULL;
      if (server && server->engine && server->engine->get_config_path) {
        path = server->engine->get_config_path(server->engine->ctx);
      }
      if (path) {
        json = server_read_file_to_string(path);
        free(path);
      }
    }
    char val_buf[2048] = "";
    if (json &&
        server_get_value_at_pointer(json, pointer, val_buf, sizeof(val_buf))) {
      cJSON* parsed_val = cJSON_Parse(val_buf);
      if (parsed_val) {
        reply_ok(cmd_name, parsed_val, ds);
      } else {
        reply_ok(cmd_name, cJSON_CreateString(val_buf), ds);
      }
    } else {
      cJSON* err = cJSON_CreateObject();
      char msg[256];
      snprintf(msg, sizeof(msg), "Path not found: %s", pointer);
      cJSON_AddStringToObject(err, "InvalidRequestError", msg);
      reply_error(cmd_name, NULL, err, ds);
    }
    if (json) free(json);
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "Could not parse pointer");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_cmd_set_config_value(websocket_server_t* server,
                                        int client_idx, const char* cmd_name,
                                        cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  char pointer[256] = "";
  char* trimmed_val = NULL;
  if (arg && cJSON_IsObject(arg)) {
    cJSON* p_node = cJSON_GetObjectItemCaseSensitive(arg, "pointer");
    cJSON* v_node = cJSON_GetObjectItemCaseSensitive(arg, "value");
    if (p_node && cJSON_IsString(p_node)) {
      strncpy(pointer, p_node->valuestring, sizeof(pointer) - 1);
    }
    if (v_node) {
      trimmed_val = cJSON_PrintUnformatted(v_node);
    }
  }
  if (pointer[0] != '\0' && trimmed_val) {
    char* active_json = NULL;
    if (server && server->engine && server->engine->get_active_config_json) {
      server->engine->get_active_config_json(server->engine->ctx, &active_json);
    }
    if (!active_json) {
      char* path = NULL;
      if (server && server->engine && server->engine->get_config_path) {
        path = server->engine->get_config_path(server->engine->ctx);
      }
      if (path) {
        active_json = server_read_file_to_string(path);
        free(path);
      }
    }
    if (active_json) {
      char* updated_json =
          server_set_value_at_pointer_str(active_json, pointer, trimmed_val);
      if (updated_json) {
        audio_backend_error_t err;
        memset(&err, 0, sizeof(err));
        bool ok = server && server->engine && server->engine->set_config_json &&
                  server->engine->set_config_json(server->engine->ctx,
                                                  updated_json, &err);
        if (ok) {
          reply_ok(cmd_name, NULL, ds);
        } else {
          cJSON* err_obj = cJSON_CreateObject();
          cJSON_AddStringToObject(err_obj, get_websocket_error_key(err.type),
                                  err.message);
          reply_error(cmd_name, NULL, err_obj, ds);
        }
        free(updated_json);
      } else {
        cJSON* err = cJSON_CreateObject();
        char msg[256];
        snprintf(msg, sizeof(msg), "Path not found: %s", pointer);
        cJSON_AddStringToObject(err, "InvalidRequestError", msg);
        reply_error(cmd_name, NULL, err, ds);
      }
      free(active_json);
    } else {
      cJSON* err = cJSON_CreateObject();
      cJSON_AddStringToObject(err, "InvalidRequestError",
                              "No active config to modify");
      reply_error(cmd_name, NULL, err, ds);
    }
    free(trimmed_val);
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "Could not parse SetConfigValue command");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_cmd_patch_config(websocket_server_t* server, int client_idx,
                                    const char* cmd_name, cJSON* arg,
                                    dyn_string_t* ds) {
  (void)client_idx;
  if (arg && cJSON_IsObject(arg)) {
    char* active_json = NULL;
    if (server && server->engine && server->engine->get_active_config_json) {
      server->engine->get_active_config_json(server->engine->ctx, &active_json);
    }
    if (!active_json) {
      char* path = NULL;
      if (server && server->engine && server->engine->get_config_path) {
        path = server->engine->get_config_path(server->engine->ctx);
      }
      if (path) {
        active_json = server_read_file_to_string(path);
        free(path);
      }
    }
    if (active_json) {
      cJSON* target_root = cJSON_Parse(active_json);
      if (target_root) {
        cjson_merge_patch(target_root, arg);
        char* target_json = cJSON_PrintUnformatted(target_root);
        if (target_json) {
          audio_backend_error_t err;
          memset(&err, 0, sizeof(err));
          bool ok = server && server->engine &&
                    server->engine->set_config_json &&
                    server->engine->set_config_json(server->engine->ctx,
                                                    target_json, &err);
          if (ok) {
            reply_ok(cmd_name, NULL, ds);
          } else {
            cJSON* err_obj = cJSON_CreateObject();
            cJSON_AddStringToObject(err_obj, get_websocket_error_key(err.type),
                                    err.message);
            reply_error(cmd_name, NULL, err_obj, ds);
          }
          free(target_json);
        } else {
          cJSON* err = cJSON_CreateObject();
          cJSON_AddStringToObject(err, "InvalidRequestError",
                                  "Failed to format target JSON");
          reply_error(cmd_name, NULL, err, ds);
        }
        cJSON_Delete(target_root);
      } else {
        cJSON* err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "InvalidRequestError",
                                "Failed to parse target JSON");
        reply_error(cmd_name, NULL, err, ds);
      }
      free(active_json);
    } else {
      cJSON* err = cJSON_CreateObject();
      cJSON_AddStringToObject(err, "InvalidRequestError",
                              "No active config to patch");
      reply_error(cmd_name, NULL, err, ds);
    }
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "Could not parse PatchConfig command");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_cmd_read_config_json(websocket_server_t* server,
                                        int client_idx, const char* cmd_name,
                                        cJSON* arg, dyn_string_t* ds) {
  (void)server;
  (void)client_idx;
  if (arg && cJSON_IsString(arg) && arg->valuestring) {
    const char* config_json = arg->valuestring;
    dsp_config_t* parsed = NULL;
    config_error_t cerr;
    memset(&cerr, 0, sizeof(cerr));
    if (config_loader_parse(config_json, &parsed, &cerr) == 0 && parsed) {
      reply_ok(cmd_name, cJSON_CreateString(config_json), ds);
      dsp_config_free(parsed);
    } else {
      cJSON* err = cJSON_CreateObject();
      cJSON_AddStringToObject(err, "ConfigValidationError", cerr.message);
      reply_error(cmd_name, NULL, err, ds);
    }
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "Could not parse input config JSON");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_get_signal_single_helper(websocket_server_t* server,
                                            const char* cmd_name,
                                            bool is_capture, bool is_rms,
                                            dyn_string_t* ds) {
  vu_levels_t vu;
  memset(&vu, 0, sizeof(vu));
  if (server && server->engine && server->engine->get_vu_levels &&
      server->engine->get_vu_levels(server->engine->ctx, &vu)) {
    double* arr = NULL;
    size_t count = 0;
    if (is_capture) {
      arr = is_rms ? vu.capture_rms : vu.capture_peak;
      count = vu.capture_channels;
    } else {
      arr = is_rms ? vu.playback_rms : vu.playback_peak;
      count = vu.playback_channels;
    }
    reply_ok(cmd_name, cJSON_CreateDoubleArray(arr, (int)count), ds);
    free_vu_levels_arrays(&vu);
  } else {
    reply_error(cmd_name, "ProcessingNotRunningError", NULL, ds);
  }
}

static void handle_cmd_get_capture_signal_rms(websocket_server_t* server,
                                              int client_idx,
                                              const char* cmd_name, cJSON* arg,
                                              dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  handle_get_signal_single_helper(server, cmd_name, true, true, ds);
}

static void handle_cmd_get_capture_signal_peak(websocket_server_t* server,
                                               int client_idx,
                                               const char* cmd_name, cJSON* arg,
                                               dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  handle_get_signal_single_helper(server, cmd_name, true, false, ds);
}

static void handle_cmd_get_playback_signal_rms(websocket_server_t* server,
                                               int client_idx,
                                               const char* cmd_name, cJSON* arg,
                                               dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  handle_get_signal_single_helper(server, cmd_name, false, true, ds);
}

static void handle_cmd_get_playback_signal_peak(websocket_server_t* server,
                                                int client_idx,
                                                const char* cmd_name,
                                                cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  handle_get_signal_single_helper(server, cmd_name, false, false, ds);
}

static void handle_get_signal_since_last_helper(websocket_server_t* server,
                                                int client_idx,
                                                const char* cmd_name,
                                                bool is_capture, bool is_rms,
                                                dyn_string_t* ds) {
  state_update_t status;
  if (server && server->engine && server->engine->get_status &&
      server->engine->get_status(server->engine->ctx, &status) &&
      status.state == PROCESSING_STATE_RUNNING) {
    uint64_t since = 0;
    uint64_t now = get_time_ms();
    level_history_t* hist = NULL;
    if (is_capture) {
      if (is_rms) {
        since = server->client_sessions[client_idx].last_cap_rms_time;
        server->client_sessions[client_idx].last_cap_rms_time = now;
        hist = &server->capture_rms_history;
      } else {
        since = server->client_sessions[client_idx].last_cap_peak_time;
        server->client_sessions[client_idx].last_cap_peak_time = now;
        hist = &server->capture_peak_history;
      }
    } else {
      if (is_rms) {
        since = server->client_sessions[client_idx].last_pb_rms_time;
        server->client_sessions[client_idx].last_pb_rms_time = now;
        hist = &server->playback_rms_history;
      } else {
        since = server->client_sessions[client_idx].last_pb_peak_time;
        server->client_sessions[client_idx].last_pb_peak_time = now;
        hist = &server->playback_peak_history;
      }
    }
    size_t ch = hist->channels;
    double* vals = (double*)calloc(ch, sizeof(double));
    if (is_rms) {
      level_history_get_rms_since(hist, since, vals);
    } else {
      level_history_get_max_since(hist, since, vals);
    }
    reply_ok(cmd_name, cJSON_CreateDoubleArray(vals, (int)ch), ds);
    free(vals);
  } else {
    reply_error(cmd_name, "ProcessingNotRunningError", NULL, ds);
  }
}

static void handle_cmd_get_capture_signal_rms_since_last(
    websocket_server_t* server, int client_idx, const char* cmd_name,
    cJSON* arg, dyn_string_t* ds) {
  (void)arg;
  handle_get_signal_since_last_helper(server, client_idx, cmd_name, true, true,
                                      ds);
}

static void handle_cmd_get_capture_signal_peak_since_last(
    websocket_server_t* server, int client_idx, const char* cmd_name,
    cJSON* arg, dyn_string_t* ds) {
  (void)arg;
  handle_get_signal_since_last_helper(server, client_idx, cmd_name, true, false,
                                      ds);
}

static void handle_cmd_get_playback_signal_rms_since_last(
    websocket_server_t* server, int client_idx, const char* cmd_name,
    cJSON* arg, dyn_string_t* ds) {
  (void)arg;
  handle_get_signal_since_last_helper(server, client_idx, cmd_name, false, true,
                                      ds);
}

static void handle_cmd_get_playback_signal_peak_since_last(
    websocket_server_t* server, int client_idx, const char* cmd_name,
    cJSON* arg, dyn_string_t* ds) {
  (void)arg;
  handle_get_signal_since_last_helper(server, client_idx, cmd_name, false,
                                      false, ds);
}

static void handle_get_signal_since_helper(websocket_server_t* server,
                                           const char* cmd_name, cJSON* arg,
                                           bool is_capture, bool is_rms,
                                           dyn_string_t* ds) {
  double secs = 0;
  if (arg && cJSON_IsNumber(arg)) {
    secs = arg->valuedouble;
    state_update_t status;
    if (server && server->engine && server->engine->get_status &&
        server->engine->get_status(server->engine->ctx, &status) &&
        status.state == PROCESSING_STATE_RUNNING) {
      uint64_t now = get_time_ms();
      uint64_t since = now - (uint64_t)(secs * 1000.0);
      level_history_t* hist = NULL;
      if (is_capture) {
        hist = is_rms ? &server->capture_rms_history
                      : &server->capture_peak_history;
      } else {
        hist = is_rms ? &server->playback_rms_history
                      : &server->playback_peak_history;
      }
      size_t ch = hist->channels;
      double* vals = (double*)calloc(ch, sizeof(double));
      if (is_rms) {
        level_history_get_rms_since(hist, since, vals);
      } else {
        level_history_get_max_since(hist, since, vals);
      }
      reply_ok(cmd_name, cJSON_CreateDoubleArray(vals, (int)ch), ds);
      free(vals);
    } else {
      reply_error(cmd_name, "ProcessingNotRunningError", NULL, ds);
    }
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "Could not parse seconds");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_cmd_get_capture_signal_rms_since(websocket_server_t* server,
                                                    int client_idx,
                                                    const char* cmd_name,
                                                    cJSON* arg,
                                                    dyn_string_t* ds) {
  (void)client_idx;
  handle_get_signal_since_helper(server, cmd_name, arg, true, true, ds);
}

static void handle_cmd_get_capture_signal_peak_since(websocket_server_t* server,
                                                     int client_idx,
                                                     const char* cmd_name,
                                                     cJSON* arg,
                                                     dyn_string_t* ds) {
  (void)client_idx;
  handle_get_signal_since_helper(server, cmd_name, arg, true, false, ds);
}

static void handle_cmd_get_playback_signal_rms_since(websocket_server_t* server,
                                                     int client_idx,
                                                     const char* cmd_name,
                                                     cJSON* arg,
                                                     dyn_string_t* ds) {
  (void)client_idx;
  handle_get_signal_since_helper(server, cmd_name, arg, false, true, ds);
}

static void handle_cmd_get_playback_signal_peak_since(
    websocket_server_t* server, int client_idx, const char* cmd_name,
    cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  handle_get_signal_since_helper(server, cmd_name, arg, false, false, ds);
}

static void handle_cmd_get_signal_levels(websocket_server_t* server,
                                         int client_idx, const char* cmd_name,
                                         cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  vu_levels_t vu;
  memset(&vu, 0, sizeof(vu));
  if (server && server->engine && server->engine->get_vu_levels &&
      server->engine->get_vu_levels(server->engine->ctx, &vu)) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddItemToObject(
        root, "playback_rms",
        cJSON_CreateDoubleArray(vu.playback_rms, (int)vu.playback_channels));
    cJSON_AddItemToObject(
        root, "playback_peak",
        cJSON_CreateDoubleArray(vu.playback_peak, (int)vu.playback_channels));
    cJSON_AddItemToObject(
        root, "capture_rms",
        cJSON_CreateDoubleArray(vu.capture_rms, (int)vu.capture_channels));
    cJSON_AddItemToObject(
        root, "capture_peak",
        cJSON_CreateDoubleArray(vu.capture_peak, (int)vu.capture_channels));
    reply_ok(cmd_name, root, ds);
    free_vu_levels_arrays(&vu);
  } else {
    reply_error(cmd_name, "ProcessingNotRunningError", NULL, ds);
  }
}

static void handle_cmd_get_signal_levels_since_last(websocket_server_t* server,
                                                    int client_idx,
                                                    const char* cmd_name,
                                                    cJSON* arg,
                                                    dyn_string_t* ds) {
  (void)arg;
  state_update_t status;
  if (server && server->engine && server->engine->get_status &&
      server->engine->get_status(server->engine->ctx, &status) &&
      status.state == PROCESSING_STATE_RUNNING) {
    uint64_t cap_rms_since =
        server->client_sessions[client_idx].last_cap_rms_time;
    uint64_t cap_pk_since =
        server->client_sessions[client_idx].last_cap_peak_time;
    uint64_t pb_rms_since =
        server->client_sessions[client_idx].last_pb_rms_time;
    uint64_t pb_pk_since =
        server->client_sessions[client_idx].last_pb_peak_time;
    uint64_t now = get_time_ms();
    server->client_sessions[client_idx].last_cap_rms_time = now;
    server->client_sessions[client_idx].last_cap_peak_time = now;
    server->client_sessions[client_idx].last_pb_rms_time = now;
    server->client_sessions[client_idx].last_pb_peak_time = now;

    size_t c_ch = server->capture_rms_history.channels;
    size_t p_ch = server->playback_rms_history.channels;
    double* c_rms = (double*)calloc(c_ch, sizeof(double));
    double* c_pk = (double*)calloc(c_ch, sizeof(double));
    double* p_rms = (double*)calloc(p_ch, sizeof(double));
    double* p_pk = (double*)calloc(p_ch, sizeof(double));

    level_history_get_rms_since(&server->capture_rms_history, cap_rms_since,
                                c_rms);
    level_history_get_max_since(&server->capture_peak_history, cap_pk_since,
                                c_pk);
    level_history_get_rms_since(&server->playback_rms_history, pb_rms_since,
                                p_rms);
    level_history_get_max_since(&server->playback_peak_history, pb_pk_since,
                                p_pk);

    cJSON* root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "playback_rms",
                          cJSON_CreateDoubleArray(p_rms, (int)p_ch));
    cJSON_AddItemToObject(root, "playback_peak",
                          cJSON_CreateDoubleArray(p_pk, (int)p_ch));
    cJSON_AddItemToObject(root, "capture_rms",
                          cJSON_CreateDoubleArray(c_rms, (int)c_ch));
    cJSON_AddItemToObject(root, "capture_peak",
                          cJSON_CreateDoubleArray(c_pk, (int)c_ch));

    reply_ok(cmd_name, root, ds);

    free(c_rms);
    free(c_pk);
    free(p_rms);
    free(p_pk);
  } else {
    reply_error(cmd_name, "ProcessingNotRunningError", NULL, ds);
  }
}

static void handle_cmd_get_signal_levels_since(websocket_server_t* server,
                                               int client_idx,
                                               const char* cmd_name, cJSON* arg,
                                               dyn_string_t* ds) {
  (void)client_idx;
  double secs = 0;
  if (arg && cJSON_IsNumber(arg)) {
    secs = arg->valuedouble;
    state_update_t status;
    if (server && server->engine && server->engine->get_status &&
        server->engine->get_status(server->engine->ctx, &status) &&
        status.state == PROCESSING_STATE_RUNNING) {
      uint64_t now = get_time_ms();
      uint64_t since = now - (uint64_t)(secs * 1000.0);

      size_t c_ch = server->capture_rms_history.channels;
      size_t p_ch = server->playback_rms_history.channels;
      double* c_rms = (double*)calloc(c_ch, sizeof(double));
      double* c_pk = (double*)calloc(c_ch, sizeof(double));
      double* p_rms = (double*)calloc(p_ch, sizeof(double));
      double* p_pk = (double*)calloc(p_ch, sizeof(double));

      level_history_get_rms_since(&server->capture_rms_history, since, c_rms);
      level_history_get_max_since(&server->capture_peak_history, since, c_pk);
      level_history_get_rms_since(&server->playback_rms_history, since, p_rms);
      level_history_get_max_since(&server->playback_peak_history, since, p_pk);

      cJSON* root = cJSON_CreateObject();
      cJSON_AddItemToObject(root, "playback_rms",
                            cJSON_CreateDoubleArray(p_rms, (int)p_ch));
      cJSON_AddItemToObject(root, "playback_peak",
                            cJSON_CreateDoubleArray(p_pk, (int)p_ch));
      cJSON_AddItemToObject(root, "capture_rms",
                            cJSON_CreateDoubleArray(c_rms, (int)c_ch));
      cJSON_AddItemToObject(root, "capture_peak",
                            cJSON_CreateDoubleArray(c_pk, (int)c_ch));

      reply_ok(cmd_name, root, ds);

      free(c_rms);
      free(c_pk);
      free(p_rms);
      free(p_pk);
    } else {
      reply_error(cmd_name, "ProcessingNotRunningError", NULL, ds);
    }
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "Could not parse seconds");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_cmd_get_signal_peaks_since_start(websocket_server_t* server,
                                                    int client_idx,
                                                    const char* cmd_name,
                                                    cJSON* arg,
                                                    dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  cJSON* root = cJSON_CreateObject();
  cJSON_AddItemToObject(
      root, "capture",
      cJSON_CreateDoubleArray(server->capture_global_peaks,
                              (int)server->capture_global_peaks_count));
  cJSON_AddItemToObject(
      root, "playback",
      cJSON_CreateDoubleArray(server->playback_global_peaks,
                              (int)server->playback_global_peaks_count));
  reply_ok(cmd_name, root, ds);
}

static void handle_cmd_reset_signal_peaks_since_start(
    websocket_server_t* server, int client_idx, const char* cmd_name,
    cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  for (size_t i = 0; i < server->capture_global_peaks_count; i++) {
    server->capture_global_peaks[i] = -1000.0;
  }
  for (size_t i = 0; i < server->playback_global_peaks_count; i++) {
    server->playback_global_peaks[i] = -1000.0;
  }
  reply_ok(cmd_name, NULL, ds);
}

static void handle_cmd_get_channel_labels(websocket_server_t* server,
                                          int client_idx, const char* cmd_name,
                                          cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  char* json = NULL;
  if (server && server->engine && server->engine->get_active_config_json) {
    server->engine->get_active_config_json(server->engine->ctx, &json);
  }
  if (!json) {
    char* path = NULL;
    if (server && server->engine && server->engine->get_config_path) {
      path = server->engine->get_config_path(server->engine->ctx);
    }
    if (path) {
      json = server_read_file_to_string(path);
      free(path);
    }
  }
  char play_labels[2048] = "null";
  char cap_labels[2048] = "null";
  if (json) {
    server_get_value_at_pointer(json, "/devices/playback/labels", play_labels,
                                sizeof(play_labels));
    server_get_value_at_pointer(json, "/devices/capture/labels", cap_labels,
                                sizeof(cap_labels));
    free(json);
  }
  cJSON* root = cJSON_CreateObject();
  cJSON_AddItemToObject(root, "playback", cJSON_Parse(play_labels));
  cJSON_AddItemToObject(root, "capture", cJSON_Parse(cap_labels));
  reply_ok(cmd_name, root, ds);
}

static void handle_cmd_get_signal_range(websocket_server_t* server,
                                        int client_idx, const char* cmd_name,
                                        cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  vu_levels_t vu;
  memset(&vu, 0, sizeof(vu));
  if (server && server->engine && server->engine->get_vu_levels &&
      server->engine->get_vu_levels(server->engine->ctx, &vu)) {
    size_t count = vu.playback_channels;
    double max_peak = -1000.0;
    for (size_t i = 0; i < count; i++) {
      double pk = vu.playback_peak[i];
      if (pk > max_peak) max_peak = pk;
    }
    double range = 2.0 * db_to_amplitude(max_peak);
    reply_ok(cmd_name, cJSON_CreateNumber(range), ds);
    free_vu_levels_arrays(&vu);
  } else {
    reply_error(cmd_name, "ProcessingNotRunningError", NULL, ds);
  }
}

static void handle_cmd_get_spectrum(websocket_server_t* server, int client_idx,
                                    const char* cmd_name, cJSON* arg,
                                    dyn_string_t* ds) {
  (void)client_idx;
  bool is_capture = true;
  uint32_t channel = 0;
  double min_freq = 20.0;
  double max_freq = 20000.0;
  uint32_t n_bins = 1024;
  bool ok = false;
  if (arg && cJSON_IsObject(arg)) {
    cJSON* item;
    item = cJSON_GetObjectItemCaseSensitive(arg, "is_capture");
    if (item && cJSON_IsBool(item)) is_capture = cJSON_IsTrue(item);
    item = cJSON_GetObjectItemCaseSensitive(arg, "channel");
    if (item && cJSON_IsNumber(item)) channel = (uint32_t)item->valueint;
    item = cJSON_GetObjectItemCaseSensitive(arg, "min_freq");
    if (item && cJSON_IsNumber(item)) min_freq = item->valuedouble;
    item = cJSON_GetObjectItemCaseSensitive(arg, "max_freq");
    if (item && cJSON_IsNumber(item)) max_freq = item->valuedouble;
    item = cJSON_GetObjectItemCaseSensitive(arg, "n_bins");
    if (item && cJSON_IsNumber(item)) n_bins = (uint32_t)item->valueint;
    ok = true;
  }
  if (ok) {
    spectrum_t spec;
    memset(&spec, 0, sizeof(spec));
    bool spec_ok =
        server && server->engine && server->engine->get_spectrum &&
        server->engine->get_spectrum(server->engine->ctx, is_capture, channel,
                                     min_freq, max_freq, n_bins, &spec);
    if (spec_ok) {
      cJSON* spec_json = serialize_spectrum(&spec);
      if (spec_json) {
        reply_ok(cmd_name, spec_json, ds);
      } else {
        reply_error(cmd_name, "UnknownError", NULL, ds);
      }
      if (spec.frequencies) free(spec.frequencies);
      if (spec.magnitudes) free(spec.magnitudes);
    } else {
      cJSON* err = cJSON_CreateObject();
      cJSON_AddStringToObject(err, "DeviceError", "Failed to compute spectrum");
      reply_error(cmd_name, NULL, err, ds);
    }
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "Could not parse GetSpectrum arguments");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_get_available_devices_helper(websocket_server_t* server,
                                                const char* cmd_name,
                                                cJSON* arg, bool is_capture,
                                                dyn_string_t* ds) {
  if (arg && cJSON_IsString(arg) && arg->valuestring) {
    const char* backend = arg->valuestring;
    audio_device_t* devs = NULL;
    size_t count = 0;
    bool ok = server && server->engine &&
              server->engine->get_available_devices &&
              server->engine->get_available_devices(
                  server->engine->ctx, backend, is_capture, &devs, &count);
    if (ok && devs) {
      cJSON* arr = cJSON_CreateArray();
      for (size_t i = 0; i < count; i++) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(devs[i].name));
      }
      reply_ok(cmd_name, arr, ds);
      free(devs);
    } else {
      if (devs) free(devs);
      reply_ok(cmd_name, cJSON_CreateArray(), ds);
    }
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "Could not parse backend");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_cmd_get_available_capture_devices(websocket_server_t* server,
                                                     int client_idx,
                                                     const char* cmd_name,
                                                     cJSON* arg,
                                                     dyn_string_t* ds) {
  (void)client_idx;
  handle_get_available_devices_helper(server, cmd_name, arg, true, ds);
}

static void handle_cmd_get_available_playback_devices(
    websocket_server_t* server, int client_idx, const char* cmd_name,
    cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  handle_get_available_devices_helper(server, cmd_name, arg, false, ds);
}

static void handle_get_device_capabilities_helper(websocket_server_t* server,
                                                  const char* cmd_name,
                                                  cJSON* arg, bool is_capture,
                                                  dyn_string_t* ds) {
  char backend[128] = "";
  char device[256] = "";
  bool ok = false;
  if (arg && cJSON_IsArray(arg) && cJSON_GetArraySize(arg) >= 2) {
    cJSON* b_node = cJSON_GetArrayItem(arg, 0);
    cJSON* d_node = cJSON_GetArrayItem(arg, 1);
    if (b_node && d_node && cJSON_IsString(b_node) && cJSON_IsString(d_node)) {
      strncpy(backend, b_node->valuestring, sizeof(backend) - 1);
      strncpy(device, d_node->valuestring, sizeof(device) - 1);
      ok = true;
    }
  }
  if (ok) {
    audio_device_descriptor_t* desc = NULL;
    device_error_t d_err;
    device_error_clear(&d_err);
    bool cap_ok =
        server && server->engine && server->engine->get_device_capabilities &&
        server->engine->get_device_capabilities(
            server->engine->ctx, backend, device, is_capture, &desc, &d_err);
    if (cap_ok && desc) {
      char* val = format_device_descriptor(desc);
      if (val) {
        cJSON* desc_obj = cJSON_Parse(val);
        free(val);
        if (desc_obj) {
          reply_ok(cmd_name, desc_obj, ds);
        } else {
          reply_error(cmd_name, "UnknownError", NULL, ds);
        }
      } else {
        cJSON* err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "DeviceError", "Out of memory");
        reply_error(cmd_name, NULL, err, ds);
      }
      extern void dsp_engine_free_device_capabilities(
          audio_device_descriptor_t * desc);
      dsp_engine_free_device_capabilities(desc);
    } else {
      cJSON* err = cJSON_CreateObject();
      cJSON_AddStringToObject(err, get_websocket_device_error_key(d_err.type),
                              d_err.message);
      reply_error(cmd_name, NULL, err, ds);
    }
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "Could not parse backend/device array");
    reply_error(cmd_name, NULL, err, ds);
  }
}

static void handle_cmd_get_capture_device_capabilities(
    websocket_server_t* server, int client_idx, const char* cmd_name,
    cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  handle_get_device_capabilities_helper(server, cmd_name, arg, true, ds);
}

static void handle_cmd_get_playback_device_capabilities(
    websocket_server_t* server, int client_idx, const char* cmd_name,
    cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  handle_get_device_capabilities_helper(server, cmd_name, arg, false, ds);
}

static void handle_cmd_get_version(websocket_server_t* server, int client_idx,
                                   const char* cmd_name, cJSON* arg,
                                   dyn_string_t* ds) {
  (void)server;
  (void)client_idx;
  (void)arg;
  reply_ok(cmd_name, cJSON_CreateString("CamillaDSP-C-Embedded 2.0.0"), ds);
}

static void handle_cmd_get_state(websocket_server_t* server, int client_idx,
                                 const char* cmd_name, cJSON* arg,
                                 dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  processing_state_t state = PROCESSING_STATE_INACTIVE;
  if (server && server->engine && server->engine->get_status) {
    state_update_t status;
    if (server->engine->get_status(server->engine->ctx, &status)) {
      state = status.state;
    }
  }
  reply_ok(cmd_name, cJSON_CreateString(processing_state_to_string(state)), ds);
}

static void handle_cmd_get_stop_reason(websocket_server_t* server,
                                       int client_idx, const char* cmd_name,
                                       cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  char reason_str[512] = "None";
  if (server && server->engine && server->engine->get_status) {
    state_update_t status;
    if (server->engine->get_status(server->engine->ctx, &status)) {
      stop_reason_to_string(&status.stop_reason, reason_str,
                            sizeof(reason_str));
    }
  }
  reply_ok(cmd_name, cJSON_CreateString(reason_str), ds);
}

static void handle_cmd_get_capture_rate(websocket_server_t* server,
                                        int client_idx, const char* cmd_name,
                                        cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  state_update_t status;
  memset(&status, 0, sizeof(status));
  bool has_status = server && server->engine && server->engine->get_status &&
                    server->engine->get_status(server->engine->ctx, &status);
  int sr = 0;
  if (has_status && status.state == PROCESSING_STATE_RUNNING) {
    sr = (server->engine->get_active_samplerate)
             ? server->engine->get_active_samplerate(server->engine->ctx)
             : 0;
  }
  reply_ok(cmd_name, cJSON_CreateNumber(sr), ds);
}

static void handle_cmd_get_rate_adjust(websocket_server_t* server,
                                       int client_idx, const char* cmd_name,
                                       cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  double rate = 1.0;
  if (server && server->engine && server->engine->get_processing_status) {
    server->engine->get_processing_status(server->engine->ctx, &rate, NULL,
                                          NULL, NULL, NULL);
  }
  reply_ok(cmd_name, cJSON_CreateNumber(rate), ds);
}

static void handle_cmd_get_buffer_level(websocket_server_t* server,
                                        int client_idx, const char* cmd_name,
                                        cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  double lvl = 0.0;
  if (server && server->engine && server->engine->get_processing_status) {
    server->engine->get_processing_status(server->engine->ctx, NULL, &lvl, NULL,
                                          NULL, NULL);
  }
  reply_ok(cmd_name, cJSON_CreateNumber((int)lvl), ds);
}

static void handle_cmd_get_clipped_samples(websocket_server_t* server,
                                           int client_idx, const char* cmd_name,
                                           cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  uint64_t clips = 0;
  if (server && server->engine && server->engine->get_processing_status) {
    server->engine->get_processing_status(server->engine->ctx, NULL, NULL,
                                          &clips, NULL, NULL);
  }
  reply_ok(cmd_name, cJSON_CreateNumber((double)clips), ds);
}

static void handle_cmd_reset_clipped_samples(websocket_server_t* server,
                                             int client_idx,
                                             const char* cmd_name, cJSON* arg,
                                             dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  if (server && server->engine && server->engine->reset_clipped_samples) {
    server->engine->reset_clipped_samples(server->engine->ctx);
  }
  reply_ok(cmd_name, NULL, ds);
}

static void handle_cmd_get_processing_load(websocket_server_t* server,
                                           int client_idx, const char* cmd_name,
                                           cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  double load = 0.0;
  if (server && server->engine && server->engine->get_processing_status) {
    server->engine->get_processing_status(server->engine->ctx, NULL, NULL, NULL,
                                          &load, NULL);
  }
  reply_ok(cmd_name, cJSON_CreateNumber(load), ds);
}

static void handle_cmd_get_resampler_load(websocket_server_t* server,
                                          int client_idx, const char* cmd_name,
                                          cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  double load = 0.0;
  if (server && server->engine && server->engine->get_processing_status) {
    server->engine->get_processing_status(server->engine->ctx, NULL, NULL, NULL,
                                          NULL, &load);
  }
  reply_ok(cmd_name, cJSON_CreateNumber(load), ds);
}

static void handle_cmd_get_supported_device_types(websocket_server_t* server,
                                                  int client_idx,
                                                  const char* cmd_name,
                                                  cJSON* arg,
                                                  dyn_string_t* ds) {
  (void)server;
  (void)client_idx;
  (void)arg;
  cJSON* arr = cJSON_CreateArray();
  cJSON* inner_arr1 = cJSON_CreateArray();
  cJSON_AddItemToArray(inner_arr1, cJSON_CreateString("CoreAudio"));
  cJSON_AddItemToArray(arr, inner_arr1);
  cJSON* inner_arr2 = cJSON_CreateArray();
  cJSON_AddItemToArray(inner_arr2, cJSON_CreateString("CoreAudio"));
  cJSON_AddItemToArray(arr, inner_arr2);
  reply_ok(cmd_name, arr, ds);
}

static void handle_cmd_get_update_interval(websocket_server_t* server,
                                           int client_idx, const char* cmd_name,
                                           cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  (void)arg;
  int interval = server ? (int)server->update_interval : 100;
  reply_ok(cmd_name, cJSON_CreateNumber(interval), ds);
}

static void handle_cmd_set_update_interval(websocket_server_t* server,
                                           int client_idx, const char* cmd_name,
                                           cJSON* arg, dyn_string_t* ds) {
  (void)client_idx;
  if (arg && cJSON_IsNumber(arg)) {
    double val = arg->valuedouble;
    if (val >= 0.0) {
      if (server) server->update_interval = (uint32_t)val;
      reply_ok(cmd_name, NULL, ds);
    } else {
      cJSON* err = cJSON_CreateObject();
      cJSON_AddStringToObject(err, "InvalidValueError", "Value must be >= 0");
      reply_error(cmd_name, NULL, err, ds);
    }
  } else {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "InvalidRequestError",
                            "Could not parse SetUpdateInterval argument");
    reply_error(cmd_name, NULL, err, ds);
  }
}

// MARK: - Command Handler

/// Handle a control command text (either simple quoted string or JSON object)
/// and populate out_response.
void websocket_server_handle_command(websocket_server_t* server, int client_idx,
                                     const char* command_text,
                                     dyn_string_t* ds) {
  if (!server || !ds || !command_text) return;

  cJSON* root = cJSON_Parse(command_text);
  if (!root) {
    json_reply("Invalid", "{\"error\":\"Invalid JSON\"}", NULL, ds);
    return;
  }

  pthread_mutex_lock(&server->sessions_mutex);

  char cmd_name[128] = "";
  cJSON* arg = NULL;

  if (cJSON_IsString(root)) {
    strncpy(cmd_name, root->valuestring, sizeof(cmd_name) - 1);
  } else if (cJSON_IsObject(root)) {
    cJSON* child = root->child;
    if (child) {
      strncpy(cmd_name, child->string, sizeof(cmd_name) - 1);
      arg = child;
    }
  }

  const char* simple = cmd_name;

  websocket_command_t cmd_type = lookup_command(simple);
  switch (cmd_type) {
    case WS_CMD_GET_VERSION:
      handle_cmd_get_version(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_STATE:
      handle_cmd_get_state(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_STOP_REASON:
      handle_cmd_get_stop_reason(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_CAPTURE_RATE:
      handle_cmd_get_capture_rate(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_RATE_ADJUST:
      handle_cmd_get_rate_adjust(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_BUFFER_LEVEL:
      handle_cmd_get_buffer_level(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_CLIPPED_SAMPLES:
      handle_cmd_get_clipped_samples(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_RESET_CLIPPED_SAMPLES:
      handle_cmd_reset_clipped_samples(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_PROCESSING_LOAD:
      handle_cmd_get_processing_load(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_RESAMPLER_LOAD:
      handle_cmd_get_resampler_load(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_SUPPORTED_DEVICE_TYPES:
      handle_cmd_get_supported_device_types(server, client_idx, simple, arg,
                                            ds);
      break;
    case WS_CMD_GET_UPDATE_INTERVAL:
      handle_cmd_get_update_interval(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_SET_UPDATE_INTERVAL:
      handle_cmd_set_update_interval(server, client_idx, simple, arg, ds);
      break;

    case WS_CMD_GET_VOLUME:
      handle_cmd_get_volume(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_SET_VOLUME:
      handle_cmd_set_volume(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_MUTE:
      handle_cmd_get_mute(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_SET_MUTE:
      handle_cmd_set_mute(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_TOGGLE_MUTE:
      handle_cmd_toggle_mute(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_FADERS:
      handle_cmd_get_faders(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_FADER_VOLUME:
      handle_cmd_get_fader_volume(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_SET_FADER_VOLUME:
      handle_cmd_set_fader_volume(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_SET_FADER_EXTERNAL_VOLUME:
      handle_cmd_set_fader_external_volume(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_FADER_MUTE:
      handle_cmd_get_fader_mute(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_SET_FADER_MUTE:
      handle_cmd_set_fader_mute(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_TOGGLE_FADER_MUTE:
      handle_cmd_toggle_fader_mute(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_ADJUST_VOLUME:
      handle_cmd_adjust_volume(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_ADJUST_FADER_VOLUME:
      handle_cmd_adjust_fader_volume(server, client_idx, simple, arg, ds);
      break;

    case WS_CMD_GET_SPECTRUM:
      handle_cmd_get_spectrum(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_AVAILABLE_CAPTURE_DEVICES:
      handle_cmd_get_available_capture_devices(server, client_idx, simple, arg,
                                               ds);
      break;
    case WS_CMD_GET_AVAILABLE_PLAYBACK_DEVICES:
      handle_cmd_get_available_playback_devices(server, client_idx, simple, arg,
                                                ds);
      break;
    case WS_CMD_GET_CAPTURE_DEVICE_CAPABILITIES:
      handle_cmd_get_capture_device_capabilities(server, client_idx, simple,
                                                 arg, ds);
      break;
    case WS_CMD_GET_PLAYBACK_DEVICE_CAPABILITIES:
      handle_cmd_get_playback_device_capabilities(server, client_idx, simple,
                                                  arg, ds);
      break;

    case WS_CMD_GET_CAPTURE_SIGNAL_RMS:
      handle_cmd_get_capture_signal_rms(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_CAPTURE_SIGNAL_PEAK:
      handle_cmd_get_capture_signal_peak(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_PLAYBACK_SIGNAL_RMS:
      handle_cmd_get_playback_signal_rms(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_PLAYBACK_SIGNAL_PEAK:
      handle_cmd_get_playback_signal_peak(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_CAPTURE_SIGNAL_RMS_SINCE_LAST:
      handle_cmd_get_capture_signal_rms_since_last(server, client_idx, simple,
                                                   arg, ds);
      break;
    case WS_CMD_GET_CAPTURE_SIGNAL_PEAK_SINCE_LAST:
      handle_cmd_get_capture_signal_peak_since_last(server, client_idx, simple,
                                                    arg, ds);
      break;
    case WS_CMD_GET_PLAYBACK_SIGNAL_RMS_SINCE_LAST:
      handle_cmd_get_playback_signal_rms_since_last(server, client_idx, simple,
                                                    arg, ds);
      break;
    case WS_CMD_GET_PLAYBACK_SIGNAL_PEAK_SINCE_LAST:
      handle_cmd_get_playback_signal_peak_since_last(server, client_idx, simple,
                                                     arg, ds);
      break;
    case WS_CMD_GET_CAPTURE_SIGNAL_RMS_SINCE:
      handle_cmd_get_capture_signal_rms_since(server, client_idx, simple, arg,
                                              ds);
      break;
    case WS_CMD_GET_CAPTURE_SIGNAL_PEAK_SINCE:
      handle_cmd_get_capture_signal_peak_since(server, client_idx, simple, arg,
                                               ds);
      break;
    case WS_CMD_GET_PLAYBACK_SIGNAL_RMS_SINCE:
      handle_cmd_get_playback_signal_rms_since(server, client_idx, simple, arg,
                                               ds);
      break;
    case WS_CMD_GET_PLAYBACK_SIGNAL_PEAK_SINCE:
      handle_cmd_get_playback_signal_peak_since(server, client_idx, simple, arg,
                                                ds);
      break;
    case WS_CMD_GET_SIGNAL_LEVELS:
      handle_cmd_get_signal_levels(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_SIGNAL_LEVELS_SINCE_LAST:
      handle_cmd_get_signal_levels_since_last(server, client_idx, simple, arg,
                                              ds);
      break;
    case WS_CMD_GET_SIGNAL_LEVELS_SINCE:
      handle_cmd_get_signal_levels_since(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_SIGNAL_PEAKS_SINCE_START:
      handle_cmd_get_signal_peaks_since_start(server, client_idx, simple, arg,
                                              ds);
      break;
    case WS_CMD_RESET_SIGNAL_PEAKS_SINCE_START:
      handle_cmd_reset_signal_peaks_since_start(server, client_idx, simple, arg,
                                                ds);
      break;
    case WS_CMD_GET_CHANNEL_LABELS:
      handle_cmd_get_channel_labels(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_SIGNAL_RANGE:
      handle_cmd_get_signal_range(server, client_idx, simple, arg, ds);
      break;

    case WS_CMD_GET_CONFIG_FILE_PATH:
      handle_cmd_get_config_file_path(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_PREVIOUS_CONFIG:
      handle_cmd_get_previous_config(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_STATE_FILE_PATH:
      handle_cmd_get_state_file_path(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_STATE_FILE_UPDATED:
      handle_cmd_get_state_file_updated(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_CONFIG:
    case WS_CMD_GET_CONFIG_JSON:
      handle_cmd_get_config(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_CONFIG_TITLE:
      handle_cmd_get_config_title(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_CONFIG_DESCRIPTION:
      handle_cmd_get_config_description(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_RELOAD:
      handle_cmd_reload(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_STOP:
      handle_cmd_stop(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_EXIT:
      handle_cmd_exit(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_SET_CONFIG_FILE_PATH:
      handle_cmd_set_config_file_path(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_SET_CONFIG_JSON:
      handle_cmd_set_config_json(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_GET_CONFIG_VALUE:
      handle_cmd_get_config_value(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_SET_CONFIG_VALUE:
      handle_cmd_set_config_value(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_PATCH_CONFIG:
      handle_cmd_patch_config(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_READ_CONFIG_JSON:
    case WS_CMD_VALIDATE_CONFIG_JSON:
      handle_cmd_read_config_json(server, client_idx, simple, arg, ds);
      break;

    case WS_CMD_SUBSCRIBE_STATE:
      handle_cmd_subscribe_state(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_SUBSCRIBE_VU_LEVELS:
      handle_cmd_subscribe_vu_levels(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_SUBSCRIBE_SIGNAL_LEVELS:
      handle_cmd_subscribe_signal_levels(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_SUBSCRIBE_SPECTRUM:
      handle_cmd_subscribe_spectrum(server, client_idx, simple, arg, ds);
      break;
    case WS_CMD_STOP_SUBSCRIPTION:
      handle_cmd_stop_subscription(server, client_idx, simple, arg, ds);
      break;

    default:
      dyn_string_printf(ds,
                        "{\"Invalid\":{\"error\":\"Unsupported command\"}}");
      break;
  }
  pthread_mutex_unlock(&server->sessions_mutex);
  cJSON_Delete(root);
}

/**
 * @brief Sends a WebSocket text frame to a client file descriptor.
 *
 * Packages the payload into a standard RFC 6455 WebSocket text frame (opcode
 * 0x81), handles payload length fields (up to 64-bit lengths), and writes to
 * the socket.
 *
 * @param fd The client socket file descriptor.
 * @param response The null-terminated payload string to send.
 */
static void send_websocket_frame(socket_t fd, const char* response) {
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

/**
 * @brief Thread entry point for the WebSocket server.
 *
 * Handles the polling loop for accepting new client connections, receiving
 * WebSocket frames, decoding masking key, executing commands, and pushing
 * periodic updates (VU levels, signal levels, spectrum) to subscribed clients.
 *
 * @param arg Pointer to the websocket_server_t structure.
 * @return Always NULL.
 */
static void* server_thread_func(void* arg) {
  websocket_server_t* server = (websocket_server_t*)arg;
  socket_t client_fds[32];
  char last_state[32][64];
  int num_clients = 0;

  uint64_t last_broadcast_time_ms = 0;

  while (atomic_load_explicit(&server->running, memory_order_acquire)) {
    struct pollfd fds[33];
    fds[0].fd = server->server_fd;
    fds[0].events = POLLIN;
    for (int i = 0; i < num_clients; i++) {
      fds[i + 1].fd = client_fds[i];
      fds[i + 1].events = POLLIN;
    }
    int ret = poll_sockets(fds, num_clients + 1, 50);

    pthread_mutex_lock(&server->sessions_mutex);

    // Periodic broadcast tick
    uint64_t now = get_time_ms();
    if (now - last_broadcast_time_ms >= server->update_interval) {
      last_broadcast_time_ms = now;

      state_update_t status;
      memset(&status, 0, sizeof(status));
      bool has_status =
          server->engine && server->engine->get_status &&
          server->engine->get_status(server->engine->ctx, &status);

      const char* state_str = "Inactive";
      if (has_status) {
        state_str = processing_state_to_string(status.state);
      }

      double* current_cap_peak = NULL;
      double* current_cap_rms = NULL;
      double* current_pb_peak = NULL;
      double* current_pb_rms = NULL;
      size_t cap_channels = 0;
      size_t pb_channels = 0;

      vu_levels_t vu;
      memset(&vu, 0, sizeof(vu));
      if (server->engine && server->engine->get_vu_levels &&
          server->engine->get_vu_levels(server->engine->ctx, &vu)) {
        cap_channels = vu.capture_channels;
        pb_channels = vu.playback_channels;
        current_cap_peak = vu.capture_peak;
        current_cap_rms = vu.capture_rms;
        current_pb_peak = vu.playback_peak;
        current_pb_rms = vu.playback_rms;

        if (cap_channels > 0 && current_cap_peak && current_cap_rms) {
          level_history_append(&server->capture_peak_history, current_cap_peak,
                               cap_channels, now);
          level_history_append(&server->capture_rms_history, current_cap_rms,
                               cap_channels, now);

          if (server->capture_global_peaks_count != cap_channels) {
            double* new_peaks = (double*)realloc(server->capture_global_peaks,
                                                 cap_channels * sizeof(double));
            if (new_peaks) {
              server->capture_global_peaks = new_peaks;
              for (size_t k = server->capture_global_peaks_count;
                   k < cap_channels; k++) {
                server->capture_global_peaks[k] = -1000.0;
              }
              server->capture_global_peaks_count = cap_channels;
            }
          }
          size_t limit = cap_channels < server->capture_global_peaks_count
                             ? cap_channels
                             : server->capture_global_peaks_count;
          for (size_t k = 0; k < limit; k++) {
            if (server->capture_global_peaks &&
                current_cap_peak[k] > server->capture_global_peaks[k]) {
              server->capture_global_peaks[k] = current_cap_peak[k];
            }
          }
        }

        if (pb_channels > 0 && current_pb_peak && current_pb_rms) {
          level_history_append(&server->playback_peak_history, current_pb_peak,
                               pb_channels, now);
          level_history_append(&server->playback_rms_history, current_pb_rms,
                               pb_channels, now);

          if (server->playback_global_peaks_count != pb_channels) {
            double* new_peaks = (double*)realloc(server->playback_global_peaks,
                                                 pb_channels * sizeof(double));
            if (new_peaks) {
              server->playback_global_peaks = new_peaks;
              for (size_t k = server->playback_global_peaks_count;
                   k < pb_channels; k++) {
                server->playback_global_peaks[k] = -1000.0;
              }
              server->playback_global_peaks_count = pb_channels;
            }
          }
          size_t limit = pb_channels < server->playback_global_peaks_count
                             ? pb_channels
                             : server->playback_global_peaks_count;
          for (size_t k = 0; k < limit; k++) {
            if (server->playback_global_peaks &&
                current_pb_peak[k] > server->playback_global_peaks[k]) {
              server->playback_global_peaks[k] = current_pb_peak[k];
            }
          }
        }
      }

      for (int i = 0; i < num_clients; i++) {
        client_session_t* session = &server->client_sessions[i];

        if (session->state_subscribed &&
            strcmp(last_state[i], state_str) != 0) {
          strncpy(last_state[i], state_str, sizeof(last_state[i]) - 1);
          char msg[1024];
          char payload[512];
          format_state_event_payload(status.state, &status.stop_reason, payload,
                                     sizeof(payload));
          snprintf(msg, sizeof(msg),
                   "{\"StateEvent\":{\"result\":\"Ok\",\"value\":%s}}",
                   payload);
          send_websocket_frame(client_fds[i], msg);
        }

        if (session->vu_subscribed && pb_channels > 0) {
          double interval =
              session->vu_max_rate > 0.0 ? 1000.0 / session->vu_max_rate : 0.0;
          if (now - session->last_vu_push_time >= interval) {
            double dt = session->last_vu_push_time == 0
                            ? 100.0
                            : (double)(now - session->last_vu_push_time);
            double attack = smoothing_alpha(dt, session->vu_attack);
            double release = smoothing_alpha(dt, session->vu_release);

            if (session->vu_pb_channels != pb_channels) {
              double* new_rms = (double*)calloc(pb_channels, sizeof(double));
              double* new_peak = (double*)calloc(pb_channels, sizeof(double));
              if (new_rms && new_peak) {
                size_t copy_count = session->vu_pb_channels < pb_channels
                                        ? session->vu_pb_channels
                                        : pb_channels;
                if (session->vu_pb_rms) {
                  memcpy(new_rms, session->vu_pb_rms,
                         copy_count * sizeof(double));
                  free(session->vu_pb_rms);
                }
                if (session->vu_pb_peak) {
                  memcpy(new_peak, session->vu_pb_peak,
                         copy_count * sizeof(double));
                  free(session->vu_pb_peak);
                }
                for (size_t k = copy_count; k < pb_channels; k++) {
                  new_rms[k] = current_pb_rms[k];
                  new_peak[k] = current_pb_peak[k];
                }
                session->vu_pb_rms = new_rms;
                session->vu_pb_peak = new_peak;
                session->vu_pb_channels = pb_channels;
              } else {
                if (new_rms) free(new_rms);
                if (new_peak) free(new_peak);
              }
            } else {
              for (size_t k = 0; k < pb_channels; k++) {
                double prev_amp = db_to_amplitude(session->vu_pb_rms[k]);
                double curr_amp = db_to_amplitude(current_pb_rms[k]);
                double diff = curr_amp - prev_amp;
                if (diff > 0.0)
                  prev_amp += attack * diff;
                else
                  prev_amp += release * diff;
                session->vu_pb_rms[k] = amplitude_to_db(prev_amp);
              }
              for (size_t k = 0; k < pb_channels; k++) {
                double prev_amp = db_to_amplitude(session->vu_pb_peak[k]);
                double curr_amp = db_to_amplitude(current_pb_peak[k]);
                double diff = curr_amp - prev_amp;
                if (diff > 0.0)
                  prev_amp += 1.0 * diff;
                else
                  prev_amp += release * diff;
                session->vu_pb_peak[k] = amplitude_to_db(prev_amp);
              }
            }

            if (cap_channels > 0) {
              if (session->vu_cap_channels != cap_channels) {
                double* new_rms = (double*)calloc(cap_channels, sizeof(double));
                double* new_peak =
                    (double*)calloc(cap_channels, sizeof(double));
                if (new_rms && new_peak) {
                  size_t copy_count = session->vu_cap_channels < cap_channels
                                          ? session->vu_cap_channels
                                          : cap_channels;
                  if (session->vu_cap_rms) {
                    memcpy(new_rms, session->vu_cap_rms,
                           copy_count * sizeof(double));
                    free(session->vu_cap_rms);
                  }
                  if (session->vu_cap_peak) {
                    memcpy(new_peak, session->vu_cap_peak,
                           copy_count * sizeof(double));
                    free(session->vu_cap_peak);
                  }
                  for (size_t k = copy_count; k < cap_channels; k++) {
                    new_rms[k] = current_cap_rms[k];
                    new_peak[k] = current_cap_peak[k];
                  }
                  session->vu_cap_rms = new_rms;
                  session->vu_cap_peak = new_peak;
                  session->vu_cap_channels = cap_channels;
                } else {
                  if (new_rms) free(new_rms);
                  if (new_peak) free(new_peak);
                }
              } else {
                for (size_t k = 0; k < cap_channels; k++) {
                  double prev_amp = db_to_amplitude(session->vu_cap_rms[k]);
                  double curr_amp = db_to_amplitude(current_cap_rms[k]);
                  double diff = curr_amp - prev_amp;
                  if (diff > 0.0)
                    prev_amp += attack * diff;
                  else
                    prev_amp += release * diff;
                  session->vu_cap_rms[k] = amplitude_to_db(prev_amp);
                }
                for (size_t k = 0; k < cap_channels; k++) {
                  double prev_amp = db_to_amplitude(session->vu_cap_peak[k]);
                  double curr_amp = db_to_amplitude(current_cap_peak[k]);
                  double diff = curr_amp - prev_amp;
                  if (diff > 0.0)
                    prev_amp += 1.0 * diff;
                  else
                    prev_amp += release * diff;
                  session->vu_cap_peak[k] = amplitude_to_db(prev_amp);
                }
              }
            }

            cJSON* root = cJSON_CreateObject();
            cJSON* val = cJSON_CreateObject();
            cJSON_AddItemToObject(root, "VuLevelsEvent", val);
            cJSON_AddStringToObject(val, "result", "Ok");
            cJSON* val_value = cJSON_CreateObject();
            cJSON_AddItemToObject(val, "value", val_value);
            cJSON_AddItemToObject(
                val_value, "playback_rms",
                cJSON_CreateDoubleArray(session->vu_pb_rms, (int)pb_channels));
            cJSON_AddItemToObject(
                val_value, "playback_peak",
                cJSON_CreateDoubleArray(session->vu_pb_peak, (int)pb_channels));
            cJSON_AddItemToObject(val_value, "capture_rms",
                                  cJSON_CreateDoubleArray(session->vu_cap_rms,
                                                          (int)cap_channels));
            cJSON_AddItemToObject(val_value, "capture_peak",
                                  cJSON_CreateDoubleArray(session->vu_cap_peak,
                                                          (int)cap_channels));
            char* msg = cJSON_PrintUnformatted(root);
            if (msg) {
              send_websocket_frame(client_fds[i], msg);
              free(msg);
            }
            cJSON_Delete(root);
            session->last_vu_push_time = now;
          }
        }

        if (session->signal_levels_subscribed) {
          bool send_pb = strcmp(session->signal_levels_side, "playback") == 0 ||
                         strcmp(session->signal_levels_side, "both") == 0;
          bool send_cap = strcmp(session->signal_levels_side, "capture") == 0 ||
                          strcmp(session->signal_levels_side, "both") == 0;

          if (send_pb && pb_channels > 0) {
            cJSON* root = cJSON_CreateObject();
            cJSON* val = cJSON_CreateObject();
            cJSON_AddItemToObject(root, "SignalLevelsEvent", val);
            cJSON_AddStringToObject(val, "result", "Ok");
            cJSON* val_value = cJSON_CreateObject();
            cJSON_AddItemToObject(val, "value", val_value);
            cJSON_AddStringToObject(val_value, "side", "playback");
            cJSON_AddItemToObject(
                val_value, "rms",
                cJSON_CreateDoubleArray(current_pb_rms, (int)pb_channels));
            cJSON_AddItemToObject(
                val_value, "peak",
                cJSON_CreateDoubleArray(current_pb_peak, (int)pb_channels));
            char* msg = cJSON_PrintUnformatted(root);
            if (msg) {
              send_websocket_frame(client_fds[i], msg);
              free(msg);
            }
            cJSON_Delete(root);
          }
          if (send_cap && cap_channels > 0) {
            cJSON* root = cJSON_CreateObject();
            cJSON* val = cJSON_CreateObject();
            cJSON_AddItemToObject(root, "SignalLevelsEvent", val);
            cJSON_AddStringToObject(val, "result", "Ok");
            cJSON* val_value = cJSON_CreateObject();
            cJSON_AddItemToObject(val, "value", val_value);
            cJSON_AddStringToObject(val_value, "side", "capture");
            cJSON_AddItemToObject(
                val_value, "rms",
                cJSON_CreateDoubleArray(current_cap_rms, (int)cap_channels));
            cJSON_AddItemToObject(
                val_value, "peak",
                cJSON_CreateDoubleArray(current_cap_peak, (int)cap_channels));
            char* msg = cJSON_PrintUnformatted(root);
            if (msg) {
              send_websocket_frame(client_fds[i], msg);
              free(msg);
            }
            cJSON_Delete(root);
          }
        }

        if (session->spectrum_subscribed) {
          double interval = session->spectrum_max_rate > 0.0
                                ? 1000.0 / session->spectrum_max_rate
                                : 0.0;
          if (now - session->last_spectrum_push_time >= interval) {
            spectrum_t spec;
            memset(&spec, 0, sizeof(spec));
            bool spec_ok =
                server && server->engine && server->engine->get_spectrum &&
                server->engine->get_spectrum(
                    server->engine->ctx, session->spectrum_is_capture,
                    session->spectrum_channel, session->spectrum_min_freq,
                    session->spectrum_max_freq, session->spectrum_n_bins,
                    &spec);
            if (spec_ok) {
              cJSON* root = cJSON_CreateObject();
              cJSON* val = cJSON_CreateObject();
              cJSON_AddItemToObject(root, "SpectrumEvent", val);
              cJSON_AddStringToObject(val, "result", "Ok");
              cJSON_AddItemToObject(val, "value", serialize_spectrum(&spec));
              char* msg = cJSON_PrintUnformatted(root);
              if (msg) {
                send_websocket_frame(client_fds[i], msg);
                free(msg);
              }
              cJSON_Delete(root);
              if (spec.frequencies) free(spec.frequencies);
              if (spec.magnitudes) free(spec.magnitudes);
              session->last_spectrum_push_time = now;
            }
          }
        }
      }

      free_vu_levels_arrays(&vu);
    }
    pthread_mutex_unlock(&server->sessions_mutex);

    if (ret > 0) {
      if (fds[0].revents & POLLIN) {
        socket_t cfd = accept(server->server_fd, NULL, NULL);
        if (!IS_INVALID_SOCKET(cfd) && num_clients < 32) {
          client_fds[num_clients] = cfd;
          last_state[num_clients][0] = '\0';

          pthread_mutex_lock(&server->sessions_mutex);
          client_session_t* session = &server->client_sessions[num_clients];
          memset(session, 0, sizeof(client_session_t));
          uint64_t now_ms = get_time_ms();
          session->last_cap_peak_time = now_ms;
          session->last_cap_rms_time = now_ms;
          session->last_pb_peak_time = now_ms;
          session->last_pb_rms_time = now_ms;
          pthread_mutex_unlock(&server->sessions_mutex);

          num_clients++;
        } else if (!IS_INVALID_SOCKET(cfd)) {
          CLOSE_SOCKET(cfd);
        }
      }
      for (int i = 0; i < num_clients; i++) {
        if (fds[i + 1].revents & (POLLIN | POLLERR | POLLHUP)) {
          char buf[4096];
          int n = recv(client_fds[i], buf, sizeof(buf) - 1, 0);
          if (n <= 0) {
            CLOSE_SOCKET(client_fds[i]);
            pthread_mutex_lock(&server->sessions_mutex);
            client_session_clear(&server->client_sessions[i]);

            for (int j = i; j < num_clients - 1; j++) {
              client_fds[j] = client_fds[j + 1];
              strcpy(last_state[j], last_state[j + 1]);
              server->client_sessions[j] = server->client_sessions[j + 1];
            }
            memset(&server->client_sessions[num_clients - 1], 0,
                   sizeof(client_session_t));
            pthread_mutex_unlock(&server->sessions_mutex);
            num_clients--;
            i--;
          } else {
            buf[n] = '\0';
            if (strncmp(buf, "GET ", 4) == 0 && strstr(buf, "Upgrade: ")) {
              char* key_ptr = strstr(buf, "Sec-WebSocket-Key: ");
              if (key_ptr) {
                key_ptr += 19;
                char key[64];
                int k = 0;
                while (*key_ptr && *key_ptr != '\r' && *key_ptr != '\n' &&
                       k < 63) {
                  key[k++] = *key_ptr++;
                }
                key[k] = '\0';
                char concat[128];
                snprintf(concat, sizeof(concat),
                         "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
                unsigned char hash[CC_SHA1_DIGEST_LENGTH];
                CC_SHA1(concat, (CC_LONG)strlen(concat), hash);

                static const char b64[] =
                    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz012345"
                    "6789+/";
                char b64_hash[32];
                int b_idx = 0;
                for (int idx = 0; idx < 20; idx += 3) {
                  uint32_t val = (hash[idx] << 16) |
                                 ((idx + 1 < 20 ? hash[idx + 1] : 0) << 8) |
                                 (idx + 2 < 20 ? hash[idx + 2] : 0);
                  b64_hash[b_idx++] = b64[(val >> 18) & 63];
                  b64_hash[b_idx++] = b64[(val >> 12) & 63];
                  b64_hash[b_idx++] =
                      (idx + 1 < 20) ? b64[(val >> 6) & 63] : '=';
                  b64_hash[b_idx++] = (idx + 2 < 20) ? b64[val & 63] : '=';
                }
                b64_hash[b_idx] = '\0';

                char reply[512];
                snprintf(reply, sizeof(reply),
                         "HTTP/1.1 101 Switching Protocols\r\nUpgrade: "
                         "websocket\r\nConnection: "
                         "Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n",
                         b64_hash);
                send(client_fds[i], reply, (int)strlen(reply), 0);
              }
              continue;
            }

            int offset = 0;
            while (offset < n) {
              unsigned char first_byte = (unsigned char)buf[offset];
              if (first_byte == 0x81 || (first_byte & 0x0F) == 0x08) {
                if ((first_byte & 0x0F) == 0x08) {
                  CLOSE_SOCKET(client_fds[i]);
                  pthread_mutex_lock(&server->sessions_mutex);
                  client_session_clear(&server->client_sessions[i]);
                  for (int j = i; j < num_clients - 1; j++) {
                    client_fds[j] = client_fds[j + 1];
                    strcpy(last_state[j], last_state[j + 1]);
                    server->client_sessions[j] = server->client_sessions[j + 1];
                  }
                  memset(&server->client_sessions[num_clients - 1], 0,
                         sizeof(client_session_t));
                  pthread_mutex_unlock(&server->sessions_mutex);
                  num_clients--;
                  i--;
                  break;
                }
                if (offset + 2 > n) break;
                unsigned char len_byte = (unsigned char)buf[offset + 1];
                size_t payload_len = len_byte & 0x7F;
                int mask_offset = 2;
                if (payload_len == 126) {
                  if (offset + 4 > n) break;
                  payload_len = ((unsigned char)buf[offset + 2] << 8) |
                                (unsigned char)buf[offset + 3];
                  mask_offset = 4;
                } else if (payload_len == 127) {
                  if (offset + 10 > n) break;
                  payload_len = 0;
                  for (int j = 0; j < 8; j++) {
                    payload_len =
                        (payload_len << 8) | (unsigned char)buf[offset + 2 + j];
                  }
                  mask_offset = 10;
                }

                // Safety check: protect against large payloads (config should
                // never exceed 128k)
                if (payload_len > 128 * 1024) {
                  CLOSE_SOCKET(client_fds[i]);
                  pthread_mutex_lock(&server->sessions_mutex);
                  client_session_clear(&server->client_sessions[i]);

                  for (int j = i; j < num_clients - 1; j++) {
                    client_fds[j] = client_fds[j + 1];
                    strcpy(last_state[j], last_state[j + 1]);
                    server->client_sessions[j] = server->client_sessions[j + 1];
                  }
                  memset(&server->client_sessions[num_clients - 1], 0,
                         sizeof(client_session_t));
                  pthread_mutex_unlock(&server->sessions_mutex);
                  num_clients--;
                  i--;
                  break;
                }

                size_t header_and_mask_len = mask_offset + 4;
                if (offset + header_and_mask_len > (size_t)n) break;

                unsigned char* mask =
                    (unsigned char*)&buf[offset + mask_offset];

                size_t bytes_in_buf =
                    (size_t)n - (offset + header_and_mask_len);
                size_t to_copy =
                    bytes_in_buf < payload_len ? bytes_in_buf : payload_len;

                char* payload = (char*)malloc(payload_len + 1);
                if (!payload) {
                  CLOSE_SOCKET(client_fds[i]);
                  pthread_mutex_lock(&server->sessions_mutex);
                  client_session_clear(&server->client_sessions[i]);

                  for (int j = i; j < num_clients - 1; j++) {
                    client_fds[j] = client_fds[j + 1];
                    strcpy(last_state[j], last_state[j + 1]);
                    server->client_sessions[j] = server->client_sessions[j + 1];
                  }
                  memset(&server->client_sessions[num_clients - 1], 0,
                         sizeof(client_session_t));
                  pthread_mutex_unlock(&server->sessions_mutex);
                  num_clients--;
                  i--;
                  break;
                }

                memcpy(payload, &buf[offset + header_and_mask_len], to_copy);
                size_t total_read = to_copy;
                bool read_ok = true;
                while (total_read < payload_len) {
                  int r = recv(client_fds[i], payload + total_read,
                               (int)(payload_len - total_read), 0);
                  if (r <= 0) {
                    read_ok = false;
                    break;
                  }
                  total_read += (size_t)r;
                }

                if (!read_ok) {
                  free(payload);
                  CLOSE_SOCKET(client_fds[i]);
                  pthread_mutex_lock(&server->sessions_mutex);
                  client_session_clear(&server->client_sessions[i]);

                  for (int j = i; j < num_clients - 1; j++) {
                    client_fds[j] = client_fds[j + 1];
                    strcpy(last_state[j], last_state[j + 1]);
                    server->client_sessions[j] = server->client_sessions[j + 1];
                  }
                  memset(&server->client_sessions[num_clients - 1], 0,
                         sizeof(client_session_t));
                  pthread_mutex_unlock(&server->sessions_mutex);
                  num_clients--;
                  i--;
                  break;
                }

                for (size_t p = 0; p < payload_len; p++) {
                  payload[p] ^= mask[p % 4];
                }
                payload[payload_len] = '\0';

                logger_debug(&server_logger, "Received WS frame: %s",
                             log_arg_string(payload), log_arg_none(),
                             log_arg_none(), log_arg_none());

                dyn_string_t ds;
                dyn_string_init(&ds, 4096);
                websocket_server_handle_command(server, i, payload, &ds);

                if (ds.data && ds.data[0] != '\0') {
                  logger_debug(&server_logger, "Sending WS response: %s",
                               log_arg_string(ds.data), log_arg_none(),
                               log_arg_none(), log_arg_none());
                  send_websocket_frame(client_fds[i], ds.data);
                }
                dyn_string_free(&ds);
                free(payload);

                if (bytes_in_buf < payload_len) {
                  break;
                } else {
                  offset += header_and_mask_len + payload_len;
                }
              } else {
                logger_debug(&server_logger, "Received raw TCP: %s",
                             log_arg_string(&buf[offset]), log_arg_none(),
                             log_arg_none(), log_arg_none());

                dyn_string_t ds;
                dyn_string_init(&ds, 4096);
                websocket_server_handle_command(server, i, &buf[offset], &ds);
                if (ds.data && ds.data[0] != '\0') {
                  logger_debug(&server_logger, "Sending raw TCP response: %s",
                               log_arg_string(ds.data), log_arg_none(),
                               log_arg_none(), log_arg_none());
                  send(client_fds[i], ds.data, (int)strlen(ds.data), 0);
                }
                dyn_string_free(&ds);
                break;
              }
            }
          }
        }
      }
    }
  }
  for (int i = 0; i < num_clients; i++) {
    CLOSE_SOCKET(client_fds[i]);
  }
  return NULL;
}

/// Start the WebSocket server listening and processing connections.
bool websocket_server_start(websocket_server_t* server) {
  if (!server || atomic_load_explicit(&server->running, memory_order_acquire))
    return false;

#ifdef _WIN32
  WSADATA wsaData;
  if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
    return false;
  }
#endif

  server->server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (IS_INVALID_SOCKET(server->server_fd)) {
#ifdef _WIN32
    WSACleanup();
#endif
    return false;
  }

  int opt = 1;
#ifdef _WIN32
  setsockopt(server->server_fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt,
             sizeof(opt));
#else
  setsockopt(server->server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(server->port);
  inet_pton(AF_INET, server->host, &addr.sin_addr);

  if (IS_SOCKET_ERROR(
          bind(server->server_fd, (struct sockaddr*)&addr, sizeof(addr)))) {
    CLOSE_SOCKET(server->server_fd);
    server->server_fd = INVALID_SOCKET_VAL;
#ifdef _WIN32
    WSACleanup();
#endif
    return false;
  }

  if (IS_SOCKET_ERROR(listen(server->server_fd, 10))) {
    CLOSE_SOCKET(server->server_fd);
    server->server_fd = INVALID_SOCKET_VAL;
#ifdef _WIN32
    WSACleanup();
#endif
    return false;
  }

  atomic_store_explicit(&server->running, true, memory_order_release);
  if (pthread_create(&server->thread, NULL, server_thread_func, server) != 0) {
    atomic_store_explicit(&server->running, false, memory_order_release);
    CLOSE_SOCKET(server->server_fd);
    server->server_fd = INVALID_SOCKET_VAL;
#ifdef _WIN32
    WSACleanup();
#endif
    return false;
  }

  return true;
}

/// Stop the WebSocket server and disconnect all clients.
void websocket_server_stop(websocket_server_t* server) {
  if (!server || !atomic_load_explicit(&server->running, memory_order_acquire))
    return;
  atomic_store_explicit(&server->running, false, memory_order_release);
  pthread_join(server->thread, NULL);
  if (!IS_INVALID_SOCKET(server->server_fd)) {
    CLOSE_SOCKET(server->server_fd);
    server->server_fd = INVALID_SOCKET_VAL;
  }
#ifdef _WIN32
  WSACleanup();
#endif
}

/// Destroy and free the WebSocket server.
void websocket_server_free(websocket_server_t* server) {
  if (!server) return;
  websocket_server_stop(server);

  // Free level history arrays
  level_history_clear(&server->capture_peak_history);
  level_history_clear(&server->capture_rms_history);
  level_history_clear(&server->playback_peak_history);
  level_history_clear(&server->playback_rms_history);

  // Free global peak arrays
  if (server->capture_global_peaks) free(server->capture_global_peaks);
  if (server->playback_global_peaks) free(server->playback_global_peaks);

  // Free client sessions
  for (size_t i = 0; i < 32; i++) {
    client_session_clear(&server->client_sessions[i]);
  }

  pthread_mutex_destroy(&server->sessions_mutex);
  free(server);
}

bool websocket_server_get_client_vu_subscribed(const websocket_server_t* server,
                                               int client_idx) {
  if (!server || client_idx < 0 || client_idx >= 32) return false;
  pthread_mutex_lock((pthread_mutex_t*)&server->sessions_mutex);
  bool res = server->client_sessions[client_idx].vu_subscribed;
  pthread_mutex_unlock((pthread_mutex_t*)&server->sessions_mutex);
  return res;
}

double websocket_server_get_client_vu_max_rate(const websocket_server_t* server,
                                               int client_idx) {
  if (!server || client_idx < 0 || client_idx >= 32) return 0.0;
  pthread_mutex_lock((pthread_mutex_t*)&server->sessions_mutex);
  double res = server->client_sessions[client_idx].vu_max_rate;
  pthread_mutex_unlock((pthread_mutex_t*)&server->sessions_mutex);
  return res;
}

double websocket_server_get_client_vu_attack(const websocket_server_t* server,
                                             int client_idx) {
  if (!server || client_idx < 0 || client_idx >= 32) return 0.0;
  pthread_mutex_lock((pthread_mutex_t*)&server->sessions_mutex);
  double res = server->client_sessions[client_idx].vu_attack;
  pthread_mutex_unlock((pthread_mutex_t*)&server->sessions_mutex);
  return res;
}

double websocket_server_get_client_vu_release(const websocket_server_t* server,
                                              int client_idx) {
  if (!server || client_idx < 0 || client_idx >= 32) return 0.0;
  pthread_mutex_lock((pthread_mutex_t*)&server->sessions_mutex);
  double res = server->client_sessions[client_idx].vu_release;
  pthread_mutex_unlock((pthread_mutex_t*)&server->sessions_mutex);
  return res;
}

void websocket_server_set_client_vu_subscribed(websocket_server_t* server,
                                               int client_idx,
                                               bool subscribed) {
  if (!server || client_idx < 0 || client_idx >= 32) return;
  pthread_mutex_lock(&server->sessions_mutex);
  server->client_sessions[client_idx].vu_subscribed = subscribed;
  pthread_mutex_unlock(&server->sessions_mutex);
}
