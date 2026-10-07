// Audit regression tests for the engine (report 07 + report 09 Finding 1).
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audio/audio_chunk.h"
#include "audio/processing_parameters.h"
#include "backend/audio_backend.h"
#include "backend/backend_error.h"
#include "cdsp/general.h"
#include "cdsp/processing.h"
#include "config/config_error.h"
#include "config/config_gen.h"
#include "engine/dsp_engine.h"
#include "engine/engine_capture_loop.h"
#include "engine/engine_shared_state.h"
#include "engine/sample_rate_watcher.h"
#include "resampler/audio_resampler.h"
#include "test_support.h"
#include "utils/cdsp_time.h"
#include "utils/lock_free_ring_buffer.h"

// Report 09 Finding 1: the capture loop must apply the relative resampler
// ratio *before* querying resampler_get_input_frames_next(). Previously the
// ratio was applied between the device read and resampler_process(), which
// recomputed needed_input_size so that the freshly captured, complete chunk
// was treated as a short (partial / EOF) chunk: the resampler zero-padded the
// input and scaled the output valid_frames below chunk_size.
static void run_capture_ratio_order_case(resampler_type_t type) {
  const size_t chunk = 64;
  engine_shared_state_t *shared = engine_shared_state_create(8, 8);
  ASSERT_TRUE(shared != NULL);
  round_robin_chunk_pool_t *pool = round_robin_chunk_pool_create(10, chunk, 1);
  ASSERT_TRUE(pool != NULL);

  capture_device_config_t cap_cfg;
  memset(&cap_cfg, 0, sizeof(cap_cfg));
  cap_cfg.type = AUDIO_BACKEND_TYPE_GENERATOR;
  cap_cfg.cfg.generator.channels = 1;
  cap_cfg.cfg.generator.signal.type = SIGNAL_TYPE_SINE;
  cap_cfg.cfg.generator.signal.freq = 1000.0;
  cap_cfg.cfg.generator.signal.level = -6.0;

  resampler_config_t rcfg;
  memset(&rcfg, 0, sizeof(rcfg));
  rcfg.type = type;
  if (type == RESAMPLER_TYPE_ASYNC_POLY) {
    rcfg.has_interpolation = true;
    strcpy(rcfg.interpolation, "Cubic");
  } else {
    rcfg.has_profile = true;
    strcpy(rcfg.profile, "Balanced");
  }
  config_error_t cerr;
  config_error_init(&cerr);
  resampler_t *res =
      resampler_create_from_config(&rcfg, 48000, 48000, 1, chunk, &cerr);
  if (!res)
    fprintf(stderr, "resampler create failed: %s\n", cerr.message);
  ASSERT_TRUE(res != NULL);

  backend_error_t berr;
  backend_error_init(&berr, BACKEND_ERROR_NONE, "");
  capture_backend_t *cap = create_capture_backend(
      &cap_cfg, 48000, resampler_get_max_input_frames(res), false, NULL, &berr);
  ASSERT_TRUE(cap != NULL);
  cap->is_realtime = true;

  engine_capture_loop_config_t loop_cfg = {
      .shared = shared,
      .capture = cap,
      .processing_params = NULL,
      .dsd_decoder = NULL,
      .chunk_pool = pool,
      .resampler = res,
      .chunk_size = resampler_get_max_input_frames(res),
      .pipeline_chunk_size = chunk,
      .channels = 1,
      .samplerate = 48000,
      .pipeline_rate = 48000,
      .silence_threshold_db = -200.0,
      .silence_timeout_seconds = 0.0,
      .stop_on_rate_change = false,
      .rate_measure_interval_s = 10.0,
  };
  engine_capture_loop_t *loop = engine_capture_loop_create(&loop_cfg);
  ASSERT_TRUE(loop != NULL);
  engine_shared_state_set_state(shared, PROCESSING_STATE_RUNNING);

  // Warm up at unity ratio.
  for (int i = 0; i < 3; i++) {
    ASSERT_FALSE(engine_capture_loop_step(loop));
    audio_chunk_t *out = (audio_chunk_t *)spsc_queue_dequeue(
        engine_shared_state_get_captured_queue(shared));
    ASSERT_TRUE(out != NULL);
    ASSERT_EQ(audio_chunk_get_valid_frames(out), chunk);
  }

  // Rate controller now asks for a slower capture (ratio < 1 -> the
  // resampler needs *more* input frames per output chunk). Every produced
  // chunk must still be a full chunk.
  engine_shared_state_set_resampler_ratio(shared, 0.96);
  for (int i = 0; i < 6; i++) {
    ASSERT_FALSE(engine_capture_loop_step(loop));
    audio_chunk_t *out = (audio_chunk_t *)spsc_queue_dequeue(
        engine_shared_state_get_captured_queue(shared));
    ASSERT_TRUE(out != NULL);
    ASSERT_EQ(audio_chunk_get_valid_frames(out), chunk);
  }

  engine_capture_loop_free(loop);
  capture_backend_close(cap);
  capture_backend_free(cap);
  resampler_free(res);
  round_robin_chunk_pool_free(pool);
  engine_shared_state_free(shared);
}

TEST(AuditEngine_CaptureLoop_RatioAppliedBeforeInputFramesNext_AsyncPoly) {
  run_capture_ratio_order_case(RESAMPLER_TYPE_ASYNC_POLY);
}

TEST(AuditEngine_CaptureLoop_RatioAppliedBeforeInputFramesNext_AsyncSinc) {
  run_capture_ratio_order_case(RESAMPLER_TYPE_ASYNC_SINC);
}

// ---------------------------------------------------------------------------
// Helpers for engine-level tests.
// ---------------------------------------------------------------------------
static dsp_engine_t *start_generator_engine(const char *out_file,
                                            const char *extra_devices) {
  char json[2048];
  snprintf(
      json, sizeof(json),
      "{\n"
      "  \"devices\": {\n"
      "    \"samplerate\": 16000,\n"
      "    \"chunksize\": 256,\n"
      "    \"queuelimit\": 8%s\n"
      "    ,\"capture\": {\"type\": \"SignalGenerator\", \"channels\": 1,\n"
      "      \"signal\": {\"type\": \"Sine\", \"freq\": 440.0, "
      "\"level\": -20.0}},\n"
      "    \"playback\": {\"type\": \"File\", \"filename\": \"%s\",\n"
      "      \"format\": \"S16_LE\", \"channels\": 1, \"realtime\": true}\n"
      "  }\n"
      "}",
      extra_devices ? extra_devices : "", out_file);
  dsp_engine_t *engine = dsp_engine_create();
  if (!engine)
    return NULL;
  audio_backend_error_t err;
  memset(&err, 0, sizeof(err));
  if (!engine->set_config_json(engine->ctx, json, &err)) {
    fprintf(stderr, "set_config_json failed: %s\n", err.message);
    engine->free(engine->ctx);
    return NULL;
  }
  return engine;
}

static bool wait_for_state(dsp_engine_t *engine, cdsp_processing_state_t st,
                           int timeout_ms) {
  for (int i = 0; i < timeout_ms / 10; i++) {
    cdsp_engine_poll(engine);
    if (cdsp_get_state(engine) == st)
      return true;
    cdsp_sleep_ms(10);
  }
  return false;
}

// Report 07 §2.1: with rate adjust disabled, upstream never publishes a
// buffer level (RateAdjustReporter::update returns early), so it stays 0.
TEST(AuditEngine_BufferLevel_NotPublishedWhenRateAdjustDisabled) {
  char out_file[256];
  snprintf(out_file, sizeof(out_file), "/tmp/audit_engine_bl_%d.raw", getpid());
  dsp_engine_t *engine = start_generator_engine(out_file, NULL);
  ASSERT_TRUE(engine != NULL);
  ASSERT_TRUE(wait_for_state(engine, CDSP_PROCESSING_STATE_RUNNING, 2000));
  cdsp_sleep_ms(300);

  double rate_adjust = -1.0, buffer_level = -1.0, pl = 0.0, rl = 0.0;
  uint64_t clipped = 0;
  ASSERT_TRUE(engine->get_processing_status(engine->ctx, &rate_adjust,
                                            &buffer_level, &clipped, &pl, &rl));
  ASSERT_TRUE(buffer_level == 0.0);

  cdsp_stop(engine);
  engine->free(engine->ctx);
  remove(out_file);
}

// Report 07 §3.1: sample_rate_watcher_reset must also clear the last measured
// rate so a stale pre-stall rate is not republished after a stall.
TEST(AuditEngine_SampleRateWatcher_ResetClearsLastMeasuredRate) {
  sample_rate_watcher_t *w = sample_rate_watcher_create(48000.0, 0.02, false);
  ASSERT_TRUE(w != NULL);
  sample_rate_watcher_reset(w);
  cdsp_sleep_ms(30);
  double measured = 0.0;
  (void)sample_rate_watcher_tick(w, 1440, &measured);
  ASSERT_TRUE(sample_rate_watcher_get_last_measured_rate(w) > 0.0);
  sample_rate_watcher_reset(w);
  ASSERT_TRUE(sample_rate_watcher_get_last_measured_rate(w) == 0.0);
  sample_rate_watcher_free(w);
}

// Report 07 §4.2: a capture device that stops delivering while the engine is
// silence-PAUSED must be reported as STALLED (upstream coreaudio/file
// backends do this regardless of the paused state).
extern _Atomic bool g_generator_mock_hang;

extern _Atomic bool g_generator_mock_silence_gated;

TEST(AuditEngine_Watchdog_StallDetectedWhilePaused) {
  // Quiet generator used as a silent input (generators are not silence-
  // gated by default, 06 F-07).
  atomic_store_explicit(&g_generator_mock_silence_gated, true,
                        memory_order_relaxed);
  atomic_store_explicit(&g_generator_mock_hang, false, memory_order_relaxed);
  char out_file[256];
  snprintf(out_file, sizeof(out_file), "/tmp/audit_engine_pstall_%d.raw",
           getpid());

  char json[2048];
  snprintf(
      json, sizeof(json),
      "{\n"
      "  \"devices\": {\n"
      "    \"samplerate\": 16000, \"chunksize\": 256, \"queuelimit\": 8,\n"
      "    \"silence_threshold\": -60.0, \"silence_timeout_s\": 0.1,\n"
      "    \"capture\": {\"type\": \"SignalGenerator\", \"channels\": 1,\n"
      "      \"signal\": {\"type\": \"Sine\", \"freq\": 440.0, "
      "\"level\": -120.0}},\n"
      "    \"playback\": {\"type\": \"File\", \"filename\": \"%s\",\n"
      "      \"format\": \"S16_LE\", \"channels\": 1, \"realtime\": true}\n"
      "  }\n"
      "}",
      out_file);
  dsp_engine_t *engine = dsp_engine_create();
  ASSERT_TRUE(engine != NULL);
  audio_backend_error_t err;
  memset(&err, 0, sizeof(err));
  ASSERT_TRUE(engine->set_config_json(engine->ctx, json, &err));

  ASSERT_TRUE(wait_for_state(engine, CDSP_PROCESSING_STATE_PAUSED, 3000));

  atomic_store_explicit(&g_generator_mock_hang, true, memory_order_relaxed);
  bool stalled = wait_for_state(engine, CDSP_PROCESSING_STATE_STALLED, 3000);
  atomic_store_explicit(&g_generator_mock_hang, false, memory_order_relaxed);
  ASSERT_TRUE(stalled);

  // Recovery: data flows again; the capture loop leaves STALLED and the
  // silence counter puts the engine back into PAUSED.
  ASSERT_TRUE(wait_for_state(engine, CDSP_PROCESSING_STATE_PAUSED, 3000));

  cdsp_stop(engine);
  engine->free(engine->ctx);
  remove(out_file);
  atomic_store_explicit(&g_generator_mock_silence_gated, false,
                        memory_order_relaxed);
}

// Report 07 §5.1: on stop, upstream moves the active config to previous and
// clears the active config; starting again from inactive keeps previous.
TEST(AuditEngine_Stop_RotatesActiveConfigToPrevious) {
  char out_file[256];
  snprintf(out_file, sizeof(out_file), "/tmp/audit_engine_rot_%d.raw",
           getpid());
  dsp_engine_t *engine = start_generator_engine(out_file, NULL);
  ASSERT_TRUE(engine != NULL);

  char *active = NULL;
  ASSERT_TRUE(engine->get_active_config_json(engine->ctx, &active));
  ASSERT_TRUE(active != NULL);
  char *prev = NULL;
  ASSERT_FALSE(engine->get_previous_config_json(engine->ctx, &prev));
  ASSERT_TRUE(prev == NULL);

  cdsp_stop(engine);

  char *active_after = NULL;
  ASSERT_FALSE(engine->get_active_config_json(engine->ctx, &active_after));
  ASSERT_TRUE(active_after == NULL);
  ASSERT_TRUE(engine->get_previous_config_json(engine->ctx, &prev));
  ASSERT_TRUE(prev != NULL);
  ASSERT_STR_EQ(active, prev);
  free(prev);
  prev = NULL;

  // Restart from inactive with the same config: previous must be preserved.
  audio_backend_error_t err;
  memset(&err, 0, sizeof(err));
  ASSERT_TRUE(engine->set_config_json(engine->ctx, active, &err));
  ASSERT_TRUE(engine->get_previous_config_json(engine->ctx, &prev));
  ASSERT_TRUE(prev != NULL);
  ASSERT_STR_EQ(active, prev);
  free(prev);

  free(active);
  cdsp_stop(engine);
  engine->free(engine->ctx);
  remove(out_file);
}
