// CoreAudio playback backend for macOS
//
// Real-time discipline
// --------------------
// The render callback runs on a high-priority audio thread driven by
// CoreAudio. It is absolutely forbidden to take locks, allocate, or
// otherwise call into the Swift runtime in a way that could block. To
// honour that:
//   - sample rings are SPSC instances —
//     producer and consumer are wait-free, no lock.
//   - the render callback writes directly into the AudioBufferList
//     provided by CoreAudio HAL, consuming from the pre-allocated SPSC rings.

#include "Backend/core_audio_playback.h"

#if defined(ENABLE_COREAUDIO)
#include <CoreAudio/CoreAudio.h>
#ifdef ENABLE_ACCELERATE
#include <Accelerate/Accelerate.h>
#endif
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "Audio/audio_chunk.h"
#include "Backend/backend_error.h"
#include "Backend/core_audio_device.h"
#include "Config/engine_config_types.h"
#include "Logging/app_logger.h"
#include "Utils/cdsp_time.h"
#include "Utils/lock_free_ring_buffer.h"

static const logger_t g_logger = {"dsp.backend.coreaudio.playback"};

struct core_audio_playback {
  char device_name[256];
  int channels;
  double sample_rate;
  size_t chunk_size;
  bool exclusive;
  char sample_format[16];
  bool has_sample_format;
  binary_sample_format_t binary_format;

  AudioDeviceIOProcID io_proc_id;
  spsc_byte_ring_buffer_t* ring_buffer;
  uint8_t* write_buf;
  size_t write_buf_cap;
  size_t bytes_per_sample;
  size_t blockalign;

  AudioDeviceID opened_device_id;
  bool did_acquire_hog_mode;
  rate_change_watcher_t* rate_watcher;
  _Atomic bool is_device_alive;
  _Atomic bool is_paused;
  _Atomic bool stopped;
  _Atomic int active_callbacks;
};
/**
 * @brief Audio HAL IO callback for playback.
 *
 * Called by the CoreAudio HAL real-time thread to pull audio data
 * from the internal ring buffers and write it to the output device's buffers.
 *
 * @note This function runs on a real-time thread. It must be wait-free and must
 * not:
 *       - Allocate or free memory.
 *       - Take locks (mutexes).
 *       - Call any blocking APIs.
 *
 * @param inDevice The AudioObjectID of the output device.
 * @param inNow Time stamp of the current cycle.
 * @param inInputData Input buffer list (unused for playback).
 * @param inInputTime Time stamp of the input data.
 * @param outOutputData The buffer list to fill with audio data.
 * @param inOutputTime Time stamp of the output data.
 * @param inClientData Pointer to the core_audio_playback_t instance.
 * @return OSStatus noErr on success.
 */
static OSStatus playback_io_proc(AudioObjectID inDevice,
                                 const AudioTimeStamp* inNow,
                                 const AudioBufferList* inInputData,
                                 const AudioTimeStamp* inInputTime,
                                 AudioBufferList* outOutputData,
                                 const AudioTimeStamp* inOutputTime,
                                 void* inClientData) {
  (void)inDevice;
  (void)inNow;
  (void)inInputData;
  (void)inInputTime;
  (void)inOutputTime;
  core_audio_playback_t* playback = (core_audio_playback_t*)inClientData;
  if (!playback) return noErr;

  atomic_fetch_add_explicit(&playback->active_callbacks, 1,
                            memory_order_relaxed);
  if (!outOutputData || outOutputData->mNumberBuffers == 0 ||
      atomic_load_explicit(&playback->stopped, memory_order_relaxed)) {
    atomic_fetch_sub_explicit(&playback->active_callbacks, 1,
                              memory_order_relaxed);
    return noErr;
  }

  if (atomic_load_explicit(&playback->is_paused, memory_order_relaxed)) {
    for (UInt32 b = 0; b < outOutputData->mNumberBuffers; b++) {
      if (outOutputData->mBuffers[b].mData &&
          outOutputData->mBuffers[b].mDataByteSize > 0) {
        memset(outOutputData->mBuffers[b].mData, 0,
               outOutputData->mBuffers[b].mDataByteSize);
      }
    }
    atomic_fetch_sub_explicit(&playback->active_callbacks, 1,
                              memory_order_release);
    return noErr;
  }

  if (outOutputData->mNumberBuffers == 1) {
    uint8_t* dst = (uint8_t*)outOutputData->mBuffers[0].mData;
    size_t bytes_needed = (size_t)outOutputData->mBuffers[0].mDataByteSize;
    if (dst && bytes_needed > 0) {
      size_t copied = spsc_byte_ring_buffer_consume(playback->ring_buffer, dst,
                                                    bytes_needed);
      if (copied < bytes_needed) {
        memset(dst + copied, 0, bytes_needed - copied);
      }
    }
  } else {
    for (UInt32 b = 0; b < outOutputData->mNumberBuffers; b++) {
      if (outOutputData->mBuffers[b].mData &&
          outOutputData->mBuffers[b].mDataByteSize > 0) {
        memset(outOutputData->mBuffers[b].mData, 0,
               outOutputData->mBuffers[b].mDataByteSize);
      }
    }
  }

  atomic_fetch_sub_explicit(&playback->active_callbacks, 1,
                            memory_order_release);
  return noErr;
}

/**
 * @brief Check if pitch control is supported on the CoreAudio playback device.
 *
 * @param ctx Pointer to the CoreAudio playback instance.
 * @return true if supported, false otherwise.
 */
static bool core_audio_playback_pitch_control_supported(void* ctx) {
  (void)ctx;
  return false;
}

/**
 * @brief Apply pitch correction to the CoreAudio playback device.
 *
 * @param ctx Pointer to the CoreAudio playback instance.
 * @param multiplier Pitch multiplier factor.
 */
static void core_audio_playback_set_pitch(void* ctx, double multiplier) {
  (void)ctx;
  (void)multiplier;
}

/// Close the CoreAudio playback device and release HAL resources.
static void core_audio_playback_close(void* ctx) {
  core_audio_playback_t* playback = (core_audio_playback_t*)ctx;
  if (!playback) return;
  atomic_store_explicit(&playback->stopped, true, memory_order_release);
  if (playback->opened_device_id == 0 && playback->io_proc_id == NULL) return;
  logger_info(&g_logger, "Closing CoreAudio playback device");
  if (playback->rate_watcher) {
    rate_change_watcher_free(playback->rate_watcher);
    playback->rate_watcher = NULL;
  }
  if (playback->opened_device_id != 0) {
    core_audio_device_remove_alive_watcher(playback->opened_device_id,
                                           &playback->is_device_alive);
  }
  if (playback->opened_device_id != 0 && playback->io_proc_id != NULL) {
    core_audio_device_stop_and_destroy_ioproc(playback->opened_device_id,
                                              playback->io_proc_id,
                                              &playback->active_callbacks);
    playback->io_proc_id = NULL;
  }
  if (playback->write_buf) {
    free(playback->write_buf);
    playback->write_buf = NULL;
  }
  if (playback->did_acquire_hog_mode && playback->opened_device_id != 0) {
    core_audio_device_release_hog_mode(playback->opened_device_id);
    playback->did_acquire_hog_mode = false;
  }
  playback->opened_device_id = 0;
}

/// Open the CoreAudio playback device and initialize HAL IOProc.
static bool core_audio_playback_open(void* ctx, backend_error_t* err) {
  core_audio_playback_t* playback = (core_audio_playback_t*)ctx;
  if (!playback) return false;
  logger_info(&g_logger,
              "Opening CoreAudio playback device '%s' (sample_rate=%.0f, "
              "channels=%d, exclusive=%d)",
              playback->device_name[0] ? playback->device_name : "default",
              playback->sample_rate, playback->channels,
              playback->exclusive ? 1 : 0);
  core_audio_playback_close(playback);

  AudioDeviceID dev_id = core_audio_device_id_for_name(
      playback->device_name[0] ? playback->device_name : NULL,
      CORE_AUDIO_SCOPE_OUTPUT);
  if (dev_id == 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_DEVICE_NOT_FOUND,
                         "CoreAudio playback device not found");
    goto cleanup;
  }
  playback->opened_device_id = dev_id;

  // Attempt to acquire Hog Mode if exclusive access is requested.
  if (playback->exclusive) {
    playback->did_acquire_hog_mode = core_audio_device_acquire_hog_mode(dev_id);
    if (playback->did_acquire_hog_mode) {
      logger_info(&g_logger, "Acquired exclusive hog mode on playback device");
    } else {
      logger_warn(&g_logger,
                  "Failed to acquire exclusive hog mode on playback device");
    }
  }

  // Configure physical & virtual stream formats, buffer size, and format
  // mapping.
  if (!core_audio_device_configure_stream(
          dev_id, CORE_AUDIO_SCOPE_OUTPUT, playback->sample_rate,
          playback->sample_format, playback->has_sample_format,
          playback->channels, playback->chunk_size, &playback->binary_format,
          &playback->bytes_per_sample, &playback->blockalign)) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to configure playback stream format");
    goto cleanup;
  }

  if (!playback->write_buf ||
      playback->write_buf_cap <
          playback->blockalign * (size_t)playback->chunk_size * 2) {
    if (playback->write_buf) free(playback->write_buf);
    playback->write_buf_cap =
        playback->blockalign * (size_t)playback->chunk_size * 2;
    playback->write_buf = (uint8_t*)malloc(playback->write_buf_cap);
    if (!playback->write_buf) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                           "Failed to allocate write buffer");
      goto cleanup;
    }
  }

  if (playback->ring_buffer) {
    spsc_byte_ring_buffer_drain(playback->ring_buffer);
  }

  core_audio_device_add_alive_watcher(dev_id, &playback->is_device_alive);

  OSStatus status = AudioDeviceCreateIOProcID(dev_id, playback_io_proc,
                                              playback, &playback->io_proc_id);
  if (status != noErr) {
    logger_error(&g_logger, "Failed to create Audio HAL IOProc: status=%d",
                 (int)status);
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to create Audio HAL IOProc");
    goto cleanup;
  }

  atomic_store_explicit(&playback->stopped, false, memory_order_release);
  status = AudioDeviceStart(dev_id, playback->io_proc_id);
  if (status != noErr) {
    logger_error(&g_logger, "Failed to start Audio HAL device: status=%d",
                 (int)status);
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to start Audio HAL device");
    goto cleanup;
  }

  if (core_audio_device_has_nominal_sample_rate_property(dev_id)) {
    playback->rate_watcher =
        rate_change_watcher_create(dev_id, playback->sample_rate);
  }

  logger_info(
      &g_logger,
      "CoreAudio playback successfully opened and started via Audio HAL");
  return true;

cleanup:
  core_audio_playback_close(playback);
  return false;
}

static bool core_audio_playback_write(void* ctx, const audio_chunk_t* chunk,
                                      backend_error_t* err) {
  core_audio_playback_t* playback = (core_audio_playback_t*)ctx;
  if (!playback) return false;
  if (!atomic_load_explicit(&playback->is_device_alive, memory_order_acquire)) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                         "Playback device disconnected");
    return false;
  }
  return audio_backend_ring_buffer_write(
      playback->ring_buffer, playback->write_buf, playback->write_buf_cap,
      playback->blockalign, chunk, playback->binary_format,
      (size_t)playback->channels, 1, 1000, NULL, &playback->stopped,
      &playback->is_paused, NULL, err);
}

/// Get the current buffer level in frames.
static size_t core_audio_playback_get_buffer_level(void* ctx) {
  core_audio_playback_t* playback = (core_audio_playback_t*)ctx;
  if (!playback || !playback->ring_buffer || playback->blockalign == 0)
    return 0;
  return spsc_byte_ring_buffer_get_available_to_read(playback->ring_buffer) /
         playback->blockalign;
}

/// Get any pending sample rate change detected on the playback device.
static bool core_audio_playback_get_pending_rate_change(void* ctx,
                                                        double* out_rate) {
  core_audio_playback_t* playback = (core_audio_playback_t*)ctx;
  if (!playback) return false;
  return core_audio_device_check_rate_change(playback->opened_device_id,
                                             playback->rate_watcher,
                                             playback->sample_rate, out_rate);
}

/// Push zero samples into the playback ring buffer before real audio arrives.
static bool core_audio_playback_prefill_silence(void* ctx, size_t frames,
                                                backend_error_t* err) {
  core_audio_playback_t* playback = (core_audio_playback_t*)ctx;
  (void)err;
  if (!playback || frames == 0 || !playback->ring_buffer) return true;
  size_t bytes = frames * playback->blockalign;
  uint8_t zero_buf[512] = {0};
  while (bytes > 0) {
    size_t chunk_bytes = bytes < sizeof(zero_buf) ? bytes : sizeof(zero_buf);
    size_t written = spsc_byte_ring_buffer_write(playback->ring_buffer,
                                                 zero_buf, chunk_bytes);
    if (written == 0) break;
    bytes -= written;
  }
  return true;
}

/// Check if playback is currently paused.
static bool core_audio_playback_get_is_paused(void* ctx) {
  core_audio_playback_t* playback = (core_audio_playback_t*)ctx;
  return playback
             ? atomic_load_explicit(&playback->is_paused, memory_order_acquire)
             : false;
}

/// Set playback paused status.
static void core_audio_playback_set_is_paused(void* ctx, bool paused) {
  core_audio_playback_t* playback = (core_audio_playback_t*)ctx;
  if (playback) {
    atomic_store_explicit(&playback->is_paused, paused, memory_order_release);
  }
}

/// Destroy and free the CoreAudio playback backend.
static void core_audio_playback_stop(void* ctx) {
  core_audio_playback_t* playback = (core_audio_playback_t*)ctx;
  if (!playback) return;
  atomic_store_explicit(&playback->stopped, true, memory_order_release);
  if (playback->opened_device_id != 0 && playback->io_proc_id != NULL) {
    AudioDeviceStop(playback->opened_device_id, playback->io_proc_id);
  }
}

static void core_audio_playback_destroy(void* ctx) {
  core_audio_playback_t* playback = (core_audio_playback_t*)ctx;
  if (!playback) return;
  core_audio_playback_close(playback);
  if (playback->ring_buffer) {
    spsc_byte_ring_buffer_free(playback->ring_buffer);
    playback->ring_buffer = NULL;
  }
  if (playback->write_buf) {
    free(playback->write_buf);
    playback->write_buf = NULL;
  }
  free(playback);
}

/**
 * @brief Create a CoreAudio playback backend instance.
 *
 * @param config Pointer to the playback device configuration.
 * @param sample_rate The initial sample rate in Hz.
 * @param chunk_size The size of each audio chunk in frames.
 * @param full_duplex True if running in full duplex mode.
 * @param params Processing parameters.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return Pointer to the created playback_backend_t instance, or NULL on
 * failure.
 */
static playback_backend_t* core_audio_playback_create(
    const playback_device_config_t* config, int sample_rate, int chunk_size,
    bool full_duplex, processing_parameters_t* params, backend_error_t* err) {
  (void)full_duplex;
  (void)params;
  if (!config) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Config is NULL");
    return NULL;
  }
  core_audio_playback_t* playback =
      (core_audio_playback_t*)calloc(1, sizeof(core_audio_playback_t));
  if (!playback) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Out of memory");
    return NULL;
  }
  atomic_init(&playback->stopped, false);
  const char* config_device = playback_device_config_get_device(config);
  if (config_device && config_device[0] != '\0') {
    strncpy(playback->device_name, config_device,
            sizeof(playback->device_name) - 1);
  }
  int config_channels = playback_device_config_get_channels(config);
  playback->channels = config_channels;
  playback->sample_rate = (double)sample_rate;
  playback->chunk_size = chunk_size;
  playback->exclusive = playback_device_config_get_exclusive(config);

  coreaudio_sample_format_t fmt = playback_device_config_get_format(config);
  if (fmt != COREAUDIO_SAMPLE_FORMAT_INVALID) {
    const char* fmt_str = coreaudio_sample_format_to_string(fmt);
    strncpy(playback->sample_format, fmt_str,
            sizeof(playback->sample_format) - 1);
    playback->has_sample_format = true;
    playback->binary_format = coreaudio_sample_format_to_binary_format(fmt);
  } else {
    playback->binary_format = BINARY_SAMPLE_FORMAT_F32_LE;
  }

  playback->bytes_per_sample =
      sample_format_bytes_per_sample(playback->binary_format);
  if (playback->bytes_per_sample == 0) {
    playback->bytes_per_sample = sizeof(float);
  }
  playback->blockalign = (size_t)config_channels * playback->bytes_per_sample;
  size_t max_align = (size_t)config_channels * sizeof(double);
  size_t ring_size = max_align * (2 * (size_t)chunk_size + 2048);
  playback->ring_buffer = spsc_byte_ring_buffer_create(ring_size);
  if (!playback->ring_buffer) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Out of memory");
    core_audio_playback_destroy(playback);
    return NULL;
  }

  playback->write_buf_cap = playback->blockalign * (size_t)chunk_size * 2;
  playback->write_buf = NULL;

  atomic_init(&playback->is_device_alive, true);
  atomic_init(&playback->is_paused, false);
  atomic_init(&playback->active_callbacks, 0);

  playback_backend_t* backend =
      (playback_backend_t*)calloc(1, sizeof(playback_backend_t));
  if (!backend) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Out of memory");
    core_audio_playback_destroy(playback);
    return NULL;
  }
  backend->ctx = playback;
  backend->vtable = &g_core_audio_playback_vtable;
  return backend;
}

const playback_backend_vtable_t g_core_audio_playback_vtable = {
    .create = core_audio_playback_create,
    .open = core_audio_playback_open,
    .write = core_audio_playback_write,
    .close = core_audio_playback_close,
    .get_buffer_level = core_audio_playback_get_buffer_level,
    .get_pending_rate_change = core_audio_playback_get_pending_rate_change,
    .prefill_silence = core_audio_playback_prefill_silence,
    .get_is_paused = core_audio_playback_get_is_paused,
    .set_is_paused = core_audio_playback_set_is_paused,
    .pitch_control_supported = core_audio_playback_pitch_control_supported,
    .set_pitch = core_audio_playback_set_pitch,
    .stop = core_audio_playback_stop,
    .destroy = core_audio_playback_destroy};
#endif  // ENABLE_COREAUDIO
