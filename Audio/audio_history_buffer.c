// AudioHistoryBuffer — stores recent audio samples for spectrum analysis and
// vector scope.
#include "Audio/audio_history_buffer.h"

#include <stdlib.h>

#include "Utils/lock_free_ring_buffer.h"

struct audio_history_buffer {
  size_t channels;
  spsc_audio_ring_buffer_t** buffers;
  float* scratch;
  size_t scratch_capacity;
};

size_t audio_history_buffer_get_channels(
    const audio_history_buffer_t* history) {
  return history ? history->channels : 0;
}
#include <string.h>

#ifdef ENABLE_ACCELERATE
#include <Accelerate/Accelerate.h>
#endif

audio_history_buffer_t* audio_history_buffer_create(void) {
  audio_history_buffer_t* history =
      (audio_history_buffer_t*)calloc(1, sizeof(audio_history_buffer_t));
  return history;
}

static void audio_history_buffer_clear_internal(
    audio_history_buffer_t* history) {
  if (!history) return;
  if (history->buffers) {
    for (size_t ch = 0; ch < history->channels; ch++) {
      if (history->buffers[ch])
        spsc_audio_ring_buffer_free(history->buffers[ch]);
    }
    free(history->buffers);
    history->buffers = NULL;
  }
  if (history->scratch) {
    free(history->scratch);
    history->scratch = NULL;
    history->scratch_capacity = 0;
  }
  history->channels = 0;
}

void audio_history_buffer_reset(audio_history_buffer_t* history,
                                size_t channels) {
  if (!history) return;
  audio_history_buffer_clear_internal(history);

  if (channels > 0) {
    history->buffers = (spsc_audio_ring_buffer_t**)calloc(
        channels, sizeof(spsc_audio_ring_buffer_t*));
    if (!history->buffers) return;

    history->channels = channels;
    history->scratch_capacity = 16384;
    history->scratch = (float*)calloc(history->scratch_capacity, sizeof(float));
    if (!history->scratch) {
      audio_history_buffer_clear_internal(history);
      return;
    }
    for (size_t ch = 0; ch < channels; ch++) {
      history->buffers[ch] =
          spsc_audio_ring_buffer_create(AUDIO_HISTORY_BUFFER_CAPACITY);
      if (!history->buffers[ch]) {
        audio_history_buffer_clear_internal(history);
        return;
      }
      history->channels = ch + 1;
      spsc_audio_ring_buffer_set_overwrite_on_overflow(history->buffers[ch],
                                                       true);
    }
  }
}

void audio_history_buffer_free(audio_history_buffer_t* history) {
  if (!history) return;
  audio_history_buffer_clear_internal(history);
  free(history);
}

bool audio_history_buffer_has_data(const audio_history_buffer_t* history) {
  if (!history || !history->buffers) return false;
  for (size_t ch = 0; ch < history->channels; ch++) {
    if (history->buffers[ch] &&
        spsc_audio_ring_buffer_get_total_samples_written(history->buffers[ch]) >
            0) {
      return true;
    }
  }
  return false;
}

void audio_history_buffer_append(audio_history_buffer_t* history,
                                 const audio_chunk_t* chunk) {
  if (!history || !chunk || !history->buffers) return;
  size_t chunk_channels = audio_chunk_get_channels(chunk);
  if (chunk_channels != history->channels || history->channels == 0) return;
  size_t valid = audio_chunk_get_valid_frames(chunk);
  if (valid == 0) return;
  for (size_t ch = 0; ch < history->channels; ch++) {
    mutable_waveform_t src_ptr = audio_chunk_get_channel(chunk, ch);
    if (src_ptr && history->buffers[ch]) {
      spsc_audio_ring_buffer_append_converting_double_to_float(
          history->buffers[ch], src_ptr, valid);
    }
  }
}

audio_history_buffer_status_t audio_history_buffer_read_latest(
    const audio_history_buffer_t* history, float* dest, size_t count,
    int channel, bool* enough_data) {
  if (enough_data) *enough_data = false;
  if (!history || history->channels == 0 || !history->buffers) {
    return AUDIO_HISTORY_BUFFER_ERROR_EMPTY;
  }
  if (channel >= 0 && (size_t)channel >= history->channels) {
    return AUDIO_HISTORY_BUFFER_ERROR_OUT_OF_RANGE;
  }
  if (!dest || count == 0) return AUDIO_HISTORY_BUFFER_OK;

  // If a specific channel is requested, read it directly and return.
  if (channel >= 0) {
    if (!history->buffers[channel]) return AUDIO_HISTORY_BUFFER_OK;
    uint64_t written = spsc_audio_ring_buffer_get_total_samples_written(
        history->buffers[channel]);
    if (written < (uint64_t)count) return AUDIO_HISTORY_BUFFER_OK;

    bool ok = spsc_audio_ring_buffer_read_latest_at(history->buffers[channel],
                                                    dest, count, written);
    if (enough_data) *enough_data = ok;
    return AUDIO_HISTORY_BUFFER_OK;
  }

  // Phase alignment logic: Find the minimum total samples written across all
  // channels. This ensures that when we read "latest" samples, we read from
  // the same point in time (phase-aligned) even if the producer is currently
  // writing to some channels but has not finished all of them.
  uint64_t min_written = UINT64_MAX;
  for (size_t ch = 0; ch < history->channels; ch++) {
    if (!history->buffers[ch]) continue;
    uint64_t w =
        spsc_audio_ring_buffer_get_total_samples_written(history->buffers[ch]);
    if (w < min_written) {
      min_written = w;
    }
  }
  // If we haven't even written 'count' samples yet overall, we can't satisfy
  // the request.
  if (min_written == UINT64_MAX || min_written < (uint64_t)count) {
    return AUDIO_HISTORY_BUFFER_OK;
  }

  // Otherwise, we need to average all channels.
  if (count > AUDIO_HISTORY_BUFFER_CAPACITY) {
    return AUDIO_HISTORY_BUFFER_OK;
  }

  // Step 1: Read channel 0 directly into the destination buffer.
  // This avoids having to initialize dest to zero and perform an extra copy.
  bool ok = spsc_audio_ring_buffer_read_latest_at(history->buffers[0], dest,
                                                  count, min_written);
  if (!ok) return AUDIO_HISTORY_BUFFER_OK;

  if (history->channels == 1) {
    if (enough_data) *enough_data = true;
    return AUDIO_HISTORY_BUFFER_OK;
  }

  // Step 2: Read subsequent channels into stack scratch memory in 2048-frame
  // chunks to prevent data races on multi-threaded reads and avoid dynamic
  // allocation.
  float stack_scratch[2048];
  for (size_t ch = 1; ch < history->channels; ch++) {
    for (size_t offset = 0; offset < count; offset += 2048) {
      size_t chunk_len = (count - offset > 2048) ? 2048 : (count - offset);
      uint64_t chunk_written = min_written - (count - offset - chunk_len);
      ok = spsc_audio_ring_buffer_read_latest_at(
          history->buffers[ch], stack_scratch, chunk_len, chunk_written);
      if (!ok) {
        return AUDIO_HISTORY_BUFFER_OK;
      }
#ifdef ENABLE_ACCELERATE
      vDSP_vadd(dest + offset, 1, stack_scratch, 1, dest + offset, 1,
                chunk_len);
#else
      for (size_t i = 0; i < chunk_len; i++) {
        dest[offset + i] += stack_scratch[i];
      }
#endif
    }
  }

  // Step 3: Scale the accumulated sum to get the average.
  float scale = 1.0f / (float)history->channels;
#ifdef ENABLE_ACCELERATE
  vDSP_vsmul(dest, 1, &scale, dest, 1, count);
#else
  for (size_t i = 0; i < count; i++) {
    dest[i] *= scale;
  }
#endif
  if (enough_data) *enough_data = true;
  return AUDIO_HISTORY_BUFFER_OK;
}
