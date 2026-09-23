// Single-producer / single-consumer lock-free primitives used by the audio
// thread.
#include "utils/lock_free_ring_buffer.h"

#include "utils/cdsp_memory.h"

struct spsc_queue {
  size_t capacity;
  size_t mask;
  void **storage;
  _Atomic uint64_t write_index __attribute__((aligned(64)));
  _Atomic uint64_t read_index __attribute__((aligned(64)));
};

struct spsc_byte_ring_buffer {
  size_t capacity;
  size_t mask;
  uint8_t *storage;
  _Atomic uint64_t write_index __attribute__((aligned(64)));
  _Atomic uint64_t read_index __attribute__((aligned(64)));
};

size_t spsc_queue_get_count(const spsc_queue_t *queue) {
  if (!queue)
    return 0;
  uint64_t r = atomic_load_explicit(&queue->read_index, memory_order_acquire);
  uint64_t w = atomic_load_explicit(&queue->write_index, memory_order_acquire);
  if (w < r)
    return 0;
  size_t count = (size_t)(w - r);
  if (count > queue->capacity) {
    return queue->capacity;
  }
  return count;
}

size_t spsc_queue_get_capacity(const spsc_queue_t *queue) {
  return queue ? queue->capacity : 0;
}

// MARK: - SPSCQueue Implementation

spsc_queue_t *spsc_queue_create(size_t minimum_capacity) {
  size_t cap = spsc_round_up_to_power_of_two(
      minimum_capacity < 2 ? 2 : minimum_capacity);
  spsc_queue_t *queue =
      (spsc_queue_t *)cdsp_aligned_alloc(64, sizeof(spsc_queue_t));
  if (!queue)
    return NULL;
  memset(queue, 0, sizeof(spsc_queue_t));
  queue->capacity = cap;
  queue->mask = cap - 1;
  queue->storage = (void **)cdsp_aligned_alloc(64, cap * sizeof(void *));
  if (!queue->storage) {
    cdsp_aligned_free(queue);
    return NULL;
  }
  memset(queue->storage, 0, cap * sizeof(void *));
  atomic_init(&queue->write_index, 0);
  atomic_init(&queue->read_index, 0);
  return queue;
}

void spsc_queue_free(spsc_queue_t *queue) {
  if (!queue)
    return;
  // Clearing each slot to NULL; deinitialize then deallocate the raw storage.
  cdsp_aligned_free(queue->storage);
  cdsp_aligned_free(queue);
}

bool spsc_queue_enqueue(spsc_queue_t *queue, void *value) {
  if (!queue)
    return false;
  uint64_t w = atomic_load_explicit(&queue->write_index, memory_order_relaxed);
  uint64_t r = atomic_load_explicit(&queue->read_index, memory_order_acquire);
  if (w - r >= (uint64_t)queue->capacity)
    return false;
  queue->storage[(size_t)(w & queue->mask)] = value;
  atomic_store_explicit(&queue->write_index, w + 1, memory_order_release);
  return true;
}

void *spsc_queue_dequeue(spsc_queue_t *queue) {
  if (!queue)
    return NULL;
  uint64_t r = atomic_load_explicit(&queue->read_index, memory_order_relaxed);
  uint64_t w = atomic_load_explicit(&queue->write_index, memory_order_acquire);
  if (r == w)
    return NULL;
  size_t slot = (size_t)(r & queue->mask);
  void *value = queue->storage[slot];
  queue->storage[slot] = NULL;
  atomic_store_explicit(&queue->read_index, r + 1, memory_order_release);
  return value;
}

void spsc_queue_drain(spsc_queue_t *queue) {
  if (!queue)
    return;
  while (spsc_queue_dequeue(queue) != NULL) {
  }
}

// MARK: - SPSCByteRingBuffer Implementation

spsc_byte_ring_buffer_t *spsc_byte_ring_buffer_create(size_t minimum_capacity) {
  size_t cap = spsc_round_up_to_power_of_two(
      minimum_capacity < 2 ? 2 : minimum_capacity);
  spsc_byte_ring_buffer_t *ring = (spsc_byte_ring_buffer_t *)cdsp_aligned_alloc(
      64, sizeof(spsc_byte_ring_buffer_t));
  if (!ring)
    return NULL;
  memset(ring, 0, sizeof(spsc_byte_ring_buffer_t));
  ring->capacity = cap;
  ring->mask = cap - 1;
  ring->storage = (uint8_t *)cdsp_aligned_alloc(64, cap);
  if (!ring->storage) {
    cdsp_aligned_free(ring);
    return NULL;
  }
  memset(ring->storage, 0, cap);
  atomic_init(&ring->write_index, 0);
  atomic_init(&ring->read_index, 0);
  return ring;
}

void spsc_byte_ring_buffer_free(spsc_byte_ring_buffer_t *ring) {
  if (!ring)
    return;
  cdsp_aligned_free(ring->storage);
  cdsp_aligned_free(ring);
}

size_t spsc_byte_ring_buffer_get_available_to_read(
    const spsc_byte_ring_buffer_t *ring) {
  if (!ring)
    return 0;
  uint64_t r = atomic_load_explicit(&ring->read_index, memory_order_acquire);
  uint64_t w = atomic_load_explicit(&ring->write_index, memory_order_acquire);
  if (w < r)
    return 0;
  size_t avail = (size_t)(w - r);
  return (avail > ring->capacity) ? ring->capacity : avail;
}

size_t spsc_byte_ring_buffer_get_available_to_write(
    const spsc_byte_ring_buffer_t *ring) {
  if (!ring)
    return 0;
  uint64_t w = atomic_load_explicit(&ring->write_index, memory_order_relaxed);
  uint64_t r = atomic_load_explicit(&ring->read_index, memory_order_acquire);
  size_t occupied = (size_t)(w - r);
  if (occupied >= ring->capacity)
    return 0;
  return ring->capacity - occupied;
}

size_t spsc_byte_ring_buffer_get_capacity(const spsc_byte_ring_buffer_t *ring) {
  return ring ? ring->capacity : 0;
}

size_t spsc_byte_ring_buffer_write(spsc_byte_ring_buffer_t *ring,
                                   const uint8_t *source, size_t count) {
  if (!ring || !source || count == 0)
    return 0;
  size_t free_space = spsc_byte_ring_buffer_get_available_to_write(ring);
  size_t to_write = (count < free_space) ? count : free_space;
  if (to_write == 0)
    return 0;
  uint64_t w = atomic_load_explicit(&ring->write_index, memory_order_relaxed);
  size_t offset = (size_t)(w & ring->mask);
  size_t first_chunk = ring->capacity - offset;
  if (to_write <= first_chunk) {
    memcpy(ring->storage + offset, source, to_write);
  } else {
    memcpy(ring->storage + offset, source, first_chunk);
    memcpy(ring->storage, source + first_chunk, to_write - first_chunk);
  }
  atomic_store_explicit(&ring->write_index, w + to_write, memory_order_release);
  return to_write;
}

size_t spsc_byte_ring_buffer_consume(spsc_byte_ring_buffer_t *ring,
                                     uint8_t *dest, size_t count) {
  if (!ring || !dest || count == 0)
    return 0;
  size_t avail = spsc_byte_ring_buffer_get_available_to_read(ring);
  size_t to_read = (count < avail) ? count : avail;
  if (to_read == 0)
    return 0;
  uint64_t r = atomic_load_explicit(&ring->read_index, memory_order_relaxed);
  size_t offset = (size_t)(r & ring->mask);
  size_t first_chunk = ring->capacity - offset;
  if (to_read <= first_chunk) {
    memcpy(dest, ring->storage + offset, to_read);
  } else {
    memcpy(dest, ring->storage + offset, first_chunk);
    memcpy(dest + first_chunk, ring->storage, to_read - first_chunk);
  }
  atomic_store_explicit(&ring->read_index, r + to_read, memory_order_release);
  return to_read;
}

size_t spsc_byte_ring_buffer_read_with_silence(spsc_byte_ring_buffer_t *ring,
                                               void *dst, size_t frames,
                                               size_t blockalign,
                                               uint8_t silence_byte,
                                               _Atomic size_t *silence_frames,
                                               _Atomic bool *is_running) {
  if (!dst || frames == 0 || blockalign == 0)
    return 0;

  size_t silence = 0;
  if (silence_frames) {
    size_t pending = atomic_load_explicit(silence_frames, memory_order_relaxed);
    silence = (pending < frames) ? pending : frames;
    if (silence > 0) {
      atomic_fetch_sub_explicit(silence_frames, silence, memory_order_relaxed);
    }
  }

  uint8_t *ptr = (uint8_t *)dst;
  if (silence > 0) {
    memset(ptr, silence_byte, silence * blockalign);
    ptr += silence * blockalign;
  }

  size_t audio_needed = frames - silence;
  size_t consumed_bytes = 0;
  if (ring && audio_needed > 0) {
    consumed_bytes =
        spsc_byte_ring_buffer_consume(ring, ptr, audio_needed * blockalign);
  }

  size_t consumed_frames = consumed_bytes / blockalign;
  if (consumed_frames < audio_needed) {
    size_t missing_bytes = (audio_needed - consumed_frames) * blockalign;
    memset(ptr + consumed_bytes, silence_byte, missing_bytes);
    if (is_running) {
      atomic_store_explicit(is_running, false, memory_order_release);
    }
  }

  return consumed_frames;
}

size_t
spsc_byte_ring_buffer_get_read_slices(const spsc_byte_ring_buffer_t *ring,
                                      size_t max_bytes, const uint8_t **slice1,
                                      size_t *len1, const uint8_t **slice2,
                                      size_t *len2) {
  if (slice1)
    *slice1 = NULL;
  if (len1)
    *len1 = 0;
  if (slice2)
    *slice2 = NULL;
  if (len2)
    *len2 = 0;
  if (!ring || max_bytes == 0)
    return 0;

  size_t avail = spsc_byte_ring_buffer_get_available_to_read(ring);
  size_t to_read = (max_bytes < avail) ? max_bytes : avail;
  if (to_read == 0)
    return 0;

  uint64_t r = atomic_load_explicit(&ring->read_index, memory_order_relaxed);
  size_t offset = (size_t)(r & ring->mask);
  size_t first_chunk = ring->capacity - offset;

  if (to_read <= first_chunk) {
    if (slice1)
      *slice1 = ring->storage + offset;
    if (len1)
      *len1 = to_read;
  } else {
    if (slice1)
      *slice1 = ring->storage + offset;
    if (len1)
      *len1 = first_chunk;
    if (slice2)
      *slice2 = ring->storage;
    if (len2)
      *len2 = to_read - first_chunk;
  }
  return to_read;
}

void spsc_byte_ring_buffer_advance_read(spsc_byte_ring_buffer_t *ring,
                                        size_t count) {
  if (!ring || count == 0)
    return;
  uint64_t r = atomic_load_explicit(&ring->read_index, memory_order_relaxed);
  atomic_store_explicit(&ring->read_index, r + count, memory_order_release);
}

size_t spsc_byte_ring_buffer_get_write_slices(
    const spsc_byte_ring_buffer_t *ring, size_t max_bytes, uint8_t **slice1,
    size_t *len1, uint8_t **slice2, size_t *len2) {
  if (slice1)
    *slice1 = NULL;
  if (len1)
    *len1 = 0;
  if (slice2)
    *slice2 = NULL;
  if (len2)
    *len2 = 0;
  if (!ring || max_bytes == 0)
    return 0;

  size_t free_space = spsc_byte_ring_buffer_get_available_to_write(ring);
  size_t to_write = (max_bytes < free_space) ? max_bytes : free_space;
  if (to_write == 0)
    return 0;

  uint64_t w = atomic_load_explicit(&ring->write_index, memory_order_relaxed);
  size_t offset = (size_t)(w & ring->mask);
  size_t first_chunk = ring->capacity - offset;

  if (to_write <= first_chunk) {
    if (slice1)
      *slice1 = ring->storage + offset;
    if (len1)
      *len1 = to_write;
  } else {
    if (slice1)
      *slice1 = ring->storage + offset;
    if (len1)
      *len1 = first_chunk;
    if (slice2)
      *slice2 = ring->storage;
    if (len2)
      *len2 = to_write - first_chunk;
  }
  return to_write;
}

void spsc_byte_ring_buffer_advance_write(spsc_byte_ring_buffer_t *ring,
                                         size_t count) {
  if (!ring || count == 0)
    return;
  uint64_t w = atomic_load_explicit(&ring->write_index, memory_order_relaxed);
  atomic_store_explicit(&ring->write_index, w + count, memory_order_release);
}

size_t spsc_byte_ring_buffer_write_silence(spsc_byte_ring_buffer_t *ring,
                                           size_t bytes, uint8_t silence_byte) {
  if (!ring || bytes == 0)
    return 0;

  uint8_t *s1 = NULL, *s2 = NULL;
  size_t l1 = 0, l2 = 0;
  size_t to_write =
      spsc_byte_ring_buffer_get_write_slices(ring, bytes, &s1, &l1, &s2, &l2);
  if (to_write == 0)
    return 0;

  if (s1 && l1 > 0) {
    memset(s1, silence_byte, l1);
  }
  if (s2 && l2 > 0) {
    memset(s2, silence_byte, l2);
  }
  spsc_byte_ring_buffer_advance_write(ring, to_write);
  return to_write;
}

void spsc_byte_ring_buffer_drain(spsc_byte_ring_buffer_t *ring) {
  if (!ring)
    return;
  uint64_t w = atomic_load_explicit(&ring->write_index, memory_order_acquire);
  atomic_store_explicit(&ring->read_index, w, memory_order_release);
}

// MARK: - SPSCPlanarRingBuffer Implementation

struct spsc_planar_ring_buffer {
  size_t channels;
  size_t bytes_per_sample;
  size_t capacity; // in frames (power of 2)
  size_t mask;
  uint8_t **channel_storage;
  _Atomic uint64_t write_index __attribute__((aligned(64)));
  _Atomic uint64_t read_index __attribute__((aligned(64)));
};

spsc_planar_ring_buffer_t *
spsc_planar_ring_buffer_create(size_t channels, size_t bytes_per_sample,
                               size_t minimum_capacity_frames) {
  if (channels == 0 || bytes_per_sample == 0)
    return NULL;

  size_t cap = spsc_round_up_to_power_of_two(
      minimum_capacity_frames < 2 ? 2 : minimum_capacity_frames);

  spsc_planar_ring_buffer_t *ring =
      (spsc_planar_ring_buffer_t *)cdsp_aligned_alloc(
          64, sizeof(spsc_planar_ring_buffer_t));
  if (!ring)
    return NULL;

  memset(ring, 0, sizeof(spsc_planar_ring_buffer_t));
  ring->channels = channels;
  ring->bytes_per_sample = bytes_per_sample;
  ring->capacity = cap;
  ring->mask = cap - 1;

  ring->channel_storage =
      (uint8_t **)cdsp_aligned_alloc(64, channels * sizeof(uint8_t *));
  if (!ring->channel_storage) {
    cdsp_aligned_free(ring);
    return NULL;
  }
  memset(ring->channel_storage, 0, channels * sizeof(uint8_t *));

  size_t bytes_per_channel = cap * bytes_per_sample;
  for (size_t ch = 0; ch < channels; ch++) {
    ring->channel_storage[ch] =
        (uint8_t *)cdsp_aligned_alloc(64, bytes_per_channel);
    if (!ring->channel_storage[ch]) {
      for (size_t i = 0; i < ch; i++) {
        cdsp_aligned_free(ring->channel_storage[i]);
      }
      cdsp_aligned_free(ring->channel_storage);
      cdsp_aligned_free(ring);
      return NULL;
    }
    memset(ring->channel_storage[ch], 0, bytes_per_channel);
  }

  atomic_init(&ring->write_index, 0);
  atomic_init(&ring->read_index, 0);
  return ring;
}

void spsc_planar_ring_buffer_free(spsc_planar_ring_buffer_t *ring) {
  if (!ring)
    return;
  if (ring->channel_storage) {
    for (size_t ch = 0; ch < ring->channels; ch++) {
      if (ring->channel_storage[ch]) {
        cdsp_aligned_free(ring->channel_storage[ch]);
      }
    }
    cdsp_aligned_free(ring->channel_storage);
  }
  cdsp_aligned_free(ring);
}

size_t spsc_planar_ring_buffer_get_available_to_read(
    const spsc_planar_ring_buffer_t *ring) {
  if (!ring)
    return 0;
  uint64_t w = atomic_load_explicit(&ring->write_index, memory_order_acquire);
  uint64_t r = atomic_load_explicit(&ring->read_index, memory_order_relaxed);
  return (w >= r) ? (size_t)(w - r) : 0;
}

size_t spsc_planar_ring_buffer_get_available_to_write(
    const spsc_planar_ring_buffer_t *ring) {
  if (!ring)
    return 0;
  uint64_t r = atomic_load_explicit(&ring->read_index, memory_order_acquire);
  uint64_t w = atomic_load_explicit(&ring->write_index, memory_order_relaxed);
  size_t used = (w >= r) ? (size_t)(w - r) : 0;
  return (ring->capacity > used) ? (ring->capacity - used) : 0;
}

size_t
spsc_planar_ring_buffer_get_capacity(const spsc_planar_ring_buffer_t *ring) {
  return ring ? ring->capacity : 0;
}

size_t
spsc_planar_ring_buffer_get_channels(const spsc_planar_ring_buffer_t *ring) {
  return ring ? ring->channels : 0;
}

size_t spsc_planar_ring_buffer_get_bytes_per_sample(
    const spsc_planar_ring_buffer_t *ring) {
  return ring ? ring->bytes_per_sample : 0;
}

size_t
spsc_planar_ring_buffer_get_read_slices(const spsc_planar_ring_buffer_t *ring,
                                        size_t max_frames,
                                        const uint8_t **slice1, size_t *len1,
                                        const uint8_t **slice2, size_t *len2) {
  if (len1)
    *len1 = 0;
  if (len2)
    *len2 = 0;
  if (!ring || max_frames == 0)
    return 0;

  size_t avail = spsc_planar_ring_buffer_get_available_to_read(ring);
  size_t to_read = (max_frames < avail) ? max_frames : avail;
  if (to_read == 0)
    return 0;

  uint64_t r = atomic_load_explicit(&ring->read_index, memory_order_relaxed);
  size_t offset = (size_t)(r & ring->mask);
  size_t first_chunk = ring->capacity - offset;
  size_t bps = ring->bytes_per_sample;

  if (to_read <= first_chunk) {
    if (len1)
      *len1 = to_read;
    if (slice1) {
      for (size_t ch = 0; ch < ring->channels; ch++) {
        slice1[ch] = ring->channel_storage[ch] + offset * bps;
      }
    }
    if (slice2) {
      for (size_t ch = 0; ch < ring->channels; ch++) {
        slice2[ch] = NULL;
      }
    }
  } else {
    if (len1)
      *len1 = first_chunk;
    if (len2)
      *len2 = to_read - first_chunk;
    if (slice1) {
      for (size_t ch = 0; ch < ring->channels; ch++) {
        slice1[ch] = ring->channel_storage[ch] + offset * bps;
      }
    }
    if (slice2) {
      for (size_t ch = 0; ch < ring->channels; ch++) {
        slice2[ch] = ring->channel_storage[ch];
      }
    }
  }
  return to_read;
}

void spsc_planar_ring_buffer_advance_read(spsc_planar_ring_buffer_t *ring,
                                          size_t frames) {
  if (!ring || frames == 0)
    return;
  uint64_t r = atomic_load_explicit(&ring->read_index, memory_order_relaxed);
  atomic_store_explicit(&ring->read_index, r + frames, memory_order_release);
}

size_t spsc_planar_ring_buffer_get_write_slices(
    const spsc_planar_ring_buffer_t *ring, size_t max_frames, uint8_t **slice1,
    size_t *len1, uint8_t **slice2, size_t *len2) {
  if (len1)
    *len1 = 0;
  if (len2)
    *len2 = 0;
  if (!ring || max_frames == 0)
    return 0;

  size_t free_space = spsc_planar_ring_buffer_get_available_to_write(ring);
  size_t to_write = (max_frames < free_space) ? max_frames : free_space;
  if (to_write == 0)
    return 0;

  uint64_t w = atomic_load_explicit(&ring->write_index, memory_order_relaxed);
  size_t offset = (size_t)(w & ring->mask);
  size_t first_chunk = ring->capacity - offset;
  size_t bps = ring->bytes_per_sample;

  if (to_write <= first_chunk) {
    if (len1)
      *len1 = to_write;
    if (slice1) {
      for (size_t ch = 0; ch < ring->channels; ch++) {
        slice1[ch] = ring->channel_storage[ch] + offset * bps;
      }
    }
    if (slice2) {
      for (size_t ch = 0; ch < ring->channels; ch++) {
        slice2[ch] = NULL;
      }
    }
  } else {
    if (len1)
      *len1 = first_chunk;
    if (len2)
      *len2 = to_write - first_chunk;
    if (slice1) {
      for (size_t ch = 0; ch < ring->channels; ch++) {
        slice1[ch] = ring->channel_storage[ch] + offset * bps;
      }
    }
    if (slice2) {
      for (size_t ch = 0; ch < ring->channels; ch++) {
        slice2[ch] = ring->channel_storage[ch];
      }
    }
  }
  return to_write;
}

void spsc_planar_ring_buffer_advance_write(spsc_planar_ring_buffer_t *ring,
                                           size_t frames) {
  if (!ring || frames == 0)
    return;
  uint64_t w = atomic_load_explicit(&ring->write_index, memory_order_relaxed);
  atomic_store_explicit(&ring->write_index, w + frames, memory_order_release);
}

size_t spsc_planar_ring_buffer_write_channels(spsc_planar_ring_buffer_t *ring,
                                              const void *const *channel_ptrs,
                                              size_t frames) {
  if (!ring || !channel_ptrs || frames == 0)
    return 0;

  size_t free_space = spsc_planar_ring_buffer_get_available_to_write(ring);
  size_t to_write = (frames < free_space) ? frames : free_space;
  if (to_write == 0)
    return 0;

  uint64_t w = atomic_load_explicit(&ring->write_index, memory_order_relaxed);
  size_t offset = (size_t)(w & ring->mask);
  size_t first_chunk = ring->capacity - offset;
  size_t bps = ring->bytes_per_sample;

  size_t l1 = (to_write <= first_chunk) ? to_write : first_chunk;
  size_t l2 = to_write - l1;

  for (size_t ch = 0; ch < ring->channels; ch++) {
    const uint8_t *src = (const uint8_t *)channel_ptrs[ch];
    if (src) {
      uint8_t *dst = ring->channel_storage[ch];
      memcpy(dst + offset * bps, src, l1 * bps);
      if (l2 > 0) {
        memcpy(dst, src + l1 * bps, l2 * bps);
      }
    }
  }

  atomic_store_explicit(&ring->write_index, w + to_write, memory_order_release);
  return to_write;
}

size_t spsc_planar_ring_buffer_read_channels(spsc_planar_ring_buffer_t *ring,
                                             void *const *channel_ptrs,
                                             size_t frames) {
  if (!ring || !channel_ptrs || frames == 0)
    return 0;

  size_t avail = spsc_planar_ring_buffer_get_available_to_read(ring);
  size_t to_read = (frames < avail) ? frames : avail;
  if (to_read == 0)
    return 0;

  uint64_t r = atomic_load_explicit(&ring->read_index, memory_order_relaxed);
  size_t offset = (size_t)(r & ring->mask);
  size_t first_chunk = ring->capacity - offset;
  size_t bps = ring->bytes_per_sample;

  size_t l1 = (to_read <= first_chunk) ? to_read : first_chunk;
  size_t l2 = to_read - l1;

  for (size_t ch = 0; ch < ring->channels; ch++) {
    uint8_t *dst = (uint8_t *)channel_ptrs[ch];
    if (dst) {
      const uint8_t *src = ring->channel_storage[ch];
      memcpy(dst, src + offset * bps, l1 * bps);
      if (l2 > 0) {
        memcpy(dst + l1 * bps, src, l2 * bps);
      }
    }
  }

  atomic_store_explicit(&ring->read_index, r + to_read, memory_order_release);
  return to_read;
}

size_t spsc_planar_ring_buffer_read_with_silence(
    spsc_planar_ring_buffer_t *ring, void *const *dst_channels, size_t frames,
    uint8_t silence_byte, _Atomic size_t *silence_frames,
    _Atomic bool *is_running) {
  if (!ring || !dst_channels || frames == 0)
    return 0;

  size_t silence = 0;
  if (silence_frames) {
    size_t pending = atomic_load_explicit(silence_frames, memory_order_relaxed);
    silence = (pending < frames) ? pending : frames;
    if (silence > 0) {
      atomic_fetch_sub_explicit(silence_frames, silence, memory_order_relaxed);
    }
  }

  size_t bps = ring->bytes_per_sample;
  size_t channels = ring->channels;

  // 1. Fill silence prefix
  if (silence > 0) {
    for (size_t ch = 0; ch < channels; ch++) {
      if (dst_channels[ch]) {
        memset(dst_channels[ch], silence_byte, silence * bps);
      }
    }
  }

  // 2. Read audio frames from ring buffer into remaining space
  size_t audio_needed = frames - silence;
  size_t copied = 0;
  if (audio_needed > 0) {
    size_t avail = spsc_planar_ring_buffer_get_available_to_read(ring);
    copied = (audio_needed < avail) ? audio_needed : avail;

    if (copied > 0) {
      uint64_t r =
          atomic_load_explicit(&ring->read_index, memory_order_relaxed);
      size_t offset = (size_t)(r & ring->mask);
      size_t first_chunk = ring->capacity - offset;
      size_t cl1 = (copied <= first_chunk) ? copied : first_chunk;
      size_t cl2 = copied - cl1;

      for (size_t ch = 0; ch < channels; ch++) {
        uint8_t *dst = (uint8_t *)dst_channels[ch] + silence * bps;
        const uint8_t *src = ring->channel_storage[ch];
        if (dst && src) {
          if (cl1 > 0) {
            memcpy(dst, src + offset * bps, cl1 * bps);
          }
          if (cl2 > 0) {
            memcpy(dst + cl1 * bps, src, cl2 * bps);
          }
        }
      }
      atomic_store_explicit(&ring->read_index, r + copied,
                            memory_order_release);
    }

    // 3. If underrun, fill missing remainder with silence_byte
    if (copied < audio_needed) {
      size_t missing = audio_needed - copied;
      for (size_t ch = 0; ch < channels; ch++) {
        if (dst_channels[ch]) {
          memset((uint8_t *)dst_channels[ch] + (silence + copied) * bps,
                 silence_byte, missing * bps);
        }
      }
      if (is_running) {
        atomic_store_explicit(is_running, false, memory_order_relaxed);
      }
    }
  }

  return copied;
}

size_t
spsc_planar_ring_buffer_get_read_indices(const spsc_planar_ring_buffer_t *ring,
                                         size_t max_frames, size_t *offset,
                                         size_t *len1, size_t *len2) {
  if (offset)
    *offset = 0;
  if (len1)
    *len1 = 0;
  if (len2)
    *len2 = 0;
  if (!ring || max_frames == 0)
    return 0;

  size_t avail = spsc_planar_ring_buffer_get_available_to_read(ring);
  size_t to_read = (max_frames < avail) ? max_frames : avail;
  if (to_read == 0)
    return 0;

  uint64_t r = atomic_load_explicit(&ring->read_index, memory_order_relaxed);
  size_t off = (size_t)(r & ring->mask);
  size_t first_chunk = ring->capacity - off;

  if (offset)
    *offset = off;
  if (to_read <= first_chunk) {
    if (len1)
      *len1 = to_read;
  } else {
    if (len1)
      *len1 = first_chunk;
    if (len2)
      *len2 = to_read - first_chunk;
  }
  return to_read;
}

size_t
spsc_planar_ring_buffer_get_write_indices(const spsc_planar_ring_buffer_t *ring,
                                          size_t max_frames, size_t *offset,
                                          size_t *len1, size_t *len2) {
  if (offset)
    *offset = 0;
  if (len1)
    *len1 = 0;
  if (len2)
    *len2 = 0;
  if (!ring || max_frames == 0)
    return 0;

  size_t free_space = spsc_planar_ring_buffer_get_available_to_write(ring);
  size_t to_write = (max_frames < free_space) ? max_frames : free_space;
  if (to_write == 0)
    return 0;

  uint64_t w = atomic_load_explicit(&ring->write_index, memory_order_relaxed);
  size_t off = (size_t)(w & ring->mask);
  size_t first_chunk = ring->capacity - off;

  if (offset)
    *offset = off;
  if (to_write <= first_chunk) {
    if (len1)
      *len1 = to_write;
  } else {
    if (len1)
      *len1 = first_chunk;
    if (len2)
      *len2 = to_write - first_chunk;
  }
  return to_write;
}

uint8_t *
spsc_planar_ring_buffer_get_channel_ptr(const spsc_planar_ring_buffer_t *ring,
                                        size_t channel) {
  if (!ring || channel >= ring->channels || !ring->channel_storage)
    return NULL;
  return ring->channel_storage[channel];
}

size_t spsc_planar_ring_buffer_write_silence(spsc_planar_ring_buffer_t *ring,
                                             size_t frames) {
  if (!ring || frames == 0)
    return 0;

  size_t offset = 0, l1 = 0, l2 = 0;
  size_t to_write = spsc_planar_ring_buffer_get_write_indices(
      ring, frames, &offset, &l1, &l2);
  if (to_write == 0)
    return 0;

  size_t bps = ring->bytes_per_sample;
  for (size_t ch = 0; ch < ring->channels; ch++) {
    uint8_t *dst = ring->channel_storage[ch];
    if (dst) {
      if (l1 > 0) {
        memset(dst + offset * bps, 0, l1 * bps);
      }
      if (l2 > 0) {
        memset(dst, 0, l2 * bps);
      }
    }
  }

  uint64_t w = atomic_load_explicit(&ring->write_index, memory_order_relaxed);
  atomic_store_explicit(&ring->write_index, w + to_write, memory_order_release);
  return to_write;
}

void spsc_planar_ring_buffer_drain(spsc_planar_ring_buffer_t *ring) {
  if (!ring)
    return;
  uint64_t w = atomic_load_explicit(&ring->write_index, memory_order_acquire);
  atomic_store_explicit(&ring->read_index, w, memory_order_release);
}
