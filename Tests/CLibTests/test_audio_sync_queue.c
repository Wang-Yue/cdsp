#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Engine/audio_sync_queue.h"
#include "test_support.h"

TEST(AudioSyncQueueBasicEnqueueDequeue) {
  audio_sync_queue_t* queue = audio_sync_queue_create(16);
  ASSERT_TRUE(queue != NULL);

  spsc_queue_t* spsc = audio_sync_queue_get_spsc_queue(queue);
  ASSERT_TRUE(spsc != NULL);

  int val1 = 42;
  int val2 = 99;

  bool ok = audio_sync_queue_enqueue(queue, &val1);
  ASSERT_TRUE(ok);

  ok = audio_sync_queue_enqueue(queue, &val2);
  ASSERT_TRUE(ok);

  int* res1 = (int*)audio_sync_queue_dequeue_blocking(queue);
  ASSERT_TRUE(res1 != NULL);
  ASSERT_EQ(*res1, 42);

  int* res2 = (int*)audio_sync_queue_dequeue_blocking(queue);
  ASSERT_TRUE(res2 != NULL);
  ASSERT_EQ(*res2, 99);

  audio_sync_queue_free(queue);
}

TEST(AudioSyncQueueShutdownWakeup) {
  audio_sync_queue_t* queue = audio_sync_queue_create(8);
  ASSERT_TRUE(queue != NULL);

  audio_sync_queue_shutdown(queue);

  void* item = audio_sync_queue_dequeue_blocking(queue);
  ASSERT_TRUE(item == NULL);

  audio_sync_queue_free(queue);
}

TEST(AudioSyncQueueCapacityLimits) {
  audio_sync_queue_t* queue = audio_sync_queue_create(2);
  ASSERT_TRUE(queue != NULL);

  int dummy = 1;
  int items_added = 0;
  for (int i = 0; i < 16; i++) {
    if (audio_sync_queue_enqueue(queue, &dummy)) {
      items_added++;
    }
  }

  ASSERT_TRUE(items_added > 0);
  ASSERT_TRUE(items_added <= 4);

  audio_sync_queue_free(queue);
}

TEST_MAIN()
