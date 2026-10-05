/**
 * @file test_wasm.c
 * @brief End-to-end WebAssembly test of the multithreaded engine with the
 * WebAudio backend.
 *
 * Runs under node (PROXY_TO_PTHREAD). The engine runs exactly as in the
 * browser and on desktop: capture, processing and playback pthreads exchanging
 * audio with the WebAudio device through lock-free rings. The test plays the
 * part of the AudioWorklet, calling webaudio_device_process() once per
 * 128-frame render quantum in real time, and checks that the engine's output is
 * bit-exact with the same pipeline run directly.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "audio/audio_chunk.h"
#include "audio/processing_parameters.h"
#include "backend/webaudio_backend.h"
#include "cdsp/cdsp.h"
#include "config/config_error.h"
#include "config/configuration.h"
#include "logging/app_logger.h"
#include "pipeline/pipeline.h"
#include "test_support.h"

#define RATE 48000
#define QUANTUM 128
#define CHUNK 512
#define CHANNELS 2
/** Test signal length: 180 engine chunks (~1.9 s). */
#define SIGNAL_FRAMES (CHUNK * 180)
/** Upper bound on the engine's capture-to-playback latency. */
#define MAX_LATENCY_FRAMES (RATE / 2)
#define CAPTURED_FRAMES (SIGNAL_FRAMES + MAX_LATENCY_FRAMES)

static const char *kConfigJson =
    "{\n"
    "  \"title\": \"WebAudio engine test\",\n"
    "  \"devices\": {\n"
    "    \"samplerate\": 48000,\n"
    "    \"chunksize\": 512,\n"
    "    \"capture\": { \"type\": \"WebAudio\", \"channels\": 2 },\n"
    "    \"playback\": { \"type\": \"WebAudio\", \"channels\": 2 }\n"
    "  },\n"
    "  \"filters\": {\n"
    "    \"low_shelf\": { \"type\": \"Biquad\", \"parameters\": "
    "{ \"type\": \"Lowshelf\", \"freq\": 120.0, \"gain\": 5.5, \"q\": 0.707 } },\n"
    "    \"peaking\": { \"type\": \"Biquad\", \"parameters\": "
    "{ \"type\": \"Peaking\", \"freq\": 1000.0, \"gain\": -3.0, \"q\": 1.414 } },\n"
    "    \"high_shelf\": { \"type\": \"Biquad\", \"parameters\": "
    "{ \"type\": \"Highshelf\", \"freq\": 8000.0, \"gain\": 4.0, \"q\": 0.707 } }\n"
    "  },\n"
    "  \"pipeline\": [\n"
    "    { \"type\": \"Filter\", \"channels\": [0], \"names\": [\"low_shelf\", "
    "\"peaking\", \"high_shelf\"] },\n"
    "    { \"type\": \"Filter\", \"channels\": [1], \"names\": [\"low_shelf\", "
    "\"peaking\", \"high_shelf\"] }\n"
    "  ]\n"
    "}\n";

static float g_input[CHANNELS][SIGNAL_FRAMES];
static float g_expected[CHANNELS][SIGNAL_FRAMES];
static float g_output[CHANNELS][CAPTURED_FRAMES];

/** Multi-tone test signal, kept well below full scale so nothing clips. */
static void generate_input(void) {
  for (size_t t = 0; t < SIGNAL_FRAMES; t++) {
    double x = (double)t / RATE;
    g_input[0][t] = (float)(0.2 * sin(2.0 * M_PI * 100.0 * x) +
                            0.2 * sin(2.0 * M_PI * 1000.0 * x) +
                            0.2 * sin(2.0 * M_PI * 5000.0 * x));
    g_input[1][t] = (float)(0.3 * cos(2.0 * M_PI * 150.0 * x) +
                            0.3 * cos(2.0 * M_PI * 3000.0 * x));
  }
}

/** Runs the same pipeline directly, without the engine or any backend. */
static bool compute_expected(void) {
  config_error_t err;
  config_error_init(&err);
  dsp_config_t *config = NULL;
  if (dsp_config_parse_json(kConfigJson, &config, &err) != 0 || !config)
    return false;
  processing_parameters_t *params = processing_parameters_create(CHANNELS, CHANNELS);
  pipeline_t *pipeline = pipeline_create(config, params, CHUNK, &err);
  audio_chunk_t *in = audio_chunk_create(CHUNK, CHANNELS);
  audio_chunk_t *out = audio_chunk_create(CHUNK, CHANNELS);
  bool ok = params && pipeline && in && out;
  for (size_t start = 0; ok && start < SIGNAL_FRAMES; start += CHUNK) {
    for (size_t c = 0; c < CHANNELS; c++) {
      double *dst = audio_chunk_get_channel(in, c);
      for (size_t i = 0; i < CHUNK; i++)
        dst[i] = (double)g_input[c][start + i];
    }
    audio_chunk_set_valid_frames(in, CHUNK);
    ok = pipeline_process(pipeline, in, out) == PIPELINE_OK;
    for (size_t c = 0; ok && c < CHANNELS; c++) {
      const double *src = audio_chunk_get_channel(out, c);
      for (size_t i = 0; i < CHUNK; i++)
        g_expected[c][start + i] = (float)src[i];
    }
  }
  audio_chunk_free(in);
  audio_chunk_free(out);
  if (pipeline)
    pipeline_free(pipeline);
  if (params)
    processing_parameters_free(params);
  dsp_config_free(config);
  return ok;
}

static double now_seconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/** Sleeps until the wall-clock time of render quantum @p index. */
static void pace(double start, size_t index) {
  double target = start + (double)(index * QUANTUM) / RATE;
  double remaining = target - now_seconds();
  if (remaining > 0) {
    struct timespec ts = {(time_t)remaining,
                          (long)((remaining - (double)(time_t)remaining) * 1e9)};
    nanosleep(&ts, NULL);
  }
}

/**
 * One AudioWorklet render quantum: feeds @p in (or silence when NULL) and
 * stores the rendered output at @p out_offset (discarded when out of range).
 */
static void render_quantum(const float *in0, const float *in1,
                           size_t out_offset) {
  static const float silence[QUANTUM];
  static float out_buf[CHANNELS][QUANTUM];
  const float *in[CHANNELS] = {in0 ? in0 : silence, in1 ? in1 : silence};
  float *out[CHANNELS] = {out_buf[0], out_buf[1]};
  webaudio_device_process(in, CHANNELS, out, CHANNELS, QUANTUM);
  if (out_offset + QUANTUM <= CAPTURED_FRAMES) {
    for (size_t c = 0; c < CHANNELS; c++)
      memcpy(&g_output[c][out_offset], out_buf[c], sizeof(out_buf[c]));
  }
}

/** Renders silent quanta in real time until the engine is running. */
static bool wait_until_running(dsp_engine_t *engine) {
  double start = now_seconds();
  for (size_t q = 0; q < (size_t)(5 * RATE / QUANTUM); q++) {
    render_quantum(NULL, NULL, CAPTURED_FRAMES);
    if (q % 8 == 0) {
      cdsp_engine_poll(engine);
      if (cdsp_get_state(engine) == CDSP_PROCESSING_STATE_RUNNING)
        return true;
    }
    pace(start, q + 1);
  }
  return false;
}

static bool start_engine(dsp_engine_t *engine) {
  cdsp_backend_error_t err;
  memset(&err, 0, sizeof(err));
  if (!cdsp_set_config_json(engine, kConfigJson, &err)) {
    printf("  [FAIL] cdsp_set_config_json: %s\n", err.message);
    return false;
  }
  return wait_until_running(engine);
}

/** Finds the latency at which the captured output starts matching. */
static long find_latency(void) {
  const size_t probe = 256;
  for (size_t lat = 0; lat + probe <= MAX_LATENCY_FRAMES; lat++) {
    bool match = true;
    for (size_t c = 0; c < CHANNELS && match; c++) {
      match = memcmp(&g_output[c][lat], g_expected[c], probe * sizeof(float)) == 0;
    }
    if (match)
      return (long)lat;
  }
  return -1;
}

TEST(wasm_engine_output_is_bit_exact_with_pipeline) {
  generate_input();
  ASSERT_TRUE(compute_expected());

  dsp_engine_t *engine = cdsp_engine_create();
  ASSERT_TRUE(engine != NULL);
  ASSERT_TRUE(start_engine(engine));

  // Stream the signal through the engine in real time, one quantum at a time.
  double start = now_seconds();
  size_t total_quanta = CAPTURED_FRAMES / QUANTUM;
  for (size_t q = 0; q < total_quanta; q++) {
    size_t t = q * QUANTUM;
    bool has_signal = t + QUANTUM <= SIGNAL_FRAMES;
    render_quantum(has_signal ? &g_input[0][t] : NULL,
                   has_signal ? &g_input[1][t] : NULL, t);
    if (q % 32 == 0)
      cdsp_engine_poll(engine);
    pace(start, q + 1);
  }
  ASSERT_EQ(cdsp_get_state(engine), CDSP_PROCESSING_STATE_RUNNING);

  cdsp_vu_levels_t levels_query = {0};
  ASSERT_TRUE(cdsp_get_vu_levels(engine, &levels_query));
  ASSERT_EQ(levels_query.capture_channels, CHANNELS);
  ASSERT_EQ(levels_query.playback_channels, CHANNELS);

  cdsp_stop(engine);
  for (int i = 0; i < 100 && cdsp_get_state(engine) != CDSP_PROCESSING_STATE_INACTIVE; i++) {
    cdsp_engine_poll(engine);
    struct timespec ts = {0, 10 * 1000000};
    nanosleep(&ts, NULL);
  }
  cdsp_engine_free(engine);

  long latency = find_latency();
  printf("  [INFO] engine latency: %ld frames\n", latency);
  ASSERT_TRUE(latency >= 0);

  size_t mismatches = 0;
  for (size_t c = 0; c < CHANNELS; c++) {
    for (size_t i = 0; i < SIGNAL_FRAMES; i++) {
      uint32_t expected_bits, actual_bits;
      memcpy(&expected_bits, &g_expected[c][i], sizeof(expected_bits));
      memcpy(&actual_bits, &g_output[c][(size_t)latency + i], sizeof(actual_bits));
      if (expected_bits != actual_bits && mismatches++ < 5) {
        printf("  [FAIL] channel %zu frame %zu: expected %a, got %a\n", c, i,
               g_expected[c][i], g_output[c][(size_t)latency + i]);
      }
    }
  }
  ASSERT_EQ(mismatches, 0);
}

TEST(wasm_device_is_silent_after_engine_stops) {
  dsp_engine_t *engine = cdsp_engine_create();
  ASSERT_TRUE(engine != NULL);
  ASSERT_TRUE(start_engine(engine));
  cdsp_stop(engine);
  cdsp_engine_free(engine);

  // Backends detached on teardown: the device renders silence.
  static float ones[QUANTUM];
  for (size_t i = 0; i < QUANTUM; i++)
    ones[i] = 1.0f;
  render_quantum(ones, ones, 0);
  for (size_t c = 0; c < CHANNELS; c++) {
    for (size_t i = 0; i < QUANTUM; i++)
      ASSERT_NEAR(g_output[c][i], 0.0f, 0.0f);
  }
}

TEST(wasm_webaudio_device_listing) {
  cdsp_device_info_t *devs = NULL;
  size_t count = 0;
  ASSERT_TRUE(cdsp_get_available_devices("WebAudio", true, &devs, &count));
  ASSERT_EQ(count, 1);
  ASSERT_STR_EQ(devs[0].identifier, "default");
  free(devs);

  cdsp_device_descriptor_t *desc = NULL;
  cdsp_device_error_t derr;
  memset(&derr, 0, sizeof(derr));
  ASSERT_TRUE(cdsp_get_device_capabilities("WebAudio", "default", false, &desc, &derr));
  ASSERT_EQ(desc->capability_sets_count, 1);
  // No browser device under Node: playback offers the stereo default, 1..2.
  ASSERT_EQ(desc->capability_sets[0].capabilities_count, 2);
  ASSERT_EQ(desc->capability_sets[0].capabilities[1].channels, 2);
  cdsp_free_device_capabilities(desc);
}

// Drain the shared logger thread before returning: under PROXY_TO_PTHREAD a
// logger write still in flight while exit() flushes stdout on the browser main
// thread deadlocks, so the process would never exit.
#define main run_registered_tests
TEST_MAIN()
#undef main

int main(int argc, char *argv[]) {
  int rc = run_registered_tests(argc, argv);
  app_logger_flush_and_stop(app_logger_get_shared());
  return rc;
}
