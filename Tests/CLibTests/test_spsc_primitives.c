#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "Utils/lock_free_ring_buffer.h"
#include "test_support.h"

TEST(CapacityRoundsUpToPowerOfTwo) {
  ASSERT_EQ(1, spsc_round_up_to_power_of_two(1));
  ASSERT_EQ(2, spsc_round_up_to_power_of_two(2));
  ASSERT_EQ(128, spsc_round_up_to_power_of_two(100));
  ASSERT_EQ(1024, spsc_round_up_to_power_of_two(1024));
  ASSERT_EQ(2048, spsc_round_up_to_power_of_two(1025));
}

TEST(SpscByteRingBuffer_CapacityAndAvailable) {
  spsc_byte_ring_buffer_t* ring = spsc_byte_ring_buffer_create(100);
  ASSERT_EQ(128, spsc_byte_ring_buffer_get_capacity(ring));
  ASSERT_EQ(128, spsc_byte_ring_buffer_get_available_to_write(ring));
  ASSERT_EQ(0, spsc_byte_ring_buffer_get_available_to_read(ring));
  spsc_byte_ring_buffer_free(ring);
}

TEST(SpscByteRingBuffer_BasicRoundTrip) {
  spsc_byte_ring_buffer_t* ring = spsc_byte_ring_buffer_create(100);
  uint8_t write_data[64];
  for (int i = 0; i < 64; i++) write_data[i] = (uint8_t)(i & 0xFF);

  size_t written = spsc_byte_ring_buffer_write(ring, write_data, 64);
  ASSERT_EQ(64, written);
  ASSERT_EQ(64, spsc_byte_ring_buffer_get_available_to_read(ring));
  ASSERT_EQ(64, spsc_byte_ring_buffer_get_available_to_write(ring));

  uint8_t read_data[64] = {0};
  size_t consumed = spsc_byte_ring_buffer_consume(ring, read_data, 64);
  ASSERT_EQ(64, consumed);
  for (int i = 0; i < 64; i++) {
    ASSERT_EQ((uint8_t)(i & 0xFF), read_data[i]);
  }
  ASSERT_EQ(0, spsc_byte_ring_buffer_get_available_to_read(ring));
  ASSERT_EQ(128, spsc_byte_ring_buffer_get_available_to_write(ring));

  spsc_byte_ring_buffer_free(ring);
}

TEST(SpscByteRingBuffer_WrapAround) {
  spsc_byte_ring_buffer_t* ring = spsc_byte_ring_buffer_create(8);
  ASSERT_EQ(8, spsc_byte_ring_buffer_get_capacity(ring));

  uint8_t first_batch[] = {1, 2, 3, 4, 5, 6};
  size_t w1 = spsc_byte_ring_buffer_write(ring, first_batch, 6);
  ASSERT_EQ(6, w1);

  uint8_t dest[4] = {0};
  size_t r1 = spsc_byte_ring_buffer_consume(ring, dest, 4);
  ASSERT_EQ(4, r1);
  for (int i = 0; i < 4; i++) {
    ASSERT_EQ((uint8_t)(i + 1), dest[i]);
  }

  uint8_t second_batch[] = {7, 8, 9, 10, 11, 12};
  size_t w2 = spsc_byte_ring_buffer_write(ring, second_batch, 6);
  ASSERT_EQ(6, w2);

  uint8_t dest2[8] = {0};
  size_t r2 = spsc_byte_ring_buffer_consume(ring, dest2, 8);
  ASSERT_EQ(8, r2);
  for (int i = 0; i < 8; i++) {
    ASSERT_EQ((uint8_t)(i + 5), dest2[i]);
  }

  spsc_byte_ring_buffer_free(ring);
}

TEST(SpscByteRingBuffer_Drain) {
  spsc_byte_ring_buffer_t* ring = spsc_byte_ring_buffer_create(8);
  ASSERT_TRUE(ring != NULL);
  uint8_t src[] = {1, 2, 3, 4};
  spsc_byte_ring_buffer_write(ring, src, 4);
  ASSERT_EQ(4, spsc_byte_ring_buffer_get_available_to_read(ring));
  spsc_byte_ring_buffer_drain(ring);
  ASSERT_EQ(0, spsc_byte_ring_buffer_get_available_to_read(ring));
  spsc_byte_ring_buffer_free(ring);
}

typedef struct {
  spsc_byte_ring_buffer_t* ring;
  size_t total_bytes;
} byte_thread_ctx_t;

static void* byte_producer_thread(void* arg) {
  byte_thread_ctx_t* ctx = (byte_thread_ctx_t*)arg;
  size_t total = ctx->total_bytes;
  size_t written = 0;
  uint8_t chunk[128];
  while (written < total) {
    size_t to_write = (total - written < 128) ? total - written : 128;
    for (size_t i = 0; i < to_write; i++) {
      chunk[i] = (uint8_t)((written + i) & 0xFF);
    }
    size_t pushed = spsc_byte_ring_buffer_write(ctx->ring, chunk, to_write);
    written += pushed;
    if (pushed == 0) sched_yield();
  }
  return NULL;
}

TEST(SpscByteRingBuffer_Concurrent) {
  spsc_byte_ring_buffer_t* ring = spsc_byte_ring_buffer_create(256);
  byte_thread_ctx_t ctx = {.ring = ring, .total_bytes = 100000};
  pthread_t th;
  pthread_create(&th, NULL, byte_producer_thread, &ctx);

  size_t consumed = 0;
  uint8_t chunk[128];
  while (consumed < ctx.total_bytes) {
    size_t to_read =
        (ctx.total_bytes - consumed < 128) ? ctx.total_bytes - consumed : 128;
    size_t n = spsc_byte_ring_buffer_consume(ring, chunk, to_read);
    for (size_t i = 0; i < n; i++) {
      ASSERT_EQ((uint8_t)((consumed + i) & 0xFF), chunk[i]);
    }
    consumed += n;
    if (n == 0) sched_yield();
  }

  pthread_join(th, NULL);
  spsc_byte_ring_buffer_free(ring);
}

TEST(SpscQueueRoundTripFifo) {
  spsc_queue_t* queue = spsc_queue_create(8);
  ASSERT_EQ(8, spsc_queue_get_capacity(queue));
  ASSERT_EQ(0, spsc_queue_get_count(queue));
  ASSERT_TRUE(spsc_queue_dequeue(queue) == NULL);
  for (intptr_t i = 1; i <= 5; i++) {
    ASSERT_TRUE(spsc_queue_enqueue(queue, (void*)i));
  }
  ASSERT_EQ(5, spsc_queue_get_count(queue));
  for (intptr_t i = 1; i <= 5; i++) {
    ASSERT_EQ((void*)i, spsc_queue_dequeue(queue));
  }
  ASSERT_EQ(0, spsc_queue_get_count(queue));
  ASSERT_TRUE(spsc_queue_dequeue(queue) == NULL);
  spsc_queue_free(queue);
}

TEST(SpscQueueEnqueueReturnsFalseWhenFull) {
  spsc_queue_t* queue = spsc_queue_create(4);
  for (intptr_t i = 1; i <= 4; i++) {
    ASSERT_TRUE(spsc_queue_enqueue(queue, (void*)i));
  }
  ASSERT_FALSE(spsc_queue_enqueue(queue, (void*)99));
  ASSERT_EQ((void*)1, spsc_queue_dequeue(queue));
  ASSERT_TRUE(spsc_queue_enqueue(queue, (void*)99));
  spsc_queue_free(queue);
}

TEST(SpscQueueWrapsAroundIndices) {
  spsc_queue_t* queue = spsc_queue_create(4);
  for (intptr_t i = 1; i <= 3; i++) {
    ASSERT_TRUE(spsc_queue_enqueue(queue, (void*)i));
  }
  for (intptr_t i = 1; i <= 3; i++) {
    ASSERT_EQ((void*)i, spsc_queue_dequeue(queue));
  }
  for (intptr_t i = 100; i < 104; i++) {
    ASSERT_TRUE(spsc_queue_enqueue(queue, (void*)i));
  }
  for (intptr_t i = 100; i < 104; i++) {
    ASSERT_EQ((void*)i, spsc_queue_dequeue(queue));
  }
  spsc_queue_free(queue);
}

typedef struct {
  int value;
} non_sendable_item_t;

TEST(SpscQueueTransferNonSendable) {
  spsc_queue_t* queue = spsc_queue_create(4);
  non_sendable_item_t item = {42};
  ASSERT_TRUE(spsc_queue_enqueue(queue, &item));
  non_sendable_item_t* popped = (non_sendable_item_t*)spsc_queue_dequeue(queue);
  ASSERT_TRUE(popped != NULL);
  ASSERT_EQ(42, popped->value);
  spsc_queue_free(queue);
}

typedef struct {
  spsc_queue_t* queue;
  int total_to_write;
} queue_concurrent_arg_t;

static void* queue_producer_thread(void* arg) {
  queue_concurrent_arg_t* a = (queue_concurrent_arg_t*)arg;
  int i = 0;
  while (i < a->total_to_write) {
    if (spsc_queue_enqueue(a->queue, (void*)(intptr_t)(i + 1))) {
      i++;
    }
  }
  return NULL;
}

TEST(SpscQueueConcurrentNoDataLoss) {
  spsc_queue_t* queue = spsc_queue_create(64);
  int total_to_write = 200000;
  queue_concurrent_arg_t arg = {queue, total_to_write};
  pthread_t th;
  pthread_create(&th, NULL, queue_producer_thread, &arg);

  int last_seen = 0;
  int consumed = 0;
  while (consumed < total_to_write) {
    void* val = spsc_queue_dequeue(queue);
    if (val) {
      int v = (int)(intptr_t)val;
      ASSERT_EQ(last_seen + 1, v);
      last_seen = v;
      consumed++;
    }
  }
  pthread_join(th, NULL);
  spsc_queue_free(queue);
  ASSERT_EQ(total_to_write, consumed);
}

TEST(AtomicDoubleRoundTrip) {
  atomic_double_t value;
  atomic_double_init(&value, 1.5);
  ASSERT_DOUBLE_EQ(1.5, atomic_double_get(&value));
  atomic_double_set(&value, 2.71828);
  ASSERT_DOUBLE_EQ(2.71828, atomic_double_get(&value));
  atomic_double_set(&value, -0.0);
  double d = atomic_double_get(&value);
  uint64_t u1, u2;
  memcpy(&u1, &d, sizeof(uint64_t));
  double minus_zero = -0.0;
  memcpy(&u2, &minus_zero, sizeof(uint64_t));
  ASSERT_EQ(u2, u1);
  atomic_double_set(&value, INFINITY);
  ASSERT_DOUBLE_EQ(INFINITY, atomic_double_get(&value));
}

TEST(AtomicFloatRoundTrip) {
  atomic_float_t value;
  atomic_float_init(&value, 1.5f);
  ASSERT_NEAR(1.5f, atomic_float_get(&value), 1e-6);
  atomic_float_set(&value, 2.71828f);
  ASSERT_NEAR(2.71828f, atomic_float_get(&value), 1e-6);
  atomic_float_set(&value, -0.0f);
  float f = atomic_float_get(&value);
  uint32_t u1, u2;
  memcpy(&u1, &f, sizeof(uint32_t));
  float minus_zero = -0.0f;
  memcpy(&u2, &minus_zero, sizeof(uint32_t));
  ASSERT_EQ(u2, u1);
  atomic_float_set(&value, INFINITY);
  ASSERT_TRUE(isinf(atomic_float_get(&value)));
}

TEST(SpscNullCheck) {
  ASSERT_EQ(0, spsc_queue_get_count(NULL));
  ASSERT_EQ(0, spsc_byte_ring_buffer_get_available_to_read(NULL));
  ASSERT_EQ(0, spsc_byte_ring_buffer_get_available_to_write(NULL));
  ASSERT_EQ(0, spsc_byte_ring_buffer_get_capacity(NULL));
}

TEST_MAIN()
