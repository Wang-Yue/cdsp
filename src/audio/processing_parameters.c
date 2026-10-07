// Concurrency model
// -----------------
// Every field is backed by lock-free atomics (`atomic_double_t` or `_Atomic
// bool`) — no mutexes or locks.
#include "audio/processing_parameters.h"

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "audio/audio_chunk.h"
#include "utils/cdsp_memory.h"
#include "utils/cdsp_time.h"
#include "utils/double_helpers.h"
#include "utils/float_helpers.h"
#include "utils/lock_free_ring_buffer.h"

/**
 * @brief Lock-free ring buffer tracking the last 1,024 chunk-level Peak or RMS
 * values per channel.
 *
 * Storage format (private to this file):
 * - peak histories store the per-chunk peak in dB (max is order-preserving, so
 *   the max of dB values equals the dB of the max linear value);
 * - RMS histories store the per-chunk **mean square** (linear power), like
 *   upstream `ValueHistory::add_record_squared`. Queries average the stored
 *   powers and convert to dB once (`average_sqrt_since`), so the audio thread
 *   never round-trips through log/pow for the history.
 *
 * Synchronization: a seqlock. A writer claims the record by CAS-ing `write_seq`
 * from even to odd (so the audio thread and a telemetry transfer can never
 * write at the same time; the audio thread drops one history record rather
 * than wait). All per-channel values are computed into `pending` *before* the
 * claim, so the odd window only covers copying a few floats.
 */
typedef struct {
  size_t channels;
  _Atomic uint64_t write_seq;
  _Atomic size_t write_pos;
  _Atomic size_t total_written;
  // Seqlock-protected payload. Accessed only with relaxed atomics so that the
  // reader's speculative loads racing the writer are well-defined (C11); on
  // arm64/x86 these compile to plain loads/stores.
  _Atomic uint64_t timestamps_ns[CHUNK_LEVEL_HISTORY_CAPACITY];
  _Atomic float *data; // planar array: channels * CHUNK_LEVEL_HISTORY_CAPACITY
  float *pending;      // producer-only staging for the next record: channels
} chunk_level_history_t;

/// Reader retries before giving up. The first few retries spin, later ones
/// back off with a short sleep (readers are never on the audio thread).
#define CHUNK_LEVEL_HISTORY_READ_RETRIES 100
#define CHUNK_LEVEL_HISTORY_SPIN_RETRIES 4

static inline void chunk_level_history_init(chunk_level_history_t *hist,
                                            size_t channels, float fill) {
  if (!hist)
    return;
  hist->channels = channels;
  atomic_init(&hist->write_seq, 0ULL);
  atomic_init(&hist->write_pos, 0);
  atomic_init(&hist->total_written, 0);
  for (size_t i = 0; i < CHUNK_LEVEL_HISTORY_CAPACITY; i++) {
    atomic_init(&hist->timestamps_ns[i], 0ULL);
  }
  hist->data = NULL;
  hist->pending = NULL;
  if (channels > 0) {
    size_t total = channels * CHUNK_LEVEL_HISTORY_CAPACITY;
    hist->data =
        (_Atomic float *)cdsp_aligned_alloc(64, total * sizeof(_Atomic float));
    hist->pending = (float *)cdsp_aligned_alloc(64, channels * sizeof(float));
    if (!hist->data || !hist->pending) {
      if (hist->data)
        cdsp_aligned_free((void *)hist->data);
      if (hist->pending)
        cdsp_aligned_free(hist->pending);
      hist->data = NULL;
      hist->pending = NULL;
      return;
    }
    for (size_t i = 0; i < total; i++) {
      atomic_init(&hist->data[i], fill);
    }
    for (size_t i = 0; i < channels; i++) {
      hist->pending[i] = fill;
    }
  }
}

static inline void chunk_level_history_free(chunk_level_history_t *hist) {
  if (!hist)
    return;
  if (hist->data) {
    cdsp_aligned_free((void *)hist->data);
    hist->data = NULL;
  }
  if (hist->pending) {
    cdsp_aligned_free(hist->pending);
    hist->pending = NULL;
  }
  hist->channels = 0;
}

static inline bool
chunk_level_history_valid(const chunk_level_history_t *hist) {
  return hist && hist->data && hist->pending && hist->channels > 0;
}

/**
 * @brief Single non-blocking attempt to enter the seqlock write section.
 * @return true with `*seq_out` = the even sequence that was claimed.
 */
static inline bool
chunk_level_history_try_begin_write(chunk_level_history_t *hist,
                                    uint64_t *seq_out) {
  uint64_t seq = atomic_load_explicit(&hist->write_seq, memory_order_relaxed);
  if (seq & 1ULL)
    return false;
  if (!atomic_compare_exchange_strong_explicit(&hist->write_seq, &seq, seq + 1,
                                               memory_order_acquire,
                                               memory_order_relaxed)) {
    return false;
  }
  // Order the odd sequence before the data stores that follow (pairs with the
  // reader's acquire fence before it re-reads `write_seq`).
  atomic_thread_fence(memory_order_release);
  *seq_out = seq;
  return true;
}

static inline void chunk_level_history_end_write(chunk_level_history_t *hist,
                                                 uint64_t seq) {
  atomic_store_explicit(&hist->write_seq, seq + 2, memory_order_release);
}

/**
 * @brief Publish `hist->pending` as a new record. Real-time safe: one CAS
 * attempt, no waiting. If the section is held (only possible during a
 * telemetry transfer), this record is dropped.
 */
static inline void chunk_level_history_publish(chunk_level_history_t *hist,
                                               uint64_t now_ns) {
  if (!chunk_level_history_valid(hist))
    return;
  uint64_t seq;
  if (!chunk_level_history_try_begin_write(hist, &seq))
    return;
  size_t pos = atomic_load_explicit(&hist->write_pos, memory_order_relaxed);
  atomic_store_explicit(&hist->timestamps_ns[pos], now_ns,
                        memory_order_relaxed);
  for (size_t ch = 0; ch < hist->channels; ch++) {
    atomic_store_explicit(
        &hist->data[(ch * CHUNK_LEVEL_HISTORY_CAPACITY) + pos],
        hist->pending[ch], memory_order_relaxed);
  }
  atomic_store_explicit(&hist->write_pos,
                        (pos + 1) & (CHUNK_LEVEL_HISTORY_CAPACITY - 1),
                        memory_order_release);
  atomic_fetch_add_explicit(&hist->total_written, 1, memory_order_release);
  chunk_level_history_end_write(hist, seq);
}

static inline void chunk_level_history_read_backoff(int retry) {
  if (retry >= CHUNK_LEVEL_HISTORY_SPIN_RETRIES)
    cdsp_sleep_us(10);
}

/**
 * @brief Reduce the records newer than `since_ns`.
 *
 * `is_rms == false`: per-channel max of the stored dB values.
 * `is_rms == true`: per-channel mean of the stored mean squares, accumulated
 * in double and converted to dB once.
 */
static bool
chunk_level_history_reduce_since_ns(const chunk_level_history_t *hist,
                                    uint64_t since_ns, float *out_levels,
                                    size_t count, bool is_rms) {
  if (out_levels && count > 0) {
    for (size_t c = 0; c < count; c++) {
      out_levels[c] = -INFINITY;
    }
  }
  if (!out_levels || count == 0)
    return false;
  if (!chunk_level_history_valid(hist))
    return false;
  size_t ch_limit = count < hist->channels ? count : hist->channels;
  const size_t mask = CHUNK_LEVEL_HISTORY_CAPACITY - 1;

  for (int retry = 0; retry < CHUNK_LEVEL_HISTORY_READ_RETRIES; retry++) {
    uint64_t seq_before =
        atomic_load_explicit(&hist->write_seq, memory_order_acquire);
    if (seq_before & 1ULL) {
      chunk_level_history_read_backoff(retry);
      continue;
    }

    size_t total =
        atomic_load_explicit(&hist->total_written, memory_order_acquire);
    size_t available = total < CHUNK_LEVEL_HISTORY_CAPACITY
                           ? total
                           : CHUNK_LEVEL_HISTORY_CAPACITY;
    size_t pos = atomic_load_explicit(&hist->write_pos, memory_order_acquire);

    // Count the records newer than `since_ns` (newest first).
    size_t start = (pos + mask) & mask;
    size_t n = 0;
    for (size_t idx = start; n < available; idx = (idx + mask) & mask) {
      if (atomic_load_explicit(&hist->timestamps_ns[idx],
                               memory_order_relaxed) <= since_ns)
        break;
      n++;
    }
    // Reduce channel-major: each channel is contiguous in the planar store.
    for (size_t ch = 0; ch < ch_limit; ch++) {
      _Atomic float *ch_data = hist->data + (ch * CHUNK_LEVEL_HISTORY_CAPACITY);
      size_t idx = start;
      if (is_rms) {
        double sum_sq = 0.0;
        for (size_t i = 0; i < n; i++) {
          sum_sq +=
              (double)atomic_load_explicit(&ch_data[idx], memory_order_relaxed);
          idx = (idx + mask) & mask;
        }
        out_levels[ch] =
            n > 0 ? (float)(10.0 * log10(sum_sq / (double)n)) : -INFINITY;
      } else {
        float max_db = -INFINITY;
        for (size_t i = 0; i < n; i++) {
          float v = atomic_load_explicit(&ch_data[idx], memory_order_relaxed);
          if (v > max_db)
            max_db = v;
          idx = (idx + mask) & mask;
        }
        out_levels[ch] = max_db;
      }
    }

    atomic_thread_fence(memory_order_acquire);
    uint64_t seq_after =
        atomic_load_explicit(&hist->write_seq, memory_order_relaxed);
    if (seq_after != seq_before) {
      for (size_t ch = 0; ch < ch_limit; ch++) {
        out_levels[ch] = -INFINITY;
      }
      chunk_level_history_read_backoff(retry);
      continue;
    }
    if (n == 0) {
      for (size_t ch = 0; ch < ch_limit; ch++) {
        out_levels[ch] = -INFINITY;
      }
      return false;
    }
    return true;
  }
  return false;
}

static inline bool
chunk_level_history_get_max_since_ns(const chunk_level_history_t *hist,
                                     uint64_t since_ns, float *out_levels,
                                     size_t count) {
  return chunk_level_history_reduce_since_ns(hist, since_ns, out_levels, count,
                                             false);
}

static inline bool
chunk_level_history_get_rms_since_ns(const chunk_level_history_t *hist,
                                     uint64_t since_ns, float *out_levels,
                                     size_t count) {
  return chunk_level_history_reduce_since_ns(hist, since_ns, out_levels, count,
                                             true);
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
  atomic_float_t *capture_signal_peak;
  /** Per-channel capture signal RMS levels (dB). Array size: capture_channels.
   */
  atomic_float_t *capture_signal_rms;
  /** Per-channel playback signal peak levels (dB). Array size:
   * playback_channels. */
  atomic_float_t *playback_signal_peak;
  /** Per-channel playback signal RMS levels (dB). Array size:
   * playback_channels. */
  atomic_float_t *playback_signal_rms;

  /** Per-channel capture global linear peak levels since start/reset. */
  atomic_float_t *capture_global_peaks;
  /** Per-channel playback global linear peak levels since start/reset. */
  atomic_float_t *playback_global_peaks;

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
  atomic_float_t signal_range;      /**< Peak-to-peak signal range. */
  _Atomic uint64_t clipped_samples; /**< Cumulative count of clipped samples. */
  atomic_double_t processing_load;  /**< Audio processing load (0.0 to 1.0). */
  atomic_double_t
      resampler_load; /**< Resampler processing load (0.0 to 1.0). */
};

size_t processing_parameters_get_capture_channels(
    const processing_parameters_t *params) {
  return params ? params->capture_channels : 0;
}

size_t processing_parameters_get_playback_channels(
    const processing_parameters_t *params) {
  return params ? params->playback_channels : 0;
}

double
processing_parameters_get_rate_adjust(const processing_parameters_t *params) {
  return params ? atomic_double_get(&params->rate_adjust) : 0.0;
}

void processing_parameters_set_rate_adjust(processing_parameters_t *params,
                                           double value) {
  if (params)
    atomic_double_set(&params->rate_adjust, value);
}

double
processing_parameters_get_buffer_level(const processing_parameters_t *params) {
  return params ? atomic_double_get(&params->buffer_level) : 0.0;
}

void processing_parameters_set_buffer_level(processing_parameters_t *params,
                                            double value) {
  if (params)
    atomic_double_set(&params->buffer_level, value);
}

uint64_t processing_parameters_get_clipped_samples(
    const processing_parameters_t *params) {
  return params ? atomic_load_explicit(&params->clipped_samples,
                                       memory_order_relaxed)
                : 0ULL;
}

void processing_parameters_add_clipped_samples(processing_parameters_t *params,
                                               uint64_t count) {
  if (params && count > 0) {
    atomic_fetch_add_explicit(&params->clipped_samples, count,
                              memory_order_relaxed);
  }
}

void processing_parameters_reset_clipped_samples(
    processing_parameters_t *params) {
  if (params) {
    atomic_store_explicit(&params->clipped_samples, 0ULL, memory_order_relaxed);
  }
}

double processing_parameters_get_processing_load(
    const processing_parameters_t *params) {
  return params ? atomic_double_get(&params->processing_load) : 0.0;
}

void processing_parameters_set_processing_load(processing_parameters_t *params,
                                               double value) {
  if (params)
    atomic_double_set(&params->processing_load, value);
}

double processing_parameters_get_resampler_load(
    const processing_parameters_t *params) {
  return params ? atomic_double_get(&params->resampler_load) : 0.0;
}

void processing_parameters_set_resampler_load(processing_parameters_t *params,
                                              double value) {
  if (params)
    atomic_double_set(&params->resampler_load, value);
}

float processing_parameters_get_signal_range(
    const processing_parameters_t *params) {
  return params ? atomic_float_get(&params->signal_range) : 0.0f;
}

void processing_parameters_set_signal_range(processing_parameters_t *params,
                                            float range) {
  if (params)
    atomic_float_set(&params->signal_range, range);
}

processing_parameters_t *
processing_parameters_create(size_t capture_channels,
                             size_t playback_channels) {
  processing_parameters_t *params =
      (processing_parameters_t *)calloc(1, sizeof(processing_parameters_t));
  if (!params)
    return NULL;

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

  // Peak histories hold dB (empty = -inf); RMS histories hold mean squares
  // (empty = 0.0, i.e. -inf dB).
  chunk_level_history_init(&params->capture_peak_history, capture_channels,
                           -INFINITY);
  chunk_level_history_init(&params->capture_rms_history, capture_channels,
                           0.0f);
  chunk_level_history_init(&params->playback_peak_history, playback_channels,
                           -INFINITY);
  chunk_level_history_init(&params->playback_rms_history, playback_channels,
                           0.0f);

  if (capture_channels > 0) {
    params->capture_signal_peak =
        (atomic_float_t *)calloc(capture_channels, sizeof(atomic_float_t));
    params->capture_signal_rms =
        (atomic_float_t *)calloc(capture_channels, sizeof(atomic_float_t));
    params->capture_global_peaks =
        (atomic_float_t *)calloc(capture_channels, sizeof(atomic_float_t));
    if (!params->capture_signal_peak || !params->capture_signal_rms ||
        !params->capture_global_peaks || !params->capture_peak_history.data ||
        !params->capture_rms_history.data) {
      processing_parameters_free(params);
      return NULL;
    }
    for (size_t i = 0; i < capture_channels; i++) {
      atomic_float_init(&params->capture_signal_peak[i], -INFINITY);
      atomic_float_init(&params->capture_signal_rms[i], -INFINITY);
      atomic_float_init(&params->capture_global_peaks[i], 0.0f);
    }
  }

  if (playback_channels > 0) {
    params->playback_signal_peak =
        (atomic_float_t *)calloc(playback_channels, sizeof(atomic_float_t));
    params->playback_signal_rms =
        (atomic_float_t *)calloc(playback_channels, sizeof(atomic_float_t));
    params->playback_global_peaks =
        (atomic_float_t *)calloc(playback_channels, sizeof(atomic_float_t));
    if (!params->playback_signal_peak || !params->playback_signal_rms ||
        !params->playback_global_peaks || !params->playback_peak_history.data ||
        !params->playback_rms_history.data) {
      processing_parameters_free(params);
      return NULL;
    }
    for (size_t i = 0; i < playback_channels; i++) {
      atomic_float_init(&params->playback_signal_peak[i], -INFINITY);
      atomic_float_init(&params->playback_signal_rms[i], -INFINITY);
      atomic_float_init(&params->playback_global_peaks[i], 0.0f);
    }
  }

  atomic_double_init(&params->rate_adjust, 0.0);
  atomic_double_init(&params->measured_capture_rate, 0.0);
  atomic_double_init(&params->buffer_level, 0.0);
  atomic_float_init(&params->signal_range, 0.0f);
  atomic_init(&params->clipped_samples, 0ULL);
  atomic_double_init(&params->processing_load, 0.0);
  atomic_double_init(&params->resampler_load, 0.0);

  return params;
}

void processing_parameters_free(processing_parameters_t *params) {
  if (!params)
    return;
  if (params->capture_signal_peak)
    free(params->capture_signal_peak);
  if (params->capture_signal_rms)
    free(params->capture_signal_rms);
  if (params->capture_global_peaks)
    free(params->capture_global_peaks);
  if (params->playback_signal_peak)
    free(params->playback_signal_peak);
  if (params->playback_signal_rms)
    free(params->playback_signal_rms);
  if (params->playback_global_peaks)
    free(params->playback_global_peaks);
  chunk_level_history_free(&params->capture_peak_history);
  chunk_level_history_free(&params->capture_rms_history);
  chunk_level_history_free(&params->playback_peak_history);
  chunk_level_history_free(&params->playback_rms_history);
  free(params);
}

double processing_parameters_get_target_volume_for_fader(
    const processing_parameters_t *params, fader_t fader) {
  if (!params || fader < 0 || fader >= FADER_COUNT)
    return 0.0;
  return atomic_double_get(&params->target_volumes[fader]);
}

void processing_parameters_set_target_volume_for_fader(
    processing_parameters_t *params, double value, fader_t fader) {
  if (!params || fader < 0 || fader >= FADER_COUNT)
    return;
  atomic_double_set(&params->target_volumes[fader], value);
}

void processing_parameters_bump_pause_count(processing_parameters_t *params) {
  if (params) {
    atomic_fetch_add_explicit(&params->pause_count, 1ULL, memory_order_relaxed);
  }
}

uint64_t
processing_parameters_get_pause_count(const processing_parameters_t *params) {
  return params
             ? atomic_load_explicit(&params->pause_count, memory_order_relaxed)
             : 0ULL;
}

double processing_parameters_get_measured_capture_rate(
    const processing_parameters_t *params) {
  return params ? atomic_double_get(&params->measured_capture_rate) : 0.0;
}

void processing_parameters_set_measured_capture_rate(
    processing_parameters_t *params, double rate) {
  if (params) {
    atomic_double_set(&params->measured_capture_rate, rate);
  }
}

double processing_parameters_get_current_volume_for_fader(
    const processing_parameters_t *params, fader_t fader) {
  if (!params || fader < 0 || fader >= FADER_COUNT)
    return 0.0;
  return atomic_double_get(&params->current_volumes[fader]);
}

void processing_parameters_set_current_volume_for_fader(
    processing_parameters_t *params, double value, fader_t fader) {
  if (!params || fader < 0 || fader >= FADER_COUNT)
    return;
  atomic_double_set(&params->current_volumes[fader], value);
}

bool processing_parameters_is_muted_for_fader(
    const processing_parameters_t *params, fader_t fader) {
  if (!params || fader < 0 || fader >= FADER_COUNT)
    return false;
  return atomic_load_explicit(&params->muted[fader], memory_order_acquire);
}

void processing_parameters_set_muted_for_fader(processing_parameters_t *params,
                                               bool value, fader_t fader) {
  if (!params || fader < 0 || fader >= FADER_COUNT)
    return;
  atomic_store_explicit(&params->muted[fader], value, memory_order_release);
}

bool processing_parameters_toggle_muted_for_fader(
    processing_parameters_t *params, fader_t fader) {
  if (!params || fader < 0 || fader >= FADER_COUNT)
    return false;
  bool expected =
      atomic_load_explicit(&params->muted[fader], memory_order_relaxed);
  while (!atomic_compare_exchange_weak_explicit(
      &params->muted[fader], &expected, !expected, memory_order_acq_rel,
      memory_order_relaxed)) {
  }
  return !expected;
}

void processing_parameters_get_capture_signal_peak(
    const processing_parameters_t *params, float *out_levels, size_t count) {
  if (!params || !out_levels || !params->capture_signal_peak)
    return;
  size_t limit =
      count < params->capture_channels ? count : params->capture_channels;
  for (size_t i = 0; i < limit; i++) {
    out_levels[i] = atomic_float_get(&params->capture_signal_peak[i]);
  }
}

void processing_parameters_set_capture_signal_peak(
    processing_parameters_t *params, const float *levels, size_t count) {
  if (!params || !levels || !params->capture_signal_peak)
    return;
  size_t limit =
      count < params->capture_channels ? count : params->capture_channels;
  for (size_t i = 0; i < limit; i++) {
    atomic_float_set(&params->capture_signal_peak[i], levels[i]);
    if (params->capture_global_peaks && isfinite(levels[i]) &&
        levels[i] > -200.0f) {
      float lin = powf(10.0f, levels[i] / 20.0f);
      atomic_float_fetch_max(&params->capture_global_peaks[i], lin);
    }
  }
}

void processing_parameters_get_capture_signal_rms(
    const processing_parameters_t *params, float *out_levels, size_t count) {
  if (!params || !out_levels || !params->capture_signal_rms)
    return;
  size_t limit =
      count < params->capture_channels ? count : params->capture_channels;
  for (size_t i = 0; i < limit; i++) {
    out_levels[i] = atomic_float_get(&params->capture_signal_rms[i]);
  }
}

void processing_parameters_set_capture_signal_rms(
    processing_parameters_t *params, const float *levels, size_t count) {
  if (!params || !levels || !params->capture_signal_rms)
    return;
  size_t limit =
      count < params->capture_channels ? count : params->capture_channels;
  for (size_t i = 0; i < limit; i++) {
    atomic_float_set(&params->capture_signal_rms[i], levels[i]);
  }
}

void processing_parameters_get_playback_signal_peak(
    const processing_parameters_t *params, float *out_levels, size_t count) {
  if (!params || !out_levels || !params->playback_signal_peak)
    return;
  size_t limit =
      count < params->playback_channels ? count : params->playback_channels;
  for (size_t i = 0; i < limit; i++) {
    out_levels[i] = atomic_float_get(&params->playback_signal_peak[i]);
  }
}

void processing_parameters_set_playback_signal_peak(
    processing_parameters_t *params, const float *levels, size_t count) {
  if (!params || !levels || !params->playback_signal_peak)
    return;
  size_t limit =
      count < params->playback_channels ? count : params->playback_channels;
  for (size_t i = 0; i < limit; i++) {
    atomic_float_set(&params->playback_signal_peak[i], levels[i]);
    if (params->playback_global_peaks && isfinite(levels[i]) &&
        levels[i] > -200.0f) {
      float lin = powf(10.0f, levels[i] / 20.0f);
      atomic_float_fetch_max(&params->playback_global_peaks[i], lin);
    }
  }
}

void processing_parameters_get_playback_signal_rms(
    const processing_parameters_t *params, float *out_levels, size_t count) {
  if (!params || !out_levels || !params->playback_signal_rms)
    return;
  size_t limit =
      count < params->playback_channels ? count : params->playback_channels;
  for (size_t i = 0; i < limit; i++) {
    out_levels[i] = atomic_float_get(&params->playback_signal_rms[i]);
  }
}

void processing_parameters_set_playback_signal_rms(
    processing_parameters_t *params, const float *levels, size_t count) {
  if (!params || !levels || !params->playback_signal_rms)
    return;
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
static float update_levels_internal(const audio_chunk_t *chunk,
                                    atomic_float_t *peak_storage,
                                    atomic_float_t *rms_storage,
                                    chunk_level_history_t *peak_hist,
                                    chunk_level_history_t *rms_hist,
                                    atomic_float_t *global_peaks,
                                    size_t storage_count) {
  if (!chunk || !peak_storage || !rms_storage)
    return -INFINITY;
  size_t chunk_channels = audio_chunk_get_channels(chunk);
  size_t channel_count =
      chunk_channels < storage_count ? chunk_channels : storage_count;
  if (channel_count == 0)
    return -INFINITY;
  size_t frame_count = audio_chunk_get_valid_frames(chunk);
  if (frame_count == 0) {
    for (size_t i = 0; i < channel_count; i++) {
      atomic_float_set(&peak_storage[i], -INFINITY);
      atomic_float_set(&rms_storage[i], -INFINITY);
    }
    return -INFINITY;
  }

  float max_peak = -INFINITY;
  const bool *used_mask = audio_chunk_get_used_channels(chunk);
  const bool peak_hist_ok = chunk_level_history_valid(peak_hist);
  const bool rms_hist_ok = chunk_level_history_valid(rms_hist);

  // Phase 1: all reductions and dB conversions happen outside the seqlock;
  // history values are staged in the producer-only `pending` arrays.
  for (size_t i = 0; i < channel_count; i++) {
    waveform_t buffer = audio_chunk_get_channel(chunk, i);
    if (!buffer || (used_mask && !used_mask[i])) {
      atomic_float_set(&peak_storage[i], -INFINITY);
      atomic_float_set(&rms_storage[i], -INFINITY);
      if (peak_hist_ok && i < peak_hist->channels)
        peak_hist->pending[i] = -INFINITY;
      if (rms_hist_ok && i < rms_hist->channels)
        rms_hist->pending[i] = 0.0f;
      continue;
    }

    float peak = dsp_ops_peak_absolute(buffer, frame_count);
    if (global_peaks) {
      atomic_float_fetch_max(&global_peaks[i], peak);
    }
    float peak_db = float_to_db(peak);
    atomic_float_set(&peak_storage[i], peak_db);
    if (peak_hist_ok && i < peak_hist->channels)
      peak_hist->pending[i] = peak_db;
    if (peak_db > max_peak) {
      max_peak = peak_db;
    }

    // Mean square accumulated in double; dB conversion done once, in double.
    float mean_sq = dsp_ops_mean_square(buffer, frame_count);
    atomic_float_set(&rms_storage[i], 10.0f * log10f(mean_sq));
    if (rms_hist_ok && i < rms_hist->channels)
      rms_hist->pending[i] = mean_sq;
  }
  if (peak_hist_ok) {
    for (size_t i = channel_count; i < peak_hist->channels; i++)
      peak_hist->pending[i] = -INFINITY;
  }
  if (rms_hist_ok) {
    for (size_t i = channel_count; i < rms_hist->channels; i++)
      rms_hist->pending[i] = 0.0f;
  }

  // Phase 2: short seqlock sections that only copy the staged values.
  uint64_t now_ns = cdsp_time_now_ns();
  if (peak_hist_ok)
    chunk_level_history_publish(peak_hist, now_ns);
  if (rms_hist_ok)
    chunk_level_history_publish(rms_hist, now_ns);

  return max_peak;
}

float processing_parameters_update_capture_levels(
    processing_parameters_t *params, const audio_chunk_t *chunk) {
  if (!params)
    return -INFINITY;
  return update_levels_internal(
      chunk, params->capture_signal_peak, params->capture_signal_rms,
      &params->capture_peak_history, &params->capture_rms_history,
      params->capture_global_peaks, params->capture_channels);
}

float processing_parameters_update_playback_levels(
    processing_parameters_t *params, const audio_chunk_t *chunk) {
  if (!params)
    return -INFINITY;
  return update_levels_internal(
      chunk, params->playback_signal_peak, params->playback_signal_rms,
      &params->playback_peak_history, &params->playback_rms_history,
      params->playback_global_peaks, params->playback_channels);
}

static inline uint64_t ms_to_ns_clamped(uint64_t ms) {
  if (ms > UINT64_MAX / 1000000ULL) {
    return UINT64_MAX;
  }
  return ms * 1000000ULL;
}

bool processing_parameters_get_capture_signal_peak_since(
    const processing_parameters_t *params, uint64_t since_ms, float *out_levels,
    size_t count) {
  if (!params) {
    if (out_levels) {
      for (size_t i = 0; i < count; i++)
        out_levels[i] = -INFINITY;
    }
    return false;
  }
  return chunk_level_history_get_max_since_ns(&params->capture_peak_history,
                                              ms_to_ns_clamped(since_ms),
                                              out_levels, count);
}

bool processing_parameters_get_capture_signal_rms_since(
    const processing_parameters_t *params, uint64_t since_ms, float *out_levels,
    size_t count) {
  if (!params) {
    if (out_levels) {
      for (size_t i = 0; i < count; i++)
        out_levels[i] = -INFINITY;
    }
    return false;
  }
  return chunk_level_history_get_rms_since_ns(&params->capture_rms_history,
                                              ms_to_ns_clamped(since_ms),
                                              out_levels, count);
}

bool processing_parameters_get_playback_signal_peak_since(
    const processing_parameters_t *params, uint64_t since_ms, float *out_levels,
    size_t count) {
  if (!params) {
    if (out_levels) {
      for (size_t i = 0; i < count; i++)
        out_levels[i] = -INFINITY;
    }
    return false;
  }
  return chunk_level_history_get_max_since_ns(&params->playback_peak_history,
                                              ms_to_ns_clamped(since_ms),
                                              out_levels, count);
}

bool processing_parameters_get_playback_signal_rms_since(
    const processing_parameters_t *params, uint64_t since_ms, float *out_levels,
    size_t count) {
  if (!params) {
    if (out_levels) {
      for (size_t i = 0; i < count; i++)
        out_levels[i] = -INFINITY;
    }
    return false;
  }
  return chunk_level_history_get_rms_since_ns(&params->playback_rms_history,
                                              ms_to_ns_clamped(since_ms),
                                              out_levels, count);
}

uint64_t processing_parameters_get_chunk_generation(
    const processing_parameters_t *params, bool is_capture) {
  if (!params)
    return 0;
  if (is_capture) {
    return atomic_load_explicit(&params->capture_peak_history.total_written,
                                memory_order_acquire);
  } else {
    return atomic_load_explicit(&params->playback_peak_history.total_written,
                                memory_order_acquire);
  }
}

void processing_parameters_get_capture_global_peaks(
    const processing_parameters_t *params, float *out_peaks, size_t count) {
  if (!params || !out_peaks || !params->capture_global_peaks)
    return;
  size_t limit =
      count < params->capture_channels ? count : params->capture_channels;
  for (size_t i = 0; i < limit; i++) {
    out_peaks[i] = atomic_float_get(&params->capture_global_peaks[i]);
  }
}

void processing_parameters_get_playback_global_peaks(
    const processing_parameters_t *params, float *out_peaks, size_t count) {
  if (!params || !out_peaks || !params->playback_global_peaks)
    return;
  size_t limit =
      count < params->playback_channels ? count : params->playback_channels;
  for (size_t i = 0; i < limit; i++) {
    out_peaks[i] = atomic_float_get(&params->playback_global_peaks[i]);
  }
}

void processing_parameters_reset_capture_global_peaks(
    processing_parameters_t *params) {
  if (!params || !params->capture_global_peaks)
    return;
  for (size_t i = 0; i < params->capture_channels; i++) {
    atomic_float_set(&params->capture_global_peaks[i], 0.0f);
  }
}

void processing_parameters_reset_playback_global_peaks(
    processing_parameters_t *params) {
  if (!params || !params->playback_global_peaks)
    return;
  for (size_t i = 0; i < params->playback_channels; i++) {
    atomic_float_set(&params->playback_global_peaks[i], 0.0f);
  }
}

void processing_parameters_reset_global_peaks(processing_parameters_t *params) {
  processing_parameters_reset_capture_global_peaks(params);
  processing_parameters_reset_playback_global_peaks(params);
}

/**
 * @brief Copy a history from `src` to `dst` (control thread only).
 *
 * Either side may still have a live audio-thread writer (the old session is
 * snapshotted before it is stopped; the new session is already running when
 * the snapshot is restored), so:
 * - `dst` is claimed as a seqlock writer via CAS; its own audio writer then
 *   drops records for the duration of the copy instead of racing with it;
 * - `src` is read under its seqlock and the copy is retried until a
 *   consistent snapshot is obtained;
 * - `dst->write_seq` keeps its own monotonic even/odd sequence (it is never
 *   overwritten with `src`'s, which may be odd if `src` was mid-write).
 */
static void chunk_level_history_transfer(chunk_level_history_t *dst,
                                         const chunk_level_history_t *src) {
  if (!chunk_level_history_valid(dst) || !chunk_level_history_valid(src))
    return;
  size_t ch_limit =
      dst->channels < src->channels ? dst->channels : src->channels;

  uint64_t dst_seq = 0;
  bool claimed = false;
  for (int retry = 0; retry < 1000; retry++) {
    if (chunk_level_history_try_begin_write(dst, &dst_seq)) {
      claimed = true;
      break;
    }
    chunk_level_history_read_backoff(retry);
  }
  if (!claimed)
    return;

  bool copied = false;
  size_t pos = 0;
  size_t total = 0;
  for (int retry = 0; retry < CHUNK_LEVEL_HISTORY_READ_RETRIES; retry++) {
    uint64_t seq_before =
        atomic_load_explicit(&src->write_seq, memory_order_acquire);
    if (seq_before & 1ULL) {
      chunk_level_history_read_backoff(retry);
      continue;
    }
    pos = atomic_load_explicit(&src->write_pos, memory_order_acquire);
    total = atomic_load_explicit(&src->total_written, memory_order_acquire);
    for (size_t i = 0; i < CHUNK_LEVEL_HISTORY_CAPACITY; i++) {
      atomic_store_explicit(
          &dst->timestamps_ns[i],
          atomic_load_explicit(&src->timestamps_ns[i], memory_order_relaxed),
          memory_order_relaxed);
    }
    for (size_t c = 0; c < ch_limit; c++) {
      for (size_t i = 0; i < CHUNK_LEVEL_HISTORY_CAPACITY; i++) {
        size_t k = (c * CHUNK_LEVEL_HISTORY_CAPACITY) + i;
        atomic_store_explicit(
            &dst->data[k],
            atomic_load_explicit(&src->data[k], memory_order_relaxed),
            memory_order_relaxed);
      }
    }
    atomic_thread_fence(memory_order_acquire);
    if (atomic_load_explicit(&src->write_seq, memory_order_relaxed) ==
        seq_before) {
      copied = true;
      break;
    }
    chunk_level_history_read_backoff(retry);
  }
  if (!copied) {
    // Never publish a torn copy: leave `dst` empty instead.
    pos = 0;
    total = 0;
    for (size_t i = 0; i < CHUNK_LEVEL_HISTORY_CAPACITY; i++) {
      atomic_store_explicit(&dst->timestamps_ns[i], 0ULL, memory_order_relaxed);
    }
  }

  atomic_store_explicit(&dst->write_pos, pos, memory_order_relaxed);
  atomic_store_explicit(&dst->total_written, total, memory_order_relaxed);
  chunk_level_history_end_write(dst, dst_seq);
}

static void transfer_atomic_floats(atomic_float_t *dst, size_t dst_count,
                                   const atomic_float_t *src,
                                   size_t src_count) {
  if (!dst || !src)
    return;
  size_t n = dst_count < src_count ? dst_count : src_count;
  for (size_t i = 0; i < n; i++) {
    atomic_float_set(&dst[i], atomic_float_get(&src[i]));
  }
}

void processing_parameters_transfer_telemetry(
    processing_parameters_t *dst, const processing_parameters_t *src) {
  if (!dst || !src)
    return;

  // Transfer global peaks
  size_t cap_ch = dst->capture_channels < src->capture_channels
                      ? dst->capture_channels
                      : src->capture_channels;
  for (size_t i = 0; i < cap_ch; i++) {
    float peak = atomic_float_get(&src->capture_global_peaks[i]);
    atomic_float_set(&dst->capture_global_peaks[i], peak);
  }

  size_t pb_ch = dst->playback_channels < src->playback_channels
                     ? dst->playback_channels
                     : src->playback_channels;
  for (size_t i = 0; i < pb_ch; i++) {
    float peak = atomic_float_get(&src->playback_global_peaks[i]);
    atomic_float_set(&dst->playback_global_peaks[i], peak);
  }

  // Transfer the latest instantaneous levels (upstream keeps reporting the
  // last record across a config change until a new chunk arrives).
  // `clipped_samples` is deliberately not transferred here: dsp_engine carries
  // it across sessions itself (`clipped_samples_accum`).
  transfer_atomic_floats(dst->capture_signal_peak, dst->capture_channels,
                         src->capture_signal_peak, src->capture_channels);
  transfer_atomic_floats(dst->capture_signal_rms, dst->capture_channels,
                         src->capture_signal_rms, src->capture_channels);
  transfer_atomic_floats(dst->playback_signal_peak, dst->playback_channels,
                         src->playback_signal_peak, src->playback_channels);
  transfer_atomic_floats(dst->playback_signal_rms, dst->playback_channels,
                         src->playback_signal_rms, src->playback_channels);

  // Transfer chunk level histories
  chunk_level_history_transfer(&dst->capture_peak_history,
                               &src->capture_peak_history);
  chunk_level_history_transfer(&dst->capture_rms_history,
                               &src->capture_rms_history);
  chunk_level_history_transfer(&dst->playback_peak_history,
                               &src->playback_peak_history);
  chunk_level_history_transfer(&dst->playback_rms_history,
                               &src->playback_rms_history);
}
