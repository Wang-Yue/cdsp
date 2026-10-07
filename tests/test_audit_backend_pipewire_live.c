/**
 * @file test_audit_backend_pipewire_live.c
 * @brief Opt-in live PipeWire tests for audit 05 (F-01, F-04).
 *
 * Requires a running PipeWire graph and `pw-metadata` in PATH. Enabled only
 * when CDSP_PW_TEST_SINK names an Audio/Sink node to capture from (monitor,
 * `loopback: true`); otherwise every test returns immediately (skip).
 *
 * Example (container):
 *   pw-cli create-node adapter '{ factory.name=support.null-audio-sink
 *       node.name=testsink media.class=Audio/Sink object.linger=true
 *       audio.position=[FL FR] }'
 *   CDSP_PW_TEST_SINK=testsink test_runner --run Audit_PipeWireLive...
 */
#if defined(__linux__) && defined(ENABLE_PIPEWIRE)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/audio_chunk.h"
#include "backend/audio_backend.h"
#include "backend/backend_error.h"
#include "test_support.h"
#include "utils/cdsp_time.h"

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

// F-01: data flows immediately after open (buffer exists before connect) and
// reads return whole chunks.
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

// F-04: a graph clock-rate change (clock.force-rate) is reported through
// get_pending_rate_change(). Before the fix only the negotiated Format was
// inspected, which stays at the requested rate because the adapter
// resamples, so nothing was ever reported.
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

  // Starting at a graph rate different from the configured one is not a
  // change: it must not be reported (PipeWire resamples, as before).
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

TEST_MAIN()

#else

#include "test_support.h"
TEST(Audit_PipeWireLiveSkippedOnNonLinux) {}
TEST_MAIN()

#endif
