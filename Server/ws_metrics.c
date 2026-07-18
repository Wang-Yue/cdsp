// Server/ws_metrics.c
// Encapsulates all DSP telemetry, envelope decay smoothing, and JSON event
// formatting

#include "ws_metrics.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "Public/processing.h"
#include "Public/signal_levels.h"
#include "Utils/cdsp_time.h"
#include "websocket_server.h"
#include "ws_rpc_dispatcher.h"

uint64_t get_time_ms(void) { return cdsp_time_now_ns() / 1000000; }

double db_to_amplitude(double db) {
  if (db <= -1000.0) return 0.0;
  return pow(10.0, db / 20.0);
}

double amplitude_to_db(double amp) {
  if (amp <= 0.0) return -1000.0;
  double db = 20.0 * log10(amp);
  return db < -1000.0 ? -1000.0 : db;
}

void level_history_clear(level_history_t* history) {
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

void level_history_get_max_since(const level_history_t* history,
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
      if (sample->levels && sample->levels[c] > out_levels[c]) {
        out_levels[c] = sample->levels[c];
      }
    }
    idx = (idx + 300 - 1) % 300;
  }
}

void level_history_get_rms_since(const level_history_t* history,
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
      if (sample->levels && sums) {
        double val = db_to_amplitude(sample->levels[c]);
        sums[c] += val * val;
      }
    }
    count++;
    idx = (idx + 300 - 1) % 300;
  }
  if (count > 0 && sums) {
    for (size_t c = 0; c < channels; c++) {
      out_levels[c] = amplitude_to_db(sqrt(sums[c] / (double)count));
    }
  }
  if (sums) free(sums);
}

static double smoothing_alpha(double dt_ms, double tc_ms) {
  if (tc_ms <= 0.0) return 1.0;
  return 1.0 - exp(-dt_ms / tc_ms);
}

void ws_metrics_broadcast_tick(websocket_server_t* server) {
  if (!server) return;

  uint64_t now = get_time_ms();

  ws_state_update_t status = {0};
  bool has_status = false;
  if (server->engine) {
    status.state = cdsp_get_state(server->engine);
    cdsp_get_stop_reason(server->engine, &status.stop_reason);
    has_status = true;
  }

  const char* state_str = "Inactive";
  if (has_status) {
    state_str = ws_processing_state_to_string(status.state);
  }

  double* current_cap_peak = NULL;
  double* current_cap_rms = NULL;
  double* current_pb_peak = NULL;
  double* current_pb_rms = NULL;
  size_t cap_channels = 0;
  size_t pb_channels = 0;

  cdsp_vu_levels_t vu = {0};
  if (server->engine && cdsp_get_vu_levels(server->engine, &vu)) {
    cap_channels = vu.capture_channels;
    pb_channels = vu.playback_channels;
    current_cap_peak = vu.capture_peak;
    current_cap_rms = vu.capture_rms;
    current_pb_peak = vu.playback_peak;
    current_pb_rms = vu.playback_rms;

    if (cap_channels > 0 && current_cap_peak && current_cap_rms) {
      level_history_append(&server->metrics.capture_peak_history,
                           current_cap_peak, cap_channels, now);
      level_history_append(&server->metrics.capture_rms_history,
                           current_cap_rms, cap_channels, now);

      if (server->metrics.capture_global_peaks_count != cap_channels) {
        double* new_peaks =
            (double*)realloc(server->metrics.capture_global_peaks,
                             cap_channels * sizeof(double));
        if (new_peaks) {
          server->metrics.capture_global_peaks = new_peaks;
          for (size_t k = server->metrics.capture_global_peaks_count;
               k < cap_channels; k++) {
            server->metrics.capture_global_peaks[k] = -1000.0;
          }
          server->metrics.capture_global_peaks_count = cap_channels;
        }
      }
      size_t limit = cap_channels < server->metrics.capture_global_peaks_count
                         ? cap_channels
                         : server->metrics.capture_global_peaks_count;
      for (size_t k = 0; k < limit; k++) {
        if (server->metrics.capture_global_peaks &&
            current_cap_peak[k] > server->metrics.capture_global_peaks[k]) {
          server->metrics.capture_global_peaks[k] = current_cap_peak[k];
        }
      }
    }

    if (pb_channels > 0 && current_pb_peak && current_pb_rms) {
      level_history_append(&server->metrics.playback_peak_history,
                           current_pb_peak, pb_channels, now);
      level_history_append(&server->metrics.playback_rms_history,
                           current_pb_rms, pb_channels, now);

      if (server->metrics.playback_global_peaks_count != pb_channels) {
        double* new_peaks =
            (double*)realloc(server->metrics.playback_global_peaks,
                             pb_channels * sizeof(double));
        if (new_peaks) {
          server->metrics.playback_global_peaks = new_peaks;
          for (size_t k = server->metrics.playback_global_peaks_count;
               k < pb_channels; k++) {
            server->metrics.playback_global_peaks[k] = -1000.0;
          }
          server->metrics.playback_global_peaks_count = pb_channels;
        }
      }
      size_t limit = pb_channels < server->metrics.playback_global_peaks_count
                         ? pb_channels
                         : server->metrics.playback_global_peaks_count;
      for (size_t k = 0; k < limit; k++) {
        if (server->metrics.playback_global_peaks &&
            current_pb_peak[k] > server->metrics.playback_global_peaks[k]) {
          server->metrics.playback_global_peaks[k] = current_pb_peak[k];
        }
      }
    }
  }

  // We need to keep a local static/history for state tracking across ticks
  static char last_state[32][64];
  static bool last_state_initialized = false;
  if (!last_state_initialized) {
    for (int i = 0; i < 32; i++) {
      last_state[i][0] = '\0';
    }
    last_state_initialized = true;
  }

  pthread_mutex_lock(&server->sessions_mutex);
  for (int i = 0; i < 32; i++) {
    client_session_t* session = server->client_sessions[i];
    if (!session) continue;

    if (session->metrics.state_subscribed &&
        strcmp(last_state[i], state_str) != 0) {
      strncpy(last_state[i], state_str, sizeof(last_state[i]) - 1);
      ws_rpc_emit_state_event(session, status.state, &status.stop_reason);
    }

    if (session->metrics.vu_subscribed && pb_channels > 0) {
      double interval = session->metrics.vu_max_rate > 0.0
                            ? 1000.0 / session->metrics.vu_max_rate
                            : 0.0;
      if (now - session->metrics.last_vu_push_time >= interval) {
        double dt = session->metrics.last_vu_push_time == 0
                        ? 100.0
                        : (double)(now - session->metrics.last_vu_push_time);
        double attack = smoothing_alpha(dt, session->metrics.vu_attack);
        double release = smoothing_alpha(dt, session->metrics.vu_release);

        if (session->metrics.vu_pb_channels != pb_channels) {
          double* new_rms = (double*)calloc(pb_channels, sizeof(double));
          double* new_peak = (double*)calloc(pb_channels, sizeof(double));
          if (new_rms && new_peak) {
            size_t copy_count = session->metrics.vu_pb_channels < pb_channels
                                    ? session->metrics.vu_pb_channels
                                    : pb_channels;
            if (session->metrics.vu_pb_rms) {
              memcpy(new_rms, session->metrics.vu_pb_rms,
                     copy_count * sizeof(double));
              free(session->metrics.vu_pb_rms);
            }
            if (session->metrics.vu_pb_peak) {
              memcpy(new_peak, session->metrics.vu_pb_peak,
                     copy_count * sizeof(double));
              free(session->metrics.vu_pb_peak);
            }
            for (size_t k = copy_count; k < pb_channels; k++) {
              new_rms[k] = current_pb_rms[k];
              new_peak[k] = current_pb_peak[k];
            }
            session->metrics.vu_pb_rms = new_rms;
            session->metrics.vu_pb_peak = new_peak;
            session->metrics.vu_pb_channels = pb_channels;
          } else {
            if (new_rms) free(new_rms);
            if (new_peak) free(new_peak);
          }
        } else {
          for (size_t k = 0; k < pb_channels; k++) {
            double prev_amp = db_to_amplitude(session->metrics.vu_pb_rms[k]);
            double curr_amp = db_to_amplitude(current_pb_rms[k]);
            double diff = curr_amp - prev_amp;
            if (diff > 0.0)
              prev_amp += attack * diff;
            else
              prev_amp += release * diff;
            session->metrics.vu_pb_rms[k] = amplitude_to_db(prev_amp);
          }
          for (size_t k = 0; k < pb_channels; k++) {
            double prev_amp = db_to_amplitude(session->metrics.vu_pb_peak[k]);
            double curr_amp = db_to_amplitude(current_pb_peak[k]);
            double diff = curr_amp - prev_amp;
            if (diff > 0.0)
              prev_amp += 1.0 * diff;
            else
              prev_amp += release * diff;
            session->metrics.vu_pb_peak[k] = amplitude_to_db(prev_amp);
          }
        }

        if (cap_channels > 0) {
          if (session->metrics.vu_cap_channels != cap_channels) {
            double* new_rms = (double*)calloc(cap_channels, sizeof(double));
            double* new_peak = (double*)calloc(cap_channels, sizeof(double));
            if (new_rms && new_peak) {
              size_t copy_count =
                  session->metrics.vu_cap_channels < cap_channels
                      ? session->metrics.vu_cap_channels
                      : cap_channels;
              if (session->metrics.vu_cap_rms) {
                memcpy(new_rms, session->metrics.vu_cap_rms,
                       copy_count * sizeof(double));
                free(session->metrics.vu_cap_rms);
              }
              if (session->metrics.vu_cap_peak) {
                memcpy(new_peak, session->metrics.vu_cap_peak,
                       copy_count * sizeof(double));
                free(session->metrics.vu_cap_peak);
              }
              for (size_t k = copy_count; k < cap_channels; k++) {
                new_rms[k] = current_cap_rms[k];
                new_peak[k] = current_cap_peak[k];
              }
              session->metrics.vu_cap_rms = new_rms;
              session->metrics.vu_cap_peak = new_peak;
              session->metrics.vu_cap_channels = cap_channels;
            } else {
              if (new_rms) free(new_rms);
              if (new_peak) free(new_peak);
            }
          } else {
            for (size_t k = 0; k < cap_channels; k++) {
              double prev_amp = db_to_amplitude(session->metrics.vu_cap_rms[k]);
              double curr_amp = db_to_amplitude(current_cap_rms[k]);
              double diff = curr_amp - prev_amp;
              if (diff > 0.0)
                prev_amp += attack * diff;
              else
                prev_amp += release * diff;
              session->metrics.vu_cap_rms[k] = amplitude_to_db(prev_amp);
            }
            for (size_t k = 0; k < cap_channels; k++) {
              double prev_amp =
                  db_to_amplitude(session->metrics.vu_cap_peak[k]);
              double curr_amp = db_to_amplitude(current_cap_peak[k]);
              double diff = curr_amp - prev_amp;
              if (diff > 0.0)
                prev_amp += 1.0 * diff;
              else
                prev_amp += release * diff;
              session->metrics.vu_cap_peak[k] = amplitude_to_db(prev_amp);
            }
          }
        }

        session->metrics.last_vu_push_time = now;
        ws_rpc_emit_vu_event(
            session, session->metrics.vu_pb_rms, session->metrics.vu_pb_peak,
            session->metrics.vu_pb_channels, session->metrics.vu_cap_rms,
            session->metrics.vu_cap_peak, session->metrics.vu_cap_channels);
      }
    }

    if (session->metrics.signal_levels_subscribed) {
      if (strcmp(session->metrics.signal_levels_side, "capture") == 0) {
        ws_rpc_emit_signal_levels_event(session, current_cap_rms,
                                        current_cap_peak, cap_channels);
      } else {
        ws_rpc_emit_signal_levels_event(session, current_pb_rms,
                                        current_pb_peak, pb_channels);
      }
    }

    if (session->metrics.spectrum_subscribed) {
      double interval = session->metrics.spectrum_max_rate > 0.0
                            ? 1000.0 / session->metrics.spectrum_max_rate
                            : 0.0;
      if (now - session->metrics.last_spectrum_push_time >= interval) {
        ws_rpc_emit_spectrum_event(server, session);
        session->metrics.last_spectrum_push_time = now;
      }
    }
  }
  pthread_mutex_unlock(&server->sessions_mutex);
}
