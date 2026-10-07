#include "backend/generator_capture.h"

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/audio_chunk.h"
#include "backend/audio_backend.h"
#include "backend/backend_error.h"
#include "config/config_gen.h"
#include "logging/app_logger.h"
#include "utils/cdsp_time.h"
#include "utils/double_helpers.h"

static const logger_t g_logger = {"dsp.backend.generator"};

#ifdef CDSP_TEST
#include <stdatomic.h>
_Atomic bool g_generator_mock_hang = false;
/// Test hook: when true, newly created generators are silence-gated like a
/// real input, so engine tests can use a quiet generator to drive auto-pause.
_Atomic bool g_generator_mock_silence_gated = false;
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/**
 * @brief splitmix64 step, used only to expand the seed into xoshiro state.
 */
static uint64_t generator_splitmix64(uint64_t *x) {
  uint64_t z = (*x += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

static inline uint64_t generator_rotl(uint64_t x, int k) {
  return (x << k) | (x >> (64 - k));
}

/**
 * @brief xoshiro256++ next value (same family as upstream's SmallRng).
 */
static inline uint64_t generator_rng_next(uint64_t s[4]) {
  uint64_t result = generator_rotl(s[0] + s[3], 23) + s[0];
  uint64_t t = s[1] << 17;
  s[2] ^= s[0];
  s[3] ^= s[1];
  s[1] ^= s[2];
  s[0] ^= s[3];
  s[2] ^= t;
  s[3] = generator_rotl(s[3], 45);
  return result;
}

/**
 * @brief Uniform double in [-1, 1) with full 53-bit resolution.
 */
static inline double generator_rng_uniform_pm1(uint64_t s[4]) {
  double u = (double)(generator_rng_next(s) >> 11) * 0x1.0p-53; // [0, 1)
  return 2.0 * u - 1.0;
}

struct generator_capture {
  signal_type_t signal_type;
  double frequency;
  double amplitude;
  int sample_rate;
  size_t channels;
  int chunk_size;
  double phase;
  uint64_t rng[4]; ///< xoshiro256++ state for white noise.
};

/**
 * @brief Helper to get monotonic time in nanoseconds.
 *
 * @return Monotonic time in nanoseconds.
 */
static uint64_t get_time_ns(void) { return cdsp_time_now_ns(); }

/**
 * @brief Open the generator capture device.
 *
 * @param ctx Pointer to the generator capture instance.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return true if successful, false otherwise.
 */
static bool generator_capture_open(void *ctx, backend_error_t *err) {
  generator_capture_t *capture = (generator_capture_t *)ctx;
  if (!capture)
    return false;
  (void)err;
  capture->phase = 0.0;

  logger_info(&g_logger,
              "Opened generator capture: type=%s, freq=%.1f Hz, amp=%.3f",
              signal_type_to_string(capture->signal_type), capture->frequency,
              capture->amplitude);
  return true;
}

/**
 * @brief Read audio frames from the generator capture device.
 *
 * @param ctx Pointer to the generator capture instance.
 * @param frames Number of frames to read.
 * @param chunk Pointer to the audio chunk to fill.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return true if successful, false otherwise.
 */
static bool generator_capture_read(void *ctx, size_t frames,
                                   audio_chunk_t *chunk, backend_error_t *err) {
  generator_capture_t *capture = (generator_capture_t *)ctx;
  if (!capture)
    return false;
#ifdef CDSP_TEST
  if (atomic_load_explicit(&g_generator_mock_hang, memory_order_relaxed)) {
    while (atomic_load_explicit(&g_generator_mock_hang, memory_order_relaxed)) {
      cdsp_sleep_ms(10);
    }
    audio_chunk_set_valid_frames(chunk, 0);
    return false;
  }
#endif
  if (audio_chunk_get_channels(chunk) < (size_t)capture->channels) {
    if (err) {
      backend_error_init(
          err, BACKEND_ERROR_INVALID_CHANNELS,
          "Chunk channels count does not match generator channels");
    }
    return false;
  }

  if (frames > audio_chunk_get_frames(chunk)) {
    frames = audio_chunk_get_frames(chunk);
  }

  double freq_delta = capture->frequency / (double)capture->sample_rate;
  freq_delta = fmod(freq_delta, 1.0);
  if (freq_delta < 0.0)
    freq_delta += 1.0;

  // Periodic signals are rendered once into channel 0 and copied, keeping all
  // channels in phase (upstream generatordevice.rs:150-174). No VLA: the
  // channel count is user-controlled.
  double *first = audio_chunk_get_channel(chunk, 0);
  if (!first) {
    audio_chunk_set_valid_frames(chunk, 0);
    return false;
  }

  switch (capture->signal_type) {
  case SIGNAL_TYPE_SINE: {
    double phase = capture->phase;
    for (size_t f = 0; f < frames; f++) {
      first[f] = sin(phase * 2.0 * M_PI) * capture->amplitude;
      phase += freq_delta;
      if (phase >= 1.0) {
        phase -= 1.0;
      }
    }
    capture->phase = phase;
    break;
  }

  case SIGNAL_TYPE_SQUARE: {
    double phase = capture->phase;
    for (size_t f = 0; f < frames; f++) {
      // Exact 50 % duty cycle. sign(sin(2*pi*phase)) gives +1 at phase 0.5
      // because sin(pi) rounds to +1.2e-16 (upstream inherits this, so
      // fs/2 renders as DC and fs/4 as a 75 % duty cycle).
      first[f] = (phase < 0.5 ? 1.0 : -1.0) * capture->amplitude;
      phase += freq_delta;
      if (phase >= 1.0) {
        phase -= 1.0;
      }
    }
    capture->phase = phase;
    break;
  }

  case SIGNAL_TYPE_WHITE_NOISE:
    // Independent per channel so the channels are uncorrelated.
    for (size_t c = 0; c < capture->channels; c++) {
      double *dst = audio_chunk_get_channel(chunk, c);
      if (!dst)
        continue;
      for (size_t f = 0; f < frames; f++) {
        dst[f] = generator_rng_uniform_pm1(capture->rng) * capture->amplitude;
      }
    }
    break;

  case SIGNAL_TYPE_INVALID:
    assert(0 && "Invalid signal_type_t");
    memset(first, 0, frames * sizeof(double));
    break;
  }

  if (capture->signal_type != SIGNAL_TYPE_WHITE_NOISE) {
    for (size_t c = 1; c < capture->channels; c++) {
      double *dst = audio_chunk_get_channel(chunk, c);
      if (dst) {
        memcpy(dst, first, frames * sizeof(double));
      }
    }
  }

  audio_chunk_set_valid_frames(chunk, frames);
  return true;
}

/**
 * @brief Close the generator capture device.
 *
 * @param ctx Pointer to the generator capture instance.
 */
static void generator_capture_close(void *ctx) { (void)ctx; }

/**
 * @brief Get any pending sample rate change.
 *
 * @param ctx Pointer to the generator capture instance.
 * @param out_rate Pointer to double to store the pending sample rate.
 * @return true if a rate change is pending, false otherwise.
 */
static bool generator_capture_get_pending_rate_change(void *ctx,
                                                      double *out_rate) {
  (void)ctx;
  (void)out_rate;
  return false;
}

/**
 * @brief Check if pitch control is supported by the generator capture backend.
 *
 * @param ctx Pointer to the generator capture instance.
 * @return true if supported, false otherwise.
 */
static bool generator_capture_pitch_control_supported(void *ctx) {
  (void)ctx;
  return false;
}

/**
 * @brief Set the pitch multiplier for the generator capture backend.
 *
 * @param ctx Pointer to the generator capture instance.
 * @param multiplier The pitch multiplier.
 */
static void generator_capture_set_pitch(void *ctx, double multiplier) {
  (void)ctx;
  (void)multiplier;
  logger_warn(
      &g_logger,
      "Signal generator does not support rate adjust. Ignoring request.");
}

/**
 * @brief Wait for the generator capture device to have data available.
 *
 * @param ctx Pointer to the generator capture instance.
 * @param timeout_ms Timeout in milliseconds.
 * @return true if data is available, false on timeout or error.
 */
static bool generator_capture_wait(void *ctx, uint32_t timeout_ms) {
  (void)ctx;
  (void)timeout_ms;
  return true;
}

/**
 * @brief Stop the generator capture device.
 *
 * @param ctx Pointer to the generator capture instance.
 */
static void generator_capture_stop(void *ctx) { (void)ctx; }

/**
 * @brief Destroy the generator capture backend instance.
 *
 * @param ctx Pointer to the generator capture instance to destroy.
 */
static void generator_capture_destroy(void *ctx) {
  generator_capture_t *capture = (generator_capture_t *)ctx;
  free(capture);
}

/**
 * @brief Create a generator capture backend instance.
 *
 * @param config Pointer to the capture device configuration.
 * @param sample_rate The sample rate in Hz.
 * @param chunk_size The size of each audio chunk in frames.
 * @param full_duplex True if running in full duplex mode.
 * @param params Pointer to processing parameters.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return Pointer to the created capture_backend_t instance, or NULL on
 * failure.
 */
static capture_backend_t *generator_capture_create(
    const capture_device_config_t *config, int sample_rate, int chunk_size,
    bool full_duplex, processing_parameters_t *params, backend_error_t *err) {
  (void)chunk_size;
  (void)full_duplex;
  (void)params;

  if (sample_rate <= 0) {
    if (err) {
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Invalid sample rate for generator");
    }
    return NULL;
  }
  const generator_signal_t *sig = &config->cfg.generator.signal;
  bool periodic =
      sig->type == SIGNAL_TYPE_SINE || sig->type == SIGNAL_TYPE_SQUARE;
  if (config->cfg.generator.channels == 0 || !isfinite(sig->level) ||
      (periodic && !isfinite(sig->freq))) {
    if (err) {
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Invalid generator configuration (channels must be "
                         "positive, level and freq finite)");
    }
    return NULL;
  }

  generator_capture_t *capture =
      (generator_capture_t *)calloc(1, sizeof(generator_capture_t));
  if (!capture) {
    if (err) {
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Memory allocation failure");
    }
    return NULL;
  }

  capture->signal_type = config->cfg.generator.signal.type;
  capture->frequency = config->cfg.generator.signal.freq;
  capture->amplitude = double_from_db(config->cfg.generator.signal.level);

  capture->sample_rate = sample_rate;
  capture->channels = config->cfg.generator.channels;
  uint64_t seed = get_time_ns() ^ (uint64_t)(uintptr_t)capture;
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
  seed = ((uint64_t)arc4random() << 32) | (uint64_t)arc4random();
#elif !defined(_WIN32)
  FILE *urandom = fopen("/dev/urandom", "rb");
  if (urandom) {
    uint64_t os_seed = 0;
    if (fread(&os_seed, sizeof(os_seed), 1, urandom) == 1) {
      seed = os_seed;
    }
    fclose(urandom);
  }
#endif
  for (int i = 0; i < 4; i++) {
    capture->rng[i] = generator_splitmix64(&seed);
  }

  capture_backend_t *backend =
      (capture_backend_t *)calloc(1, sizeof(capture_backend_t));
  if (!backend) {
    free(capture);
    if (err) {
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Memory allocation failure");
    }
    return NULL;
  }

  backend->ctx = capture;
  backend->vtable = &g_generator_capture_vtable;
  backend->is_realtime = false;
  // Upstream's signal generator has no silence counter and never auto-pauses
  // (generatordevice.rs), so its chunks are not silence-gated (06 F-07).
  backend->skip_silence_detection = true;
  // Unpaced synthetic source: its measured rate is meaningless (06 F-08).
  backend->skip_rate_watcher = true;
#ifdef CDSP_TEST
  if (atomic_load_explicit(&g_generator_mock_silence_gated,
                           memory_order_relaxed))
    backend->skip_silence_detection = false;
#endif
  return backend;
}

const capture_backend_vtable_t g_generator_capture_vtable = {
    .create = generator_capture_create,
    .open = generator_capture_open,
    .read = generator_capture_read,
    .close = generator_capture_close,
    .get_pending_rate_change = generator_capture_get_pending_rate_change,
    .is_pitch_control_supported = generator_capture_pitch_control_supported,
    .set_pitch = generator_capture_set_pitch,
    .wait_for_data = generator_capture_wait,
    .stop = generator_capture_stop,
    .destroy = generator_capture_destroy};
