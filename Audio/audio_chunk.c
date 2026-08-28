// Non-interleaved float buffers, one vector per channel.
#include "Audio/audio_chunk.h"

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

void audio_chunk_sum_channels(const audio_chunk_t* chunk,
                              const size_t* channels, size_t channels_count,
                              double* out_sum, size_t frames) {
  if (!chunk || !channels || channels_count == 0 || !out_sum || frames == 0)
    return;
  size_t max_frames = audio_chunk_get_frames(chunk);
  if (frames > max_frames) frames = max_frames;
  size_t total_channels = audio_chunk_get_channels(chunk);

  bool initialized = false;
  for (size_t ch_idx = 0; ch_idx < channels_count; ch_idx++) {
    size_t ch = channels[ch_idx];
    if (ch >= total_channels) continue;
    const double* src = audio_chunk_get_channel((audio_chunk_t*)chunk, ch);
    if (!src) continue;

    if (!initialized) {
      memcpy(out_sum, src, frames * sizeof(double));
      initialized = true;
    } else {
      dsp_ops_add(src, out_sum, frames);
    }
  }

  if (!initialized) {
    memset(out_sum, 0, frames * sizeof(double));
  }
}

void audio_chunk_apply_gain(audio_chunk_t* chunk, const size_t* channels,
                            size_t channels_count,
                            const double* gain_multipliers, size_t frames) {
  if (!chunk || !channels || channels_count == 0 || !gain_multipliers ||
      frames == 0)
    return;
  size_t max_frames = audio_chunk_get_frames(chunk);
  if (frames > max_frames) frames = max_frames;
  size_t total_channels = audio_chunk_get_channels(chunk);
  for (size_t ch_idx = 0; ch_idx < channels_count; ch_idx++) {
    size_t ch = channels[ch_idx];
    if (ch >= total_channels) continue;
    double* wave = audio_chunk_get_channel(chunk, ch);
    if (!wave) continue;
    dsp_ops_multiply(gain_multipliers, wave, frames);
  }
}

// MARK: - Internal Stereo Single-Pass Conversion Fast Paths

static inline bool audio_channel_decode_stereo(const uint8_t* src,
                                               binary_sample_format_t fmt,
                                               size_t frames,
                                               double* restrict ch0,
                                               double* restrict ch1) {
  switch (fmt) {
    case BINARY_SAMPLE_FORMAT_S16_LE:
      for (size_t f = 0; f < frames; f++, src += 4) {
        ch0[f] = pcm_sample_decode_s16_bytes(src);
        ch1[f] = pcm_sample_decode_s16_bytes(src + 2);
      }
      break;
    case BINARY_SAMPLE_FORMAT_S24_3_LE:
      for (size_t f = 0; f < frames; f++, src += 6) {
        ch0[f] = pcm_sample_decode_s24_3bytes(src);
        ch1[f] = pcm_sample_decode_s24_3bytes(src + 3);
      }
      break;
    case BINARY_SAMPLE_FORMAT_S24_4_RJ_LE:
      for (size_t f = 0; f < frames; f++, src += 8) {
        ch0[f] = pcm_sample_decode_s24_4_rj_bytes(src);
        ch1[f] = pcm_sample_decode_s24_4_rj_bytes(src + 4);
      }
      break;
    case BINARY_SAMPLE_FORMAT_S24_4_LJ_LE:
      for (size_t f = 0; f < frames; f++, src += 8) {
        ch0[f] = pcm_sample_decode_s24_4_lj_bytes(src);
        ch1[f] = pcm_sample_decode_s24_4_lj_bytes(src + 4);
      }
      break;
    case BINARY_SAMPLE_FORMAT_S32_LE:
      for (size_t f = 0; f < frames; f++, src += 8) {
        ch0[f] = pcm_sample_decode_s32_bytes(src);
        ch1[f] = pcm_sample_decode_s32_bytes(src + 4);
      }
      break;
    case BINARY_SAMPLE_FORMAT_F32_LE:
      for (size_t f = 0; f < frames; f++, src += 8) {
        ch0[f] = pcm_sample_decode_f32_bytes(src);
        ch1[f] = pcm_sample_decode_f32_bytes(src + 4);
      }
      break;
    case BINARY_SAMPLE_FORMAT_F64_LE:
      for (size_t f = 0; f < frames; f++, src += 16) {
        ch0[f] = pcm_sample_decode_f64_bytes(src);
        ch1[f] = pcm_sample_decode_f64_bytes(src + 8);
      }
      break;
    case BINARY_SAMPLE_FORMAT_DSD_U8:
      for (size_t f = 0; f < frames; f++, src += 2) {
        ch0[f] = pcm_sample_decode_dsd_u8(src[0]);
        ch1[f] = pcm_sample_decode_dsd_u8(src[1]);
      }
      break;
    case BINARY_SAMPLE_FORMAT_DSD_U16_LE:
      for (size_t f = 0; f < frames; f++, src += 4) {
        ch0[f] = pcm_sample_decode_dsd_u16_le_bytes(src);
        ch1[f] = pcm_sample_decode_dsd_u16_le_bytes(src + 2);
      }
      break;
    case BINARY_SAMPLE_FORMAT_DSD_U16_BE:
      for (size_t f = 0; f < frames; f++, src += 4) {
        ch0[f] = pcm_sample_decode_dsd_u16_be_bytes(src);
        ch1[f] = pcm_sample_decode_dsd_u16_be_bytes(src + 2);
      }
      break;
    case BINARY_SAMPLE_FORMAT_DSD_U32_LE:
      for (size_t f = 0; f < frames; f++, src += 8) {
        ch0[f] = pcm_sample_decode_dsd_u32_le_bytes(src);
        ch1[f] = pcm_sample_decode_dsd_u32_le_bytes(src + 4);
      }
      break;
    case BINARY_SAMPLE_FORMAT_DSD_U32_BE:
      for (size_t f = 0; f < frames; f++, src += 8) {
        ch0[f] = pcm_sample_decode_dsd_u32_be_bytes(src);
        ch1[f] = pcm_sample_decode_dsd_u32_be_bytes(src + 4);
      }
      break;
    case BINARY_SAMPLE_FORMAT_DSD_U32_REVERSED:
      for (size_t f = 0; f < frames; f++, src += 8) {
        ch0[f] = pcm_sample_decode_dsd_u32_reversed_bytes(src);
        ch1[f] = pcm_sample_decode_dsd_u32_reversed_bytes(src + 4);
      }
      break;
    default:
      return false;
  }
  return true;
}

static inline bool audio_channel_encode_stereo(const double* restrict ch0,
                                               const double* restrict ch1,
                                               binary_sample_format_t fmt,
                                               size_t frames, uint8_t* dst) {
  switch (fmt) {
    case BINARY_SAMPLE_FORMAT_S16_LE:
      for (size_t f = 0; f < frames; f++, dst += 4) {
        pcm_sample_encode_s16_bytes(ch0[f], dst);
        pcm_sample_encode_s16_bytes(ch1[f], dst + 2);
      }
      break;
    case BINARY_SAMPLE_FORMAT_S24_3_LE:
      for (size_t f = 0; f < frames; f++, dst += 6) {
        pcm_sample_encode_s24_3bytes(ch0[f], dst);
        pcm_sample_encode_s24_3bytes(ch1[f], dst + 3);
      }
      break;
    case BINARY_SAMPLE_FORMAT_S24_4_RJ_LE:
      for (size_t f = 0; f < frames; f++, dst += 8) {
        pcm_sample_encode_s24_4_rj_bytes(ch0[f], dst);
        pcm_sample_encode_s24_4_rj_bytes(ch1[f], dst + 4);
      }
      break;
    case BINARY_SAMPLE_FORMAT_S24_4_LJ_LE:
      for (size_t f = 0; f < frames; f++, dst += 8) {
        pcm_sample_encode_s24_4_lj_bytes(ch0[f], dst);
        pcm_sample_encode_s24_4_lj_bytes(ch1[f], dst + 4);
      }
      break;
    case BINARY_SAMPLE_FORMAT_S32_LE:
      for (size_t f = 0; f < frames; f++, dst += 8) {
        pcm_sample_encode_s32_bytes(ch0[f], dst);
        pcm_sample_encode_s32_bytes(ch1[f], dst + 4);
      }
      break;
    case BINARY_SAMPLE_FORMAT_F32_LE:
      for (size_t f = 0; f < frames; f++, dst += 8) {
        pcm_sample_encode_f32_bytes(ch0[f], dst);
        pcm_sample_encode_f32_bytes(ch1[f], dst + 4);
      }
      break;
    case BINARY_SAMPLE_FORMAT_F64_LE:
      for (size_t f = 0; f < frames; f++, dst += 16) {
        pcm_sample_encode_f64_bytes(ch0[f], dst);
        pcm_sample_encode_f64_bytes(ch1[f], dst + 8);
      }
      break;
    case BINARY_SAMPLE_FORMAT_DSD_U8:
      for (size_t f = 0; f < frames; f++, dst += 2) {
        dst[0] = pcm_sample_encode_dsd_u8(ch0[f]);
        dst[1] = pcm_sample_encode_dsd_u8(ch1[f]);
      }
      break;
    case BINARY_SAMPLE_FORMAT_DSD_U16_LE:
      for (size_t f = 0; f < frames; f++, dst += 4) {
        pcm_sample_encode_dsd_u16_le_bytes(ch0[f], dst);
        pcm_sample_encode_dsd_u16_le_bytes(ch1[f], dst + 2);
      }
      break;
    case BINARY_SAMPLE_FORMAT_DSD_U16_BE:
      for (size_t f = 0; f < frames; f++, dst += 4) {
        pcm_sample_encode_dsd_u16_be_bytes(ch0[f], dst);
        pcm_sample_encode_dsd_u16_be_bytes(ch1[f], dst + 2);
      }
      break;
    case BINARY_SAMPLE_FORMAT_DSD_U32_LE:
      for (size_t f = 0; f < frames; f++, dst += 8) {
        pcm_sample_encode_dsd_u32_le_bytes(ch0[f], dst);
        pcm_sample_encode_dsd_u32_le_bytes(ch1[f], dst + 4);
      }
      break;
    case BINARY_SAMPLE_FORMAT_DSD_U32_BE:
      for (size_t f = 0; f < frames; f++, dst += 8) {
        pcm_sample_encode_dsd_u32_be_bytes(ch0[f], dst);
        pcm_sample_encode_dsd_u32_be_bytes(ch1[f], dst + 4);
      }
      break;
    case BINARY_SAMPLE_FORMAT_DSD_U32_REVERSED:
      for (size_t f = 0; f < frames; f++, dst += 8) {
        pcm_sample_encode_dsd_u32_reversed_bytes(ch0[f], dst);
        pcm_sample_encode_dsd_u32_reversed_bytes(ch1[f], dst + 4);
      }
      break;
    default:
      return false;
  }
  return true;
}

// MARK: - Internal Strided Channel Conversion Helpers

static inline bool audio_channel_decode(const uint8_t* src,
                                        binary_sample_format_t fmt,
                                        size_t frames, size_t byte_stride,
                                        double* restrict dst) {
  switch (fmt) {
    case BINARY_SAMPLE_FORMAT_S16_LE:
      for (size_t f = 0; f < frames; f++) {
        dst[f] = pcm_sample_decode_s16_bytes(src + f * byte_stride);
      }
      break;
    case BINARY_SAMPLE_FORMAT_S24_3_LE:
      for (size_t f = 0; f < frames; f++) {
        dst[f] = pcm_sample_decode_s24_3bytes(src + f * byte_stride);
      }
      break;
    case BINARY_SAMPLE_FORMAT_S24_4_RJ_LE:
      for (size_t f = 0; f < frames; f++) {
        dst[f] = pcm_sample_decode_s24_4_rj_bytes(src + f * byte_stride);
      }
      break;
    case BINARY_SAMPLE_FORMAT_S24_4_LJ_LE:
      for (size_t f = 0; f < frames; f++) {
        dst[f] = pcm_sample_decode_s24_4_lj_bytes(src + f * byte_stride);
      }
      break;
    case BINARY_SAMPLE_FORMAT_S32_LE:
      for (size_t f = 0; f < frames; f++) {
        dst[f] = pcm_sample_decode_s32_bytes(src + f * byte_stride);
      }
      break;
    case BINARY_SAMPLE_FORMAT_F32_LE:
      for (size_t f = 0; f < frames; f++) {
        dst[f] = pcm_sample_decode_f32_bytes(src + f * byte_stride);
      }
      break;
    case BINARY_SAMPLE_FORMAT_F64_LE:
      for (size_t f = 0; f < frames; f++) {
        dst[f] = pcm_sample_decode_f64_bytes(src + f * byte_stride);
      }
      break;
    case BINARY_SAMPLE_FORMAT_DSD_U8:
      for (size_t f = 0; f < frames; f++) {
        dst[f] = pcm_sample_decode_dsd_u8(*(src + f * byte_stride));
      }
      break;
    case BINARY_SAMPLE_FORMAT_DSD_U16_LE:
      for (size_t f = 0; f < frames; f++) {
        dst[f] = pcm_sample_decode_dsd_u16_le_bytes(src + f * byte_stride);
      }
      break;
    case BINARY_SAMPLE_FORMAT_DSD_U16_BE:
      for (size_t f = 0; f < frames; f++) {
        dst[f] = pcm_sample_decode_dsd_u16_be_bytes(src + f * byte_stride);
      }
      break;
    case BINARY_SAMPLE_FORMAT_DSD_U32_LE:
      for (size_t f = 0; f < frames; f++) {
        dst[f] = pcm_sample_decode_dsd_u32_le_bytes(src + f * byte_stride);
      }
      break;
    case BINARY_SAMPLE_FORMAT_DSD_U32_BE:
      for (size_t f = 0; f < frames; f++) {
        dst[f] = pcm_sample_decode_dsd_u32_be_bytes(src + f * byte_stride);
      }
      break;
    case BINARY_SAMPLE_FORMAT_DSD_U32_REVERSED:
      for (size_t f = 0; f < frames; f++) {
        dst[f] =
            pcm_sample_decode_dsd_u32_reversed_bytes(src + f * byte_stride);
      }
      break;
    default:
      return false;
  }
  return true;
}

static inline bool audio_channel_encode(const double* restrict src,
                                        binary_sample_format_t fmt,
                                        size_t frames, size_t byte_stride,
                                        uint8_t* dst) {
  switch (fmt) {
    case BINARY_SAMPLE_FORMAT_S16_LE:
      for (size_t f = 0; f < frames; f++) {
        pcm_sample_encode_s16_bytes(src[f], dst + f * byte_stride);
      }
      break;
    case BINARY_SAMPLE_FORMAT_S24_3_LE:
      for (size_t f = 0; f < frames; f++) {
        pcm_sample_encode_s24_3bytes(src[f], dst + f * byte_stride);
      }
      break;
    case BINARY_SAMPLE_FORMAT_S24_4_RJ_LE:
      for (size_t f = 0; f < frames; f++) {
        pcm_sample_encode_s24_4_rj_bytes(src[f], dst + f * byte_stride);
      }
      break;
    case BINARY_SAMPLE_FORMAT_S24_4_LJ_LE:
      for (size_t f = 0; f < frames; f++) {
        pcm_sample_encode_s24_4_lj_bytes(src[f], dst + f * byte_stride);
      }
      break;
    case BINARY_SAMPLE_FORMAT_S32_LE:
      for (size_t f = 0; f < frames; f++) {
        pcm_sample_encode_s32_bytes(src[f], dst + f * byte_stride);
      }
      break;
    case BINARY_SAMPLE_FORMAT_F32_LE:
      for (size_t f = 0; f < frames; f++) {
        pcm_sample_encode_f32_bytes(src[f], dst + f * byte_stride);
      }
      break;
    case BINARY_SAMPLE_FORMAT_F64_LE:
      for (size_t f = 0; f < frames; f++) {
        pcm_sample_encode_f64_bytes(src[f], dst + f * byte_stride);
      }
      break;
    case BINARY_SAMPLE_FORMAT_DSD_U8:
      for (size_t f = 0; f < frames; f++) {
        *(dst + f * byte_stride) = pcm_sample_encode_dsd_u8(src[f]);
      }
      break;
    case BINARY_SAMPLE_FORMAT_DSD_U16_LE:
      for (size_t f = 0; f < frames; f++) {
        pcm_sample_encode_dsd_u16_le_bytes(src[f], dst + f * byte_stride);
      }
      break;
    case BINARY_SAMPLE_FORMAT_DSD_U16_BE:
      for (size_t f = 0; f < frames; f++) {
        pcm_sample_encode_dsd_u16_be_bytes(src[f], dst + f * byte_stride);
      }
      break;
    case BINARY_SAMPLE_FORMAT_DSD_U32_LE:
      for (size_t f = 0; f < frames; f++) {
        pcm_sample_encode_dsd_u32_le_bytes(src[f], dst + f * byte_stride);
      }
      break;
    case BINARY_SAMPLE_FORMAT_DSD_U32_BE:
      for (size_t f = 0; f < frames; f++) {
        pcm_sample_encode_dsd_u32_be_bytes(src[f], dst + f * byte_stride);
      }
      break;
    case BINARY_SAMPLE_FORMAT_DSD_U32_REVERSED:
      for (size_t f = 0; f < frames; f++) {
        pcm_sample_encode_dsd_u32_reversed_bytes(src[f], dst + f * byte_stride);
      }
      break;
    default:
      return false;
  }
  return true;
}

// MARK: - Public Interleaved Decode and Encode APIs

bool audio_chunk_decode_interleaved(const void* src, binary_sample_format_t fmt,
                                    size_t channels, size_t frames,
                                    audio_chunk_t* chunk) {
  if (!src || !chunk || channels == 0 || frames == 0) return false;
  if (audio_chunk_get_channels(chunk) < channels) return false;

  bool ok = false;
  if (channels == 2) {
    double* ch0 = audio_chunk_get_channel(chunk, 0);
    double* ch1 = audio_chunk_get_channel(chunk, 1);
    if (!ch0 || !ch1) return false;
    ok =
        audio_channel_decode_stereo((const uint8_t*)src, fmt, frames, ch0, ch1);
  } else {
    size_t bytes_per_sample = sample_format_bytes_per_sample(fmt);
    if (bytes_per_sample == 0) return false;

    const uint8_t* ptr = (const uint8_t*)src;
    size_t byte_stride = channels * bytes_per_sample;

    ok = true;
    for (size_t c = 0; c < channels; c++) {
      double* dst = audio_chunk_get_channel(chunk, c);
      if (!dst || !audio_channel_decode(ptr + c * bytes_per_sample, fmt, frames,
                                        byte_stride, dst)) {
        ok = false;
        break;
      }
    }
  }

  if (ok) {
    audio_chunk_set_valid_frames(chunk, frames);
  }
  return ok;
}

bool audio_chunk_encode_interleaved(const audio_chunk_t* chunk,
                                    binary_sample_format_t fmt, size_t channels,
                                    size_t frames, void* dst) {
  if (!chunk || !dst || channels == 0 || frames == 0) return false;
  if (audio_chunk_get_channels(chunk) < channels) return false;

  if (channels == 2) {
    const double* ch0 = audio_chunk_get_channel(chunk, 0);
    const double* ch1 = audio_chunk_get_channel(chunk, 1);
    if (!ch0 || !ch1) return false;
    return audio_channel_encode_stereo(ch0, ch1, fmt, frames, (uint8_t*)dst);
  }

  size_t bytes_per_sample = sample_format_bytes_per_sample(fmt);
  if (bytes_per_sample == 0) return false;

  uint8_t* out_ptr = (uint8_t*)dst;
  size_t byte_stride = channels * bytes_per_sample;

  for (size_t c = 0; c < channels; c++) {
    const double* src = audio_chunk_get_channel(chunk, c);
    if (!src || !audio_channel_encode(src, fmt, frames, byte_stride,
                                      out_ptr + c * bytes_per_sample)) {
      return false;
    }
  }

  return true;
}
