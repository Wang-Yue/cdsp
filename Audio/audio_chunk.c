// Non-interleaved float buffers, one vector per channel.
#include "audio_chunk.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "Utils/double_helpers.h"

struct audio_chunk {
  audio_buffers_t* buffers;
  size_t valid_frames;
  bool owns_buffers;
};

struct round_robin_chunk_pool {
  audio_chunk_t** pool;
  size_t capacity;
  _Atomic size_t current_index;
};

size_t audio_chunk_get_frames(const audio_chunk_t* chunk) {
  return chunk ? audio_buffers_get_capacity(chunk->buffers) : 0;
}

size_t audio_chunk_get_channels(const audio_chunk_t* chunk) {
  return chunk ? audio_buffers_get_channels(chunk->buffers) : 0;
}

mutable_waveform_t audio_chunk_get_channel(const audio_chunk_t* chunk,
                                           size_t ch) {
  return chunk ? audio_buffers_get_channel(chunk->buffers, ch) : NULL;
}

size_t audio_chunk_get_valid_frames(const audio_chunk_t* chunk) {
  return chunk ? chunk->valid_frames : 0;
}

void audio_chunk_set_valid_frames(audio_chunk_t* chunk, size_t valid_frames) {
  if (chunk) chunk->valid_frames = valid_frames;
}

audio_chunk_t* audio_chunk_create(size_t frames, size_t channels) {
  audio_chunk_t* chunk = (audio_chunk_t*)calloc(1, sizeof(audio_chunk_t));
  if (!chunk) return NULL;
  chunk->buffers = audio_buffers_create(channels, frames);
  if (!chunk->buffers) {
    audio_chunk_free(chunk);
    return NULL;
  }
  chunk->valid_frames = frames;
  chunk->owns_buffers = true;
  return chunk;
}

void audio_chunk_free(audio_chunk_t* chunk) {
  if (!chunk) return;
  if (chunk->owns_buffers && chunk->buffers) {
    audio_buffers_free(chunk->buffers);
  }
  free(chunk);
}

/// A preallocated round-robin pool of unique `audio_chunk_t` instances.
round_robin_chunk_pool_t* round_robin_chunk_pool_create(size_t capacity,
                                                        size_t frames,
                                                        size_t channels) {
  if (capacity == 0) return NULL;
  if (capacity > SIZE_MAX / sizeof(audio_chunk_t*)) return NULL;
  round_robin_chunk_pool_t* pool =
      (round_robin_chunk_pool_t*)calloc(1, sizeof(round_robin_chunk_pool_t));
  if (!pool) return NULL;
  pool->capacity = capacity;
  pool->current_index = 0;
  pool->pool = (audio_chunk_t**)calloc(capacity, sizeof(audio_chunk_t*));
  if (!pool->pool) {
    free(pool);
    return NULL;
  }
  for (size_t i = 0; i < capacity; i++) {
    pool->pool[i] = audio_chunk_create(frames, channels);
    if (!pool->pool[i]) {
      round_robin_chunk_pool_free(pool);
      return NULL;
    }
  }
  return pool;
}

/// Retrieves the next available unique chunk buffer from the pool.
audio_chunk_t* round_robin_chunk_pool_next(round_robin_chunk_pool_t* pool) {
  if (!pool || pool->capacity == 0) return NULL;
  size_t idx =
      atomic_fetch_add_explicit(&pool->current_index, 1, memory_order_relaxed);
  return pool->pool[idx % pool->capacity];
}

void round_robin_chunk_pool_free(round_robin_chunk_pool_t* pool) {
  if (!pool) return;
  if (pool->pool) {
    for (size_t i = 0; i < pool->capacity; i++) {
      audio_chunk_free(pool->pool[i]);
    }
    free(pool->pool);
  }
  free(pool);
}

void audio_chunk_sum_channels(const audio_chunk_t* chunk, const int* channels,
                              size_t channels_count, double* out_sum,
                              size_t frames) {
  if (!chunk || !channels || channels_count == 0 || !out_sum || frames == 0)
    return;
  size_t max_frames = audio_chunk_get_frames(chunk);
  if (frames > max_frames) frames = max_frames;
  size_t total_channels = audio_chunk_get_channels(chunk);

  bool initialized = false;
  for (size_t ch_idx = 0; ch_idx < channels_count; ch_idx++) {
    int ch = channels[ch_idx];
    if (ch < 0 || (size_t)ch >= total_channels) continue;
    const double* src =
        audio_chunk_get_channel((audio_chunk_t*)chunk, (size_t)ch);
    if (!src) continue;

    if (!initialized) {
      memcpy(out_sum, src, frames * sizeof(double));
      initialized = true;
    } else {
#ifdef ENABLE_ACCELERATE
      vDSP_vaddD(out_sum, 1, src, 1, out_sum, 1, frames);
#else
      for (size_t i = 0; i < frames; i++) {
        out_sum[i] += src[i];
      }
#endif
    }
  }

  if (!initialized) {
    memset(out_sum, 0, frames * sizeof(double));
  }
}

void audio_chunk_apply_gain(audio_chunk_t* chunk, const int* channels,
                            size_t channels_count,
                            const double* gain_multipliers, size_t frames) {
  if (!chunk || !channels || channels_count == 0 || !gain_multipliers ||
      frames == 0)
    return;
  size_t max_frames = audio_chunk_get_frames(chunk);
  if (frames > max_frames) frames = max_frames;
  size_t total_channels = audio_chunk_get_channels(chunk);
  for (size_t ch_idx = 0; ch_idx < channels_count; ch_idx++) {
    int ch = channels[ch_idx];
    if (ch < 0 || (size_t)ch >= total_channels) continue;
    double* wave = audio_chunk_get_channel(chunk, (size_t)ch);
    if (!wave) continue;
#ifdef ENABLE_ACCELERATE
    vDSP_vmulD(wave, 1, gain_multipliers, 1, wave, 1, frames);
#else
    for (size_t i = 0; i < frames; i++) {
      wave[i] *= gain_multipliers[i];
    }
#endif
  }
}
