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
//   - the AudioBufferList plus its per-channel raw data buffers are
//     preallocated in `open()` and reused for the lifetime of the unit;
//     the render callback only fills the existing struct.

#include "Backend/core_audio_capture.h"

#if defined(ENABLE_COREAUDIO)
#include <AudioToolbox/AudioToolbox.h>
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
  size_t channels;
  double sample_rate;
  size_t chunk_size;
  char sample_format[16];
  bool has_sample_format;

  AudioUnit audio_unit;
  spsc_byte_ring_buffer_t* ring_buffer;
  size_t bytes_per_sample;
  size_t blockalign;

  AudioBufferList* prealloc_buffer_list;
  void** prealloc_channel_data_pointers;
  int prealloc_bytes_per_channel_buffer;
  int callback_error_count;

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
 * @brief CoreAudio render callback for capturing audio.
 *
 * This function runs on a high-priority, real-time HAL thread. It must NOT
 * block, take locks, allocate memory, or invoke slow system APIs. It renders
 * audio samples from the AudioUnit into preallocated buffers and writes them to
 * lock-free SPSC rings.
 */
static OSStatus capture_callback(void* inRefCon,
                                 AudioUnitRenderActionFlags* ioActionFlags,
                                 const AudioTimeStamp* inTimeStamp,
                                 UInt32 inBusNumber, UInt32 inNumberFrames,
                                 AudioBufferList* ioData) {
  (void)inBusNumber;
  (void)ioData;
  core_audio_capture_t* capture = (core_audio_capture_t*)inRefCon;
  if (!capture) return noErr;

  atomic_fetch_add_explicit(&capture->active_callbacks, 1,
                            memory_order_relaxed);

  if (atomic_load_explicit(&capture->stopped, memory_order_relaxed) ||
      !capture->prealloc_buffer_list ||
      !capture->prealloc_channel_data_pointers || !capture->audio_unit) {
    atomic_fetch_sub_explicit(&capture->active_callbacks, 1,
                              memory_order_release);
    return noErr;
  }

  // Restore the size of the preallocated buffer list's buffers, since
  // AudioUnitRender may modify mDataByteSize during invocation to report actual
  // bytes written.
  AudioBufferList* buffer_list = capture->prealloc_buffer_list;
  uint32_t prealloc_size = (uint32_t)capture->prealloc_bytes_per_channel_buffer;
  for (UInt32 i = 0; i < buffer_list->mNumberBuffers; i++) {
    buffer_list->mBuffers[i].mDataByteSize = prealloc_size;
  }

  // Render the audio from the hardware into our preallocated buffers.
  OSStatus status =
      AudioUnitRender(capture->audio_unit, ioActionFlags, inTimeStamp, 1,
                      inNumberFrames, buffer_list);
  if (status != noErr) {
    if (capture->callback_error_count < 3) {
      capture->callback_error_count++;
    }
    atomic_fetch_sub_explicit(&capture->active_callbacks, 1,
                              memory_order_relaxed);
    return noErr;
  }

  size_t bytes_to_write = (size_t)buffer_list->mBuffers[0].mDataByteSize;
  const uint8_t* byte_ptr =
      (const uint8_t*)capture->prealloc_channel_data_pointers[0];
  spsc_byte_ring_buffer_write(capture->ring_buffer, byte_ptr, bytes_to_write);

  // Signal the semaphore to wake up the consumer thread waiting for new data.
  if (capture->semaphore) {
    cdsp_sem_signal(capture->semaphore);
  }

  atomic_fetch_sub_explicit(&capture->active_callbacks, 1,
                            memory_order_release);
  return noErr;
}

// MARK: - Render-callback storage

/**
 * @brief Helper function to free preallocated render buffers.
 */
static void deallocate_render_buffers(core_audio_capture_t* capture) {
  if (capture->prealloc_channel_data_pointers) {
    if (capture->prealloc_channel_data_pointers[0]) {
      free(capture->prealloc_channel_data_pointers[0]);
    }
    free(capture->prealloc_channel_data_pointers);
    capture->prealloc_channel_data_pointers = NULL;
  }
  if (capture->prealloc_buffer_list) {
    free(capture->prealloc_buffer_list);
    capture->prealloc_buffer_list = NULL;
  }
  capture->prealloc_bytes_per_channel_buffer = 0;
}

/**
 * @brief Helper function to preallocate the AudioBufferList and internal raw
 * buffers.
 */
static bool allocate_render_buffers(core_audio_capture_t* capture) {
  deallocate_render_buffers(capture);

  int buffer_frames = (int)capture->chunk_size;
  if (capture->opened_device_id != 0) {
    uint32_t actual_size = 0;
    if (core_audio_device_get_buffer_frame_size(
            capture->opened_device_id, CORE_AUDIO_SCOPE_INPUT, &actual_size)) {
      if ((int)actual_size > buffer_frames) buffer_frames = (int)actual_size;
    }
  }

  int bytes_per_buffer =
      buffer_frames * (int)capture->channels * (int)sizeof(float);
  size_t list_byte_count =
      offsetof(AudioBufferList, mBuffers) + sizeof(AudioBuffer);
  capture->prealloc_buffer_list = (AudioBufferList*)calloc(1, list_byte_count);
  capture->prealloc_channel_data_pointers = (void**)calloc(1, sizeof(void*));
  if (!capture->prealloc_buffer_list ||
      !capture->prealloc_channel_data_pointers) {
    deallocate_render_buffers(capture);
    return false;
  }

  capture->prealloc_channel_data_pointers[0] = calloc(1, bytes_per_buffer);
  if (!capture->prealloc_channel_data_pointers[0]) {
    deallocate_render_buffers(capture);
    return false;
  }
  capture->prealloc_buffer_list->mBuffers[0].mNumberChannels =
      (UInt32)capture->channels;
  capture->prealloc_buffer_list->mBuffers[0].mDataByteSize =
      (UInt32)bytes_per_buffer;
  capture->prealloc_buffer_list->mBuffers[0].mData =
      capture->prealloc_channel_data_pointers[0];
  capture->prealloc_buffer_list->mNumberBuffers = 1;
  capture->prealloc_bytes_per_channel_buffer = bytes_per_buffer;
  return true;
}

/// Close the CoreAudio capture device and release AudioUnit resources.
static void core_audio_capture_close(void* ctx) {
  core_audio_capture_t* capture = (core_audio_capture_t*)ctx;
  if (!capture) return;
  atomic_store_explicit(&capture->stopped, true, memory_order_release);
  if (!capture->audio_unit && capture->opened_device_id == 0) return;
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
  if (capture->audio_unit) {
    AudioOutputUnitStop(capture->audio_unit);
    AURenderCallbackStruct null_cb = {0};
    AudioUnitSetProperty(capture->audio_unit,
                         kAudioOutputUnitProperty_SetInputCallback,
                         kAudioUnitScope_Global, 0, &null_cb, sizeof(null_cb));
  }
  int timeout_count = 1000;  // 500ms max (1000 * 500us)
  while (atomic_load_explicit(&capture->active_callbacks,
                              memory_order_acquire) > 0 &&
         timeout_count-- > 0) {
    usleep(500);
  }
  if (capture->audio_unit) {
    AudioComponentInstanceDispose(capture->audio_unit);
    capture->audio_unit = NULL;
  }
  deallocate_render_buffers(capture);
  if (capture->read_scratch) {
    free(capture->read_scratch);
    capture->read_scratch = NULL;
  }
  capture->opened_device_id = 0;
}

/// Open the CoreAudio capture device and initialize the AudioUnit and render
/// buffers.
static bool core_audio_capture_open(void* ctx, backend_error_t* err) {
  core_audio_capture_t* capture = (core_audio_capture_t*)ctx;
  if (!capture) return false;
  core_audio_capture_close(capture);

  if (capture->ring_buffer) {
    spsc_byte_ring_buffer_drain(capture->ring_buffer);
  }

  // Set up component query for HAL Output Audio Unit.
  AudioComponentDescription desc = {
      .componentType = kAudioUnitType_Output,
      .componentSubType = kAudioUnitSubType_HALOutput,
      .componentManufacturer = kAudioUnitManufacturer_Apple,
      .componentFlags = 0,
      .componentFlagsMask = 0};

  AudioComponent comp = AudioComponentFindNext(NULL, &desc);
  if (!comp) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_DEVICE_NOT_FOUND,
                         "No HAL output component found");
    goto cleanup;
  }

  // Create the AudioUnit instance.
  OSStatus status = AudioComponentInstanceNew(comp, &capture->audio_unit);
  if (status != noErr || !capture->audio_unit) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to create AudioUnit");
    goto cleanup;
  }

  // Enable Input scope (bus 1) on the HAL AudioUnit.
  UInt32 enable_input = 1;
  status = AudioUnitSetProperty(
      capture->audio_unit, kAudioOutputUnitProperty_EnableIO,
      kAudioUnitScope_Input, 1, &enable_input, sizeof(enable_input));
  if (status != noErr) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to enable input");
    goto cleanup;
  }

  // Disable Output scope (bus 0) on the HAL AudioUnit since we are only
  // capturing.
  UInt32 disable_output = 0;
  status = AudioUnitSetProperty(
      capture->audio_unit, kAudioOutputUnitProperty_EnableIO,
      kAudioUnitScope_Output, 0, &disable_output, sizeof(disable_output));
  if (status != noErr) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to disable output");
    goto cleanup;
  }

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
  if (dev_id != 0) {
    // Bind the AudioUnit to the discovered HAL Device ID.
    AudioUnitSetProperty(capture->audio_unit,
                         kAudioOutputUnitProperty_CurrentDevice,
                         kAudioUnitScope_Global, 0, &dev_id, sizeof(dev_id));
    bool physical_format_set = false;
    if (capture->has_sample_format) {
      if (core_audio_device_set_matching_physical_format(
              dev_id, CORE_AUDIO_SCOPE_INPUT, capture->sample_rate,
              capture->sample_format, (int)capture->channels)) {
        physical_format_set = true;
      } else {
        if (err)
          backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                             "Failed to find matching physical capture format");
        goto cleanup;
      }
    }
    if (!physical_format_set) {
      core_audio_device_set_nominal_sample_rate(dev_id, capture->sample_rate);
    }
    // Set device buffer frame size matching target chunk size.
    core_audio_device_set_buffer_frame_size(
        dev_id, (uint32_t)capture->chunk_size, CORE_AUDIO_SCOPE_INPUT);

    core_audio_device_add_alive_watcher(dev_id, &capture->is_device_alive);
  }

  // Configure the client stream format on the output scope of the input bus
  // (bus 1) to interleaved float32 (matching upstream CamillaDSP).
  AudioStreamBasicDescription stream_format =
      core_audio_device_float32_stream_format(capture->sample_rate,
                                              (int)capture->channels);
  status = AudioUnitSetProperty(
      capture->audio_unit, kAudioUnitProperty_StreamFormat,
      kAudioUnitScope_Output, 1, &stream_format, sizeof(stream_format));
  if (status != noErr) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to set stream format");
    goto cleanup;
  }

  // Set the maximum frames per slice on the AudioUnit.
  UInt32 max_frames = (UInt32)capture->chunk_size;
  if (dev_id != 0) {
    uint32_t actual_size = 0;
    if (core_audio_device_get_buffer_frame_size(dev_id, CORE_AUDIO_SCOPE_INPUT,
                                                &actual_size)) {
      if ((int)actual_size > (int)max_frames) max_frames = actual_size;
    }
  }
  AudioUnitSetProperty(
      capture->audio_unit, kAudioUnitProperty_MaximumFramesPerSlice,
      kAudioUnitScope_Global, 0, &max_frames, sizeof(max_frames));

  // Preallocate render buffers.
  if (!allocate_render_buffers(capture)) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to allocate render buffers");
    goto cleanup;
  }
  if (!capture->read_scratch ||
      capture->read_scratch_cap <
          capture->blockalign * (size_t)capture->chunk_size * 2) {
    if (capture->read_scratch) free(capture->read_scratch);
    capture->read_scratch_cap =
        capture->blockalign * (size_t)capture->chunk_size * 2;
    capture->read_scratch = (uint8_t*)malloc(capture->read_scratch_cap);
  }
  if (!capture->read_scratch) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to allocate read scratch buffer");
    goto cleanup;
  }

  // Register the real-time callback.
  AURenderCallbackStruct cb = {.inputProc = capture_callback,
                               .inputProcRefCon = capture};
  status = AudioUnitSetProperty(capture->audio_unit,
                                kAudioOutputUnitProperty_SetInputCallback,
                                kAudioUnitScope_Global, 0, &cb, sizeof(cb));
  if (status != noErr) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to set callback");
    goto cleanup;
  }

  // Initialize and start the AudioUnit.
  status = AudioUnitInitialize(capture->audio_unit);
  if (status != noErr) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to initialize AudioUnit");
    goto cleanup;
  }

  if (dev_id != 0) {
    atomic_store_explicit(
        &capture->pitch_control_active,
        core_audio_device_select_adjustable_clock_source(dev_id),
        memory_order_release);
  }

  atomic_store_explicit(&capture->stopped, false, memory_order_release);
  status = AudioOutputUnitStart(capture->audio_unit);
  if (status != noErr) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to start AudioUnit");
    goto cleanup;
  }

  if (dev_id != 0 &&
      core_audio_device_has_nominal_sample_rate_property(dev_id)) {
    capture->rate_watcher =
        rate_change_watcher_create(dev_id, capture->sample_rate);
  }

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
      capture->blockalign, frames_to_read, BINARY_SAMPLE_FORMAT_F32_LE,
      capture->channels, 0, NULL, &capture->stopped, NULL, chunk, err);
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
  if (capture->audio_unit) {
    AudioOutputUnitStop(capture->audio_unit);
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
  size_t config_channels = capture_device_config_get_channels(config);
  capture->channels = config_channels;
  capture->sample_rate = (double)sample_rate;
  capture->chunk_size = (size_t)chunk_size;

  coreaudio_sample_format_t fmt = capture_device_config_get_format(config);
  if (fmt != COREAUDIO_SAMPLE_FORMAT_INVALID) {
    const char* fmt_str = coreaudio_sample_format_to_string(fmt);
    strncpy(capture->sample_format, fmt_str,
            sizeof(capture->sample_format) - 1);
    capture->has_sample_format = true;
  }

  capture->bytes_per_sample = sizeof(float);
  capture->blockalign = config_channels * sizeof(float);
  size_t ring_size = capture->blockalign * (2 * (size_t)chunk_size + 2048);
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
  atomic_init(&capture->active_callbacks, 0);

  bool pitch_active = false;
  AudioDeviceID dev_id = core_audio_device_id_for_name(
      capture->device_name[0] ? capture->device_name : NULL,
      CORE_AUDIO_SCOPE_INPUT);
  if (dev_id != 0 &&
      core_audio_device_has_nominal_sample_rate_property(dev_id)) {
    pitch_active = core_audio_device_select_adjustable_clock_source(dev_id);
  }
  atomic_init(&capture->pitch_control_active, pitch_active);

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
