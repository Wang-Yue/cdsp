/**
 * @file pipewire_internal.h
 * @brief Private, platform-independent helpers of the PipeWire backend.
 *
 * Used only by pipewire_backend.c and its unit tests. Kept outside the
 * ENABLE_PIPEWIRE guard so the tests run without libpipewire.
 */

#ifndef CLIB_BACKEND_PIPEWIRE_INTERNAL_H
#define CLIB_BACKEND_PIPEWIRE_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Clamp a PipeWire capture chunk to the mapped data region.
 *
 * A consumer must not trust `chunk->offset` and `chunk->size` blindly:
 * `offset + size` has to be bounded by `maxsize`. This mirrors the clamping in
 * PipeWire's own capture examples
 * (`offs = SPA_MIN(offset, maxsize); size = SPA_MIN(size, maxsize - offs)`)
 * and keeps the RT callback from reading outside the mapping.
 *
 * Platform independent so it can be unit tested without libpipewire.
 *
 * @param offset     `chunk->offset` reported by the producer.
 * @param size       `chunk->size` reported by the producer.
 * @param maxsize    `data->maxsize` of the mapped region.
 * @param blockalign Bytes per interleaved frame.
 * @param out_offset Validated byte offset into the region.
 * @return Number of whole frames that can be read from @p out_offset.
 */
static inline size_t
pipewire_capture_chunk_frames(uint32_t offset, uint32_t size, uint32_t maxsize,
                              size_t blockalign, size_t *out_offset) {
  if (out_offset)
    *out_offset = 0;
  if (maxsize == 0 || blockalign == 0)
    return 0;
  size_t off = offset < maxsize ? (size_t)offset : (size_t)maxsize;
  size_t avail = (size_t)maxsize - off;
  size_t sz = (size_t)size < avail ? (size_t)size : avail;
  if (out_offset)
    *out_offset = off;
  return sz / blockalign;
}

/**
 * @brief Number of bytes to render in a PipeWire playback callback.
 *
 * Same rule as upstream `pipewire_playback_callback_bytes`
 * (device.rs:235-248): use the requested size when it is valid, otherwise the
 * configured chunk size bounded by the buffer capacity, rounded down to whole
 * frames.
 */
static inline size_t pipewire_playback_callback_bytes(size_t requested_bytes,
                                                      size_t max_bytes,
                                                      size_t stride,
                                                      size_t chunk_size) {
  if (stride == 0)
    return 0;
  size_t fallback_bytes = chunk_size * stride;
  size_t bytes = (requested_bytes == 0 || requested_bytes > max_bytes)
                     ? (fallback_bytes < max_bytes ? fallback_bytes : max_bytes)
                     : requested_bytes;
  return bytes - (bytes % stride);
}

/**
 * @brief Track the PipeWire graph clock rate and detect changes.
 *
 * @param observed Graph rate from `spa_io_position.clock.rate.denom`
 *                 (0 = unknown).
 * @param seen     In/out: last observed rate, 0 before the first observation.
 * @return The new rate when it changed since the last observation, otherwise 0.
 *         The first observation only records the baseline and returns 0.
 */
static inline uint32_t pipewire_graph_rate_update(uint32_t observed,
                                                  uint32_t *seen) {
  if (observed == 0 || !seen)
    return 0;
  if (*seen == 0) {
    *seen = observed;
    return 0;
  }
  if (observed == *seen)
    return 0;
  *seen = observed;
  return observed;
}

#endif // CLIB_BACKEND_PIPEWIRE_INTERNAL_H
