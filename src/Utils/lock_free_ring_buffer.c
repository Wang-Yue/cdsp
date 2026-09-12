// Single-producer / single-consumer lock-free primitives used by the audio
// thread.
#include "Utils/lock_free_ring_buffer.h"

#include "Utils/cdsp_memory.h"

struct spsc_queue {
  size_t capacity;
  size_t mask;
  void** storage;
  _Atomic uint64_t write_index __attribute__((aligned(64)));
  _Atomic uint64_t read_index __attribute__((aligned(64)));
};

struct spsc_byte_ring_buffer {
  size_t capacity;
  size_t mask;
  uint8_t* storage;
  _Atomic uint64_t write_index __attribute__((aligned(64)));
  _Atomic uint64_t read_index __attribute__((aligned(64)));
};

size_t spsc_queue_get_count(const spsc_queue_t* queue) {
  if (!queue) return 0;
  uint64_t r = atomic_load_explicit(&queue->read_index, memory_order_acquire);
  uint64_t w = atomic_load_explicit(&queue->write_index, memory_order_acquire);
  if (w < r) return 0;
  size_t count = (size_t)(w - r);
  if (count > queue->capacity) {
    return queue->capacity;
  }
  return count;
}

size_t spsc_queue_get_capacity(const spsc_queue_t* queue) {
  return queue ? queue->capacity : 0;
}

// MARK: - SPSCQueue Implementation

spsc_queue_t* spsc_queue_create(size_t minimum_capacity) {
  size_t cap = spsc_round_up_to_power_of_two(
      minimum_capacity < 2 ? 2 : minimum_capacity);
  spsc_queue_t* queue =
      (spsc_queue_t*)cdsp_aligned_alloc(64, sizeof(spsc_queue_t));
  if (!queue) return NULL;
  memset(queue, 0, sizeof(spsc_queue_t));
  queue->capacity = cap;
  queue->mask = cap - 1;
  queue->storage = (void**)cdsp_aligned_alloc(64, cap * sizeof(void*));
  if (!queue->storage) {
    cdsp_aligned_free(queue);
    return NULL;
  }
  memset(queue->storage, 0, cap * sizeof(void*));
  atomic_init(&queue->write_index, 0);
  atomic_init(&queue->read_index, 0);
  return queue;
}

void spsc_queue_free(spsc_queue_t* queue) {
  if (!queue) return;
  // Clearing each slot to NULL; deinitialize then deallocate the raw storage.
  cdsp_aligned_free(queue->storage);
  cdsp_aligned_free(queue);
}

bool spsc_queue_enqueue(spsc_queue_t* queue, void* value) {
  if (!queue) return false;
  uint64_t w = atomic_load_explicit(&queue->write_index, memory_order_relaxed);
  uint64_t r = atomic_load_explicit(&queue->read_index, memory_order_acquire);
  if (w - r >= (uint64_t)queue->capacity) return false;
  queue->storage[(size_t)(w & queue->mask)] = value;
  atomic_store_explicit(&queue->write_index, w + 1, memory_order_release);
  return true;
}

void* spsc_queue_dequeue(spsc_queue_t* queue) {
  if (!queue) return NULL;
  uint64_t r = atomic_load_explicit(&queue->read_index, memory_order_relaxed);
  uint64_t w = atomic_load_explicit(&queue->write_index, memory_order_acquire);
  if (r == w) return NULL;
  size_t slot = (size_t)(r & queue->mask);
  void* value = queue->storage[slot];
  queue->storage[slot] = NULL;
  atomic_store_explicit(&queue->read_index, r + 1, memory_order_release);
  return value;
}

void spsc_queue_drain(spsc_queue_t* queue) {
  if (!queue) return;
  while (spsc_queue_dequeue(queue) != NULL) {
  }
}

// MARK: - SPSCByteRingBuffer Implementation

spsc_byte_ring_buffer_t* spsc_byte_ring_buffer_create(size_t minimum_capacity) {
  size_t cap = spsc_round_up_to_power_of_two(
      minimum_capacity < 2 ? 2 : minimum_capacity);
  spsc_byte_ring_buffer_t* ring = (spsc_byte_ring_buffer_t*)cdsp_aligned_alloc(
      64, sizeof(spsc_byte_ring_buffer_t));
  if (!ring) return NULL;
  memset(ring, 0, sizeof(spsc_byte_ring_buffer_t));
  ring->capacity = cap;
  ring->mask = cap - 1;
  ring->storage = (uint8_t*)cdsp_aligned_alloc(64, cap);
  if (!ring->storage) {
    cdsp_aligned_free(ring);
    return NULL;
  }
  memset(ring->storage, 0, cap);
  atomic_init(&ring->write_index, 0);
  atomic_init(&ring->read_index, 0);
  return ring;
}

void spsc_byte_ring_buffer_free(spsc_byte_ring_buffer_t* ring) {
  if (!ring) return;
  cdsp_aligned_free(ring->storage);
  cdsp_aligned_free(ring);
}

size_t spsc_byte_ring_buffer_get_available_to_read(
    const spsc_byte_ring_buffer_t* ring) {
  if (!ring) return 0;
  uint64_t r = atomic_load_explicit(&ring->read_index, memory_order_acquire);
  uint64_t w = atomic_load_explicit(&ring->write_index, memory_order_acquire);
  if (w < r) return 0;
  size_t avail = (size_t)(w - r);
  return (avail > ring->capacity) ? ring->capacity : avail;
}

size_t spsc_byte_ring_buffer_get_available_to_write(
    const spsc_byte_ring_buffer_t* ring) {
  if (!ring) return 0;
  uint64_t w = atomic_load_explicit(&ring->write_index, memory_order_relaxed);
  uint64_t r = atomic_load_explicit(&ring->read_index, memory_order_acquire);
  size_t occupied = (size_t)(w - r);
  if (occupied >= ring->capacity) return 0;
  return ring->capacity - occupied;
}

size_t spsc_byte_ring_buffer_get_capacity(const spsc_byte_ring_buffer_t* ring) {
  return ring ? ring->capacity : 0;
}

size_t spsc_byte_ring_buffer_write(spsc_byte_ring_buffer_t* ring,
                                   const uint8_t* source, size_t count) {
  if (!ring || !source || count == 0) return 0;
  size_t free_space = spsc_byte_ring_buffer_get_available_to_write(ring);
  size_t to_write = (count < free_space) ? count : free_space;
  if (to_write == 0) return 0;
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

size_t spsc_byte_ring_buffer_consume(spsc_byte_ring_buffer_t* ring,
                                     uint8_t* dest, size_t count) {
  if (!ring || !dest || count == 0) return 0;
  size_t avail = spsc_byte_ring_buffer_get_available_to_read(ring);
  size_t to_read = (count < avail) ? count : avail;
  if (to_read == 0) return 0;
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

void spsc_byte_ring_buffer_drain(spsc_byte_ring_buffer_t* ring) {
  if (!ring) return;
  uint64_t w = atomic_load_explicit(&ring->write_index, memory_order_acquire);
  atomic_store_explicit(&ring->read_index, w, memory_order_release);
}
