// AudioHistoryBuffer — stores recent audio samples for spectrum analysis and
// vector scope (matching upstream CamillaDSP spectrum::AudioRingBuffer with
// lock-free / wait-free seqlock synchronization for real-time audio threads).
#include "Audio/audio_history_buffer.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "Audio/audio_chunk.h"
#include "Utils/cdsp_memory.h"

struct audio_history_buffer {
  size_t channels;
  size_t capacity;
  _Atomic uint64_t write_pos __attribute__((aligned(64)));
  _Atomic uint64_t total_written __attribute__((aligned(64)));
  _Atomic uint64_t write_seq __attribute__((aligned(64)));
  float** data;
};

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
    for (size_t ch = 0; ch < history->channels; ch++) {
      if (history->data[ch]) cdsp_aligned_free(history->data[ch]);
    }
    free(history->data);
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
    history->data = (float**)calloc(channels, sizeof(float*));
    if (!history->data) return;

    for (size_t ch = 0; ch < channels; ch++) {
      history->data[ch] =
          (float*)cdsp_aligned_alloc(64, history->capacity * sizeof(float));
      if (!history->data[ch]) {
        audio_history_buffer_clear_internal(history);
        return;
      }
      memset(history->data[ch], 0, history->capacity * sizeof(float));
    }
  }
}

void audio_history_buffer_free(audio_history_buffer_t* history) {
  if (!history) return;
  audio_history_buffer_clear_internal(history);
  cdsp_aligned_free(history);
}

bool audio_history_buffer_has_data(const audio_history_buffer_t* history) {
  if (!history) return false;
  return atomic_load_explicit(&history->total_written, memory_order_acquire) >
         0;
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

  for (size_t frame = 0; frame < n_frames; frame++) {
    size_t write_idx = (size_t)((pos + frame) % cap);
    for (size_t ch = 0; ch < n_ch; ch++) {
      const double* ch_data = audio_chunk_get_channel(chunk, ch);
      history->data[ch][write_idx] = (float)ch_data[frame];
    }
  }

  atomic_store_explicit(&history->write_pos, (pos + n_frames) % cap,
                        memory_order_release);
  atomic_fetch_add_explicit(&history->total_written, n_frames,
                            memory_order_release);
  atomic_store_explicit(&history->write_seq, seq + 2, memory_order_release);
}

audio_history_buffer_status_t audio_history_buffer_read_latest(
    const audio_history_buffer_t* history, float* dest, size_t count,
    int channel, bool* enough_data) {
  if (enough_data) *enough_data = false;
  if (!history || history->channels == 0 || !history->data) {
    return AUDIO_HISTORY_BUFFER_ERROR_EMPTY;
  }
  if (channel >= 0 && (size_t)channel >= history->channels) {
    return AUDIO_HISTORY_BUFFER_ERROR_OUT_OF_RANGE;
  }
  if (!dest || count == 0) return AUDIO_HISTORY_BUFFER_OK;

  size_t cap = history->capacity;
  if (count > cap) return AUDIO_HISTORY_BUFFER_OK;

  int retries = 10;
  while (retries > 0) {
    uint64_t seq_before =
        atomic_load_explicit(&history->write_seq, memory_order_acquire);
    if (seq_before & 1) {
      retries--;
      continue;
    }

    uint64_t total =
        atomic_load_explicit(&history->total_written, memory_order_acquire);
    if (total < (uint64_t)count) {
      return AUDIO_HISTORY_BUFFER_OK;
    }

    uint64_t pos =
        atomic_load_explicit(&history->write_pos, memory_order_acquire);
    size_t start = (size_t)((pos + cap - (count % cap)) % cap);

    if (channel >= 0) {
      const float* ch_src = history->data[channel];
      for (size_t i = 0; i < count; i++) {
        dest[i] = ch_src[(start + i) % cap];
      }
    } else {
      float n = (float)history->channels;
      for (size_t i = 0; i < count; i++) {
        size_t idx = (start + i) % cap;
        float sum = 0.0f;
        for (size_t ch = 0; ch < history->channels; ch++) {
          sum += history->data[ch][idx];
        }
        dest[i] = sum / n;
      }
    }

    uint64_t seq_after =
        atomic_load_explicit(&history->write_seq, memory_order_acquire);
    if (seq_after == seq_before) {
      if (enough_data) *enough_data = true;
      return AUDIO_HISTORY_BUFFER_OK;
    }
    retries--;
  }

  return AUDIO_HISTORY_BUFFER_OK;
}
