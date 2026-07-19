// CoreAudio playback backend for macOS
//
// Real-time discipline
// --------------------
// The render callback runs on a high-priority audio thread driven by
// CoreAudio. It is absolutely forbidden to take locks, allocate, or
// otherwise call into the Swift runtime in a way that could block. To
// honour that:
//   - sample rings are SPSC `SPSCAudioRingBuffer<Float>` instances —
//     producer and consumer are wait-free, no `NSLock`.
//   - the render callback writes directly into the AudioBufferList
//     provided by CoreAudio, consuming from the pre-allocated SPSC rings.

#include "core_audio_playback.h"
#if defined(ENABLE_COREAUDIO)
#ifdef ENABLE_ACCELERATE
#include <Accelerate/Accelerate.h>
#endif
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "Logging/app_logger.h"
#include "Utils/cdsp_time.h"

static const logger_t g_logger = {"dsp.backend.coreaudio.playback"};


struct core_audio_playback {
  char device_name[256];
  int channels;
  double sample_rate;
  size_t chunk_size;
  bool exclusive;
  char sample_format[16];
  bool has_sample_format;

  AudioUnit audio_unit;
  /// Per-channel SPSC ring buffer of `Float` samples. `write(chunk:)`
  /// is the producer; the render callback is the consumer.
  spsc_audio_ring_buffer_t** playback_rings;
  int ring_buffer_size;

  /// HAL device the unit is bound to. Captured from the resolved
  /// device lookup in `open()` so `close()` can release hog mode
  /// without doing the lookup again (which would race a default-
  /// device change).
  AudioDeviceID opened_device_id;
  bool did_acquire_hog_mode;
  /// Watches the device's nominal sample rate so the engine can
  /// surface `.playbackFormatChange` when something else flips the
  /// device rate at runtime.
  rate_change_watcher_t* rate_watcher;
  _Atomic bool is_device_alive;
  _Atomic bool is_paused;
  bool is_interleaved;
  _Atomic bool stopped;
  _Atomic int active_callbacks;
};

/**
 * @brief CoreAudio listener callback for device liveness.
 *
 * Called by CoreAudio when the alive state of the device changes (e.g.,
 * disconnection). Updates the internal atomic flag `is_device_alive`.
 *
 * @param inObjectID The AudioObjectID of the device.
 * @param inNumberAddresses The number of addresses in inAddresses.
 * @param inAddresses The addresses of the properties that changed.
 * @param inClientData Pointer to the core_audio_playback_t instance.
 * @return OSStatus noErr on success, or an error code.
 */
static OSStatus playback_alive_listener_callback(
    AudioObjectID inObjectID, UInt32 inNumberAddresses,
    const AudioObjectPropertyAddress* inAddresses, void* inClientData) {
  (void)inNumberAddresses;
  (void)inAddresses;
  core_audio_playback_t* playback = (core_audio_playback_t*)inClientData;
  if (!playback) return noErr;
  uint32_t alive = 0;
  uint32_t size = sizeof(uint32_t);
  AudioObjectPropertyAddress addr = {
      .mSelector = kAudioDevicePropertyDeviceIsAlive,
      .mScope = kAudioObjectPropertyScopeGlobal,
      .mElement = kAudioObjectPropertyElementMain};
  if (AudioObjectGetPropertyData(inObjectID, &addr, 0, NULL, &size, &alive) ==
      noErr) {
    atomic_store_explicit(&playback->is_device_alive, (alive != 0),
                          memory_order_release);
  }
  return noErr;
}

/**
 * @brief CoreAudio render callback for playback.
 *
 * This callback is called by the CoreAudio real-time thread to pull audio data
 * from the internal ring buffers and write it to the output device's buffers.
 *
 * @note This function runs on a real-time thread. It must be wait-free and must
 * not:
 *       - Allocate or free memory.
 *       - Take locks (mutexes).
 *       - Call any blocking APIs.
 *       - Call into the Swift runtime.
 *
 * @param inRefCon Pointer to the core_audio_playback_t instance.
 * @param ioActionFlags Action flags for the render operation.
 * @param inTimeStamp Time stamp of the render cycle.
 * @param inBusNumber The bus number.
 * @param inNumberFrames The number of sample frames requested.
 * @param ioData The buffer list to fill with audio data.
 * @return OSStatus noErr on success.
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

  size_t frame_count = (size_t)inNumberFrames;

  if (playback->is_interleaved) {
    float* float_ptr = (float*)ioData->mBuffers[0].mData;
    if (float_ptr) {
      for (int ch = 0; ch < playback->channels; ch++) {
        size_t copied = spsc_audio_ring_buffer_consume_stride(
            playback->playback_rings[ch], float_ptr + ch, frame_count,
            playback->channels);
        if (copied < frame_count) {
          float zero = 0.0f;
#ifdef ENABLE_ACCELERATE
          vDSP_vfill(&zero, float_ptr + ch + (copied * playback->channels),
                     playback->channels, frame_count - copied);
#else
          float* p = float_ptr + ch + (copied * playback->channels);
          size_t count = frame_count - copied;
          int stride = playback->channels;
          for (size_t i = 0; i < count; i++) {
            *p = zero;
            p += stride;
          }
#endif
        }
      }
    }
  } else {
    for (UInt32 ch = 0; ch < ioData->mNumberBuffers; ch++) {
      float* float_ptr = (float*)ioData->mBuffers[ch].mData;
      if (!float_ptr) continue;
      if ((int)ch < playback->channels) {
        size_t copied = spsc_audio_ring_buffer_consume(
            playback->playback_rings[ch], float_ptr, frame_count);
        if (copied < frame_count) {
          memset(float_ptr + copied, 0, (frame_count - copied) * sizeof(float));
        }
      } else {
        memset(float_ptr, 0, frame_count * sizeof(float));
      }
    }
  }

  atomic_fetch_sub_explicit(&playback->active_callbacks, 1,
                            memory_order_relaxed);
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
  if (!playback->audio_unit && playback->opened_device_id == 0) return;
  logger_info(&g_logger, "Closing CoreAudio playback device");
  if (playback->rate_watcher) {
    rate_change_watcher_free(playback->rate_watcher);
    playback->rate_watcher = NULL;
  }
  if (playback->opened_device_id != 0) {
    AudioObjectPropertyAddress alive_addr = {
        .mSelector = kAudioDevicePropertyDeviceIsAlive,
        .mScope = kAudioObjectPropertyScopeGlobal,
        .mElement = kAudioObjectPropertyElementMain};
    AudioObjectRemovePropertyListener(playback->opened_device_id, &alive_addr,
                                      playback_alive_listener_callback,
                                      playback);
  }
  if (playback->audio_unit) {
    AudioOutputUnitStop(playback->audio_unit);
    AURenderCallbackStruct null_cb = {0};
    AudioUnitSetProperty(playback->audio_unit,
                         kAudioUnitProperty_SetRenderCallback,
                         kAudioUnitScope_Input, 0, &null_cb, sizeof(null_cb));
  }
  while (atomic_load_explicit(&playback->active_callbacks,
                              memory_order_acquire) > 0) {
    usleep(500);
  }
  if (playback->audio_unit) {
    AudioComponentInstanceDispose(playback->audio_unit);
    playback->audio_unit = NULL;
  }
  if (playback->did_acquire_hog_mode && playback->opened_device_id != 0) {
    pid_t pid = -1;
    AudioObjectPropertyAddress addr = {
        .mSelector = kAudioDevicePropertyHogMode,
        .mScope = kAudioObjectPropertyScopeGlobal,
        .mElement = kAudioObjectPropertyElementMain};
    AudioObjectSetPropertyData(playback->opened_device_id, &addr, 0, NULL,
                               sizeof(pid_t), &pid);
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
              "channels=%d, exclusive=%d)",
              playback->device_name[0] ? playback->device_name : "default",
              playback->sample_rate, playback->channels,
              playback->exclusive ? 1 : 0);
  core_audio_playback_close(playback);

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
  playback->opened_device_id = dev_id;
  if (dev_id != 0) {
    AudioUnitSetProperty(playback->audio_unit,
                         kAudioOutputUnitProperty_CurrentDevice,
                         kAudioUnitScope_Global, 0, &dev_id, sizeof(dev_id));
    // Attempt to acquire Hog Mode if exclusive access is requested.
    // Hog mode prevents other processes from using the device.
    if (playback->exclusive) {
      pid_t hog_pid = getpid();
      AudioObjectPropertyAddress hog_addr = {
          .mSelector = kAudioDevicePropertyHogMode,
          .mScope = kAudioObjectPropertyScopeGlobal,
          .mElement = kAudioObjectPropertyElementMain};
      if (AudioObjectSetPropertyData(dev_id, &hog_addr, 0, NULL, sizeof(pid_t),
                                     &hog_pid) == noErr) {
        playback->did_acquire_hog_mode = true;
        logger_info(&g_logger,
                    "Acquired exclusive hog mode on playback device");
      } else {
        logger_warn(&g_logger,
                    "Failed to acquire exclusive hog mode on playback device");
      }
    }
    // Set the device format.
    bool physical_format_set = false;
    if (playback->has_sample_format) {
      if (core_audio_device_set_matching_physical_format(
              dev_id, CORE_AUDIO_SCOPE_OUTPUT, playback->sample_rate,
              playback->sample_format, playback->channels)) {
        physical_format_set = true;
      } else {
        logger_error(&g_logger,
                     "Failed to set matching physical playback format: %s",
                     playback->sample_format);
        if (err)
          backend_error_init(
              err, BACKEND_ERROR_INITIALIZATION_FAILED,
              "Failed to find matching physical playback format");
        goto cleanup;
      }
    }
    if (!physical_format_set) {
      core_audio_device_set_nominal_sample_rate(dev_id, playback->sample_rate);
    }
    core_audio_device_set_buffer_frame_size(
        dev_id, (uint32_t)playback->chunk_size, CORE_AUDIO_SCOPE_OUTPUT);

    AudioObjectPropertyAddress alive_addr = {
        .mSelector = kAudioDevicePropertyDeviceIsAlive,
        .mScope = kAudioObjectPropertyScopeGlobal,
        .mElement = kAudioObjectPropertyElementMain};
    AudioObjectAddPropertyListener(dev_id, &alive_addr,
                                   playback_alive_listener_callback, playback);
  }

  AudioStreamBasicDescription stream_format =
      core_audio_device_float32_stream_format(playback->sample_rate,
                                              playback->channels, false);
  status = AudioUnitSetProperty(
      playback->audio_unit, kAudioUnitProperty_StreamFormat,
      kAudioUnitScope_Input, 0, &stream_format, sizeof(stream_format));
  if (status != noErr) {
    // Fallback to interleaved float32.
    stream_format = core_audio_device_float32_stream_format(
        playback->sample_rate, playback->channels, true);
    status = AudioUnitSetProperty(
        playback->audio_unit, kAudioUnitProperty_StreamFormat,
        kAudioUnitScope_Input, 0, &stream_format, sizeof(stream_format));
    if (status != noErr) {
      logger_error(
          &g_logger,
          "Failed to set playback stream format on AudioUnit: status=%d",
          status);
      if (err)
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                           "Failed to set playback stream format");
      goto cleanup;
    }
  }
  playback->is_interleaved =
      ((stream_format.mFormatFlags & kAudioFormatFlagIsNonInterleaved) == 0);

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

  status = AudioOutputUnitStart(playback->audio_unit);
  if (status != noErr) {
    logger_error(&g_logger, "Failed to start output AudioUnit: status=%d",
                 status);
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to start output");
    goto cleanup;
  }

  if (dev_id != 0 &&
      core_audio_device_has_nominal_sample_rate_property(dev_id)) {
    playback->rate_watcher =
        rate_change_watcher_create(dev_id, playback->sample_rate);
  }

  logger_info(&g_logger, "CoreAudio playback successfully opened and started");
  return true;

cleanup:
  core_audio_playback_close(playback);
  return false;
}

/// Write an audio chunk into the playback ring buffers.
static bool core_audio_playback_write(void* ctx,
                                      const audio_chunk_t* chunk,
                                      backend_error_t* err) {
  core_audio_playback_t* playback = (core_audio_playback_t*)ctx;
  if (!playback) return false;
  if (!atomic_load_explicit(&playback->is_device_alive, memory_order_acquire)) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                         "Playback device disconnected");
    return false;
  }
  size_t frames = audio_chunk_get_valid_frames(chunk);
  if (frames == 0) return true;

  int usable_channels =
      playback->channels < (int)audio_chunk_get_channels(chunk)
          ? playback->channels
          : (int)audio_chunk_get_channels(chunk);

  // Wait for space to become available in the ring buffers for all channels.
  // This is a blocking wait from the writer's perspective, using a sleep loop
  // to yield CPU. The consumer (CoreAudio render thread) remains lock-free.
  uint32_t elapsed_ms = 0;
  while (true) {
    if (atomic_load_explicit(&playback->stopped, memory_order_acquire)) {
      if (err) {
        backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                           "Playback stream stopped");
      }
      return false;
    }
    bool space_available = true;
    for (int ch = 0; ch < usable_channels; ch++) {
      size_t free_space = spsc_audio_ring_buffer_get_available_to_write(
          playback->playback_rings[ch]);
      if (free_space < frames) {
        space_available = false;
        break;
      }
    }
    if (space_available) {
      break;
    }

    // Sleep for 1ms to yield CPU.
    cdsp_sleep_ms(1);
    elapsed_ms += 1;

    // Timeout after 1 second to prevent infinite blocking if playback stalls.
    if (elapsed_ms >= 1000) {
      if (err) {
        backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                           "CoreAudio write timeout");
      }
      return false;  // Timeout 1s
    }

    // Abort if the device was disconnected.
    if (!atomic_load_explicit(&playback->is_device_alive,
                              memory_order_acquire)) {
      return false;
    }
    // If paused, we fake a successful write to avoid blocking the caller.
    // The caller might want to keep sending data, which will be discarded
    // or accumulate depending on buffer state.
    if (atomic_load_explicit(&playback->is_paused, memory_order_acquire)) {
      return true;
    }
  }

  for (int ch = 0; ch < usable_channels; ch++) {
    const double* src_ptr = audio_chunk_get_channel(chunk, ch);
    if (src_ptr) {
      spsc_audio_ring_buffer_append_converting_double_to_float(
          playback->playback_rings[ch], src_ptr, frames);
    }
  }
  return true;
}



/// Get the current buffer level in samples.
static size_t core_audio_playback_get_buffer_level(void* ctx) {
  core_audio_playback_t* playback = (core_audio_playback_t*)ctx;
  if (!playback || !playback->playback_rings || !playback->playback_rings[0])
    return 0;
  return spsc_audio_ring_buffer_get_available_to_read(
      playback->playback_rings[0]);
}

/// Get any pending sample rate change detected on the playback device.
static bool core_audio_playback_get_pending_rate_change(
    void* ctx, double* out_rate) {
  core_audio_playback_t* playback = (core_audio_playback_t*)ctx;
  if (!playback || !playback->rate_watcher) return false;
  return rate_change_watcher_get_pending_change(playback->rate_watcher,
                                                out_rate);
}

/// Push zero samples into the playback ring buffer before real audio arrives.
static bool core_audio_playback_prefill_silence(void* ctx,
                                                size_t frames,
                                                backend_error_t* err) {
  core_audio_playback_t* playback = (core_audio_playback_t*)ctx;
  (void)err;
  if (!playback || frames == 0) return true;
  size_t to_write = frames < (size_t)playback->ring_buffer_size
                        ? frames
                        : (size_t)playback->ring_buffer_size;
  for (int ch = 0; ch < playback->channels; ch++) {
    spsc_audio_ring_buffer_write_silence(playback->playback_rings[ch],
                                         to_write);
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
static void core_audio_playback_set_is_paused(void* ctx,
                                              bool paused) {
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
  if (playback->playback_rings) {
    for (int i = 0; i < playback->channels; i++) {
      if (playback->playback_rings[i])
        spsc_audio_ring_buffer_free(playback->playback_rings[i]);
    }
    free(playback->playback_rings);
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
 * @return Pointer to the created playback_backend_t instance, or NULL on failure.
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
  }

  playback->ring_buffer_size = chunk_size * 8;
  playback->playback_rings = (spsc_audio_ring_buffer_t**)calloc(
      config_channels, sizeof(spsc_audio_ring_buffer_t*));
  if (!playback->playback_rings) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Out of memory");
    core_audio_playback_destroy(playback);
    return NULL;
  }
  for (int i = 0; i < config_channels; i++) {
    playback->playback_rings[i] =
        spsc_audio_ring_buffer_create(playback->ring_buffer_size);
    if (!playback->playback_rings[i]) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                           "Out of memory");
      core_audio_playback_destroy(playback);
      return NULL;
    }
  }
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
