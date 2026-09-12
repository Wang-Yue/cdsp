/**
 * @file lock_free_ring_buffer.h
 * @brief Single-producer / single-consumer lock-free primitives used by the
 * audio thread.
 *
 * Contains:
 * - @ref spsc_audio_ring_buffer_t: Ring buffer of `float` samples.
 * - @ref spsc_queue_t: Generic SPSC FIFO queue for pointers.
 * - @ref atomic_double_t: Wait-free atomic `double`.
 *
 * Real-time discipline:
 * All hot-path methods are wait-free, allocation-free, and free of
 * runtime calls or syscalls that could block. The producer always succeeds
 * — if the consumer is so far behind that the buffer is full, the
 * oldest unread data is silently overwritten (matching the original
 * lock-based design's drop-on-overflow behaviour).
 */

#ifndef CLIB_UTILS_LOCK_FREE_RING_BUFFER_H
#define CLIB_UTILS_LOCK_FREE_RING_BUFFER_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// MARK: - Power of Two Helper

/**
 * @brief Round up a size to the next power of two.
 *
 * @param n Size to round.
 * @return Rounded size.
 */
static inline size_t spsc_round_up_to_power_of_two(size_t n) {
  if (n == 0) return 1;
  if (n > ((size_t)1 << (sizeof(size_t) * 8 - 1))) {
    return (size_t)1 << (sizeof(size_t) * 8 - 1);  // Cap at max power of two
  }
  n--;
  n |= n >> 1;
  n |= n >> 2;
  n |= n >> 4;
  n |= n >> 8;
  n |= n >> 16;
#if UINTPTR_MAX == 0xffffffffffffffff
  n |= n >> 32;
#endif
  return n + 1;
}

// MARK: - SPSCQueue

/**
 * @struct spsc_queue
 * @brief Lock-free single-producer / single-consumer FIFO queue of pointers.
 *
 * Used to pass audio chunk values between the capture, processing, and playback
 * threads inside the DSP engine without taking mutexes or locks.
 */
typedef struct spsc_queue spsc_queue_t;

/**
 * @brief Create a new SPSC queue.
 *
 * @param minimum_capacity Minimum requested capacity.
 * @return Pointer to the queue, or NULL on failure.
 */
spsc_queue_t* spsc_queue_create(size_t minimum_capacity);

/**
 * @brief Free the SPSC queue.
 *
 * @param queue Pointer to the queue to free.
 */
void spsc_queue_free(spsc_queue_t* queue);

/**
 * @brief Get the approximate number of queued items.
 *
 * Approximate when read from a thread that is neither producer nor consumer.
 *
 * @param queue Pointer to the queue.
 * @return Number of items in the queue.
 */
size_t spsc_queue_get_count(const spsc_queue_t* queue);

/**
 * @brief Get the capacity of the queue.
 *
 * @param queue Pointer to the queue.
 * @return The capacity.
 */
size_t spsc_queue_get_capacity(const spsc_queue_t* queue);

/**
 * @brief Enqueue a value.
 *
 * **Producer-only.** Append `value`; returns `false` (without storing it)
 * when the queue is at capacity.
 *
 * @param queue Pointer to the queue.
 * @param value The pointer to enqueue.
 * @return true if successful, false if queue is full.
 */
bool spsc_queue_enqueue(spsc_queue_t* queue, void* value);

/**
 * @brief Dequeue a value.
 *
 * **Consumer-only.** Pop the next item; returns NULL when empty.
 *
 * @param queue Pointer to the queue.
 * @return The dequeued pointer, or NULL if empty.
 */
void* spsc_queue_dequeue(spsc_queue_t* queue);

/**
 * @brief Discard all queued items.
 *
 * **Consumer-only.**
 *
 * @param queue Pointer to the queue.
 */
void spsc_queue_drain(spsc_queue_t* queue);

// MARK: - SPSCByteRingBuffer

/**
 * @struct spsc_byte_ring_buffer
 * @brief Lock-free single-producer / single-consumer byte ring buffer.
 *
 * Used for streaming raw audio bytes (PCM/Float/DSD) between real-time audio
 * callbacks and worker threads.
 */
typedef struct spsc_byte_ring_buffer spsc_byte_ring_buffer_t;

/**
 * @brief Create a new SPSC byte ring buffer.
 *
 * @param minimum_capacity The minimum requested capacity in bytes. Will be
 *                         rounded up to the next power of two.
 * @return Pointer to allocated buffer, or NULL on failure.
 */
spsc_byte_ring_buffer_t* spsc_byte_ring_buffer_create(size_t minimum_capacity);

/**
 * @brief Free the SPSC byte ring buffer.
 *
 * @param ring Pointer to the ring buffer to free.
 */
void spsc_byte_ring_buffer_free(spsc_byte_ring_buffer_t* ring);

/**
 * @brief Number of bytes currently available to be read / consumed.
 *
 * @param ring Pointer to the ring buffer.
 * @return Number of available bytes.
 */
size_t spsc_byte_ring_buffer_get_available_to_read(
    const spsc_byte_ring_buffer_t* ring);

/**
 * @brief Number of bytes that can be written without overwriting unread data.
 *
 * @param ring Pointer to the ring buffer.
 * @return Number of free bytes available to write.
 */
size_t spsc_byte_ring_buffer_get_available_to_write(
    const spsc_byte_ring_buffer_t* ring);

/**
 * @brief Get total capacity in bytes.
 *
 * @param ring Pointer to the ring buffer.
 * @return Total byte capacity.
 */
size_t spsc_byte_ring_buffer_get_capacity(const spsc_byte_ring_buffer_t* ring);

/**
 * @brief Write bytes into the ring buffer.
 *
 * **Producer-only.** Write up to `count` bytes from `source`.
 *
 * @param ring Pointer to the ring buffer.
 * @param source Pointer to source byte array.
 * @param count Number of bytes to write.
 * @return Number of bytes actually written.
 */
size_t spsc_byte_ring_buffer_write(spsc_byte_ring_buffer_t* ring,
                                   const uint8_t* source, size_t count);

/**
 * @brief Consume bytes from the ring buffer.
 *
 * **Consumer-only.** Copy up to `count` bytes into `dest` and advance read
 * cursor.
 *
 * @param ring Pointer to the ring buffer.
 * @param dest Destination byte array.
 * @param count Maximum number of bytes to consume.
 * @return Number of bytes actually consumed.
 */
size_t spsc_byte_ring_buffer_consume(spsc_byte_ring_buffer_t* ring,
                                     uint8_t* dest, size_t count);

/**
 * @brief Discard all pending bytes.
 *
 * **Consumer-only.**
 *
 * @param ring Pointer to the ring buffer.
 */
void spsc_byte_ring_buffer_drain(spsc_byte_ring_buffer_t* ring);

// MARK: - AtomicDouble

/**
 * @struct atomic_double
 * @brief Lock-free atomic `double`.
 *
 * Standard C atomic types don't directly support atomic operations on
 * floating-point types on all targets without locking, so we round-trip through
 * the IEEE-754 bit pattern via `_Atomic uint64_t`.
 */
typedef struct {
  _Atomic(uint64_t) bits; /**< Atomic bits storing the double representation. */
} atomic_double_t;

/**
 * @brief Initialize an atomic double.
 *
 * @param a Pointer to the atomic double.
 * @param value Initial value.
 */
static inline void atomic_double_init(atomic_double_t* a, double value) {
  uint64_t u;
  memcpy(&u, &value, sizeof(uint64_t));
  atomic_init(&a->bits, u);
}

/**
 * @brief Load an atomic double with explicit memory order.
 *
 * @param a Pointer to the atomic double.
 * @param order Memory order.
 * @return Loaded value.
 */
static inline double atomic_double_load(const atomic_double_t* a,
                                        memory_order order) {
  uint64_t u = atomic_load_explicit(&a->bits, order);
  double d;
  memcpy(&d, &u, sizeof(double));
  return d;
}

/**
 * @brief Store an atomic double with explicit memory order.
 *
 * @param a Pointer to the atomic double.
 * @param value Value to store.
 * @param order Memory order.
 */
static inline void atomic_double_store(atomic_double_t* a, double value,
                                       memory_order order) {
  uint64_t u;
  memcpy(&u, &value, sizeof(uint64_t));
  atomic_store_explicit(&a->bits, u, order);
}

/**
 * @brief Load an atomic double with acquire memory order.
 *
 * @param a Pointer to the atomic double.
 * @return Loaded value.
 */
static inline double atomic_double_get(const atomic_double_t* a) {
  return atomic_double_load(a, memory_order_acquire);
}

/**
 * @brief Store an atomic double with release memory order.
 *
 * @param a Pointer to the atomic double.
 * @param value Value to store.
 */
static inline void atomic_double_set(atomic_double_t* a, double value) {
  atomic_double_store(a, value, memory_order_release);
}

// MARK: - AtomicFloat

/**
 * @struct atomic_float
 * @brief Lock-free atomic `float`.
 *
 * Standard C atomic types round-trip through the IEEE-754 bit pattern via
 * `_Atomic uint32_t`.
 */
typedef struct {
  _Atomic(uint32_t) bits; /**< Atomic bits storing the float representation. */
} atomic_float_t;

/**
 * @brief Initialize an atomic float.
 *
 * @param a Pointer to the atomic float.
 * @param value Initial value.
 */
static inline void atomic_float_init(atomic_float_t* a, float value) {
  uint32_t u;
  memcpy(&u, &value, sizeof(uint32_t));
  atomic_init(&a->bits, u);
}

/**
 * @brief Load an atomic float with explicit memory order.
 *
 * @param a Pointer to the atomic float.
 * @param order Memory order.
 * @return Loaded value.
 */
static inline float atomic_float_load(const atomic_float_t* a,
                                      memory_order order) {
  uint32_t u = atomic_load_explicit(&a->bits, order);
  float f;
  memcpy(&f, &u, sizeof(float));
  return f;
}

/**
 * @brief Store an atomic float with explicit memory order.
 *
 * @param a Pointer to the atomic float.
 * @param value Value to store.
 * @param order Memory order.
 */
static inline void atomic_float_store(atomic_float_t* a, float value,
                                      memory_order order) {
  uint32_t u;
  memcpy(&u, &value, sizeof(uint32_t));
  atomic_store_explicit(&a->bits, u, order);
}

/**
 * @brief Load an atomic float with acquire memory order.
 *
 * @param a Pointer to the atomic float.
 * @return Loaded value.
 */
static inline float atomic_float_get(const atomic_float_t* a) {
  return atomic_float_load(a, memory_order_acquire);
}

/**
 * @brief Store an atomic float with release memory order.
 *
 * @param a Pointer to the atomic float.
 * @param value Value to store.
 */
static inline void atomic_float_set(atomic_float_t* a, float value) {
  atomic_float_store(a, value, memory_order_release);
}

#endif  // CLIB_UTILS_LOCK_FREE_RING_BUFFER_H
