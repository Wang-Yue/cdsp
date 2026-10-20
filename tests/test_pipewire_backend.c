#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backend/pipewire_internal.h"
#include "test_support.h"

// F-02: a well-formed chunk is passed through unchanged.
TEST(Audit_PipeWireCaptureChunkInBounds) {
  size_t off = 99;
  // 2 ch F32 => 8 bytes/frame; 256 frames at offset 64 in a 4096-byte map.
  size_t frames = pipewire_capture_chunk_frames(64, 2048, 4096, 8, &off);
  ASSERT_EQ(64, (int)off);
  ASSERT_EQ(256, (int)frames);
}

// F-02: offset + size beyond maxsize is clamped to the mapped region.
TEST(Audit_PipeWireCaptureChunkSizeClamped) {
  size_t off = 0;
  size_t frames = pipewire_capture_chunk_frames(4000, 2048, 4096, 8, &off);
  ASSERT_EQ(4000, (int)off);
  ASSERT_EQ(12, (int)frames); // (4096 - 4000) / 8
  ASSERT_TRUE(off + frames * 8 <= 4096);
}

// F-02: offset at/after maxsize yields nothing to read.
TEST(Audit_PipeWireCaptureChunkOffsetPastEnd) {
  size_t off = 1;
  ASSERT_EQ(0, (int)pipewire_capture_chunk_frames(4096, 512, 4096, 8, &off));
  ASSERT_TRUE(off <= 4096);
  ASSERT_EQ(0, (int)pipewire_capture_chunk_frames(UINT32_MAX, UINT32_MAX, 4096,
                                                  8, &off));
  ASSERT_TRUE(off <= 4096);
}

// F-02: degenerate parameters never divide by zero or read.
TEST(Audit_PipeWireCaptureChunkDegenerate) {
  size_t off = 5;
  ASSERT_EQ(0, (int)pipewire_capture_chunk_frames(0, 512, 0, 8, &off));
  ASSERT_EQ(0, (int)off);
  ASSERT_EQ(0, (int)pipewire_capture_chunk_frames(0, 512, 4096, 0, &off));
  ASSERT_EQ(0, (int)pipewire_capture_chunk_frames(0, 7, 4096, 8, NULL));
}

// Playback byte count matches upstream pipewire_playback_callback_bytes
// (device.rs:235-248).
TEST(Audit_PipeWirePlaybackCallbackBytes) {
  // Valid request: used as-is (rounded to whole frames).
  ASSERT_EQ(1024, (int)pipewire_playback_callback_bytes(1024, 8192, 8, 256));
  ASSERT_EQ(1016, (int)pipewire_playback_callback_bytes(1020, 8192, 8, 256));
  // No request: chunk size fallback.
  ASSERT_EQ(2048, (int)pipewire_playback_callback_bytes(0, 8192, 8, 256));
  // Request larger than capacity: fallback bounded by capacity.
  ASSERT_EQ(2048, (int)pipewire_playback_callback_bytes(99999, 8192, 8, 256));
  ASSERT_EQ(1000, (int)pipewire_playback_callback_bytes(0, 1004, 8, 256));
  // Zero stride never divides by zero.
  ASSERT_EQ(0, (int)pipewire_playback_callback_bytes(1024, 8192, 0, 256));
}

// F-04: graph rate tracking. The first observation is the baseline (no
// report, so a graph already running at another rate keeps working through
// PipeWire's adapter); every later change is reported once.
TEST(Audit_PipeWireGraphRateUpdate) {
  uint32_t seen = 0;
  ASSERT_EQ(0, (int)pipewire_graph_rate_update(0, &seen)); // unknown
  ASSERT_EQ(0, (int)seen);
  ASSERT_EQ(0, (int)pipewire_graph_rate_update(44100, &seen)); // baseline
  ASSERT_EQ(44100, (int)seen);
  ASSERT_EQ(0, (int)pipewire_graph_rate_update(44100, &seen)); // unchanged
  ASSERT_EQ(96000, (int)pipewire_graph_rate_update(96000, &seen));
  ASSERT_EQ(0, (int)pipewire_graph_rate_update(96000, &seen)); // reported once
  ASSERT_EQ(48000, (int)pipewire_graph_rate_update(48000, &seen));
  ASSERT_EQ(0, (int)pipewire_graph_rate_update(0, &seen)); // transient 0
  ASSERT_EQ(48000, (int)seen);
  ASSERT_EQ(0, (int)pipewire_graph_rate_update(48000, NULL));
}

#if defined(__linux__) && defined(ENABLE_PIPEWIRE)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdbool.h>

#include "audio/audio_chunk.h"
#include "backend/audio_backend.h"
#include "backend/backend_error.h"
#include "utils/cdsp_time.h"

TEST(PipeWirePlaybackBasic) {
  playback_device_config_t play_cfg;
  memset(&play_cfg, 0, sizeof(play_cfg));
  play_cfg.type = AUDIO_BACKEND_TYPE_PIPEWIRE;
  play_cfg.cfg.pipewire.channels = 2;
  snprintf(play_cfg.cfg.pipewire.device, sizeof(play_cfg.cfg.pipewire.device),
           "default");
  play_cfg.cfg.pipewire.has_device = true;

  backend_error_t err;
  playback_backend_t *playback =
      create_playback_backend(&play_cfg, 48000, 1024, false, NULL, &err);
  if (!playback) {
    printf("PipeWire playback backend creation failed: %s (skipping test)\n",
           err.message);
    return;
  }

  if (!playback_backend_open(playback, &err)) {
    printf("PipeWire playback backend open failed: %s (skipping test)\n",
           err.message);
    playback_backend_free(playback);
    return;
  }

  audio_chunk_t *chunk = audio_chunk_create(1024, 2);
  memset(audio_chunk_get_channel(chunk, 0), 0, 1024 * sizeof(double));
  memset(audio_chunk_get_channel(chunk, 1), 0, 1024 * sizeof(double));
  audio_chunk_set_valid_frames(chunk, 1024);

  ASSERT_TRUE(playback_backend_write(playback, chunk, &err));

  size_t lvl = playback_backend_get_buffer_level(playback);
  printf("PipeWire buffer level: %zu frames\n", lvl);

  audio_chunk_free(chunk);
  playback_backend_close(playback);
  playback_backend_free(playback);
}

TEST(PipeWireCaptureBasic) {
  capture_device_config_t cap_cfg;
  memset(&cap_cfg, 0, sizeof(cap_cfg));
  cap_cfg.type = AUDIO_BACKEND_TYPE_PIPEWIRE;
  cap_cfg.cfg.pipewire.channels = 2;
  snprintf(cap_cfg.cfg.pipewire.device, sizeof(cap_cfg.cfg.pipewire.device),
           "default");
  cap_cfg.cfg.pipewire.has_device = true;

  backend_error_t err;
  capture_backend_t *capture =
      create_capture_backend(&cap_cfg, 48000, 1024, false, NULL, &err);
  if (!capture) {
    printf("PipeWire capture backend creation failed: %s (skipping test)\n",
           err.message);
    return;
  }

  if (!capture_backend_open(capture, &err)) {
    printf("PipeWire capture backend open failed: %s (skipping test)\n",
           err.message);
    capture_backend_free(capture);
    return;
  }

  audio_chunk_t *chunk = audio_chunk_create(1024, 2);
  if (capture_backend_wait(capture, 100)) {
    ASSERT_TRUE(capture_backend_read(capture, 128, chunk, &err));
    ASSERT_EQ(128, audio_chunk_get_valid_frames(chunk));
  } else {
    printf("No capture data received from PipeWire within 100ms (skipping read "
           "test)\n");
  }

  audio_chunk_free(chunk);
  capture_backend_close(capture);
  capture_backend_free(capture);
}

static const char *live_sink(void) {
  const char *s = getenv("CDSP_PW_TEST_SINK");
  if (!s || !*s) {
    printf("CDSP_PW_TEST_SINK not set, skipping live PipeWire test\n");
    return NULL;
  }
  return s;
}

static capture_backend_t *open_monitor_capture(const char *sink, int rate) {
  capture_device_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.type = AUDIO_BACKEND_TYPE_PIPEWIRE;
  cfg.cfg.pipewire.channels = 2;
  cfg.cfg.pipewire.has_autoconnect_to = true;
  snprintf(cfg.cfg.pipewire.autoconnect_to,
           sizeof(cfg.cfg.pipewire.autoconnect_to), "%s", sink);
  cfg.cfg.pipewire.has_loopback = true;
  cfg.cfg.pipewire.loopback = true;
  backend_error_t err;
  capture_backend_t *c =
      create_capture_backend(&cfg, rate, 512, false, NULL, &err);
  if (!c)
    return NULL;
  if (!capture_backend_open(c, &err)) {
    printf("open failed: %s\n", err.message);
    capture_backend_free(c);
    return NULL;
  }
  return c;
}

TEST(Audit_PipeWireLiveCaptureReceivesData) {
  const char *sink = live_sink();
  if (!sink)
    return;
  capture_backend_t *c = open_monitor_capture(sink, 48000);
  ASSERT_TRUE(c != NULL);
  bool got = false;
  for (int i = 0; i < 40 && !got; i++)
    got = capture_backend_wait(c, 50);
  ASSERT_TRUE(got);
  audio_chunk_t *chunk = audio_chunk_create(256, 2);
  backend_error_t err;
  for (int i = 0; i < 40; i++) {
    if (capture_backend_wait(c, 50) &&
        capture_backend_read(c, 256, chunk, &err))
      break;
  }
  ASSERT_EQ(256, (int)audio_chunk_get_valid_frames(chunk));
  audio_chunk_free(chunk);
  capture_backend_close(c);
  capture_backend_free(c);
}

TEST(Audit_PipeWireLiveGraphRateChangeReported) {
  const char *sink = live_sink();
  if (!sink)
    return;
  ASSERT_EQ(0, system("pw-metadata -n settings 0 clock.allowed-rates "
                      "'[ 44100 48000 96000 ]' >/dev/null 2>&1"));
  ASSERT_EQ(0, system("pw-metadata -n settings 0 clock.force-rate 44100 "
                      ">/dev/null 2>&1"));
  cdsp_sleep_ms(300);

  capture_backend_t *c = open_monitor_capture(sink, 48000);
  ASSERT_TRUE(c != NULL);
  bool got = false;
  for (int i = 0; i < 40 && !got; i++)
    got = capture_backend_wait(c, 50);
  ASSERT_TRUE(got);
  cdsp_sleep_ms(200);

  double rate = 0.0;
  bool startup_report = capture_backend_get_pending_rate_change(c, &rate);

  if (system(
          "pw-metadata -n settings 0 clock.force-rate 96000 >/dev/null 2>&1") !=
      0) {
  }
  bool reported = false;
  for (int i = 0; i < 100 && !reported; i++) {
    capture_backend_wait(c, 30);
    reported = capture_backend_get_pending_rate_change(c, &rate);
  }
  if (system("pw-metadata -n settings 0 clock.force-rate 0 >/dev/null 2>&1") !=
      0) {
  }

  capture_backend_close(c);
  capture_backend_free(c);

  ASSERT_FALSE(startup_report);
  ASSERT_TRUE(reported);
  ASSERT_EQ(96000, (int)(rate + 0.5));
}
#endif

TEST_MAIN()
