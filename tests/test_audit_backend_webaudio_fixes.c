/**
 * @file test_audit_backend_webaudio_fixes.c
 * @brief Regression tests for the WebAudio backend audit fixes
 * (audit_reports/backends/08_webaudio.md).
 */

#if defined(ENABLE_WEBAUDIO)

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/audio_chunk.h"
#include "backend/audio_backend.h"
#include "backend/backend_error.h"
#include "backend/webaudio_backend.h"
#include "test_support.h"

static playback_backend_t *wa_open_playback(int rate, int chunk,
                                            size_t channels) {
  backend_error_t err;
  backend_error_init(&err, BACKEND_ERROR_NONE, "");
  playback_device_config_t cfg = {0};
  cfg.type = AUDIO_BACKEND_TYPE_WEB_AUDIO;
  cfg.cfg.webaudio.channels = channels;
  playback_backend_t *play =
      create_playback_backend(&cfg, rate, chunk, false, NULL, &err);
  if (play && !playback_backend_open(play, &err)) {
    playback_backend_free(play);
    return NULL;
  }
  return play;
}

static capture_backend_t *wa_open_capture(int rate, int chunk,
                                          size_t channels) {
  backend_error_t err;
  backend_error_init(&err, BACKEND_ERROR_NONE, "");
  capture_device_config_t cfg = {0};
  cfg.type = AUDIO_BACKEND_TYPE_WEB_AUDIO;
  cfg.cfg.webaudio.channels = channels;
  capture_backend_t *cap =
      create_capture_backend(&cfg, rate, chunk, false, NULL, &err);
  if (cap && !capture_backend_open(cap, &err)) {
    capture_backend_free(cap);
    return NULL;
  }
  return cap;
}

static void wa_reset_formats(void) {
  webaudio_device_set_capture_format(0, 0);
  webaudio_device_set_playback_format(0, 0);
}

// WA-1: drain() at end of stream must not discard the queued audio.
TEST(AuditWebAudio_DrainPlaysOutQueuedAudio) {
  wa_reset_formats();
  playback_backend_t *play = wa_open_playback(48000, 128, 2);
  ASSERT_TRUE(play != NULL);

  backend_error_t err;
  backend_error_init(&err, BACKEND_ERROR_NONE, "");
  audio_chunk_t *chunk = audio_chunk_create(128, 2);
  ASSERT_TRUE(chunk != NULL);
  for (int i = 0; i < 128; i++) {
    audio_chunk_get_channel(chunk, 0)[i] = 0.25;
    audio_chunk_get_channel(chunk, 1)[i] = -0.25;
  }
  audio_chunk_set_valid_frames(chunk, 128);
  ASSERT_TRUE(playback_backend_write(play, chunk, &err));
  ASSERT_TRUE(playback_backend_get_buffer_level(play) >= 128);

  playback_backend_drain(play);
  ASSERT_TRUE(playback_backend_get_buffer_level(play) >= 128);

  float out0[128] = {0}, out1[128] = {0};
  float *outs[2] = {out0, out1};
  webaudio_device_process(NULL, 0, outs, 2, 128);
  ASSERT_NEAR(out0[0], 0.25f, 1e-6f);
  ASSERT_NEAR(out1[127], -0.25f, 1e-6f);

  playback_backend_close(play);
  playback_backend_free(play);
  audio_chunk_free(chunk);
}

// WA-4: the captured stream's channel count becoming known after the capture
// backend opened in another count is a format change (it was ignored as
// "0 -> value", leaving a mono stream on the left channel only).
TEST(AuditWebAudio_LateCaptureChannelCountMismatchFlags) {
  wa_reset_formats();
  webaudio_device_set_capture_format(48000, 0);
  webaudio_device_set_playback_format(48000, 2);
  capture_backend_t *cap = wa_open_capture(48000, 128, 2);
  ASSERT_TRUE(cap != NULL);
  playback_backend_t *play = wa_open_playback(48000, 128, 2);
  ASSERT_TRUE(play != NULL);

  double rate = 0.0;
  // Becoming known in the backend's own count is not a change.
  webaudio_device_set_capture_format(48000, 2);
  ASSERT_FALSE(capture_backend_get_pending_rate_change(cap, &rate));
  ASSERT_FALSE(playback_backend_get_pending_rate_change(play, &rate));

  // Unknown again, then known as mono: flags both sides.
  webaudio_device_set_capture_format(48000, 0);
  ASSERT_FALSE(playback_backend_get_pending_rate_change(play, &rate));
  webaudio_device_set_capture_format(48000, 1);
  ASSERT_TRUE(capture_backend_get_pending_rate_change(cap, &rate));
  ASSERT_NEAR(rate, 48000.0, 0.0);
  ASSERT_TRUE(playback_backend_get_pending_rate_change(play, &rate));

  capture_backend_close(cap);
  capture_backend_free(cap);
  playback_backend_close(play);
  playback_backend_free(play);
  wa_reset_formats();
}

/** Host thread flipping the playback rate 48000 <-> 44100, ending at 44100. */
static void *wa_toggle_playback_rate(void *arg) {
  (void)arg;
  for (int i = 0; i < 50; i++)
    webaudio_device_set_playback_format(i % 2 == 0 ? 48000 : 44100, 2);
  return NULL;
}

// WA-3: a format change racing open() must still leave the backend flagged
// (it could attach in a stale format, unflagged and never served).
// WA-2: format-change signalling racing close()/free must not touch freed
// memory (meaningful under ./run_sanitizers.sh).
TEST(AuditWebAudio_FormatChangeRacingOpenAndClose) {
  for (int iter = 0; iter < 300; iter++) {
    wa_reset_formats();
    webaudio_device_set_playback_format(48000, 2);
    pthread_t toggler;
    ASSERT_EQ(0, pthread_create(&toggler, NULL, wa_toggle_playback_rate, NULL));
    playback_backend_t *play = wa_open_playback(48000, 128, 2);
    pthread_join(toggler, NULL);
    if (!play)
      continue; // Refused: the device was at 44100 during the check.
    // The device ended at 44100, so the 48 kHz backend must be flagged.
    double rate = 0.0;
    ASSERT_TRUE(playback_backend_get_pending_rate_change(play, &rate));
    ASSERT_NEAR(rate, 44100.0, 0.0);

    ASSERT_EQ(0, pthread_create(&toggler, NULL, wa_toggle_playback_rate, NULL));
    playback_backend_close(play);
    playback_backend_free(play);
    pthread_join(toggler, NULL);
  }
  wa_reset_formats();
}

static atomic_bool g_wa_render_run;

/** Simulated AudioWorklet threads: one capture, one playback context. */
static void *wa_capture_worklet(void *arg) {
  (void)arg;
  float in0[128] = {0}, in1[128] = {0};
  const float *ins[2] = {in0, in1};
  while (atomic_load(&g_wa_render_run))
    webaudio_device_process(ins, 2, NULL, 0, 128);
  return NULL;
}

static void *wa_playback_worklet(void *arg) {
  (void)arg;
  float out0[128], out1[128];
  float *outs[2] = {out0, out1};
  while (atomic_load(&g_wa_render_run))
    webaudio_device_process(NULL, 0, outs, 2, 128);
  return NULL;
}

// WA-10: per-direction in-flight counters. Attach/detach both directions while
// both "worklet" threads render; close() of one direction must neither hang
// nor free a ring the other thread is using (meaningful under sanitizers).
TEST(AuditWebAudio_AttachDetachWhileBothWorkletsRender) {
  wa_reset_formats();
  atomic_store(&g_wa_render_run, true);
  pthread_t cap_thread, play_thread;
  ASSERT_EQ(0, pthread_create(&cap_thread, NULL, wa_capture_worklet, NULL));
  ASSERT_EQ(0, pthread_create(&play_thread, NULL, wa_playback_worklet, NULL));
  for (int iter = 0; iter < 200; iter++) {
    capture_backend_t *cap = wa_open_capture(48000, 128, 2);
    playback_backend_t *play = wa_open_playback(48000, 128, 2);
    ASSERT_TRUE(cap != NULL);
    ASSERT_TRUE(play != NULL);
    capture_backend_close(cap);
    capture_backend_free(cap);
    playback_backend_close(play);
    playback_backend_free(play);
  }
  atomic_store(&g_wa_render_run, false);
  pthread_join(cap_thread, NULL);
  pthread_join(play_thread, NULL);
}

// WA-5: the engine's prefill (devices.target_level) must fit in the ring with
// headroom; the ring used to be sized from chunk_size only and silently
// truncated a large prefill.
TEST(AuditWebAudio_PrefillLargerThanChunkSizedRingFits) {
  wa_reset_formats();
  playback_backend_t *play = wa_open_playback(48000, 128, 2);
  ASSERT_TRUE(play != NULL);
  backend_error_t err;
  backend_error_init(&err, BACKEND_ERROR_NONE, "");
  ASSERT_TRUE(playback_backend_prefill_silence(play, 3000, &err));
  ASSERT_EQ(playback_backend_get_buffer_level(play), 3000);

  // Four chunks of headroom on top of the target level.
  audio_chunk_t *chunk = audio_chunk_create(128, 2);
  ASSERT_TRUE(chunk != NULL);
  for (int i = 0; i < 128; i++) {
    audio_chunk_get_channel(chunk, 0)[i] = 0.5;
    audio_chunk_get_channel(chunk, 1)[i] = 0.5;
  }
  audio_chunk_set_valid_frames(chunk, 128);
  for (int k = 0; k < 4; k++)
    ASSERT_TRUE(playback_backend_write(play, chunk, &err));
  ASSERT_EQ(playback_backend_get_buffer_level(play), 3000 + 4 * 128);

  // Still attached after the resize: the device renders from the new ring.
  float out0[128], out1[128];
  float *outs[2] = {out0, out1};
  for (int q = 0; q < 3000 / 128 + 1; q++)
    webaudio_device_process(NULL, 0, outs, 2, 128);
  ASSERT_NEAR(out0[127], 0.5f, 1e-6f);

  playback_backend_close(play);
  playback_backend_free(play);
  audio_chunk_free(chunk);
}

// WA-17: a second open() without close() is refused instead of leaking the
// first ring.
TEST(AuditWebAudio_DoubleOpenRefused) {
  wa_reset_formats();
  backend_error_t err;
  backend_error_init(&err, BACKEND_ERROR_NONE, "");
  capture_backend_t *cap = wa_open_capture(48000, 128, 2);
  ASSERT_TRUE(cap != NULL);
  ASSERT_FALSE(capture_backend_open(cap, &err));
  capture_backend_close(cap);
  ASSERT_TRUE(capture_backend_open(cap, &err));
  capture_backend_close(cap);
  capture_backend_free(cap);

  playback_backend_t *play = wa_open_playback(48000, 128, 2);
  ASSERT_TRUE(play != NULL);
  ASSERT_FALSE(playback_backend_open(play, &err));
  playback_backend_close(play);
  ASSERT_TRUE(playback_backend_open(play, &err));
  playback_backend_close(play);
  playback_backend_free(play);
}

TEST_MAIN()

#endif // ENABLE_WEBAUDIO
