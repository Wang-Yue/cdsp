// Concurrency model
// -----------------
// Every field is backed by lock-free atomics (`atomic_double_t` or `_Atomic
// bool`) — no mutexes or locks.
#include "Audio/processing_parameters.h"

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "Audio/audio_chunk.h"
#include "Utils/cdsp_memory.h"
#include "Utils/cdsp_time.h"
#include "Utils/double_helpers.h"
#include "Utils/float_helpers.h"
#include "Utils/lock_free_ring_buffer.h"

/**
 * @brief Lock-free ring buffer tracking the last 1,024 chunk-level Peak or RMS
 * values per channel.
 */
typedef struct {
  size_t channels;
  _Atomic uint64_t write_seq;
  _Atomic size_t write_pos;
  _Atomic size_t total_written;
  uint64_t timestamps_ms[CHUNK_LEVEL_HISTORY_CAPACITY];
  float* data;  // planar array: channels * CHUNK_LEVEL_HISTORY_CAPACITY
} chunk_level_history_t;

static inline void chunk_level_history_init(chunk_level_history_t* hist,
                                            size_t channels) {
  if (!hist) return;
  hist->channels = channels;
  atomic_init(&hist->write_seq, 0ULL);
  atomic_init(&hist->write_pos, 0);
  atomic_init(&hist->total_written, 0);
  memset(hist->timestamps_ms, 0, sizeof(hist->timestamps_ms));
  if (channels > 0) {
    size_t total = channels * CHUNK_LEVEL_HISTORY_CAPACITY;
    hist->data = (float*)cdsp_aligned_alloc(64, total * sizeof(float));
    if (hist->data) {
      for (size_t i = 0; i < total; i++) {
        hist->data[i] = -INFINITY;
      }
    }
  } else {
    hist->data = NULL;
  }
}

static inline void chunk_level_history_free(chunk_level_history_t* hist) {
  if (!hist) return;
  if (hist->data) {
    cdsp_aligned_free(hist->data);
    hist->data = NULL;
  }
  hist->channels = 0;
}

static inline void chunk_level_history_get_max_since(
    const chunk_level_history_t* hist, uint64_t since_ms, float* out_levels,
    size_t count) {
  if (!out_levels || count == 0) return;
  for (size_t c = 0; c < count; c++) {
    out_levels[c] = -INFINITY;
  }
  if (!hist || !hist->data || hist->channels == 0) return;
  size_t ch_limit = count < hist->channels ? count : hist->channels;

  for (int retry = 0; retry < 10; retry++) {
    uint64_t seq_before =
        atomic_load_explicit(&hist->write_seq, memory_order_acquire);
    if (seq_before & 1) continue;

    size_t total =
        atomic_load_explicit(&hist->total_written, memory_order_acquire);
    if (total == 0) return;
    size_t available = total < CHUNK_LEVEL_HISTORY_CAPACITY
                           ? total
                           : CHUNK_LEVEL_HISTORY_CAPACITY;
    size_t pos = atomic_load_explicit(&hist->write_pos, memory_order_acquire);
    size_t mask = CHUNK_LEVEL_HISTORY_CAPACITY - 1;
    size_t idx = (pos + mask) & mask;

    for (size_t c = 0; c < ch_limit; c++) {
      out_levels[c] = -INFINITY;
    }

    for (size_t i = 0; i < available; i++) {
      uint64_t ts = hist->timestamps_ms[idx];
      if (ts < since_ms) break;
      for (size_t ch = 0; ch < ch_limit; ch++) {
        float val = hist->data[(ch * CHUNK_LEVEL_HISTORY_CAPACITY) + idx];
        if (val > out_levels[ch]) {
          out_levels[ch] = val;
        }
      }
      idx = (idx + mask) & mask;
    }

    uint64_t seq_after =
        atomic_load_explicit(&hist->write_seq, memory_order_acquire);
    if (seq_after == seq_before) return;
  }
}

static inline void chunk_level_history_get_rms_since(
    const chunk_level_history_t* hist, uint64_t since_ms, float* out_levels,
    size_t count) {
  if (!out_levels || count == 0) return;
  for (size_t c = 0; c < count; c++) {
    out_levels[c] = -INFINITY;
  }
  if (!hist || !hist->data || hist->channels == 0) return;
  size_t ch_limit = count < hist->channels ? count : hist->channels;

  for (int retry = 0; retry < 10; retry++) {
    uint64_t seq_before =
        atomic_load_explicit(&hist->write_seq, memory_order_acquire);
    if (seq_before & 1) continue;

    size_t total =
        atomic_load_explicit(&hist->total_written, memory_order_acquire);
    if (total == 0) return;
    size_t available = total < CHUNK_LEVEL_HISTORY_CAPACITY
                           ? total
                           : CHUNK_LEVEL_HISTORY_CAPACITY;
    size_t pos = atomic_load_explicit(&hist->write_pos, memory_order_acquire);
    size_t mask = CHUNK_LEVEL_HISTORY_CAPACITY - 1;
    size_t idx = (pos + mask) & mask;

    for (size_t ch = 0; ch < ch_limit; ch++) {
      out_levels[ch] = 0.0f;
    }
    size_t count_samples = 0;

    for (size_t i = 0; i < available; i++) {
      uint64_t ts = hist->timestamps_ms[idx];
      if (ts < since_ms) break;
      for (size_t ch = 0; ch < ch_limit; ch++) {
        float db = hist->data[(ch * CHUNK_LEVEL_HISTORY_CAPACITY) + idx];
        float amp = float_from_db(db);
        out_levels[ch] += amp * amp;
      }
      count_samples++;
      idx = (idx + mask) & mask;
    }

    uint64_t seq_after =
        atomic_load_explicit(&hist->write_seq, memory_order_acquire);
    if (seq_after == seq_before) {
      if (count_samples > 0) {
        for (size_t ch = 0; ch < ch_limit; ch++) {
          float mean_sq = out_levels[ch] / (float)count_samples;
          out_levels[ch] = 10.0f * log10f(mean_sq);
        }
      } else {
        for (size_t ch = 0; ch < count; ch++) {
          out_levels[ch] = -INFINITY;
        }
      }
      return;
    }
  }

  for (size_t ch = 0; ch < count; ch++) {
    out_levels[ch] = -INFINITY;
  }
}

struct processing_parameters {
  /** Target volume (dB) for fader 0-4. UI thread writes; audio thread reads. */
  atomic_double_t target_volumes[FADER_COUNT];
  /** Current volume (dB) for fader 0-4. Audio thread updates during ramping. */
  atomic_double_t current_volumes[FADER_COUNT];
  /** Mute state for fader 0-4. UI thread writes; audio thread reads. */
  _Atomic bool muted[FADER_COUNT];
  /** Interruptions counter (bumped whenever audio stream pauses or stalls). */
  _Atomic uint64_t pause_count;

  size_t capture_channels;  /**< Number of capture channels. */
  size_t playback_channels; /**< Number of playback channels. */

  /** Per-channel capture signal peak levels (dB). Array size: capture_channels.
   */
  atomic_float_t* capture_signal_peak;
  /** Per-channel capture signal RMS levels (dB). Array size: capture_channels.
   */
  atomic_float_t* capture_signal_rms;
  /** Per-channel playback signal peak levels (dB). Array size:
   * playback_channels. */
  atomic_float_t* playback_signal_peak;
  /** Per-channel playback signal RMS levels (dB). Array size:
   * playback_channels. */
  atomic_float_t* playback_signal_rms;

  /** Per-chunk history of capture peak levels (1,024 chunk records). */
  chunk_level_history_t capture_peak_history;
  /** Per-chunk history of capture RMS levels (1,024 chunk records). */
  chunk_level_history_t capture_rms_history;
  /** Per-chunk history of playback peak levels (1,024 chunk records). */
  chunk_level_history_t playback_peak_history;
  /** Per-chunk history of playback RMS levels (1,024 chunk records). */
  chunk_level_history_t playback_rms_history;

  // MARK: - Telemetry
  atomic_double_t rate_adjust; /**< Current rate adjustment factor. */
  atomic_double_t
      measured_capture_rate;        /**< Measured capture sample rate (Hz). */
  atomic_double_t buffer_level;     /**< Current buffer level. */
  _Atomic uint64_t clipped_samples; /**< Cumulative count of clipped samples. */
  atomic_double_t processing_load;  /**< Audio processing load (0.0 to 1.0). */
  atomic_double_t
      resampler_load; /**< Resampler processing load (0.0 to 1.0). */
};

size_t processing_parameters_get_capture_channels(
    const processing_parameters_t* params) {
  return params ? params->capture_channels : 0;
}

size_t processing_parameters_get_playback_channels(
    const processing_parameters_t* params) {
  return params ? params->playback_channels : 0;
}

double processing_parameters_get_rate_adjust(
    const processing_parameters_t* params) {
  return params ? atomic_double_get(&params->rate_adjust) : 1.0;
}

void processing_parameters_set_rate_adjust(processing_parameters_t* params,
                                           double value) {
  if (params) atomic_double_set(&params->rate_adjust, value);
}

double processing_parameters_get_buffer_level(
    const processing_parameters_t* params) {
  return params ? atomic_double_get(&params->buffer_level) : 0.0;
}

void processing_parameters_set_buffer_level(processing_parameters_t* params,
                                            double value) {
  if (params) atomic_double_set(&params->buffer_level, value);
}

uint64_t processing_parameters_get_clipped_samples(
    const processing_parameters_t* params) {
  return params ? atomic_load_explicit(&params->clipped_samples,
                                       memory_order_relaxed)
                : 0ULL;
}

void processing_parameters_add_clipped_samples(processing_parameters_t* params,
                                               uint64_t count) {
  if (params && count > 0) {
    atomic_fetch_add_explicit(&params->clipped_samples, count,
                              memory_order_relaxed);
  }
}

void processing_parameters_reset_clipped_samples(
    processing_parameters_t* params) {
  if (params) {
    atomic_store_explicit(&params->clipped_samples, 0ULL, memory_order_relaxed);
  }
}

double processing_parameters_get_processing_load(
    const processing_parameters_t* params) {
  return params ? atomic_double_get(&params->processing_load) : 0.0;
}

void processing_parameters_set_processing_load(processing_parameters_t* params,
                                               double value) {
  if (params) atomic_double_set(&params->processing_load, value);
}

double processing_parameters_get_resampler_load(
    const processing_parameters_t* params) {
  return params ? atomic_double_get(&params->resampler_load) : 0.0;
}

void processing_parameters_set_resampler_load(processing_parameters_t* params,
                                              double value) {
  if (params) atomic_double_set(&params->resampler_load, value);
}

processing_parameters_t* processing_parameters_create(
    size_t capture_channels, size_t playback_channels) {
  processing_parameters_t* params =
      (processing_parameters_t*)calloc(1, sizeof(processing_parameters_t));
  if (!params) return NULL;

  for (int i = 0; i < FADER_COUNT; i++) {
    atomic_double_init(&params->target_volumes[i],
                       PROCESSING_PARAMETERS_DEFAULT_VOLUME);
    atomic_double_init(&params->current_volumes[i],
                       PROCESSING_PARAMETERS_DEFAULT_VOLUME);
    atomic_init(&params->muted[i], PROCESSING_PARAMETERS_DEFAULT_MUTE);
  }
  atomic_init(&params->pause_count, 0ULL);

  params->capture_channels = capture_channels;
  params->playback_channels = playback_channels;

  chunk_level_history_init(&params->capture_peak_history, capture_channels);
  chunk_level_history_init(&params->capture_rms_history, capture_channels);
  chunk_level_history_init(&params->playback_peak_history, playback_channels);
  chunk_level_history_init(&params->playback_rms_history, playback_channels);

  if (capture_channels > 0) {
    params->capture_signal_peak =
        (atomic_float_t*)calloc(capture_channels, sizeof(atomic_float_t));
    params->capture_signal_rms =
        (atomic_float_t*)calloc(capture_channels, sizeof(atomic_float_t));
    if (!params->capture_signal_peak || !params->capture_signal_rms ||
        !params->capture_peak_history.data ||
        !params->capture_rms_history.data) {
      processing_parameters_free(params);
      return NULL;
    }
    for (size_t i = 0; i < capture_channels; i++) {
      atomic_float_init(&params->capture_signal_peak[i], -INFINITY);
      atomic_float_init(&params->capture_signal_rms[i], -INFINITY);
    }
  }

  if (playback_channels > 0) {
    params->playback_signal_peak =
        (atomic_float_t*)calloc(playback_channels, sizeof(atomic_float_t));
    params->playback_signal_rms =
        (atomic_float_t*)calloc(playback_channels, sizeof(atomic_float_t));
    if (!params->playback_signal_peak || !params->playback_signal_rms ||
        !params->playback_peak_history.data ||
        !params->playback_rms_history.data) {
      processing_parameters_free(params);
      return NULL;
    }
    for (size_t i = 0; i < playback_channels; i++) {
      atomic_float_init(&params->playback_signal_peak[i], -INFINITY);
      atomic_float_init(&params->playback_signal_rms[i], -INFINITY);
    }
  }

  atomic_double_init(&params->rate_adjust, 1.0);
  atomic_double_init(&params->measured_capture_rate, 0.0);
  atomic_double_init(&params->buffer_level, 0.0);
  atomic_init(&params->clipped_samples, 0ULL);
  atomic_double_init(&params->processing_load, 0.0);
  atomic_double_init(&params->resampler_load, 0.0);

  return params;
}

void processing_parameters_free(processing_parameters_t* params) {
  if (!params) return;
  if (params->capture_signal_peak) free(params->capture_signal_peak);
  if (params->capture_signal_rms) free(params->capture_signal_rms);
  if (params->playback_signal_peak) free(params->playback_signal_peak);
  if (params->playback_signal_rms) free(params->playback_signal_rms);
  chunk_level_history_free(&params->capture_peak_history);
  chunk_level_history_free(&params->capture_rms_history);
  chunk_level_history_free(&params->playback_peak_history);
  chunk_level_history_free(&params->playback_rms_history);
  free(params);
}

double processing_parameters_get_target_volume_for_fader(
    const processing_parameters_t* params, fader_t fader) {
  if (!params || fader < 0 || fader >= FADER_COUNT) return 0.0;
  return atomic_double_get(&params->target_volumes[fader]);
}

void processing_parameters_set_target_volume_for_fader(
    processing_parameters_t* params, double value, fader_t fader) {
  if (!params || fader < 0 || fader >= FADER_COUNT) return;
  atomic_double_set(&params->target_volumes[fader], value);
}

void processing_parameters_bump_pause_count(processing_parameters_t* params) {
  if (params) {
    atomic_fetch_add_explicit(&params->pause_count, 1ULL, memory_order_relaxed);
  }
}

uint64_t processing_parameters_get_pause_count(
    const processing_parameters_t* params) {
  return params
             ? atomic_load_explicit(&params->pause_count, memory_order_relaxed)
             : 0ULL;
}

double processing_parameters_get_measured_capture_rate(
    const processing_parameters_t* params) {
  return params ? atomic_double_get(&params->measured_capture_rate) : 0.0;
}

void processing_parameters_set_measured_capture_rate(
    processing_parameters_t* params, double rate) {
  if (params) {
    atomic_double_set(&params->measured_capture_rate, rate);
  }
}

double processing_parameters_get_current_volume_for_fader(
    const processing_parameters_t* params, fader_t fader) {
  if (!params || fader < 0 || fader >= FADER_COUNT) return 0.0;
  return atomic_double_get(&params->current_volumes[fader]);
}

void processing_parameters_set_current_volume_for_fader(
    processing_parameters_t* params, double value, fader_t fader) {
  if (!params || fader < 0 || fader >= FADER_COUNT) return;
  atomic_double_set(&params->current_volumes[fader], value);
}

bool processing_parameters_is_muted_for_fader(
    const processing_parameters_t* params, fader_t fader) {
  if (!params || fader < 0 || fader >= FADER_COUNT) return false;
  return atomic_load_explicit(&params->muted[fader], memory_order_acquire);
}

void processing_parameters_set_muted_for_fader(processing_parameters_t* params,
                                               bool value, fader_t fader) {
  if (!params || fader < 0 || fader >= FADER_COUNT) return;
  atomic_store_explicit(&params->muted[fader], value, memory_order_release);
}

void processing_parameters_get_capture_signal_peak(
    const processing_parameters_t* params, float* out_levels, size_t count) {
  if (!params || !out_levels || !params->capture_signal_peak) return;
  size_t limit =
      count < params->capture_channels ? count : params->capture_channels;
  for (size_t i = 0; i < limit; i++) {
    out_levels[i] = atomic_float_get(&params->capture_signal_peak[i]);
  }
}

void processing_parameters_set_capture_signal_peak(
    processing_parameters_t* params, const float* levels, size_t count) {
  if (!params || !levels || !params->capture_signal_peak) return;
  size_t limit =
      count < params->capture_channels ? count : params->capture_channels;
  for (size_t i = 0; i < limit; i++) {
    atomic_float_set(&params->capture_signal_peak[i], levels[i]);
  }
}

void processing_parameters_get_capture_signal_rms(
    const processing_parameters_t* params, float* out_levels, size_t count) {
  if (!params || !out_levels || !params->capture_signal_rms) return;
  size_t limit =
      count < params->capture_channels ? count : params->capture_channels;
  for (size_t i = 0; i < limit; i++) {
    out_levels[i] = atomic_float_get(&params->capture_signal_rms[i]);
  }
}

void processing_parameters_set_capture_signal_rms(
    processing_parameters_t* params, const float* levels, size_t count) {
  if (!params || !levels || !params->capture_signal_rms) return;
  size_t limit =
      count < params->capture_channels ? count : params->capture_channels;
  for (size_t i = 0; i < limit; i++) {
    atomic_float_set(&params->capture_signal_rms[i], levels[i]);
  }
}

void processing_parameters_get_playback_signal_peak(
    const processing_parameters_t* params, float* out_levels, size_t count) {
  if (!params || !out_levels || !params->playback_signal_peak) return;
  size_t limit =
      count < params->playback_channels ? count : params->playback_channels;
  for (size_t i = 0; i < limit; i++) {
    out_levels[i] = atomic_float_get(&params->playback_signal_peak[i]);
  }
}

void processing_parameters_set_playback_signal_peak(
    processing_parameters_t* params, const float* levels, size_t count) {
  if (!params || !levels || !params->playback_signal_peak) return;
  size_t limit =
      count < params->playback_channels ? count : params->playback_channels;
  for (size_t i = 0; i < limit; i++) {
    atomic_float_set(&params->playback_signal_peak[i], levels[i]);
  }
}

void processing_parameters_get_playback_signal_rms(
    const processing_parameters_t* params, float* out_levels, size_t count) {
  if (!params || !out_levels || !params->playback_signal_rms) return;
  size_t limit =
      count < params->playback_channels ? count : params->playback_channels;
  for (size_t i = 0; i < limit; i++) {
    out_levels[i] = atomic_float_get(&params->playback_signal_rms[i]);
  }
}

void processing_parameters_set_playback_signal_rms(
    processing_parameters_t* params, const float* levels, size_t count) {
  if (!params || !levels || !params->playback_signal_rms) return;
  size_t limit =
      count < params->playback_channels ? count : params->playback_channels;
  for (size_t i = 0; i < limit; i++) {
    atomic_float_set(&params->playback_signal_rms[i], levels[i]);
  }
}

/**
 * @brief Lock-free helper to update audio levels (Peak and RMS) for each
 * channel.
 *
 * Calculates peak and RMS values in dB for the active channels in the chunk
 * and updates the respective atomic storage. It avoids dynamic allocation,
 * making it suitable for the audio processing thread.
 *
 * @param chunk The audio chunk to process.
 * @param peak_storage Atomic storage array for peak levels.
 * @param rms_storage Atomic storage array for RMS levels.
 * @param storage_count Capacity of the storage arrays.
 * @return The maximum peak level (dB) across all processed channels.
 */
static float update_levels_internal(const audio_chunk_t* chunk,
                                    atomic_float_t* peak_storage,
                                    atomic_float_t* rms_storage,
                                    chunk_level_history_t* peak_hist,
                                    chunk_level_history_t* rms_hist,
                                    size_t storage_count) {
  if (!chunk || !peak_storage || !rms_storage) return -INFINITY;
  size_t chunk_channels = audio_chunk_get_channels(chunk);
  size_t channel_count =
      chunk_channels < storage_count ? chunk_channels : storage_count;
  if (channel_count == 0) return -INFINITY;
  size_t frame_count = audio_chunk_get_valid_frames(chunk);
  if (frame_count == 0) {
    for (size_t i = 0; i < channel_count; i++) {
      atomic_float_set(&peak_storage[i], -INFINITY);
      atomic_float_set(&rms_storage[i], -INFINITY);
    }
    return -INFINITY;
  }

  uint64_t now_ms = cdsp_time_now_ns() / 1000000ULL;
  size_t peak_pos = (size_t)-1;
  size_t rms_pos = (size_t)-1;
  uint64_t peak_seq = 0;
  uint64_t rms_seq = 0;

  if (peak_hist && peak_hist->data && peak_hist->channels > 0) {
    peak_seq =
        atomic_load_explicit(&peak_hist->write_seq, memory_order_relaxed);
    atomic_store_explicit(&peak_hist->write_seq, peak_seq + 1,
                          memory_order_release);
    peak_pos =
        atomic_load_explicit(&peak_hist->write_pos, memory_order_relaxed);
    peak_hist->timestamps_ms[peak_pos] = now_ms;
  }

  if (rms_hist && rms_hist->data && rms_hist->channels > 0) {
    rms_seq = atomic_load_explicit(&rms_hist->write_seq, memory_order_relaxed);
    atomic_store_explicit(&rms_hist->write_seq, rms_seq + 1,
                          memory_order_release);
    rms_pos = atomic_load_explicit(&rms_hist->write_pos, memory_order_relaxed);
    rms_hist->timestamps_ms[rms_pos] = now_ms;
  }

  float max_peak = -INFINITY;

  for (size_t i = 0; i < channel_count; i++) {
    waveform_t buffer = audio_chunk_get_channel(chunk, i);
    if (!buffer) {
      atomic_float_set(&peak_storage[i], -INFINITY);
      atomic_float_set(&rms_storage[i], -INFINITY);
      if (peak_pos != (size_t)-1 && i < peak_hist->channels) {
        peak_hist->data[(i * CHUNK_LEVEL_HISTORY_CAPACITY) + peak_pos] =
            -INFINITY;
      }
      if (rms_pos != (size_t)-1 && i < rms_hist->channels) {
        rms_hist->data[(i * CHUNK_LEVEL_HISTORY_CAPACITY) + rms_pos] =
            -INFINITY;
      }
      continue;
    }

    float peak = dsp_ops_peak_absolute(buffer, frame_count);
    float peak_db = float_to_db(peak);
    atomic_float_set(&peak_storage[i], peak_db);
    if (peak_pos != (size_t)-1 && i < peak_hist->channels) {
      peak_hist->data[(i * CHUNK_LEVEL_HISTORY_CAPACITY) + peak_pos] = peak_db;
    }
    if (peak_db > max_peak) {
      max_peak = peak_db;
    }

    float rms = dsp_ops_rms(buffer, frame_count);
    float rms_db = float_to_db(rms);
    atomic_float_set(&rms_storage[i], rms_db);
    if (rms_pos != (size_t)-1 && i < rms_hist->channels) {
      rms_hist->data[(i * CHUNK_LEVEL_HISTORY_CAPACITY) + rms_pos] = rms_db;
    }
  }

  if (peak_pos != (size_t)-1) {
    atomic_store_explicit(&peak_hist->write_pos,
                          (peak_pos + 1) & (CHUNK_LEVEL_HISTORY_CAPACITY - 1),
                          memory_order_release);
    atomic_fetch_add_explicit(&peak_hist->total_written, 1,
                              memory_order_release);
    atomic_store_explicit(&peak_hist->write_seq, peak_seq + 2,
                          memory_order_release);
  }

  if (rms_pos != (size_t)-1) {
    atomic_store_explicit(&rms_hist->write_pos,
                          (rms_pos + 1) & (CHUNK_LEVEL_HISTORY_CAPACITY - 1),
                          memory_order_release);
    atomic_fetch_add_explicit(&rms_hist->total_written, 1,
                              memory_order_release);
    atomic_store_explicit(&rms_hist->write_seq, rms_seq + 2,
                          memory_order_release);
  }

  return max_peak;
}

float processing_parameters_update_capture_levels(
    processing_parameters_t* params, const audio_chunk_t* chunk) {
  if (!params) return -INFINITY;
  return update_levels_internal(
      chunk, params->capture_signal_peak, params->capture_signal_rms,
      &params->capture_peak_history, &params->capture_rms_history,
      params->capture_channels);
}

float processing_parameters_update_playback_levels(
    processing_parameters_t* params, const audio_chunk_t* chunk) {
  if (!params) return -INFINITY;
  return update_levels_internal(
      chunk, params->playback_signal_peak, params->playback_signal_rms,
      &params->playback_peak_history, &params->playback_rms_history,
      params->playback_channels);
}

void processing_parameters_get_capture_signal_peak_since(
    const processing_parameters_t* params, uint64_t since_ms, float* out_levels,
    size_t count) {
  if (!params) {
    if (out_levels) {
      for (size_t i = 0; i < count; i++) out_levels[i] = -INFINITY;
    }
    return;
  }
  chunk_level_history_get_max_since(&params->capture_peak_history, since_ms,
                                    out_levels, count);
}

void processing_parameters_get_capture_signal_rms_since(
    const processing_parameters_t* params, uint64_t since_ms, float* out_levels,
    size_t count) {
  if (!params) {
    if (out_levels) {
      for (size_t i = 0; i < count; i++) out_levels[i] = -INFINITY;
    }
    return;
  }
  chunk_level_history_get_rms_since(&params->capture_rms_history, since_ms,
                                    out_levels, count);
}

void processing_parameters_get_playback_signal_peak_since(
    const processing_parameters_t* params, uint64_t since_ms, float* out_levels,
    size_t count) {
  if (!params) {
    if (out_levels) {
      for (size_t i = 0; i < count; i++) out_levels[i] = -INFINITY;
    }
    return;
  }
  chunk_level_history_get_max_since(&params->playback_peak_history, since_ms,
                                    out_levels, count);
}

void processing_parameters_get_playback_signal_rms_since(
    const processing_parameters_t* params, uint64_t since_ms, float* out_levels,
    size_t count) {
  if (!params) {
    if (out_levels) {
      for (size_t i = 0; i < count; i++) out_levels[i] = -INFINITY;
    }
    return;
  }
  chunk_level_history_get_rms_since(&params->playback_rms_history, since_ms,
                                    out_levels, count);
}
