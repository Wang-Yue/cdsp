#ifndef CLIB_SERVER_WEBSOCKET_SERVER_INTERNAL_H
#define CLIB_SERVER_WEBSOCKET_SERVER_INTERNAL_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "Server/websocket_server.h"

typedef struct cJSON cJSON;

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
typedef SOCKET socket_t;
#define CLOSE_SOCKET(s) closesocket(s)
#define INVALID_SOCKET_VAL INVALID_SOCKET
#define IS_INVALID_SOCKET(s) ((s) == INVALID_SOCKET)
#define IS_SOCKET_ERROR(r) ((r) == SOCKET_ERROR)
#else
#include <unistd.h>
typedef int socket_t;
#define CLOSE_SOCKET(s) close(s)
#define INVALID_SOCKET_VAL (-1)
#define IS_INVALID_SOCKET(s) ((s) < 0)
#define IS_SOCKET_ERROR(r) ((r) < 0)
#endif

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

typedef struct {
  uint64_t last_cap_peak_time;
  uint64_t last_cap_rms_time;
  uint64_t last_pb_peak_time;
  uint64_t last_pb_rms_time;

  bool state_subscribed;
  bool vu_subscribed;
  bool signal_levels_subscribed;
  char signal_levels_side[16];
  bool spectrum_subscribed;
  bool spectrum_is_capture;
  uint32_t spectrum_channel;
  float spectrum_min_freq;
  float spectrum_max_freq;
  uint32_t spectrum_n_bins;
  float spectrum_max_rate;
  uint64_t last_spectrum_push_time;

  float vu_max_rate;
  float vu_attack;
  float vu_release;
  uint64_t last_vu_push_time;

  float* vu_pb_rms;
  float* vu_pb_peak;
  float* vu_cap_rms;
  float* vu_cap_peak;
  size_t vu_pb_channels;
  size_t vu_cap_channels;
} client_session_t;

struct websocket_server {
  uint16_t port;
  char host[128];
  dsp_engine_t* engine;

  socket_t server_fd;
  _Atomic bool running;
  pthread_t thread;

  uint32_t update_interval;

  float* capture_global_peaks;
  float* playback_global_peaks;
  size_t capture_global_peaks_count;
  size_t playback_global_peaks_count;

  pthread_mutex_t sessions_mutex;
  client_session_t client_sessions[32];
};

uint64_t get_time_ms(void);
float db_to_amplitude(float db);
float amplitude_to_db(float amp);
void client_session_clear(client_session_t* session);
void dyn_string_printf(dyn_string_t* ds, const char* fmt, ...);
void free_vu_levels_arrays(cdsp_vu_levels_t* vu);
cJSON* serialize_stop_reason(const cdsp_stop_reason_t* reason);
cJSON* create_state_event_value(cdsp_processing_state_t state,
                                const cdsp_stop_reason_t* reason);

#endif  // CLIB_SERVER_WEBSOCKET_SERVER_INTERNAL_H
