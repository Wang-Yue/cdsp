#include "Backend/wasapi_playback.h"

/**
 * @file wasapi_playback.c
 * @brief WASAPI playback backend implementation using CDSP standard SPSC ring
 * buffer.
 */

#if defined(ENABLE_WASAPI)

#include <windef.h>
#include <windows.h>
#ifndef CDECL
#define CDECL __cdecl
#endif
#ifndef COBJMACROS
#define COBJMACROS
#endif
#include <initguid.h>
#include <ks.h>
#include <ksmedia.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Audio/sample_conversion.h"
#include "Backend/wasapi_capabilities.h"
#include "Backend/wasapi_device.h"
#include "Engine/cdsp_sem.h"
#include "Utils/cdsp_time.h"
#include "Utils/lock_free_ring_buffer.h"

struct wasapi_playback {
  char device[256];
  int sample_rate;
  int channels;
  int chunk_size;
  wasapi_sample_format_t format;
  bool exclusive;
  bool polling;
  int target_level;

  wasapi_binary_sample_format_t bin_fmt;
  size_t bytes_per_sample;
  size_t blockalign;
  bool com_initialized;

  IMMDeviceEnumerator* enumerator;
  IMMDevice* mm_device;
  IAudioClient* client;
  IAudioRenderClient* render_client;
  IAudioSessionControl* session_control;
  IAudioSessionEvents* session_events_listener;
  UINT32 buffer_frame_count;
  REFERENCE_TIME def_period;
  HANDLE event_handle;

  spsc_byte_ring_buffer_t* ring_buffer;
  uint8_t* write_buf;
  size_t write_buf_cap;

  pthread_t inner_thread;
  _Atomic bool thread_running;
  _Atomic bool stopped;
  _Atomic bool paused;
  double pending_rate;
  _Atomic bool has_pending_rate_change;
};

static void wasapi_playback_on_format_change(void* parent, double new_rate) {
  wasapi_playback_t* playback = (wasapi_playback_t*)parent;
  playback->pending_rate = new_rate;
  atomic_store_explicit(&playback->has_pending_rate_change, true,
                        memory_order_release);
}

/**
 * @brief get_available_space_in_frames matching wasapi-rs api.rs.
 */
static inline UINT32 wasapi_audio_client_get_available_space_in_frames(
    IAudioClient* client, bool exclusive, bool events_timing) {
  if (exclusive && events_timing) {
    UINT32 buffer_frame_count = 0;
    IAudioClient_GetBufferSize(client, &buffer_frame_count);
    return buffer_frame_count;
  }
  UINT32 padding_count = 0;
  UINT32 buffer_frame_count = 0;
  if (FAILED(IAudioClient_GetCurrentPadding(client, &padding_count))) {
    return 0;
  }
  if (FAILED(IAudioClient_GetBufferSize(client, &buffer_frame_count))) {
    return 0;
  }
  return (buffer_frame_count > padding_count)
             ? (buffer_frame_count - padding_count)
             : 0;
}

/**
 * @brief playback_loop matching CamillaDSP device.rs:playback_loop (lines
 * 494-625).
 */
static void* wasapi_playback_loop(void* arg) {
  wasapi_playback_t* playback = (wasapi_playback_t*)arg;
  HRESULT init_hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  (void)init_hr;

  size_t chunksize = (size_t)playback->chunk_size;
  size_t blockalign = playback->blockalign;
  size_t target_level =
      playback->target_level > 0 ? (size_t)playback->target_level : chunksize;

  // Pre-roll wait: wait for data to start playback, will time out after one
  // second
  int waited_millis = 0;
  logger_trace(
      &g_wasapi_logger,
      "Waiting for data to start playback, will time out after one second.");
  while (spsc_byte_ring_buffer_get_available_to_read(playback->ring_buffer) <
             2 * chunksize * blockalign &&
         waited_millis < 1000) {
    if (atomic_load_explicit(&playback->stopped, memory_order_acquire)) {
      CoUninitialize();
      return NULL;
    }
    cdsp_sleep_ms(10);
    waited_millis += 10;
  }
  logger_debug(&g_wasapi_logger, "Waited for data for %d ms.", waited_millis);

#ifdef _WIN32
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#endif

  bool running = false;
  bool starting = true;
  bool started = false;
  size_t silence_frames_to_insert = 0;
  REFERENCE_TIME def_time = 0, min_time = 0;
  IAudioClient_GetDevicePeriod(playback->client, &def_time, &min_time);
  DWORD poll_delay_ms = (DWORD)(def_time / 10000);
  if (poll_delay_ms == 0) poll_delay_ms = 1;

  if (!playback->event_handle) {
    logger_debug(&g_wasapi_logger,
                 "Playback uses polling mode, delay is %lu us.",
                 (unsigned long)(def_time / 10));
  }

  // Fill the endpoint buffer with the first block before starting the stream,
  // so playback begins with real audio (preceded by the target_level silence)
  // instead of an empty buffer. This follows the ordering recommended by
  // Microsoft:
  // https://learn.microsoft.com/en-us/windows/win32/coreaudio/rendering-a-stream
  // The first pass does this prefill and then starts the stream. Every later
  // pass is preceded by the event wait (or poll sleep) at the end of the
  // previous pass, which matters because in exclusive event mode
  // get_available_space_in_frames always returns the full buffer size: without
  // the preceding wait we would rewrite the buffer while the hardware reads it.
  while (
      atomic_load_explicit(&playback->thread_running, memory_order_acquire)) {
    UINT32 buffer_free_frame_count =
        wasapi_audio_client_get_available_space_in_frames(
            playback->client, playback->exclusive,
            playback->event_handle != NULL);
    logger_trace(&g_wasapi_logger, "Playback, new buffer frame count %u.",
                 buffer_free_frame_count);

    if (buffer_free_frame_count > 0) {
      size_t avail_bytes =
          spsc_byte_ring_buffer_get_available_to_read(playback->ring_buffer);
      size_t avail_frames = (blockalign > 0) ? (avail_bytes / blockalign) : 0;

      if (!running && avail_frames > 0) {
        running = true;
        if (starting) {
          starting = false;
        } else {
          logger_warn(&g_wasapi_logger,
                      "Restarting playback after buffer underrun.");
        }
        logger_debug(
            &g_wasapi_logger,
            "Playback, inserting %zu silent frames to reach target delay.",
            target_level);
        silence_frames_to_insert = target_level;
      }

      size_t frames_to_write = (size_t)buffer_free_frame_count;
      size_t silence_frames = 0;
      if (silence_frames_to_insert > 0) {
        silence_frames = (silence_frames_to_insert < frames_to_write)
                             ? silence_frames_to_insert
                             : frames_to_write;
        silence_frames_to_insert -= silence_frames;
      }

      size_t silence_bytes = silence_frames * blockalign;
      size_t frames_from_ring = frames_to_write - silence_frames;
      size_t bytes_from_ring = frames_from_ring * blockalign;

      BYTE* bufferptr = NULL;
      HRESULT hr = IAudioRenderClient_GetBuffer(
          playback->render_client, (UINT32)frames_to_write, &bufferptr);
      if (SUCCEEDED(hr) && bufferptr) {
        if (silence_bytes > 0) {
          memset(bufferptr, 0, silence_bytes);
        }
        size_t consumed_bytes = 0;
        if (bytes_from_ring > 0) {
          consumed_bytes = spsc_byte_ring_buffer_consume(
              playback->ring_buffer, bufferptr + silence_bytes,
              bytes_from_ring);
        }
        if (consumed_bytes < bytes_from_ring) {
          memset(bufferptr + silence_bytes + consumed_bytes, 0,
                 bytes_from_ring - consumed_bytes);
          // While prefilling (before the stream is started) a short
          // fill just gets padded with silence and is not an
          // interruption, so skip the underrun handling until started.
          if (started && running) {
            running = false;
            logger_warn(&g_wasapi_logger,
                        "Playback interrupted, no data available.");
          }
        }
        IAudioRenderClient_ReleaseBuffer(playback->render_client,
                                         (UINT32)frames_to_write, 0);
      } else {
        if (atomic_load_explicit(&playback->has_pending_rate_change,
                                 memory_order_acquire))
          break;
      }
    }

    if (!started) {
      // The buffer now holds the first block, start playback.
      HRESULT hr_start = IAudioClient_Start(playback->client);
      if (FAILED(hr_start)) {
        logger_error(&g_wasapi_logger,
                     "Playback start stream failed: hr=0x%08lX",
                     (unsigned long)hr_start);
        break;
      }
      started = true;
    }

    if (playback->event_handle) {
      DWORD wait_res = WaitForSingleObject(playback->event_handle, 1000);
      if (wait_res != WAIT_OBJECT_0) {
        logger_error(&g_wasapi_logger, "Error on playback, stopping stream");
        IAudioClient_Stop(playback->client);
        break;
      }
    } else {
      cdsp_sleep_ms(poll_delay_ms);
    }
  }

  IAudioClient_Stop(playback->client);
  CoUninitialize();
  return NULL;
}

// MARK: - open_playback matching CamillaDSP device.rs:open_playback (lines
// 334-406)

static bool wasapi_playback_open(void* ctx, backend_error_t* err) {
  wasapi_playback_t* playback = (wasapi_playback_t*)ctx;
  if (!playback) return false;

  HRESULT init_hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  playback->com_initialized = SUCCEEDED(init_hr);
  atomic_init(&playback->stopped, false);
  atomic_init(&playback->paused, false);
  atomic_init(&playback->has_pending_rate_change, false);

  if (!wasapi_create_device_and_client(
          playback->device, false, false, &playback->enumerator,
          &playback->mm_device, &playback->client, err)) {
    goto error_cleanup;
  }
  logger_trace(&g_wasapi_logger, "Got playback iaudioclient.");

  WAVEFORMATEXTENSIBLE wfx;
  bool is_std_wfx = false;
  wasapi_binary_sample_format_t bin_fmt;
  bool has_format = (playback->format != WASAPI_SAMPLE_FORMAT_INVALID);

  if (!wasapi_get_device_format(playback->client, playback->sample_rate,
                                playback->channels, playback->format,
                                has_format, playback->exclusive, "Render", &wfx,
                                &is_std_wfx, &bin_fmt, err)) {
    goto error_cleanup;
  }
  playback->bin_fmt = bin_fmt;
  playback->blockalign = (size_t)wfx.Format.nBlockAlign;

  logger_debug(&g_wasapi_logger, "Opening Wasapi playback device.");
  if (!wasapi_initialize_stream(
          playback->client, &wfx, playback->sample_rate, playback->blockalign,
          playback->exclusive, playback->polling, false, &playback->def_period,
          &playback->event_handle, &playback->buffer_frame_count, "Playback",
          err)) {
    goto error_cleanup;
  }

  HRESULT hr =
      IAudioClient_GetService(playback->client, &IID_IAudioRenderClient,
                              (void**)&playback->render_client);
  if (FAILED(hr)) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to get IAudioRenderClient");
    goto error_cleanup;
  }

  wasapi_register_session_events(
      playback->client, playback, wasapi_playback_on_format_change,
      &playback->session_control, &playback->session_events_listener);

  logger_debug(&g_wasapi_logger, "Opened Wasapi playback device \"%s\".",
               playback->device[0] != '\0' ? playback->device : "default");

  playback->bytes_per_sample =
      wasapi_binary_format_bytes_per_sample(playback->bin_fmt);
  playback->blockalign =
      (size_t)playback->channels * playback->bytes_per_sample;

  // Allocate SPSC byte ring buffer matching upstream CamillaDSP
  size_t ring_size = (size_t)playback->channels * 4 *
                     (2 * (size_t)playback->chunk_size + 2048);
  playback->ring_buffer = spsc_byte_ring_buffer_create(ring_size);

  playback->write_buf_cap =
      (size_t)playback->chunk_size * playback->blockalign * 2;
  playback->write_buf = (uint8_t*)malloc(playback->write_buf_cap);

  atomic_store_explicit(&playback->thread_running, true, memory_order_release);
  if (pthread_create(&playback->inner_thread, NULL, wasapi_playback_loop,
                     playback) != 0) {
    atomic_store_explicit(&playback->thread_running, false,
                          memory_order_release);
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to create inner playback thread");
    goto error_cleanup;
  }

  return true;

error_cleanup:
  if (playback->write_buf) {
    free(playback->write_buf);
    playback->write_buf = NULL;
  }
  if (playback->ring_buffer) {
    spsc_byte_ring_buffer_free(playback->ring_buffer);
    playback->ring_buffer = NULL;
  }
  wasapi_cleanup_device_resources(
      &playback->client, (IUnknown**)&playback->render_client,
      &playback->session_control, &playback->session_events_listener,
      &playback->event_handle, &playback->mm_device, &playback->enumerator,
      &playback->com_initialized);
  return false;
}

// MARK: - wasapi_playback_write matching CamillaDSP device.rs (lines 1033-1078)

static bool wasapi_playback_write(void* ctx, const audio_chunk_t* chunk,
                                  backend_error_t* err) {
  wasapi_playback_t* playback = (wasapi_playback_t*)ctx;
  if (!playback) return false;

  DWORD sleep_duration_us =
      (DWORD)(1000000ULL * (unsigned long long)playback->chunk_size /
              (unsigned long long)playback->sample_rate / 2ULL);
  DWORD sleep_duration_ms = sleep_duration_us / 1000;
  if (sleep_duration_ms == 0) sleep_duration_ms = 1;

  return audio_backend_ring_buffer_write(
      playback->ring_buffer, playback->write_buf, playback->write_buf_cap,
      playback->blockalign, chunk, (binary_sample_format_t)playback->bin_fmt,
      (size_t)playback->channels, (uint32_t)sleep_duration_ms, 8,
      &playback->thread_running, &playback->stopped, &playback->paused,
      &playback->has_pending_rate_change, err);
}

static void wasapi_playback_close(void* ctx) {
  wasapi_playback_t* playback = (wasapi_playback_t*)ctx;
  if (!playback) return;

  if (playback->thread_running) {
    atomic_store_explicit(&playback->thread_running, false,
                          memory_order_release);
    atomic_store_explicit(&playback->stopped, true, memory_order_release);
    if (playback->event_handle) SetEvent(playback->event_handle);
    pthread_join(playback->inner_thread, NULL);
  }

  if (playback->write_buf) {
    free(playback->write_buf);
    playback->write_buf = NULL;
  }
  if (playback->ring_buffer) {
    spsc_byte_ring_buffer_free(playback->ring_buffer);
    playback->ring_buffer = NULL;
  }
  wasapi_cleanup_device_resources(
      &playback->client, (IUnknown**)&playback->render_client,
      &playback->session_control, &playback->session_events_listener,
      &playback->event_handle, &playback->mm_device, &playback->enumerator,
      &playback->com_initialized);
}

static size_t wasapi_playback_get_buffer_level(void* ctx) {
  wasapi_playback_t* playback = (wasapi_playback_t*)ctx;
  if (!playback || !playback->ring_buffer || playback->blockalign == 0)
    return 0;
  return spsc_byte_ring_buffer_get_available_to_read(playback->ring_buffer) /
         playback->blockalign;
}

static bool wasapi_playback_get_pending_rate_change(void* ctx,
                                                    double* out_rate) {
  wasapi_playback_t* playback = (wasapi_playback_t*)ctx;
  if (!playback) return false;
  return wasapi_check_and_resolve_pending_rate(
      playback->device, false, playback->pending_rate,
      &playback->has_pending_rate_change, out_rate);
}

static bool wasapi_playback_prefill_silence(void* ctx, size_t frames,
                                            backend_error_t* err) {
  (void)err;
  wasapi_playback_t* playback = (wasapi_playback_t*)ctx;
  if (!playback) return false;
  playback->target_level = (int)frames;
  return true;
}

static bool wasapi_playback_get_is_paused(void* ctx) {
  wasapi_playback_t* playback = (wasapi_playback_t*)ctx;
  if (!playback) return false;
  return atomic_load_explicit(&playback->paused, memory_order_acquire);
}

static void wasapi_playback_set_is_paused(void* ctx, bool paused) {
  wasapi_playback_t* playback = (wasapi_playback_t*)ctx;
  if (!playback) return;
  atomic_store_explicit(&playback->paused, paused, memory_order_release);
}

static bool wasapi_playback_pitch_control_supported(void* ctx) {
  (void)ctx;
  return false;
}

static void wasapi_playback_set_pitch(void* ctx, double multiplier) {
  (void)ctx;
  (void)multiplier;
}

static void wasapi_playback_stop(void* ctx) {
  wasapi_playback_t* playback = (wasapi_playback_t*)ctx;
  if (!playback) return;
  atomic_store_explicit(&playback->stopped, true, memory_order_release);
  if (playback->event_handle) {
    SetEvent(playback->event_handle);
  }
}

static void wasapi_playback_destroy(void* ctx) {
  wasapi_playback_t* playback = (wasapi_playback_t*)ctx;
  if (playback) {
    wasapi_playback_close(playback);
    free(playback);
  }
}

static playback_backend_t* wasapi_playback_create(
    const playback_device_config_t* config, int sample_rate, int chunk_size,
    bool full_duplex, processing_parameters_t* params, backend_error_t* err) {
  (void)full_duplex;
  (void)params;
  (void)err;
  wasapi_playback_t* playback =
      (wasapi_playback_t*)calloc(1, sizeof(wasapi_playback_t));
  if (!playback) return NULL;

  wasapi_extract_device_name(config->cfg.wasapi.has_device,
                             config->cfg.wasapi.device, playback->device,
                             sizeof(playback->device));

  playback->sample_rate = sample_rate;
  playback->channels = config->cfg.wasapi.channels;
  playback->chunk_size = chunk_size;
  playback->format = config->cfg.wasapi.format;
  playback->exclusive =
      config->cfg.wasapi.has_exclusive ? config->cfg.wasapi.exclusive : false;
  playback->polling =
      config->cfg.wasapi.has_polling ? config->cfg.wasapi.polling : false;

  atomic_init(&playback->paused, false);
  playback_backend_t* backend =
      (playback_backend_t*)calloc(1, sizeof(playback_backend_t));
  if (!backend) {
    free(playback);
    return NULL;
  }
  backend->ctx = playback;
  backend->vtable = &g_wasapi_playback_vtable;
  return backend;
}

const playback_backend_vtable_t g_wasapi_playback_vtable = {
    .create = wasapi_playback_create,
    .open = wasapi_playback_open,
    .write = wasapi_playback_write,
    .close = wasapi_playback_close,
    .get_buffer_level = wasapi_playback_get_buffer_level,
    .get_pending_rate_change = wasapi_playback_get_pending_rate_change,
    .prefill_silence = wasapi_playback_prefill_silence,
    .get_is_paused = wasapi_playback_get_is_paused,
    .set_is_paused = wasapi_playback_set_is_paused,
    .pitch_control_supported = wasapi_playback_pitch_control_supported,
    .set_pitch = wasapi_playback_set_pitch,
    .stop = wasapi_playback_stop,
    .destroy = wasapi_playback_destroy};

#endif  // ENABLE_WASAPI
