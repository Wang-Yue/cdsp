#include "generator_capture.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "Logging/app_logger.h"
#include "Utils/cdsp_time.h"

static const logger_t g_logger = {"dsp.backend.generator"};

#ifdef CDSP_TEST
volatile bool g_generator_mock_hang = false;
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#if defined(_WIN32)
/**
 * @brief Simple thread-safe random number generator helper for Windows.
 *
 * Replaces POSIX rand_r.
 *
 * @param seed Pointer to the seed.
 * @return Pseudo-random integer.
 */
static inline int rand_r(unsigned int* seed) {
  *seed = *seed * 1103515245 + 12345;
  return (unsigned int)(*seed / 65536) % 32768;
}
#endif

struct generator_capture {
  signal_type_t signal_type;
  double frequency;
  double amplitude;
  int sample_rate;
  int channels;
  int chunk_size;
  double phase;
  unsigned int rand_seed;
  uint64_t last_read_time_ns;
  bool is_paused;
};

/**
 * @brief Helper to get monotonic time in nanoseconds.
 *
 * @return Monotonic time in nanoseconds.
 */
static uint64_t get_time_ns(void) { return cdsp_time_now_ns(); }

/**
 * @brief Convert decibels to linear amplitude multiplier.
 *
 * @param db Value in dB.
 * @return Linear amplitude multiplier.
 */
static double db_to_linear(double db) { return pow(10.0, db / 20.0); }





/**
 * @brief Open the generator capture device.
 *
 * @param ctx Pointer to the generator capture instance.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return true if successful, false otherwise.
 */
static bool generator_capture_open(void* ctx, backend_error_t* err) {
  generator_capture_t* capture = (generator_capture_t*)ctx;
  if (!capture) return false;
  (void)err;
  capture->last_read_time_ns = get_time_ns();
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
static bool generator_capture_read(void* ctx, size_t frames,
                                   audio_chunk_t* chunk, backend_error_t* err) {
  generator_capture_t* capture = (generator_capture_t*)ctx;
  if (!capture) return false;
#ifdef CDSP_TEST
  if (g_generator_mock_hang) {
    while (g_generator_mock_hang) {
      cdsp_sleep_ms(10);
    }
    audio_chunk_set_valid_frames(chunk, 0);
    return false;
  }
#endif
  if (capture->is_paused) {
    cdsp_sleep_ms(10);
  }
  if (audio_chunk_get_channels(chunk) < (size_t)capture->channels) {
    if (err) {
      backend_error_init(
          err, BACKEND_ERROR_INVALID_CHANNELS,
          "Chunk channels count does not match generator channels");
    }
    return false;
  }

  double freq_delta = capture->frequency / (double)capture->sample_rate;
  freq_delta = fmod(freq_delta, 1.0);
  if (freq_delta < 0.0) freq_delta += 1.0;

  switch (capture->signal_type) {
    case SIGNAL_TYPE_SINE: {
      double phase = capture->phase;
      for (size_t f = 0; f < frames; f++) {
        double val = sin(phase * 2.0 * M_PI) * capture->amplitude;
        for (int c = 0; c < capture->channels; c++) {
          audio_chunk_get_channel(chunk, c)[f] = val;
        }
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
        double val =
            (sin(phase * 2.0 * M_PI) >= 0.0 ? 1.0 : -1.0) * capture->amplitude;
        for (int c = 0; c < capture->channels; c++) {
          audio_chunk_get_channel(chunk, c)[f] = val;
        }
        phase += freq_delta;
        if (phase >= 1.0) {
          phase -= 1.0;
        }
      }
      capture->phase = phase;
      break;
    }

    case SIGNAL_TYPE_WHITE_NOISE: {
      for (size_t f = 0; f < frames; f++) {
        for (int c = 0; c < capture->channels; c++) {
          double noise_val =
              (((double)rand_r(&capture->rand_seed) / (double)RAND_MAX) * 2.0 -
               1.0) *
              capture->amplitude;
          audio_chunk_get_channel(chunk, c)[f] = noise_val;
        }
      }
      break;
    }

    default: {
      for (size_t f = 0; f < frames; f++) {
        for (int c = 0; c < capture->channels; c++) {
          audio_chunk_get_channel(chunk, c)[f] = 0.0;
        }
      }
      break;
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
static void generator_capture_close(void* ctx) { (void)ctx; }

/**
 * @brief Get any pending sample rate change.
 *
 * @param ctx Pointer to the generator capture instance.
 * @param out_rate Pointer to double to store the pending sample rate.
 * @return true if a rate change is pending, false otherwise.
 */
static bool generator_capture_get_pending_rate_change(void* ctx,
                                                      double* out_rate) {
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
static bool generator_capture_pitch_control_supported(void* ctx) {
  (void)ctx;
  return false;
}

/**
 * @brief Set the pitch multiplier for the generator capture backend.
 *
 * @param ctx Pointer to the generator capture instance.
 * @param multiplier The pitch multiplier.
 */
static void generator_capture_set_pitch(void* ctx,
                                        double multiplier) {
  (void)ctx;
  (void)multiplier;
}

/**
 * @brief Wait for the generator capture device to have data available.
 *
 * @param ctx Pointer to the generator capture instance.
 * @param timeout_ms Timeout in milliseconds.
 * @return true if data is available, false on timeout or error.
 */
static bool generator_capture_wait(void* ctx, uint32_t timeout_ms) {
  (void)ctx;
  cdsp_sleep_ms(timeout_ms);
  return true;
}

/**
 * @brief Stop the generator capture device.
 *
 * @param ctx Pointer to the generator capture instance.
 */
static void generator_capture_stop(void* ctx) { (void)ctx; }

/**
 * @brief Destroy the generator capture backend instance.
 *
 * @param ctx Pointer to the generator capture instance to destroy.
 */
static void generator_capture_destroy(void* ctx) {
  generator_capture_t* capture = (generator_capture_t*)ctx;
  free(capture);
}

/**
 * @brief Set the paused state of the generator capture backend.
 *
 * @param ctx Pointer to the generator capture instance.
 * @param paused true to pause, false to resume.
 */
static void generator_capture_set_is_paused(void* ctx,
                                            bool paused) {
  generator_capture_t* capture = (generator_capture_t*)ctx;
  if (capture) {
    capture->is_paused = paused;
  }
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
 * @return Pointer to the created capture_backend_t instance, or NULL on failure.
 */
static capture_backend_t* generator_capture_create(
    const capture_device_config_t* config, int sample_rate, int chunk_size,
    bool full_duplex, processing_parameters_t* params, backend_error_t* err) {
  (void)full_duplex;
  (void)params;
  (void)err;

  if (sample_rate <= 0) {
    if (err) {
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Invalid sample rate for generator");
    }
    return NULL;
  }

  generator_capture_t* capture =
      (generator_capture_t*)calloc(1, sizeof(generator_capture_t));
  if (!capture) return NULL;

  capture->signal_type = config->cfg.generator.signal.type;
  capture->frequency = config->cfg.generator.signal.frequency;
  capture->amplitude = db_to_linear(config->cfg.generator.signal.level);

  capture->sample_rate = sample_rate;
  capture->channels = config->cfg.generator.channels;
  capture->chunk_size = chunk_size;
  capture->rand_seed = (unsigned int)(get_time_ns() & 0xFFFFFFFF);

  capture_backend_t* backend =
      (capture_backend_t*)calloc(1, sizeof(capture_backend_t));
  if (!backend) {
    free(capture);
    return NULL;
  }

  backend->ctx = capture;
  backend->vtable = &g_generator_capture_vtable;
  backend->is_realtime = true;
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
    .set_is_paused = generator_capture_set_is_paused,
    .stop = generator_capture_stop,
    .destroy = generator_capture_destroy};
