// CoreAudio capture backend for macOS
//
// Real-time discipline
// --------------------
// The render callback runs on a high-priority audio thread driven by
// CoreAudio. It is absolutely forbidden to take locks, allocate, or
// otherwise call into the Swift runtime in a way that could block. To
// honour that:
//   - sample rings are SPSC instances —
//     producer and consumer are wait-free, no lock.
//   - the render callback writes directly into the pre-allocated SPSC rings
//     from the AudioBufferList provided by CoreAudio HAL.

#include "Backend/core_audio_capture.h"

#if defined(ENABLE_COREAUDIO)
#include <CoreAudio/CoreAudio.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef ENABLE_ACCELERATE
#include <Accelerate/Accelerate.h>
#endif

#include "Audio/audio_chunk.h"
#include "Backend/backend_error.h"
#include "Backend/core_audio_device.h"
#include "Config/engine_config_types.h"
#include "Engine/cdsp_sem.h"
#include "Logging/app_logger.h"
#include "Utils/lock_free_ring_buffer.h"

static const logger_t g_logger = {"dsp.backend.coreaudio.capture"};

struct core_audio_capture {
  char device_name[256];
  int channels;
  double sample_rate;
  size_t chunk_size;
  char sample_format[16];
  bool has_sample_format;
  binary_sample_format_t binary_format;

  AudioDeviceIOProcID io_proc_id;
  spsc_byte_ring_buffer_t* ring_buffer;
  size_t bytes_per_sample;
  size_t blockalign;

  AudioDeviceID opened_device_id;
  rate_change_watcher_t* rate_watcher;
  _Atomic bool pitch_control_active;
  _Atomic bool is_device_alive;

  uint8_t* read_scratch;
  size_t read_scratch_cap;
  cdsp_sem_t semaphore;
  _Atomic bool stopped;
  _Atomic int active_callbacks;
};

/**
 * @brief Audio HAL IO callback for capturing audio.
 *
 * This function runs on a high-priority, real-time HAL thread. It must NOT
 * block, take locks, allocate memory, or invoke slow system APIs. It reads
 * audio samples directly from CoreAudio HAL and writes them to lock-free SPSC
 * rings.
 */
static OSStatus capture_io_proc(AudioObjectID inDevice,
                                const AudioTimeStamp* inNow,
                                const AudioBufferList* inInputData,
                                const AudioTimeStamp* inInputTime,
                                AudioBufferList* outOutputData,
                                const AudioTimeStamp* inOutputTime,
                                void* inClientData) {
  (void)inDevice;
  (void)inNow;
  (void)inInputTime;
  (void)outOutputData;
  (void)inOutputTime;
  core_audio_capture_t* capture = (core_audio_capture_t*)inClientData;
  if (!capture) return noErr;

  atomic_fetch_add_explicit(&capture->active_callbacks, 1,
                            memory_order_relaxed);
  if (atomic_load_explicit(&capture->stopped, memory_order_relaxed) ||
      !inInputData || inInputData->mNumberBuffers == 0 ||
      !inInputData->mBuffers[0].mData) {
    atomic_fetch_sub_explicit(&capture->active_callbacks, 1,
                              memory_order_relaxed);
    return noErr;
  }

  if (inInputData->mNumberBuffers == 1) {
    const uint8_t* byte_ptr = (const uint8_t*)inInputData->mBuffers[0].mData;
    size_t bytes_to_write = (size_t)inInputData->mBuffers[0].mDataByteSize;
    if (byte_ptr && bytes_to_write > 0) {
      spsc_byte_ring_buffer_write(capture->ring_buffer, byte_ptr,
                                  bytes_to_write);
      if (capture->semaphore) {
        cdsp_sem_signal(capture->semaphore);
      }
    }
  }

  atomic_fetch_sub_explicit(&capture->active_callbacks, 1,
                            memory_order_release);
  return noErr;
}

/// Close the CoreAudio capture device and release HAL resources.
static void core_audio_capture_close(void* ctx) {
  core_audio_capture_t* capture = (core_audio_capture_t*)ctx;
  if (!capture) return;
  atomic_store_explicit(&capture->stopped, true, memory_order_release);
  if (capture->opened_device_id == 0 && capture->io_proc_id == NULL) return;
  logger_info(&g_logger, "Closing CoreAudio capture device");
  if (capture->semaphore) {
    cdsp_sem_signal(capture->semaphore);
  }
  if (capture->rate_watcher) {
    rate_change_watcher_free(capture->rate_watcher);
    capture->rate_watcher = NULL;
  }
  if (capture->opened_device_id != 0) {
    core_audio_device_remove_alive_watcher(capture->opened_device_id,
                                           &capture->is_device_alive);
  }
  if (capture->opened_device_id != 0 && capture->io_proc_id != NULL) {
    core_audio_device_stop_and_destroy_ioproc(capture->opened_device_id,
                                              capture->io_proc_id,
                                              &capture->active_callbacks);
    capture->io_proc_id = NULL;
  }
  if (capture->read_scratch) {
    free(capture->read_scratch);
    capture->read_scratch = NULL;
  }
  capture->opened_device_id = 0;
}

/// Open the CoreAudio capture device and initialize Audio HAL IOProc.
static bool core_audio_capture_open(void* ctx, backend_error_t* err) {
  core_audio_capture_t* capture = (core_audio_capture_t*)ctx;
  if (!capture) return false;
  core_audio_capture_close(capture);

  AudioDeviceID dev_id = core_audio_device_id_for_name(
      capture->device_name[0] ? capture->device_name : NULL,
      CORE_AUDIO_SCOPE_INPUT);
  if (dev_id == 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_DEVICE_NOT_FOUND,
                         "CoreAudio capture device not found");
    goto cleanup;
  }
  capture->opened_device_id = dev_id;

  // Configure physical & virtual stream formats, buffer size, and format
  // mapping.
  if (!core_audio_device_configure_stream(
          dev_id, CORE_AUDIO_SCOPE_INPUT, capture->sample_rate,
          capture->sample_format, capture->has_sample_format, capture->channels,
          capture->chunk_size, &capture->binary_format,
          &capture->bytes_per_sample, &capture->blockalign)) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to configure capture stream format");
    goto cleanup;
  }

  if (!capture->read_scratch ||
      capture->read_scratch_cap <
          capture->blockalign * (size_t)capture->chunk_size * 2) {
    if (capture->read_scratch) free(capture->read_scratch);
    capture->read_scratch_cap =
        capture->blockalign * (size_t)capture->chunk_size * 2;
    capture->read_scratch = (uint8_t*)malloc(capture->read_scratch_cap);
    if (!capture->read_scratch) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                           "Failed to allocate read scratch buffer");
      goto cleanup;
    }
  }

  if (capture->ring_buffer) {
    spsc_byte_ring_buffer_drain(capture->ring_buffer);
  }

  core_audio_device_add_alive_watcher(dev_id, &capture->is_device_alive);

  OSStatus status = AudioDeviceCreateIOProcID(dev_id, capture_io_proc, capture,
                                              &capture->io_proc_id);
  if (status != noErr) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to create Audio HAL IOProc");
    goto cleanup;
  }

  atomic_store_explicit(&capture->stopped, false, memory_order_release);
  status = AudioDeviceStart(dev_id, capture->io_proc_id);
  if (status != noErr) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to start Audio HAL device");
    goto cleanup;
  }

  if (core_audio_device_has_nominal_sample_rate_property(dev_id)) {
    capture->rate_watcher =
        rate_change_watcher_create(dev_id, capture->sample_rate);
  }
  atomic_store_explicit(
      &capture->pitch_control_active,
      core_audio_device_select_adjustable_clock_source(dev_id),
      memory_order_release);

  return true;

cleanup:
  core_audio_capture_close(capture);
  return false;
}

static bool core_audio_capture_read(void* ctx, size_t frames,
                                    audio_chunk_t* chunk,
                                    backend_error_t* err) {
  core_audio_capture_t* capture = (core_audio_capture_t*)ctx;
  if (!capture) return false;
  // Verify that the hardware device is still alive using atomic access.
  if (!atomic_load_explicit(&capture->is_device_alive, memory_order_acquire)) {
    logger_warn(&g_logger,
                "CoreAudio capture read failed: device is disconnected");
    if (err)
      backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                         "Capture device disconnected");
    return false;
  }
  size_t frames_to_read = (frames > (size_t)capture->chunk_size)
                              ? (size_t)capture->chunk_size
                              : frames;
  return audio_backend_ring_buffer_read(
      capture->ring_buffer, capture->read_scratch, capture->read_scratch_cap,
      capture->blockalign, frames_to_read, capture->binary_format,
      (size_t)capture->channels, 0, NULL, &capture->stopped, NULL, chunk, err);
}

/// Get any pending sample rate change detected on the capture device.
static bool core_audio_capture_get_pending_rate_change(void* ctx,
                                                       double* out_rate) {
  core_audio_capture_t* capture = (core_audio_capture_t*)ctx;
  if (!capture) return false;
  return core_audio_device_check_rate_change(capture->opened_device_id,
                                             capture->rate_watcher,
                                             capture->sample_rate, out_rate);
}

/// Check if clock-pitch control is supported on the capture device.
static bool core_audio_capture_pitch_control_supported(void* ctx) {
  core_audio_capture_t* capture = (core_audio_capture_t*)ctx;
  return capture ? atomic_load_explicit(&capture->pitch_control_active,
                                        memory_order_acquire)
                 : false;
}

/// Apply a clock-pitch correction to the capture device.
static void core_audio_capture_set_pitch(void* ctx, double multiplier) {
  core_audio_capture_t* capture = (core_audio_capture_t*)ctx;
  if (!capture ||
      !atomic_load_explicit(&capture->pitch_control_active,
                            memory_order_acquire) ||
      capture->opened_device_id == 0)
    return;
  core_audio_device_set_pitch(capture->opened_device_id, multiplier);
}

/**
 * @brief Wait for new samples to become available.
 *
 * @param ctx Pointer to the CoreAudio capture instance.
 * @param timeout_ms Timeout in milliseconds.
 * @return true if data is available, false if timed out or error occurred.
 */
static bool core_audio_capture_wait(void* ctx, uint32_t timeout_ms) {
  core_audio_capture_t* capture = (core_audio_capture_t*)ctx;
  if (!capture || !capture->semaphore) return false;
  if (atomic_load_explicit(&capture->stopped, memory_order_acquire))
    return false;
  return cdsp_sem_timedwait(capture->semaphore, timeout_ms);
}

/**
 * @brief Set the paused state of the capture backend.
 *
 * @param ctx Pointer to the CoreAudio capture instance.
 * @param paused true to pause, false to resume.
 */
static void core_audio_capture_set_is_paused(void* ctx, bool paused) {
  (void)ctx;
  (void)paused;
}

/**
 * @brief Stop the CoreAudio capture device.
 *
 * @param ctx Pointer to the CoreAudio capture instance.
 */
static void core_audio_capture_stop(void* ctx) {
  core_audio_capture_t* capture = (core_audio_capture_t*)ctx;
  if (!capture) return;
  atomic_store_explicit(&capture->stopped, true, memory_order_release);
  if (capture->opened_device_id != 0 && capture->io_proc_id != NULL) {
    AudioDeviceStop(capture->opened_device_id, capture->io_proc_id);
  }
  if (capture->semaphore) {
    cdsp_sem_signal(capture->semaphore);
  }
}

/// Destroy and free the CoreAudio capture backend.
static void core_audio_capture_destroy(void* ctx) {
  core_audio_capture_t* capture = (core_audio_capture_t*)ctx;
  if (!capture) return;
  core_audio_capture_close(capture);
  if (capture->read_scratch) {
    free(capture->read_scratch);
    capture->read_scratch = NULL;
  }
  if (capture->ring_buffer) {
    spsc_byte_ring_buffer_free(capture->ring_buffer);
    capture->ring_buffer = NULL;
  }
  if (capture->semaphore) {
    cdsp_sem_destroy(capture->semaphore);
    capture->semaphore = NULL;
  }
  free(capture);
}

/**
 * @brief Create a CoreAudio capture backend instance.
 *
 * @param config Configuration for the capture device.
 * @param sample_rate Target sample rate in Hz.
 * @param chunk_size Size of audio chunks to read.
 * @param full_duplex True if running in full duplex mode.
 * @param params Processing parameters.
 * @param err Pointer to backend error structure to report errors.
 * @return Pointer to the created capture_backend_t, or NULL on failure.
 */
static capture_backend_t* core_audio_capture_create(
    const capture_device_config_t* config, int sample_rate, int chunk_size,
    bool full_duplex, processing_parameters_t* params, backend_error_t* err) {
  (void)full_duplex;
  (void)params;
  if (!config) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Config is NULL");
    return NULL;
  }
  core_audio_capture_t* capture =
      (core_audio_capture_t*)calloc(1, sizeof(core_audio_capture_t));
  if (!capture) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Out of memory");
    return NULL;
  }
  capture->semaphore = cdsp_sem_create();
  if (!capture->semaphore) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to create semaphore");
    core_audio_capture_destroy(capture);
    return NULL;
  }
  const char* config_device = capture_device_config_get_device(config);
  if (config_device && config_device[0] != '\0') {
    strncpy(capture->device_name, config_device,
            sizeof(capture->device_name) - 1);
  }
  int config_channels = capture_device_config_get_channels(config);
  capture->channels = config_channels;
  capture->sample_rate = (double)sample_rate;
  capture->chunk_size = chunk_size;

  coreaudio_sample_format_t fmt = capture_device_config_get_format(config);
  if (fmt != COREAUDIO_SAMPLE_FORMAT_INVALID) {
    const char* fmt_str = coreaudio_sample_format_to_string(fmt);
    strncpy(capture->sample_format, fmt_str,
            sizeof(capture->sample_format) - 1);
    capture->has_sample_format = true;
    capture->binary_format = coreaudio_sample_format_to_binary_format(fmt);
  } else {
    capture->binary_format = BINARY_SAMPLE_FORMAT_F32_LE;
  }

  capture->bytes_per_sample =
      sample_format_bytes_per_sample(capture->binary_format);
  if (capture->bytes_per_sample == 0) {
    capture->bytes_per_sample = sizeof(float);
  }
  capture->blockalign = (size_t)config_channels * capture->bytes_per_sample;
  size_t max_align = (size_t)config_channels * sizeof(double);
  size_t ring_size = max_align * (2 * (size_t)chunk_size + 2048);
  capture->ring_buffer = spsc_byte_ring_buffer_create(ring_size);
  if (!capture->ring_buffer) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Out of memory");
    core_audio_capture_destroy(capture);
    return NULL;
  }

  capture->read_scratch_cap = capture->blockalign * (size_t)chunk_size * 2;
  capture->read_scratch = NULL;

  atomic_init(&capture->is_device_alive, true);
  atomic_init(&capture->stopped, false);

  AudioDeviceID dev_id = core_audio_device_id_for_name(
      capture->device_name[0] ? capture->device_name : NULL,
      CORE_AUDIO_SCOPE_INPUT);
  if (dev_id != 0 &&
      core_audio_device_has_nominal_sample_rate_property(dev_id)) {
    capture->pitch_control_active =
        core_audio_device_select_adjustable_clock_source(dev_id);
  } else {
    capture->pitch_control_active = false;
  }

  capture_backend_t* backend =
      (capture_backend_t*)calloc(1, sizeof(capture_backend_t));
  if (!backend) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Out of memory");
    core_audio_capture_destroy(capture);
    return NULL;
  }
  backend->ctx = capture;
  backend->vtable = &g_core_audio_capture_vtable;
  backend->is_realtime = true;
  return backend;
}

const capture_backend_vtable_t g_core_audio_capture_vtable = {
    .create = core_audio_capture_create,
    .open = core_audio_capture_open,
    .read = core_audio_capture_read,
    .close = core_audio_capture_close,
    .get_pending_rate_change = core_audio_capture_get_pending_rate_change,
    .is_pitch_control_supported = core_audio_capture_pitch_control_supported,
    .set_pitch = core_audio_capture_set_pitch,
    .wait_for_data = core_audio_capture_wait,
    .set_is_paused = core_audio_capture_set_is_paused,
    .stop = core_audio_capture_stop,
    .destroy = core_audio_capture_destroy};
#endif  // ENABLE_COREAUDIO
