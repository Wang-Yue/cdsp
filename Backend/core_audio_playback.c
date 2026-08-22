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
//     provided by CoreAudio AudioUnit, consuming from the pre-allocated SPSC
//     rings.

#include "Backend/core_audio_playback.h"

#if defined(ENABLE_COREAUDIO)
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "Audio/audio_chunk.h"
#include "Backend/backend_error.h"
#include "Backend/core_audio_device.h"
#include "Config/engine_config_types.h"
#include "Logging/app_logger.h"
#include "Utils/lock_free_ring_buffer.h"

static const logger_t g_logger = {"dsp.backend.coreaudio.playback"};

struct core_audio_playback {
  char device_name[256];
  size_t channels;
  double sample_rate;
  size_t chunk_size;
  bool exclusive;
  char sample_format[16];
  bool has_sample_format;

  AudioUnit audio_unit;
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
 * @brief CoreAudio render callback for playback.
 *
 * Called by the CoreAudio real-time thread to pull audio data
 * from the internal ring buffers and write it to the output device's buffers.
 *
 * @note This function runs on a real-time thread. It must be wait-free and must
 * not:
 *       - Allocate or free memory.
 *       - Take locks (mutexes).
 *       - Call any blocking APIs.
 */
static OSStatus playback_callback(void* inRefCon,
                                  AudioUnitRenderActionFlags* ioActionFlags,
                                  const AudioTimeStamp* inTimeStamp,
                                  UInt32 inBusNumber, UInt32 inNumberFrames,
                                  AudioBufferList* ioData) {
  (void)ioActionFlags;
  (void)inTimeStamp;
  (void)inBusNumber;
  core_audio_playback_t* playback = (core_audio_playback_t*)inRefCon;
  if (!playback) return noErr;

  atomic_fetch_add_explicit(&playback->active_callbacks, 1,
                            memory_order_relaxed);
  if (!ioData || ioData->mNumberBuffers == 0 || !ioData->mBuffers[0].mData ||
      atomic_load_explicit(&playback->stopped, memory_order_relaxed)) {
    atomic_fetch_sub_explicit(&playback->active_callbacks, 1,
                              memory_order_relaxed);
    return noErr;
  }

  if (atomic_load_explicit(&playback->is_paused, memory_order_relaxed)) {
    for (UInt32 b = 0; b < ioData->mNumberBuffers; b++) {
      if (ioData->mBuffers[b].mData) {
        memset(ioData->mBuffers[b].mData, 0, ioData->mBuffers[b].mDataByteSize);
      }
    }
    atomic_fetch_sub_explicit(&playback->active_callbacks, 1,
                              memory_order_release);
    return noErr;
  }

  size_t frame_count = (size_t)inNumberFrames;
  uint8_t* dst = (uint8_t*)ioData->mBuffers[0].mData;
  if (dst) {
    size_t bytes_needed = frame_count * playback->blockalign;
    size_t copied =
        spsc_byte_ring_buffer_consume(playback->ring_buffer, dst, bytes_needed);
    if (copied < bytes_needed) {
      memset(dst + copied, 0, bytes_needed - copied);
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

/// Close the CoreAudio playback device and release AudioUnit resources.
static void core_audio_playback_close(void* ctx) {
  core_audio_playback_t* playback = (core_audio_playback_t*)ctx;
  if (!playback) return;
  atomic_store_explicit(&playback->stopped, true, memory_order_release);
  if (!playback->audio_unit && playback->opened_device_id == 0) return;
  logger_info(&g_logger, "Closing CoreAudio playback device");
  if (playback->rate_watcher) {
    rate_change_watcher_free(playback->rate_watcher);
    playback->rate_watcher = NULL;
  }
  if (playback->opened_device_id != 0) {
    core_audio_device_remove_alive_watcher(playback->opened_device_id,
                                           &playback->is_device_alive);
  }
  if (playback->audio_unit) {
    AudioOutputUnitStop(playback->audio_unit);
    AURenderCallbackStruct null_cb = {0};
    AudioUnitSetProperty(playback->audio_unit,
                         kAudioUnitProperty_SetRenderCallback,
                         kAudioUnitScope_Input, 0, &null_cb, sizeof(null_cb));
  }
  int timeout_count = 1000;  // 500ms max (1000 * 500us)
  while (atomic_load_explicit(&playback->active_callbacks,
                              memory_order_acquire) > 0 &&
         timeout_count-- > 0) {
    usleep(500);
  }
  if (playback->audio_unit) {
    AudioComponentInstanceDispose(playback->audio_unit);
    playback->audio_unit = NULL;
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

/// Open the CoreAudio playback device and initialize output AudioUnit.
static bool core_audio_playback_open(void* ctx, backend_error_t* err) {
  core_audio_playback_t* playback = (core_audio_playback_t*)ctx;
  if (!playback) return false;
  logger_info(&g_logger,
              "Opening CoreAudio playback device '%s' (sample_rate=%.0f, "
              "channels=%zu, exclusive=%d)",
              playback->device_name[0] ? playback->device_name : "default",
              playback->sample_rate, playback->channels,
              playback->exclusive ? 1 : 0);
  core_audio_playback_close(playback);

  if (playback->ring_buffer) {
    spsc_byte_ring_buffer_drain(playback->ring_buffer);
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

  AudioComponentDescription desc = {
      .componentType = kAudioUnitType_Output,
      .componentSubType = kAudioUnitSubType_HALOutput,
      .componentManufacturer = kAudioUnitManufacturer_Apple,
      .componentFlags = 0,
      .componentFlagsMask = 0};

  AudioComponent comp = AudioComponentFindNext(NULL, &desc);
  if (!comp) {
    logger_error(&g_logger,
                 "No HAL output component found for CoreAudio playback");
    if (err)
      backend_error_init(err, BACKEND_ERROR_DEVICE_NOT_FOUND,
                         "No HAL output component found");
    goto cleanup;
  }

  OSStatus status = AudioComponentInstanceNew(comp, &playback->audio_unit);
  if (status != noErr || !playback->audio_unit) {
    logger_error(&g_logger, "Failed to create output AudioUnit: status=%d",
                 status);
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to create output AudioUnit");
    goto cleanup;
  }
  logger_trace(&g_logger, "Created playback audio unit.");

  UInt32 enable_output = 1;
  status = AudioUnitSetProperty(
      playback->audio_unit, kAudioOutputUnitProperty_EnableIO,
      kAudioUnitScope_Output, 0, &enable_output, sizeof(enable_output));
  if (status != noErr) {
    logger_error(&g_logger, "Failed to enable output on AudioUnit: status=%d",
                 status);
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to enable output");
    goto cleanup;
  }

  UInt32 disable_input = 0;
  status = AudioUnitSetProperty(
      playback->audio_unit, kAudioOutputUnitProperty_EnableIO,
      kAudioUnitScope_Input, 1, &disable_input, sizeof(disable_input));
  if (status != noErr) {
    logger_error(&g_logger, "Failed to disable input on AudioUnit: status=%d",
                 status);
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to disable input");
    goto cleanup;
  }

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

  AudioUnitSetProperty(playback->audio_unit,
                       kAudioOutputUnitProperty_CurrentDevice,
                       kAudioUnitScope_Global, 0, &dev_id, sizeof(dev_id));

  // Attempt to acquire Hog Mode if exclusive access is requested.
  if (playback->exclusive) {
    playback->did_acquire_hog_mode = core_audio_device_acquire_hog_mode(dev_id);
  }

  // Set the device format.
  bool physical_format_set = false;
  if (playback->has_sample_format) {
    if (core_audio_device_set_matching_physical_format(
            dev_id, CORE_AUDIO_SCOPE_OUTPUT, playback->sample_rate,
            playback->sample_format, (int)playback->channels)) {
      physical_format_set = true;
      logger_debug(&g_logger, "Set phys playback stream format.");
    } else {
      logger_error(&g_logger,
                   "Failed to set matching physical playback format: %s",
                   playback->sample_format);
      if (err)
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                           "Failed to find matching physical playback format");
      goto cleanup;
    }
  }
  if (!physical_format_set) {
    core_audio_device_set_nominal_sample_rate(dev_id, playback->sample_rate);
    logger_trace(&g_logger, "Set playback device sample rate.");
  }
  core_audio_device_set_buffer_frame_size(
      dev_id, (uint32_t)playback->chunk_size, CORE_AUDIO_SCOPE_OUTPUT);

  core_audio_device_add_alive_watcher(dev_id, &playback->is_device_alive);

  AudioStreamBasicDescription stream_format =
      core_audio_device_float32_stream_format(playback->sample_rate,
                                              (int)playback->channels);
  status = AudioUnitSetProperty(
      playback->audio_unit, kAudioUnitProperty_StreamFormat,
      kAudioUnitScope_Input, 0, &stream_format, sizeof(stream_format));
  if (status != noErr) {
    logger_error(&g_logger,
                 "Failed to set playback stream format on AudioUnit: status=%d",
                 status);
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to set playback stream format");
    goto cleanup;
  }

  AURenderCallbackStruct cb = {.inputProc = playback_callback,
                               .inputProcRefCon = playback};
  status = AudioUnitSetProperty(playback->audio_unit,
                                kAudioUnitProperty_SetRenderCallback,
                                kAudioUnitScope_Input, 0, &cb, sizeof(cb));
  if (status != noErr) {
    logger_error(&g_logger, "Failed to set render callback: status=%d", status);
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to set render callback");
    goto cleanup;
  }

  UInt32 max_frames = (UInt32)playback->chunk_size;
  if (dev_id != 0) {
    uint32_t actual_size = 0;
    if (core_audio_device_get_buffer_frame_size(dev_id, CORE_AUDIO_SCOPE_OUTPUT,
                                                &actual_size)) {
      if ((int)actual_size > (int)max_frames) max_frames = actual_size;
    }
  }
  AudioUnitSetProperty(
      playback->audio_unit, kAudioUnitProperty_MaximumFramesPerSlice,
      kAudioUnitScope_Global, 0, &max_frames, sizeof(max_frames));

  status = AudioUnitInitialize(playback->audio_unit);
  if (status != noErr) {
    logger_error(&g_logger,
                 "Failed to initialize playback AudioUnit: status=%d", status);
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to initialize output");
    goto cleanup;
  }

  atomic_store_explicit(&playback->stopped, false, memory_order_release);
  status = AudioOutputUnitStart(playback->audio_unit);
  if (status != noErr) {
    logger_error(&g_logger, "Failed to start playback AudioUnit: status=%d",
                 status);
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to start output");
    goto cleanup;
  }

  if (core_audio_device_has_nominal_sample_rate_property(dev_id)) {
    playback->rate_watcher =
        rate_change_watcher_create(dev_id, playback->sample_rate);
  }

  logger_debug(&g_logger, "Opened CoreAudio playback device \"%s\".",
               playback->device_name[0] ? playback->device_name : "default");
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
      playback->blockalign, chunk, BINARY_SAMPLE_FORMAT_F32_LE,
      playback->channels, 1, 1000, NULL, &playback->stopped,
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
  if (playback->audio_unit) {
    AudioOutputUnitStop(playback->audio_unit);
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
  atomic_init(&playback->active_callbacks, 0);
  const char* config_device = playback_device_config_get_device(config);
  if (config_device && config_device[0] != '\0') {
    strncpy(playback->device_name, config_device,
            sizeof(playback->device_name) - 1);
  }
  size_t config_channels = playback_device_config_get_channels(config);
  playback->channels = config_channels;
  playback->sample_rate = (double)sample_rate;
  playback->chunk_size = (size_t)chunk_size;
  playback->exclusive = playback_device_config_get_exclusive(config);

  coreaudio_sample_format_t fmt = playback_device_config_get_format(config);
  if (fmt != COREAUDIO_SAMPLE_FORMAT_INVALID) {
    const char* fmt_str = coreaudio_sample_format_to_string(fmt);
    strncpy(playback->sample_format, fmt_str,
            sizeof(playback->sample_format) - 1);
    playback->has_sample_format = true;
  }

  playback->bytes_per_sample = sizeof(float);
  playback->blockalign = config_channels * sizeof(float);
  size_t ring_size = playback->blockalign * (2 * (size_t)chunk_size + 2048);
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
