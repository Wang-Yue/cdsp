#ifndef CLIB_SERVER_WS_METRICS_H
#define CLIB_SERVER_WS_METRICS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "Public/cdsp_pub_types.h"

typedef struct {
  uint64_t timestamp_ms;
  double* levels;
} level_sample_t;

typedef struct {
  level_sample_t samples[300];
  size_t head;
  size_t size;
  size_t channels;
} level_history_t;

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
  double spectrum_min_freq;
  double spectrum_max_freq;
  uint32_t spectrum_n_bins;
  double spectrum_max_rate;
  uint64_t last_spectrum_push_time;

  double vu_max_rate;
  double vu_attack;
  double vu_release;
  uint64_t last_vu_push_time;

  double* vu_pb_rms;
  double* vu_pb_peak;
  double* vu_cap_rms;
  double* vu_cap_peak;
  size_t vu_pb_channels;
  size_t vu_cap_channels;
} session_metrics_t;

typedef struct {
  level_history_t capture_peak_history;
  level_history_t capture_rms_history;
  level_history_t playback_peak_history;
  level_history_t playback_rms_history;

  double* capture_global_peaks;
  double* playback_global_peaks;
  size_t capture_global_peaks_count;
  size_t playback_global_peaks_count;
} ws_metrics_state_t;

uint64_t get_time_ms(void);
double db_to_amplitude(double db);
double amplitude_to_db(double amp);
void level_history_clear(level_history_t* history);
void level_history_get_max_since(const level_history_t* history,
                                 uint64_t since_ms, double* out_levels);
void level_history_get_rms_since(const level_history_t* history,
                                 uint64_t since_ms, double* out_levels);

typedef struct websocket_server websocket_server_t;
typedef struct client_session_s client_session_t;

void ws_metrics_broadcast_tick(websocket_server_t* server);

#endif  // CLIB_SERVER_WS_METRICS_H
