// Audit regression tests for report 07 (backend common), cross-report items
// from report 06 (file / generator capture) that live in the capture loop.
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "audio/audio_chunk.h"
#include "backend/audio_backend.h"
#include "backend/backend_error.h"
#include "config/config_error.h"
#include "config/config_gen.h"
#include "config/configuration.h"
#include "engine/engine_capture_loop.h"
#include "engine/engine_shared_state.h"
#include "test_support.h"
#include "utils/cdsp_time.h"
#include "utils/lock_free_ring_buffer.h"

// ---------------------------------------------------------------------------
// Fake capture backend: returns `frames` frames of digital silence.
static bool fake_silent_read(void *ctx, size_t frames, audio_chunk_t *chunk,
                             backend_error_t *err) {
  (void)ctx;
  (void)err;
  for (size_t c = 0; c < audio_chunk_get_channels(chunk); c++) {
    double *d = audio_chunk_get_channel(chunk, c);
    if (d)
      memset(d, 0, frames * sizeof(double));
  }
  audio_chunk_set_valid_frames(chunk, frames);
  return true;
}

static const capture_backend_vtable_t g_fake_silent_vtable = {
    .read = fake_silent_read,
};

typedef struct {
  engine_shared_state_t *shared;
  round_robin_chunk_pool_t *pool;
  engine_capture_loop_t *loop;
} loop_fixture_t;

static void fixture_init(loop_fixture_t *f, capture_backend_t *cap,
                         size_t chunk, double threshold_db, double timeout_s,
                         bool stop_on_rate_change, double rate_interval_s) {
  f->shared = engine_shared_state_create(8, 8);
  ASSERT_TRUE(f->shared != NULL);
  f->pool = round_robin_chunk_pool_create(10, chunk, 1);
  ASSERT_TRUE(f->pool != NULL);
  engine_capture_loop_config_t cfg = {
      .shared = f->shared,
      .capture = cap,
      .chunk_pool = f->pool,
      .chunk_size = chunk,
      .pipeline_chunk_size = chunk,
      .channels = 1,
      .samplerate = 48000,
      .pipeline_rate = 48000,
      .silence_threshold_db = threshold_db,
      .silence_timeout_seconds = timeout_s,
      .stop_on_rate_change = stop_on_rate_change,
      .rate_measure_interval_s = rate_interval_s,
  };
  f->loop = engine_capture_loop_create(&cfg);
  ASSERT_TRUE(f->loop != NULL);
  engine_shared_state_set_state(f->shared, PROCESSING_STATE_RUNNING);
}

static void fixture_free(loop_fixture_t *f) {
  engine_capture_loop_free(f->loop);
  round_robin_chunk_pool_free(f->pool);
  engine_shared_state_free(f->shared);
}

static void drain_captured(loop_fixture_t *f) {
  while (spsc_queue_dequeue(engine_shared_state_get_captured_queue(f->shared)))
    ;
}

static capture_backend_t *make_generator(size_t chunk) {
  capture_device_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.type = AUDIO_BACKEND_TYPE_GENERATOR;
  cfg.cfg.generator.channels = 1;
  cfg.cfg.generator.signal.type = SIGNAL_TYPE_SINE;
  cfg.cfg.generator.signal.freq = 1000.0;
  cfg.cfg.generator.signal.level = -60.0;
  backend_error_t berr;
  backend_error_init(&berr, BACKEND_ERROR_NONE, "");
  capture_backend_t *cap =
      create_capture_backend(&cfg, 48000, (int)chunk, false, NULL, &berr);
  return cap;
}

// 06 F-08: the generator is unpaced, so its measured rate is meaningless and
// must not trip stop_on_rate_change.
TEST(AuditCommon_06F08_GeneratorDoesNotTripRateWatcher) {
  const size_t chunk = 64;
  capture_backend_t *cap = make_generator(chunk);
  ASSERT_TRUE(cap != NULL);
  loop_fixture_t f;
  fixture_init(&f, cap, chunk, -200.0, 0.0, true, 0.01);
  uint64_t start = cdsp_time_now_ns();
  while (cdsp_time_now_ns() - start < 100ULL * 1000000ULL) {
    ASSERT_FALSE(engine_capture_loop_step(f.loop));
    drain_captured(&f);
  }
  ASSERT_FALSE(engine_shared_state_should_stop(f.shared));
  fixture_free(&f);
  capture_backend_close(cap);
  capture_backend_free(cap);
}

// 06 F-10: chunks flagged skip_silence_detection (file EOF extra_samples
// tail) are never silence gated. Control: without the flag the same silent
// source does pause, which proves the threshold/timeout setup is effective.
TEST(AuditCommon_06F10_SkipSilenceDetectionFlagBypassesCounter) {
  const size_t chunk = 64;
  for (int flagged = 0; flagged <= 1; flagged++) {
    capture_backend_t cap = {
        .ctx = NULL,
        .vtable = &g_fake_silent_vtable,
        .is_realtime = true,
        .skip_silence_detection = flagged != 0,
    };
    loop_fixture_t f;
    fixture_init(&f, &cap, chunk, -100.0, 0.01, false, 10.0);
    bool paused = false;
    for (int i = 0; i < 40; i++) {
      ASSERT_FALSE(engine_capture_loop_step(f.loop));
      drain_captured(&f);
      if (engine_shared_state_get_state(f.shared) == PROCESSING_STATE_PAUSED)
        paused = true;
    }
    ASSERT_EQ(paused, flagged == 0);
    fixture_free(&f);
  }
}

// 06 F-11: a paused non-realtime source must not spin; each paused step
// sleeps about one chunk of capture time (upstream sleep_until_next).
TEST(AuditCommon_06F11_PausedNonRealtimeCaptureSleeps) {
  const size_t chunk = 480; // 10 ms @ 48 kHz -> ~8 ms sleep per step
  capture_backend_t cap = {
      .ctx = NULL,
      .vtable = &g_fake_silent_vtable,
      .is_realtime = false,
  };
  loop_fixture_t f;
  // limit = round(0.02 * 48000 / 480) = 2 chunks.
  fixture_init(&f, &cap, chunk, -100.0, 0.02, false, 10.0);
  int guard = 0;
  while (engine_shared_state_get_state(f.shared) != PROCESSING_STATE_PAUSED) {
    ASSERT_FALSE(engine_capture_loop_step(f.loop));
    drain_captured(&f);
    ASSERT_TRUE(++guard < 20);
  }
  uint64_t start = cdsp_time_now_ns();
  for (int i = 0; i < 5; i++) {
    ASSERT_FALSE(engine_capture_loop_step(f.loop));
    drain_captured(&f);
  }
  uint64_t elapsed_ms = (cdsp_time_now_ns() - start) / 1000000ULL;
  ASSERT_TRUE(elapsed_ms >= 30);
  fixture_free(&f);
}

// 03 CA-01: CoreAudio only knows after open whether pitch control exists.
// The capture loop must re-read support after capture_backend_open and
// publish it (lock-free, via the shared state) to the playback side.
static bool g_fake_opened;
static bool fake_pitch_open(void *ctx, backend_error_t *err) {
  (void)ctx;
  (void)err;
  g_fake_opened = true;
  return true;
}
static bool fake_pitch_supported(void *ctx) {
  (void)ctx;
  return g_fake_opened;
}
static const capture_backend_vtable_t g_fake_pitch_vtable = {
    .open = fake_pitch_open,
    .read = fake_silent_read,
    .is_pitch_control_supported = fake_pitch_supported,
};

TEST(AuditCommon_03CA01_PitchSupportRefreshedAfterOpen) {
  g_fake_opened = false;
  capture_backend_t cap = {
      .ctx = NULL, .vtable = &g_fake_pitch_vtable, .is_realtime = true};
  loop_fixture_t f;
  fixture_init(&f, &cap, 64, -200.0, 0.0, false, 10.0);
  ASSERT_FALSE(engine_shared_state_get_capture_pitch_supported(f.shared));
  // Ask the loop to stop right after open so run() returns immediately.
  processing_stop_reason_t reason = {.type = STOP_REASON_UNKNOWN_ERROR};
  engine_shared_state_request_stop(f.shared, reason);
  engine_capture_loop_run(f.loop);
  ASSERT_TRUE(g_fake_opened);
  ASSERT_TRUE(engine_shared_state_get_capture_pitch_supported(f.shared));
  fixture_free(&f);
}

// F13: the input-file open error carries upstream's ". Reason: <strerror>".
TEST(AuditCommon_F13_FileOpenErrorHasReason) {
  dsp_config_t config;
  memset(&config, 0, sizeof(config));
  config.devices.samplerate = 44100;
  config.devices.chunksize = 1024;
  config.devices.capture.type = AUDIO_BACKEND_TYPE_FILE;
  snprintf(config.devices.capture.cfg.raw_file.filename,
           sizeof(config.devices.capture.cfg.raw_file.filename),
           "/nonexistent/audit_common/f13.raw");
  config.devices.capture.cfg.raw_file.channels = 2;
  config.devices.playback.type = AUDIO_BACKEND_TYPE_FILE;
  config.devices.playback.cfg.raw_file.channels = 2;
  config_error_t err;
  config_error_init(&err);
  ASSERT_NE(0, dsp_config_validate(&config, &err));
  ASSERT_TRUE(strstr(err.message, "Could not open input file") != NULL);
  ASSERT_TRUE(strstr(err.message, ". Reason: ") != NULL);
}

// F14: an out-of-range error type still produces a description.
TEST(AuditCommon_F14_ErrorDescriptionUnknownType) {
  audio_backend_error_t e;
  memset(&e, 0, sizeof(e));
  e.type = (audio_backend_error_type_t)9999;
  snprintf(e.message, sizeof(e.message), "detail");
  char buf[128];
  memset(buf, 'x', sizeof(buf));
  audio_backend_error_description(&e, buf, sizeof(buf));
  ASSERT_TRUE(strstr(buf, "Unknown backend error") != NULL);
  ASSERT_TRUE(strstr(buf, "detail") != NULL);
}

TEST_MAIN()
