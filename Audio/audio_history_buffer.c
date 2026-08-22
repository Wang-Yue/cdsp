// AudioHistoryBuffer — stores recent audio samples for spectrum analysis and
// vector scope (matching upstream CamillaDSP spectrum::AudioRingBuffer with
// lock-free / wait-free seqlock synchronization for real-time audio threads).
#include "Audio/audio_history_buffer.h"

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#if defined(ENABLE_ACCELERATE)
#include <Accelerate/Accelerate.h>
#endif

#include "Audio/audio_chunk.h"
#include "Utils/cdsp_memory.h"

struct audio_history_buffer {
  size_t channels;
  size_t capacity;
  _Atomic uint64_t write_pos __attribute__((aligned(64)));
  _Atomic uint64_t total_written __attribute__((aligned(64)));
  _Atomic uint64_t write_seq __attribute__((aligned(64)));
  float* data;
};

// --- Internal Helper Functions ---

/**
 * @brief Converts a contiguous segment of double-precision samples to
 * single-precision float and writes them into a ring buffer with wrap-around
 * handling.
 *
 * @param src Input double-precision source array.
 * @param dst Destination single-precision ring buffer.
 * @param start_idx Starting write index in the destination ring buffer.
 * @param count Total number of samples to convert and write.
 * @param cap Capacity of the destination ring buffer.
 */
static inline void copy_double_to_float_segment(const double* src, float* dst,
                                                size_t start_idx, size_t count,
                                                size_t cap) {
  size_t first = cap - start_idx;
  if (first > count) first = count;
  size_t second = count - first;

#if defined(ENABLE_ACCELERATE)
  vDSP_vdpsp(src, 1, dst + start_idx, 1, first);
  if (second > 0) {
    vDSP_vdpsp(src + first, 1, dst, 1, second);
  }
#else
#if defined(__clang__)
#pragma clang loop vectorize(enable) interleave(enable)
#elif defined(__GNUC__)
#pragma GCC ivdep
#endif
  for (size_t f = 0; f < first; f++) {
    dst[start_idx + f] = (float)src[f];
  }
#if defined(__clang__)
#pragma clang loop vectorize(enable) interleave(enable)
#elif defined(__GNUC__)
#pragma GCC ivdep
#endif
  for (size_t f = 0; f < second; f++) {
    dst[f] = (float)src[first + f];
  }
#endif
}

/**
 * @brief Reads a contiguous segment of float samples from a ring buffer into a
 * destination buffer with wrap-around handling.
 *
 * @param ch_src Source single-precision ring buffer.
 * @param dest Output buffer for copied samples.
 * @param start Starting read index in the source ring buffer.
 * @param count Total number of samples to read.
 * @param cap Capacity of the source ring buffer.
 */
static inline void read_channel_segment(const float* ch_src, float* dest,
                                        size_t start, size_t count,
                                        size_t cap) {
  size_t first = cap - start;
  if (first > count) first = count;
  size_t second = count - first;

  memcpy(dest, ch_src + start, first * sizeof(float));
  if (second > 0) {
    memcpy(dest + first, ch_src, second * sizeof(float));
  }
}

/**
 * @brief Vector-accumulates float samples from a ring buffer into an existing
 * destination buffer with wrap-around handling.
 *
 * @param ch_src Source single-precision ring buffer to accumulate.
 * @param dest Output buffer to add samples into in-place.
 * @param start Starting read index in the source ring buffer.
 * @param count Total number of samples to accumulate.
 * @param cap Capacity of the source ring buffer.
 */
static inline void add_channel_segment(const float* ch_src, float* dest,
                                       size_t start, size_t count, size_t cap) {
  size_t first = cap - start;
  if (first > count) first = count;
  size_t second = count - first;

#if defined(ENABLE_ACCELERATE)
  vDSP_vadd(dest, 1, ch_src + start, 1, dest, 1, first);
  if (second > 0) {
    vDSP_vadd(dest + first, 1, ch_src, 1, dest + first, 1, second);
  }
#else
#if defined(__clang__)
#pragma clang loop vectorize(enable) interleave(enable)
#elif defined(__GNUC__)
#pragma GCC ivdep
#endif
  for (size_t i = 0; i < first; i++) {
    dest[i] += ch_src[start + i];
  }
#if defined(__clang__)
#pragma clang loop vectorize(enable) interleave(enable)
#elif defined(__GNUC__)
#pragma GCC ivdep
#endif
  for (size_t i = 0; i < second; i++) {
    dest[first + i] += ch_src[i];
  }
#endif
}

/**
 * @brief Multiplies all elements of a float vector by a scalar value in-place.
 *
 * @param dest Buffer of float samples to scale in-place.
 * @param count Total number of samples to scale.
 * @param scale Scalar multiplication factor.
 */
static inline void scale_vector(float* dest, size_t count, float scale) {
#if defined(ENABLE_ACCELERATE)
  vDSP_vsmul(dest, 1, &scale, dest, 1, count);
#else
#if defined(__clang__)
#pragma clang loop vectorize(enable) interleave(enable)
#elif defined(__GNUC__)
#pragma GCC ivdep
#endif
  for (size_t i = 0; i < count; i++) {
    dest[i] *= scale;
  }
#endif
}

// --- Public API ---

size_t audio_history_buffer_get_channels(
    const audio_history_buffer_t* history) {
  return history ? history->channels : 0;
}

audio_history_buffer_t* audio_history_buffer_create(void) {
  audio_history_buffer_t* history = (audio_history_buffer_t*)cdsp_aligned_alloc(
      64, sizeof(audio_history_buffer_t));
  if (history) {
    memset(history, 0, sizeof(audio_history_buffer_t));
    history->capacity = AUDIO_HISTORY_BUFFER_CAPACITY;
    atomic_init(&history->write_pos, 0);
    atomic_init(&history->total_written, 0);
    atomic_init(&history->write_seq, 0);
  }
  return history;
}

static void audio_history_buffer_clear_internal(
    audio_history_buffer_t* history) {
  if (!history) return;
  if (history->data) {
    cdsp_aligned_free(history->data);
    history->data = NULL;
  }
  history->channels = 0;
  atomic_store_explicit(&history->write_pos, 0, memory_order_relaxed);
  atomic_store_explicit(&history->total_written, 0, memory_order_relaxed);
  atomic_store_explicit(&history->write_seq, 0, memory_order_relaxed);
}

void audio_history_buffer_reset(audio_history_buffer_t* history,
                                size_t channels) {
  if (!history) return;
  audio_history_buffer_clear_internal(history);

  if (channels > 0) {
    history->channels = channels;
    history->capacity = AUDIO_HISTORY_BUFFER_CAPACITY;
    atomic_store_explicit(&history->write_pos, 0, memory_order_relaxed);
    atomic_store_explicit(&history->total_written, 0, memory_order_relaxed);
    atomic_store_explicit(&history->write_seq, 0, memory_order_relaxed);

    size_t total_samples = channels * history->capacity;
    history->data =
        (float*)cdsp_aligned_alloc(64, total_samples * sizeof(float));
    if (!history->data) {
      audio_history_buffer_clear_internal(history);
      return;
    }
    memset(history->data, 0, total_samples * sizeof(float));
  }
}

void audio_history_buffer_free(audio_history_buffer_t* history) {
  if (!history) return;
  audio_history_buffer_clear_internal(history);
  cdsp_aligned_free(history);
}

void audio_history_buffer_append(audio_history_buffer_t* history,
                                 const audio_chunk_t* chunk) {
  if (!history || !chunk) return;
  size_t n_frames = audio_chunk_get_valid_frames(chunk);
  size_t n_ch = audio_chunk_get_channels(chunk);
  if (n_frames == 0 || n_ch == 0) return;

  if (history->channels != n_ch || !history->data) {
    audio_history_buffer_reset(history, n_ch);
    if (!history->data) return;
  }

  uint64_t seq =
      atomic_load_explicit(&history->write_seq, memory_order_relaxed);
  atomic_store_explicit(&history->write_seq, seq + 1, memory_order_release);

  uint64_t pos =
      atomic_load_explicit(&history->write_pos, memory_order_relaxed);
  size_t cap = history->capacity;
  size_t mask = cap - 1;

  size_t frames_to_copy = n_frames;
  size_t offset_in_chunk = 0;
  if (frames_to_copy > cap) {
    offset_in_chunk = frames_to_copy - cap;
    frames_to_copy = cap;
  }

  size_t start_idx = (size_t)((pos + n_frames - frames_to_copy) & mask);

  for (size_t ch = 0; ch < n_ch; ch++) {
    const double* ch_data = audio_chunk_get_channel(chunk, ch);
    if (!ch_data) continue;
    copy_double_to_float_segment(ch_data + offset_in_chunk,
                                 history->data + (ch * cap), start_idx,
                                 frames_to_copy, cap);
  }

  atomic_store_explicit(&history->write_pos, (pos + n_frames) & mask,
                        memory_order_release);
  atomic_fetch_add_explicit(&history->total_written, n_frames,
                            memory_order_release);
  atomic_store_explicit(&history->write_seq, seq + 2, memory_order_release);
}

audio_history_buffer_status_t audio_history_buffer_read_latest(
    const audio_history_buffer_t* history, float* dest, size_t count,
    const size_t* channel, bool* enough_data) {
  if (enough_data) *enough_data = false;
  if (!history || history->channels == 0 || !history->data) {
    return AUDIO_HISTORY_BUFFER_ERROR_EMPTY;
  }
  if (channel && *channel >= history->channels) {
    return AUDIO_HISTORY_BUFFER_ERROR_OUT_OF_RANGE;
  }
  if (!dest || count == 0) return AUDIO_HISTORY_BUFFER_OK;

  size_t cap = history->capacity;
  if (count > cap) return AUDIO_HISTORY_BUFFER_ERROR_OUT_OF_RANGE;
  size_t mask = cap - 1;

  for (int retry = 0; retry < 10; retry++) {
    uint64_t seq_before =
        atomic_load_explicit(&history->write_seq, memory_order_acquire);
    if (seq_before & 1) continue;

    uint64_t total =
        atomic_load_explicit(&history->total_written, memory_order_acquire);
    if (total < (uint64_t)count) {
      return AUDIO_HISTORY_BUFFER_OK;
    }

    uint64_t pos =
        atomic_load_explicit(&history->write_pos, memory_order_acquire);
    size_t start = (size_t)((pos + cap - (count & mask)) & mask);

    if (channel) {
      read_channel_segment(history->data + (*channel * cap), dest, start, count,
                           cap);
    } else {
      read_channel_segment(history->data, dest, start, count, cap);
      for (size_t ch = 1; ch < history->channels; ch++) {
        add_channel_segment(history->data + (ch * cap), dest, start, count,
                            cap);
      }
      if (history->channels > 1) {
        scale_vector(dest, count, 1.0f / (float)history->channels);
      }
    }

    uint64_t seq_after =
        atomic_load_explicit(&history->write_seq, memory_order_acquire);
    if (seq_after == seq_before) {
      if (enough_data) *enough_data = true;
      return AUDIO_HISTORY_BUFFER_OK;
    }
  }

  return AUDIO_HISTORY_BUFFER_OK;
}
