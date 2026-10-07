// Regression tests for the shared backend layer audit (07_common.md).

#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "audio/audio_chunk.h"
#include "audio/processing_parameters.h"
#include "backend/audio_backend_registry.h"
#include "backend/backend_buffer.h"
#include "backend/backend_error.h"
#include "logging/app_logger.h"
#include "test_support.h"
#include "utils/cdsp_time.h"

// F1: planar prefill must honour the silence byte (DSD idle pattern 0x69).
TEST(AuditCommon_F1_PlanarPrefillUsesSilenceByte) {
  const size_t channels = 2;
  const size_t frames = 32;
  backend_buffer_t *bb = backend_buffer_create(
      256, BINARY_SAMPLE_FORMAT_DSD_U32_BE, channels, 88200.0, true, NULL);
  ASSERT_TRUE(bb != NULL);

  backend_buffer_prefill_silence(bb, frames, 0x69);
  ASSERT_EQ(backend_buffer_get_available_read_frames(bb), frames);

  uint8_t ch0[32 * 4];
  uint8_t ch1[32 * 4];
  memset(ch0, 0xAA, sizeof(ch0));
  memset(ch1, 0xAA, sizeof(ch1));
  void *dst[2] = {ch0, ch1};
  size_t consumed = backend_buffer_render(bb, dst, frames, 0x69);
  ASSERT_EQ(consumed, frames);
  for (size_t i = 0; i < sizeof(ch0); i++) {
    ASSERT_EQ(ch0[i], 0x69);
    ASSERT_EQ(ch1[i], 0x69);
  }
  backend_buffer_free(bb);
}

// F1: PCM planar prefill still writes zeros.
TEST(AuditCommon_F1_PlanarPrefillPcmZero) {
  backend_buffer_t *bb = backend_buffer_create(64, BINARY_SAMPLE_FORMAT_S32_LE,
                                               1, 48000.0, true, NULL);
  ASSERT_TRUE(bb != NULL);
  backend_buffer_prefill_silence(bb, 8, 0x00);
  uint8_t ch0[8 * 4];
  memset(ch0, 0xAA, sizeof(ch0));
  void *dst[1] = {ch0};
  ASSERT_EQ(backend_buffer_render(bb, dst, 8, 0x00), 8);
  for (size_t i = 0; i < sizeof(ch0); i++) {
    ASSERT_EQ(ch0[i], 0x00);
  }
  backend_buffer_free(bb);
}

// F2: underrun / overflow are logged once per episode, not per callback.
static int g_f2_warns;
static int g_f2_interrupted;
static int g_f2_restarted;
static int g_f2_capture_full;
static void f2_log_cb(log_level_t level, const char *label, const char *message,
                      void *user_data) {
  (void)user_data;
  if (!strstr(label, "dsp.backend.buffer"))
    return;
  if (level == LOG_LEVEL_WARN)
    g_f2_warns++;
  if (strstr(message, "Playback interrupted"))
    g_f2_interrupted++;
  if (strstr(message, "Restarting playback"))
    g_f2_restarted++;
  if (strstr(message, "Capture ring buffer is full, dropping samples"))
    g_f2_capture_full++;
}

TEST(AuditCommon_F2_UnderrunLoggedOncePerEpisode) {
  g_f2_warns = g_f2_interrupted = g_f2_restarted = g_f2_capture_full = 0;
  app_logger_set_level(LOG_LEVEL_INFO);
  app_logger_set_callback(f2_log_cb, NULL);

  backend_buffer_t *bb = backend_buffer_create(256, BINARY_SAMPLE_FORMAT_F32_LE,
                                               1, 48000.0, false, NULL);
  ASSERT_TRUE(bb != NULL);
  backend_buffer_set_state(bb, BACKEND_STREAM_RUNNING);
  backend_buffer_prefill_silence(bb, 4, 0x00);

  float out[16];
  ASSERT_EQ(backend_buffer_render(bb, out, 4, 0x00), 4); // plays the cushion
  for (int i = 0; i < 20; i++) {
    backend_buffer_render(bb, out, 4, 0x00); // starved
  }

  audio_chunk_t *chunk = audio_chunk_create(8, 1);
  audio_chunk_set_valid_frames(chunk, 8);
  backend_error_t err;
  backend_error_init(&err, BACKEND_ERROR_NONE, "");
  ASSERT_TRUE(backend_buffer_write_chunk(bb, chunk, 1, 1, &err));
  backend_buffer_render(bb, out, 4, 0x00); // re-arms the cushion

  // Capture overflow: 300 frames pushed into a 256-frame ring, repeatedly.
  backend_buffer_t *cap = backend_buffer_create(
      256, BINARY_SAMPLE_FORMAT_F32_LE, 1, 48000.0, false, NULL);
  ASSERT_TRUE(cap != NULL);
  float src[300] = {0};
  for (int i = 0; i < 10; i++) {
    backend_buffer_push(cap, src, 300);
  }

  app_logger_flush_and_stop(app_logger_get_shared());
  app_logger_set_callback(NULL, NULL);

  ASSERT_EQ(g_f2_interrupted, 1);
  ASSERT_EQ(g_f2_restarted, 1);
  ASSERT_EQ(g_f2_capture_full, 1);
  ASSERT_EQ(g_f2_warns, 2);

  audio_chunk_free(chunk);
  backend_buffer_free(bb);
  backend_buffer_free(cap);
}

// F3: stale semaphore posts must not use up the writer's backoff budget.
static void *f3_consumer(void *arg) {
  backend_buffer_t *bb = (backend_buffer_t *)arg;
  cdsp_sleep_ms(3);
  float out[64];
  backend_buffer_render(bb, out, 64, 0x00);
  return NULL;
}

TEST(AuditCommon_F3_StaleSignalsDoNotForceDrop) {
  // Capacity 64 frames of mono F32 = 256 bytes (power of two, no rounding).
  backend_buffer_t *bb = backend_buffer_create(64, BINARY_SAMPLE_FORMAT_F32_LE,
                                               1, 48000.0, false, NULL);
  ASSERT_TRUE(bb != NULL);
  backend_buffer_set_state(bb, BACKEND_STREAM_RUNNING);
  backend_buffer_prefill_silence(bb, 64, 0x00); // ring full
  ASSERT_EQ(backend_buffer_get_available_write_frames(bb), 0);

  for (int i = 0; i < 200; i++) {
    backend_buffer_signal(bb); // stale posts, as from earlier renders
  }

  pthread_t th;
  ASSERT_EQ(pthread_create(&th, NULL, f3_consumer, bb), 0);

  audio_chunk_t *chunk = audio_chunk_create(32, 1);
  audio_chunk_set_valid_frames(chunk, 32);
  backend_error_t err;
  backend_error_init(&err, BACKEND_ERROR_NONE, "");
  // Budget 8 x 200 ms (scaled down in tests, still well above 3 ms).
  ASSERT_TRUE(backend_buffer_write_chunk(bb, chunk, 200, 8, &err));
  pthread_join(th, NULL);

  // The chunk must have been written, not dropped.
  ASSERT_EQ(backend_buffer_get_available_read_frames(bb), 32);

  audio_chunk_free(chunk);
  backend_buffer_free(bb);
}

// F5: the registry fills the caller's array bounded by max_devices only, and
// counting (NULL array) reports the same total.
TEST(AuditCommon_F5_RegistryNotCappedAt32) {
#if defined(ENABLE_COREAUDIO)
  const char *backend = "coreaudio";
#elif defined(ENABLE_ALSA)
  const char *backend = "alsa";
#else
  const char *backend = NULL;
#endif
  if (!backend)
    return;
  audio_device_t *devs = calloc(1024, sizeof(audio_device_t));
  ASSERT_TRUE(devs != NULL);
  int n =
      audio_backend_registry_get_available_devices(backend, false, devs, 1024);
  int counted =
      audio_backend_registry_get_available_devices(backend, false, NULL, 0);
  ASSERT_TRUE(n >= 0);
  ASSERT_EQ(n, counted);
  for (int i = 0; i < n; i++) {
    ASSERT_TRUE(memchr(devs[i].name, '\0', sizeof(devs[i].name)) != NULL);
  }
  if (n > 1) {
    int one =
        audio_backend_registry_get_available_devices(backend, false, devs, 1);
    ASSERT_EQ(one, 1);
  }
  ASSERT_EQ(
      audio_backend_registry_get_available_devices(backend, false, devs, 0), 0);
  free(devs);
}

// F6: clip statistics count only encoded channels, count NaN, and ignore
// chunks that are not written.
TEST(AuditCommon_F6_ClipCountingMatchesOutput) {
  processing_parameters_t *params = processing_parameters_create(2, 2);
  ASSERT_TRUE(params != NULL);
  backend_buffer_t *bb = backend_buffer_create(256, BINARY_SAMPLE_FORMAT_S16_LE,
                                               1, 48000.0, false, params);
  ASSERT_TRUE(bb != NULL);
  backend_buffer_set_state(bb, BACKEND_STREAM_RUNNING);

  audio_chunk_t *chunk = audio_chunk_create(4, 2);
  double *c0 = audio_chunk_get_channel(chunk, 0);
  double *c1 = audio_chunk_get_channel(chunk, 1);
  c0[0] = NAN;
  c0[1] = 0.5;
  c0[2] = 0.0;
  c0[3] = -0.5;
  for (int i = 0; i < 4; i++)
    c1[i] = 5.0; // extra channel, never output
  audio_chunk_set_valid_frames(chunk, 4);

  backend_error_t err;
  backend_error_init(&err, BACKEND_ERROR_NONE, "");
  ASSERT_TRUE(backend_buffer_write_chunk(bb, chunk, 1, 1, &err));
  ASSERT_EQ(processing_parameters_get_clipped_samples(params), 1);

  backend_buffer_set_state(bb, BACKEND_STREAM_PAUSED);
  c0[1] = 2.0;
  ASSERT_TRUE(backend_buffer_write_chunk(bb, chunk, 1, 1, &err));
  ASSERT_EQ(processing_parameters_get_clipped_samples(params), 1);

  audio_chunk_free(chunk);
  backend_buffer_free(bb);
  processing_parameters_free(params);
}

// F18 / 02 AS-09: a NULL channel pointer with pending silence must be skipped.
TEST(AuditCommon_F18_PlanarRenderSkipsNullChannel) {
  backend_buffer_t *bb = backend_buffer_create(64, BINARY_SAMPLE_FORMAT_S32_LE,
                                               2, 48000.0, true, NULL);
  ASSERT_TRUE(bb != NULL);
  backend_buffer_set_state(bb, BACKEND_STREAM_RUNNING);
  backend_buffer_set_target_level(bb, 4);
  // Force an underrun so the next render re-arms a 4-frame cushion.
  uint8_t ch0[16 * 4];
  void *dst[2] = {ch0, NULL};
  backend_buffer_prefill_silence(bb, 4, 0x00);
  backend_buffer_render(bb, dst, 8, 0x00); // drains and underruns
  audio_chunk_t *chunk = audio_chunk_create(8, 2);
  audio_chunk_set_valid_frames(chunk, 8);
  backend_error_t err;
  backend_error_init(&err, BACKEND_ERROR_NONE, "");
  ASSERT_TRUE(backend_buffer_write_chunk(bb, chunk, 1, 1, &err));
  // 4 silence + 4 audio frames; channel 1 is NULL and must not be written.
  ASSERT_EQ(backend_buffer_render(bb, dst, 8, 0x00), 4);
  audio_chunk_free(chunk);
  backend_buffer_free(bb);
}

// ALSA L4 handoff: zero-copy slice API. With S24_3 stereo (6-byte frames)
// in a 64-byte ring, frames straddle the wrap regularly; the slice API must
// expose whole frames only, the documented one-frame push/consume fallback
// must cover the straddle, and the byte stream must come out intact.
TEST(AuditCommon_L4_SliceApiWholeFramesAcrossWrap) {
  backend_buffer_t *bb = backend_buffer_create(
      10, BINARY_SAMPLE_FORMAT_S24_3_LE, 2, 48000.0, false, NULL);
  ASSERT_TRUE(bb != NULL);
  const size_t ba = 6;
  uint8_t next_w = 0, next_r = 0;
  size_t straddles = 0;
  for (int iter = 0; iter < 200; iter++) {
    // Producer: up to 3 frames.
    void *w1, *w2;
    size_t wn1, wn2;
    size_t wn = backend_buffer_get_write_slices(bb, 3, &w1, &wn1, &w2, &wn2);
    ASSERT_EQ(wn, wn1 + wn2);
    if (wn == 0 && backend_buffer_get_available_write_frames(bb) > 0) {
      uint8_t frame[6];
      for (size_t i = 0; i < ba; i++)
        frame[i] = next_w++;
      ASSERT_EQ(backend_buffer_push(bb, frame, 1), (size_t)1);
      straddles++;
    } else {
      for (size_t i = 0; i < wn1 * ba; i++)
        ((uint8_t *)w1)[i] = next_w++;
      for (size_t i = 0; i < wn2 * ba; i++)
        ((uint8_t *)w2)[i] = next_w++;
      backend_buffer_commit_write(bb, wn, 0);
    }
    // Consumer: up to 2 frames.
    const void *r1, *r2;
    size_t rn1, rn2;
    size_t rn = backend_buffer_get_read_slices(bb, 2, &r1, &rn1, &r2, &rn2);
    if (rn == 0 && backend_buffer_get_available_read_frames(bb) > 0) {
      uint8_t frame[6];
      ASSERT_EQ(backend_buffer_consume(bb, frame, 1), (size_t)1);
      for (size_t i = 0; i < ba; i++)
        ASSERT_EQ(frame[i], next_r++);
    } else {
      for (size_t i = 0; i < rn1 * ba; i++)
        ASSERT_EQ(((const uint8_t *)r1)[i], next_r++);
      for (size_t i = 0; i < rn2 * ba; i++)
        ASSERT_EQ(((const uint8_t *)r2)[i], next_r++);
      backend_buffer_commit_read(bb, rn);
    }
  }
  ASSERT_TRUE(straddles > 0);
  backend_buffer_free(bb);
}

TEST_MAIN()
