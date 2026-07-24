#include "sample_rate_watcher.h"

#include <stdlib.h>

#include "Logging/app_logger.h"
#include "Utils/cdsp_time.h"

static const logger_t g_logger = {"dsp.engine.rate_watcher"};

struct sample_rate_watcher {
  double target_rate;
  double measure_interval;
  bool stop_on_rate_change;
  size_t captured_frames;
  uint64_t last_reset_ns;
  int deviation_count;
  double last_measured_rate;
};

sample_rate_watcher_t* sample_rate_watcher_create(double target_rate,
                                                  double measure_interval,
                                                  bool stop_on_rate_change) {
  sample_rate_watcher_t* watcher =
      (sample_rate_watcher_t*)calloc(1, sizeof(sample_rate_watcher_t));
  if (!watcher) return NULL;
  watcher->target_rate = target_rate;
  watcher->measure_interval = measure_interval > 0.1 ? measure_interval : 1.0;
  watcher->stop_on_rate_change = stop_on_rate_change;
  watcher->captured_frames = 0;
  watcher->last_reset_ns = 0;
  watcher->deviation_count = 0;
  watcher->last_measured_rate = target_rate;
  return watcher;
}

void sample_rate_watcher_free(sample_rate_watcher_t* watcher) { free(watcher); }

void sample_rate_watcher_reset(sample_rate_watcher_t* watcher) {
  if (!watcher) return;
  watcher->captured_frames = 0;
  watcher->last_reset_ns = cdsp_time_now_ns();
  watcher->deviation_count = 0;
}

bool sample_rate_watcher_tick(sample_rate_watcher_t* watcher, size_t frames,
                              double* out_measured_rate) {
  if (!watcher) return false;
  if (watcher->last_reset_ns == 0) {
    watcher->last_reset_ns = cdsp_time_now_ns();
  }
  watcher->captured_frames += frames;
  uint64_t now = cdsp_time_now_ns();
  double elapsed = (double)(now - watcher->last_reset_ns) / 1000000000.0;

  if (elapsed <= 0.0 || elapsed < watcher->measure_interval) {
    return false;
  }

  double measured_rate = (double)watcher->captured_frames / elapsed;
  watcher->last_measured_rate = measured_rate;
  watcher->captured_frames = 0;
  watcher->last_reset_ns = now;

  double min_val = watcher->target_rate / 1.04;
  double max_val = watcher->target_rate * 1.04;

  if (measured_rate < min_val || measured_rate > max_val) {
    watcher->deviation_count++;
    logger_warn(&g_logger,
                "Sample rate deviation tick %d: measured %.1fHz, target %.1fHz",
                watcher->deviation_count, measured_rate, watcher->target_rate);
  } else {
    watcher->deviation_count = 0;
  }

  if (watcher->deviation_count >= 4) {
    logger_error(&g_logger,
                 "Sample rate deviation persistent! Measured rate: %.1fHz "
                 "(target %.1fHz)",
                 measured_rate, watcher->target_rate);
    if (out_measured_rate) {
      *out_measured_rate = measured_rate;
    }
    return true;
  }
  return false;
}

bool sample_rate_watcher_get_stop_on_rate_change(
    const sample_rate_watcher_t* watcher) {
  return watcher ? watcher->stop_on_rate_change : false;
}

double sample_rate_watcher_get_last_measured_rate(
    const sample_rate_watcher_t* watcher) {
  if (!watcher) return 0.0;
  return watcher->last_measured_rate > 0.0 ? watcher->last_measured_rate
                                           : watcher->target_rate;
}
