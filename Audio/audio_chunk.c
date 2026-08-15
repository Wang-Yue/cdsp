// Non-interleaved float buffers, one vector per channel.
#include "Audio/audio_chunk.h"

#ifdef ENABLE_ACCELERATE
#include <Accelerate/Accelerate.h>
#endif
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "Audio/audio_buffers.h"
#include "Audio/sample_conversion.h"
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

size_t sample_format_bytes_per_sample(binary_sample_format_t fmt) {
  switch (fmt) {
    case BINARY_SAMPLE_FORMAT_DSD_U8:
      return 1;
    case BINARY_SAMPLE_FORMAT_S16_LE:
    case BINARY_SAMPLE_FORMAT_S16_BE:
    case BINARY_SAMPLE_FORMAT_DSD_U16_LE:
    case BINARY_SAMPLE_FORMAT_DSD_U16_BE:
      return 2;
    case BINARY_SAMPLE_FORMAT_S24_3_LE:
    case BINARY_SAMPLE_FORMAT_S24_3_BE:
      return 3;
    case BINARY_SAMPLE_FORMAT_S24_4_RJ_LE:
    case BINARY_SAMPLE_FORMAT_S24_4_RJ_BE:
    case BINARY_SAMPLE_FORMAT_S24_4_LJ_LE:
    case BINARY_SAMPLE_FORMAT_S24_4_LJ_BE:
    case BINARY_SAMPLE_FORMAT_S32_LE:
    case BINARY_SAMPLE_FORMAT_S32_BE:
    case BINARY_SAMPLE_FORMAT_F32_LE:
    case BINARY_SAMPLE_FORMAT_F32_BE:
    case BINARY_SAMPLE_FORMAT_DSD_U32_LE:
    case BINARY_SAMPLE_FORMAT_DSD_U32_BE:
    case BINARY_SAMPLE_FORMAT_DSD_U32_REVERSED:
      return 4;
    case BINARY_SAMPLE_FORMAT_F64_LE:
    case BINARY_SAMPLE_FORMAT_F64_BE:
      return 8;
    default:
      return 0;
  }
}

bool audio_chunk_decode_interleaved(const void* src, binary_sample_format_t fmt,
                                    size_t channels, size_t frames,
                                    audio_chunk_t* chunk) {
  if (!src || !chunk || channels == 0 || frames == 0) return false;
  if (audio_chunk_get_channels(chunk) < channels) return false;

  double* dst_channels[channels];
  for (size_t c = 0; c < channels; c++) {
    dst_channels[c] = audio_chunk_get_channel(chunk, c);
  }

  switch (fmt) {
    case BINARY_SAMPLE_FORMAT_S16_LE: {
      const uint8_t* ptr = (const uint8_t*)src;
      for (size_t f = 0; f < frames; f++) {
        for (size_t c = 0; c < channels; c++) {
          dst_channels[c][f] = pcm_sample_decode_s16_bytes(ptr);
          ptr += 2;
        }
      }
      break;
    }
    case BINARY_SAMPLE_FORMAT_S24_3_LE: {
      const uint8_t* ptr = (const uint8_t*)src;
      for (size_t f = 0; f < frames; f++) {
        for (size_t c = 0; c < channels; c++) {
          dst_channels[c][f] = pcm_sample_decode_s24_3bytes(ptr);
          ptr += 3;
        }
      }
      break;
    }
    case BINARY_SAMPLE_FORMAT_S24_4_RJ_LE: {
      const uint8_t* ptr = (const uint8_t*)src;
      for (size_t f = 0; f < frames; f++) {
        for (size_t c = 0; c < channels; c++) {
          dst_channels[c][f] = pcm_sample_decode_s24_4_rj_bytes(ptr);
          ptr += 4;
        }
      }
      break;
    }
    case BINARY_SAMPLE_FORMAT_S24_4_LJ_LE: {
      const uint8_t* ptr = (const uint8_t*)src;
      for (size_t f = 0; f < frames; f++) {
        for (size_t c = 0; c < channels; c++) {
          dst_channels[c][f] = pcm_sample_decode_s24_4_lj_bytes(ptr);
          ptr += 4;
        }
      }
      break;
    }
    case BINARY_SAMPLE_FORMAT_S32_LE: {
      const uint8_t* ptr = (const uint8_t*)src;
      for (size_t f = 0; f < frames; f++) {
        for (size_t c = 0; c < channels; c++) {
          dst_channels[c][f] = pcm_sample_decode_s32_bytes(ptr);
          ptr += 4;
        }
      }
      break;
    }
    case BINARY_SAMPLE_FORMAT_F32_LE: {
      const uint8_t* ptr = (const uint8_t*)src;
      for (size_t f = 0; f < frames; f++) {
        for (size_t c = 0; c < channels; c++) {
          dst_channels[c][f] = pcm_sample_decode_f32_bytes(ptr);
          ptr += 4;
        }
      }
      break;
    }
    case BINARY_SAMPLE_FORMAT_F64_LE: {
      const uint8_t* ptr = (const uint8_t*)src;
      for (size_t f = 0; f < frames; f++) {
        for (size_t c = 0; c < channels; c++) {
          dst_channels[c][f] = pcm_sample_decode_f64_bytes(ptr);
          ptr += 8;
        }
      }
      break;
    }
    case BINARY_SAMPLE_FORMAT_DSD_U8: {
      const uint8_t* ptr = (const uint8_t*)src;
      for (size_t f = 0; f < frames; f++) {
        for (size_t c = 0; c < channels; c++) {
          dst_channels[c][f] = pcm_sample_decode_dsd_u8(*ptr);
          ptr += 1;
        }
      }
      break;
    }
    case BINARY_SAMPLE_FORMAT_DSD_U16_LE: {
      const uint8_t* ptr = (const uint8_t*)src;
      for (size_t f = 0; f < frames; f++) {
        for (size_t c = 0; c < channels; c++) {
          dst_channels[c][f] = pcm_sample_decode_dsd_u16_le_bytes(ptr);
          ptr += 2;
        }
      }
      break;
    }
    case BINARY_SAMPLE_FORMAT_DSD_U16_BE: {
      const uint8_t* ptr = (const uint8_t*)src;
      for (size_t f = 0; f < frames; f++) {
        for (size_t c = 0; c < channels; c++) {
          dst_channels[c][f] = pcm_sample_decode_dsd_u16_be_bytes(ptr);
          ptr += 2;
        }
      }
      break;
    }
    case BINARY_SAMPLE_FORMAT_DSD_U32_LE: {
      const uint8_t* ptr = (const uint8_t*)src;
      for (size_t f = 0; f < frames; f++) {
        for (size_t c = 0; c < channels; c++) {
          dst_channels[c][f] = pcm_sample_decode_dsd_u32_le_bytes(ptr);
          ptr += 4;
        }
      }
      break;
    }
    case BINARY_SAMPLE_FORMAT_DSD_U32_BE: {
      const uint8_t* ptr = (const uint8_t*)src;
      for (size_t f = 0; f < frames; f++) {
        for (size_t c = 0; c < channels; c++) {
          dst_channels[c][f] = pcm_sample_decode_dsd_u32_be_bytes(ptr);
          ptr += 4;
        }
      }
      break;
    }
    case BINARY_SAMPLE_FORMAT_DSD_U32_REVERSED: {
      const uint8_t* ptr = (const uint8_t*)src;
      for (size_t f = 0; f < frames; f++) {
        for (size_t c = 0; c < channels; c++) {
          dst_channels[c][f] = pcm_sample_decode_dsd_u32_reversed_bytes(ptr);
          ptr += 4;
        }
      }
      break;
    }
    default:
      return false;
  }

  audio_chunk_set_valid_frames(chunk, frames);
  return true;
}

bool audio_chunk_encode_interleaved(const audio_chunk_t* chunk,
                                    binary_sample_format_t fmt, size_t channels,
                                    size_t frames, void* dst) {
  if (!chunk || !dst || channels == 0 || frames == 0) return false;
  if (audio_chunk_get_channels(chunk) < channels) return false;

  const double* src_channels[channels];
  for (size_t c = 0; c < channels; c++) {
    src_channels[c] = audio_chunk_get_channel(chunk, c);
  }

  switch (fmt) {
    case BINARY_SAMPLE_FORMAT_S16_LE: {
      uint8_t* ptr = (uint8_t*)dst;
      for (size_t f = 0; f < frames; f++) {
        for (size_t c = 0; c < channels; c++) {
          pcm_sample_encode_s16_bytes(src_channels[c][f], ptr);
          ptr += 2;
        }
      }
      break;
    }
    case BINARY_SAMPLE_FORMAT_S24_3_LE: {
      uint8_t* ptr = (uint8_t*)dst;
      for (size_t f = 0; f < frames; f++) {
        for (size_t c = 0; c < channels; c++) {
          pcm_sample_encode_s24_3bytes(src_channels[c][f], ptr);
          ptr += 3;
        }
      }
      break;
    }
    case BINARY_SAMPLE_FORMAT_S24_4_RJ_LE: {
      uint8_t* ptr = (uint8_t*)dst;
      for (size_t f = 0; f < frames; f++) {
        for (size_t c = 0; c < channels; c++) {
          pcm_sample_encode_s24_4_rj_bytes(src_channels[c][f], ptr);
          ptr += 4;
        }
      }
      break;
    }
    case BINARY_SAMPLE_FORMAT_S24_4_LJ_LE: {
      uint8_t* ptr = (uint8_t*)dst;
      for (size_t f = 0; f < frames; f++) {
        for (size_t c = 0; c < channels; c++) {
          pcm_sample_encode_s24_4_lj_bytes(src_channels[c][f], ptr);
          ptr += 4;
        }
      }
      break;
    }
    case BINARY_SAMPLE_FORMAT_S32_LE: {
      uint8_t* ptr = (uint8_t*)dst;
      for (size_t f = 0; f < frames; f++) {
        for (size_t c = 0; c < channels; c++) {
          pcm_sample_encode_s32_bytes(src_channels[c][f], ptr);
          ptr += 4;
        }
      }
      break;
    }
    case BINARY_SAMPLE_FORMAT_F32_LE: {
      uint8_t* ptr = (uint8_t*)dst;
      for (size_t f = 0; f < frames; f++) {
        for (size_t c = 0; c < channels; c++) {
          pcm_sample_encode_f32_bytes(src_channels[c][f], ptr);
          ptr += 4;
        }
      }
      break;
    }
    case BINARY_SAMPLE_FORMAT_F64_LE: {
      uint8_t* ptr = (uint8_t*)dst;
      for (size_t f = 0; f < frames; f++) {
        for (size_t c = 0; c < channels; c++) {
          pcm_sample_encode_f64_bytes(src_channels[c][f], ptr);
          ptr += 8;
        }
      }
      break;
    }
    case BINARY_SAMPLE_FORMAT_DSD_U8: {
      uint8_t* ptr = (uint8_t*)dst;
      for (size_t f = 0; f < frames; f++) {
        for (size_t c = 0; c < channels; c++) {
          *ptr = pcm_sample_encode_dsd_u8(src_channels[c][f]);
          ptr += 1;
        }
      }
      break;
    }
    case BINARY_SAMPLE_FORMAT_DSD_U16_LE: {
      uint8_t* ptr = (uint8_t*)dst;
      for (size_t f = 0; f < frames; f++) {
        for (size_t c = 0; c < channels; c++) {
          pcm_sample_encode_dsd_u16_le_bytes(src_channels[c][f], ptr);
          ptr += 2;
        }
      }
      break;
    }
    case BINARY_SAMPLE_FORMAT_DSD_U16_BE: {
      uint8_t* ptr = (uint8_t*)dst;
      for (size_t f = 0; f < frames; f++) {
        for (size_t c = 0; c < channels; c++) {
          pcm_sample_encode_dsd_u16_be_bytes(src_channels[c][f], ptr);
          ptr += 2;
        }
      }
      break;
    }
    case BINARY_SAMPLE_FORMAT_DSD_U32_LE: {
      uint8_t* ptr = (uint8_t*)dst;
      for (size_t f = 0; f < frames; f++) {
        for (size_t c = 0; c < channels; c++) {
          pcm_sample_encode_dsd_u32_le_bytes(src_channels[c][f], ptr);
          ptr += 4;
        }
      }
      break;
    }
    case BINARY_SAMPLE_FORMAT_DSD_U32_BE: {
      uint8_t* ptr = (uint8_t*)dst;
      for (size_t f = 0; f < frames; f++) {
        for (size_t c = 0; c < channels; c++) {
          pcm_sample_encode_dsd_u32_be_bytes(src_channels[c][f], ptr);
          ptr += 4;
        }
      }
      break;
    }
    case BINARY_SAMPLE_FORMAT_DSD_U32_REVERSED: {
      uint8_t* ptr = (uint8_t*)dst;
      for (size_t f = 0; f < frames; f++) {
        for (size_t c = 0; c < channels; c++) {
          pcm_sample_encode_dsd_u32_reversed_bytes(src_channels[c][f], ptr);
          ptr += 4;
        }
      }
      break;
    }
    default:
      return false;
  }
  return true;
}

bool audio_chunk_decode_planar(const void* const* src_ptrs,
                               binary_sample_format_t fmt, size_t channels,
                               size_t frames, audio_chunk_t* chunk) {
  if (!src_ptrs || !chunk || channels == 0 || frames == 0) return false;
  if (audio_chunk_get_channels(chunk) < channels) return false;

  for (size_t c = 0; c < channels; c++) {
    double* dst = audio_chunk_get_channel(chunk, c);
    const uint8_t* src = (const uint8_t*)src_ptrs[c];
    if (!src || !dst) continue;

    switch (fmt) {
      case BINARY_SAMPLE_FORMAT_S16_LE: {
        for (size_t f = 0; f < frames; f++) {
          dst[f] = pcm_sample_decode_s16_bytes(src + f * 2);
        }
        break;
      }
      case BINARY_SAMPLE_FORMAT_S24_3_LE: {
        for (size_t f = 0; f < frames; f++) {
          dst[f] = pcm_sample_decode_s24_3bytes(src + f * 3);
        }
        break;
      }
      case BINARY_SAMPLE_FORMAT_S24_4_RJ_LE: {
        for (size_t f = 0; f < frames; f++) {
          dst[f] = pcm_sample_decode_s24_4_rj_bytes(src + f * 4);
        }
        break;
      }
      case BINARY_SAMPLE_FORMAT_S24_4_LJ_LE: {
        for (size_t f = 0; f < frames; f++) {
          dst[f] = pcm_sample_decode_s24_4_lj_bytes(src + f * 4);
        }
        break;
      }
      case BINARY_SAMPLE_FORMAT_S32_LE: {
        for (size_t f = 0; f < frames; f++) {
          dst[f] = pcm_sample_decode_s32_bytes(src + f * 4);
        }
        break;
      }
      case BINARY_SAMPLE_FORMAT_F32_LE: {
        for (size_t f = 0; f < frames; f++) {
          dst[f] = pcm_sample_decode_f32_bytes(src + f * 4);
        }
        break;
      }
      case BINARY_SAMPLE_FORMAT_F64_LE: {
        for (size_t f = 0; f < frames; f++) {
          dst[f] = pcm_sample_decode_f64_bytes(src + f * 8);
        }
        break;
      }
      case BINARY_SAMPLE_FORMAT_DSD_U8: {
        for (size_t f = 0; f < frames; f++) {
          dst[f] = pcm_sample_decode_dsd_u8(src[f]);
        }
        break;
      }
      case BINARY_SAMPLE_FORMAT_DSD_U16_LE: {
        for (size_t f = 0; f < frames; f++) {
          dst[f] = pcm_sample_decode_dsd_u16_le_bytes(src + f * 2);
        }
        break;
      }
      case BINARY_SAMPLE_FORMAT_DSD_U16_BE: {
        for (size_t f = 0; f < frames; f++) {
          dst[f] = pcm_sample_decode_dsd_u16_be_bytes(src + f * 2);
        }
        break;
      }
      case BINARY_SAMPLE_FORMAT_DSD_U32_LE: {
        for (size_t f = 0; f < frames; f++) {
          dst[f] = pcm_sample_decode_dsd_u32_le_bytes(src + f * 4);
        }
        break;
      }
      case BINARY_SAMPLE_FORMAT_DSD_U32_BE: {
        for (size_t f = 0; f < frames; f++) {
          dst[f] = pcm_sample_decode_dsd_u32_be_bytes(src + f * 4);
        }
        break;
      }
      case BINARY_SAMPLE_FORMAT_DSD_U32_REVERSED: {
        for (size_t f = 0; f < frames; f++) {
          dst[f] = pcm_sample_decode_dsd_u32_reversed_bytes(src + f * 4);
        }
        break;
      }
      default:
        return false;
    }
  }

  audio_chunk_set_valid_frames(chunk, frames);
  return true;
}

bool audio_chunk_encode_planar(const audio_chunk_t* chunk,
                               binary_sample_format_t fmt, size_t channels,
                               size_t frames, void* const* dst_ptrs) {
  if (!chunk || !dst_ptrs || channels == 0 || frames == 0) return false;
  if (audio_chunk_get_channels(chunk) < channels) return false;

  for (size_t c = 0; c < channels; c++) {
    const double* src = audio_chunk_get_channel(chunk, c);
    uint8_t* dst = (uint8_t*)dst_ptrs[c];
    if (!src || !dst) continue;

    switch (fmt) {
      case BINARY_SAMPLE_FORMAT_S16_LE: {
        for (size_t f = 0; f < frames; f++) {
          pcm_sample_encode_s16_bytes(src[f], dst + f * 2);
        }
        break;
      }
      case BINARY_SAMPLE_FORMAT_S24_3_LE: {
        for (size_t f = 0; f < frames; f++) {
          pcm_sample_encode_s24_3bytes(src[f], dst + f * 3);
        }
        break;
      }
      case BINARY_SAMPLE_FORMAT_S24_4_RJ_LE: {
        for (size_t f = 0; f < frames; f++) {
          pcm_sample_encode_s24_4_rj_bytes(src[f], dst + f * 4);
        }
        break;
      }
      case BINARY_SAMPLE_FORMAT_S24_4_LJ_LE: {
        for (size_t f = 0; f < frames; f++) {
          pcm_sample_encode_s24_4_lj_bytes(src[f], dst + f * 4);
        }
        break;
      }
      case BINARY_SAMPLE_FORMAT_S32_LE: {
        for (size_t f = 0; f < frames; f++) {
          pcm_sample_encode_s32_bytes(src[f], dst + f * 4);
        }
        break;
      }
      case BINARY_SAMPLE_FORMAT_F32_LE: {
        for (size_t f = 0; f < frames; f++) {
          pcm_sample_encode_f32_bytes(src[f], dst + f * 4);
        }
        break;
      }
      case BINARY_SAMPLE_FORMAT_F64_LE: {
        for (size_t f = 0; f < frames; f++) {
          pcm_sample_encode_f64_bytes(src[f], dst + f * 8);
        }
        break;
      }
      case BINARY_SAMPLE_FORMAT_DSD_U8: {
        for (size_t f = 0; f < frames; f++) {
          dst[f] = pcm_sample_encode_dsd_u8(src[f]);
        }
        break;
      }
      case BINARY_SAMPLE_FORMAT_DSD_U16_LE: {
        for (size_t f = 0; f < frames; f++) {
          pcm_sample_encode_dsd_u16_le_bytes(src[f], dst + f * 2);
        }
        break;
      }
      case BINARY_SAMPLE_FORMAT_DSD_U16_BE: {
        for (size_t f = 0; f < frames; f++) {
          pcm_sample_encode_dsd_u16_be_bytes(src[f], dst + f * 2);
        }
        break;
      }
      case BINARY_SAMPLE_FORMAT_DSD_U32_LE: {
        for (size_t f = 0; f < frames; f++) {
          pcm_sample_encode_dsd_u32_le_bytes(src[f], dst + f * 4);
        }
        break;
      }
      case BINARY_SAMPLE_FORMAT_DSD_U32_BE: {
        for (size_t f = 0; f < frames; f++) {
          pcm_sample_encode_dsd_u32_be_bytes(src[f], dst + f * 4);
        }
        break;
      }
      case BINARY_SAMPLE_FORMAT_DSD_U32_REVERSED: {
        for (size_t f = 0; f < frames; f++) {
          pcm_sample_encode_dsd_u32_reversed_bytes(src[f], dst + f * 4);
        }
        break;
      }
      default:
        return false;
    }
  }

  return true;
}
