#include "Backend/wasapi_capture.h"

/**
 * @file wasapi_capture.c
 * @brief WASAPI capture backend implementation using CDSP standard SPSC ring
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

struct wasapi_capture {
  char device[256];
  int sample_rate;
  int channels;
  int chunk_size;
  wasapi_sample_format_t format;
  bool loopback;
  bool exclusive;
  bool polling;

  wasapi_binary_sample_format_t bin_fmt;
  size_t bytes_per_sample;
  size_t blockalign;
  bool com_initialized;

  IMMDeviceEnumerator* enumerator;
  IMMDevice* mm_device;
  IAudioClient* client;
  IAudioCaptureClient* capture_client;
  IAudioSessionControl* session_control;
  IAudioSessionEvents* session_events_listener;
  UINT32 buffer_frame_count;
  REFERENCE_TIME def_period;
  HANDLE event_handle;

  spsc_byte_ring_buffer_t* ring_buffer;
  uint8_t* decode_buf;
  size_t decode_buf_cap;

  pthread_t inner_thread;
  _Atomic bool thread_running;
  _Atomic bool stopped;
  _Atomic bool paused;
  double pending_rate;
  _Atomic bool has_pending_rate_change;
};

static void wasapi_capture_on_format_change(void* parent, double new_rate) {
  wasapi_capture_t* capture = (wasapi_capture_t*)parent;
  capture->pending_rate = new_rate;
  atomic_store_explicit(&capture->has_pending_rate_change, true,
                        memory_order_release);
}

static inline size_t wasapi_binary_format_bytes_per_sample(
    wasapi_binary_sample_format_t fmt) {
  switch (fmt) {
    case WASAPI_BINARY_FORMAT_S16_LE:
      return 2;
    case WASAPI_BINARY_FORMAT_S24_3_LE:
      return 3;
    case WASAPI_BINARY_FORMAT_S24_4_LJ_LE:
    case WASAPI_BINARY_FORMAT_S32_LE:
    case WASAPI_BINARY_FORMAT_F32_LE:
    default:
      return 4;
  }
}

/**
 * @brief get_next_packet_size matching wasapi-rs api.rs.
 */
static inline bool wasapi_capture_get_next_packet_size(
    IAudioCaptureClient* capture_client, bool exclusive, UINT32* out_frames) {
  if (exclusive) {
    return false;
  }
  UINT32 frames = 0;
  HRESULT hr = IAudioCaptureClient_GetNextPacketSize(capture_client, &frames);
  if (SUCCEEDED(hr)) {
    *out_frames = frames;
    return true;
  }
  *out_frames = 0;
  return false;
}

/**
 * @brief read_from_device matching wasapi-rs api.rs.
 */
static inline bool wasapi_capture_read_from_device(
    IAudioCaptureClient* capture_client, uint8_t* data, size_t max_bytes,
    size_t bytes_per_frame, UINT32* out_frames_read, DWORD* out_flags) {
  size_t data_len_in_frames = max_bytes / bytes_per_frame;
  if (data_len_in_frames == 0) {
    *out_frames_read = 0;
    *out_flags = 0;
    return true;
  }

  BYTE* buffer_ptr = NULL;
  UINT32 nbr_frames_returned = 0;
  DWORD flags = 0;
  UINT64 index = 0;
  UINT64 timestamp = 0;

  HRESULT hr = IAudioCaptureClient_GetBuffer(capture_client, &buffer_ptr,
                                             &nbr_frames_returned, &flags,
                                             &index, &timestamp);
  if (FAILED(hr)) {
    return false;
  }

  *out_frames_read = nbr_frames_returned;
  *out_flags = flags;

  if (nbr_frames_returned == 0) {
    return true;
  }

  size_t len_in_bytes = (size_t)nbr_frames_returned * bytes_per_frame;
  if (len_in_bytes > max_bytes) {
    IAudioCaptureClient_ReleaseBuffer(capture_client, nbr_frames_returned);
    return false;
  }

  if (buffer_ptr) {
    memcpy(data, buffer_ptr, len_in_bytes);
  }

  hr = IAudioCaptureClient_ReleaseBuffer(capture_client, nbr_frames_returned);
  return SUCCEEDED(hr);
}

/**
 * @brief capture_loop matching CamillaDSP device.rs:capture_loop (lines
 * 634-838).
 */
static void* wasapi_capture_loop(void* arg) {
  wasapi_capture_t* capture = (wasapi_capture_t*)arg;
  HRESULT init_hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  (void)init_hr;

  size_t blockalign = capture->blockalign;
  bool inactive = false;

  size_t data_buf_size = 8 * blockalign * 1024;
  uint8_t* data = (uint8_t*)malloc(data_buf_size);
  if (!data) {
    CoUninitialize();
    return NULL;
  }

  REFERENCE_TIME def_time = 0, min_time = 0;
  IAudioClient_GetDevicePeriod(capture->client, &def_time, &min_time);
  DWORD poll_delay_ms = (DWORD)(def_time / 10000);
  if (poll_delay_ms == 0) poll_delay_ms = 1;

  if (!capture->event_handle) {
    logger_debug(&g_wasapi_logger,
                 "Capture uses polling mode, delay is %lu us.",
                 (unsigned long)(def_time / 10));
  }

  int no_frames_counter = 0;

#ifdef _WIN32
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#endif

  logger_trace(&g_wasapi_logger, "Starting capture stream.");
  HRESULT hr = IAudioClient_Start(capture->client);
  if (FAILED(hr)) {
    logger_error(&g_wasapi_logger, "Capture start stream failed: hr=0x%08lX",
                 (unsigned long)hr);
    free(data);
    CoUninitialize();
    return NULL;
  }
  logger_trace(&g_wasapi_logger, "Started capture stream.");

  while (atomic_load_explicit(&capture->thread_running, memory_order_acquire)) {
    logger_trace(&g_wasapi_logger, "Capturing.");
    if (atomic_load_explicit(&capture->stopped, memory_order_acquire)) {
      logger_debug(&g_wasapi_logger, "Stopping inner capture loop on request.");
      IAudioClient_Stop(capture->client);
      free(data);
      CoUninitialize();
      return NULL;
    }

    if (capture->event_handle) {
      DWORD wait_res = WaitForSingleObject(capture->event_handle, 250);
      if (wait_res != WAIT_OBJECT_0) {
        logger_debug(&g_wasapi_logger, "Capture, timeout on event.");
        if (!inactive) {
          logger_warn(&g_wasapi_logger,
                      "Capture, no data received, pausing stream.");
          inactive = true;
        }
        continue;
      }
    } else {
      cdsp_sleep_ms(poll_delay_ms);
      UINT32 frames_ready = 0;
      hr = IAudioClient_GetCurrentPadding(capture->client, &frames_ready);
      logger_trace(&g_wasapi_logger,
                   "Capture, nbr frames ready after sleep: %u.", frames_ready);
      if (SUCCEEDED(hr) && frames_ready > 0) {
        no_frames_counter = 0;
      } else {
        no_frames_counter++;
        if (no_frames_counter > 10) {
          logger_debug(
              &g_wasapi_logger,
              "Capture, no new frames from device in the last %d iterations.",
              no_frames_counter);
          if (!inactive) {
            logger_warn(&g_wasapi_logger,
                        "Capture, no data received, pausing stream.");
            inactive = true;
          }
          continue;
        }
      }
    }

    if (inactive) {
      logger_info(&g_wasapi_logger,
                  "Capture, new data received, resuming stream.");
      inactive = false;
    }

    UINT32 available_frames = 0;
    UINT32 next_packet_size = 0;
    if (wasapi_capture_get_next_packet_size(
            capture->capture_client, capture->exclusive, &next_packet_size)) {
      available_frames = next_packet_size;
    } else {
      if (capture->event_handle) {
        available_frames = capture->buffer_frame_count;
      } else {
        UINT32 padding = 0;
        IAudioClient_GetCurrentPadding(capture->client, &padding);
        available_frames = padding;
      }
    }

    logger_trace(&g_wasapi_logger, "Capture, available frames from dev: %u.",
                 available_frames);

    if (available_frames > 0) {
      while (true) {
        UINT32 nbr_frames_read = 0;
        DWORD flags = 0;
        if (!wasapi_capture_read_from_device(capture->capture_client, data,
                                             data_buf_size, blockalign,
                                             &nbr_frames_read, &flags)) {
          if (atomic_load_explicit(&capture->has_pending_rate_change,
                                   memory_order_acquire))
            break;
          break;
        }

        if (nbr_frames_read < available_frames) {
          logger_debug(&g_wasapi_logger, "Expected %u frames, got %u.",
                       available_frames, nbr_frames_read);
        }
        size_t nbr_bytes_loop = (size_t)nbr_frames_read * blockalign;
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
          logger_debug(&g_wasapi_logger, "Captured a buffer marked as silent.");
          memset(data, 0, nbr_bytes_loop);
        }

        spsc_byte_ring_buffer_write(capture->ring_buffer, data, nbr_bytes_loop);

        if (capture->exclusive && capture->event_handle) {
          break;
        }

        if (!capture->exclusive) {
          UINT32 next_frames = 0;
          if (wasapi_capture_get_next_packet_size(capture->capture_client,
                                                  false, &next_frames)) {
            if (next_frames == 0) break;
            logger_trace(&g_wasapi_logger,
                         "Capture, additional packet available with %u frames.",
                         next_frames);
            available_frames = next_frames;
          } else {
            break;
          }
        } else {
          UINT32 padding = 0;
          if (SUCCEEDED(
                  IAudioClient_GetCurrentPadding(capture->client, &padding))) {
            if (padding == 0) break;
            logger_trace(
                &g_wasapi_logger,
                "Capture, more frames available, current padding is %u frames.",
                padding);
            available_frames = padding;
          } else {
            break;
          }
        }
      }
    }
  }

  IAudioClient_Stop(capture->client);
  free(data);
  CoUninitialize();
  return NULL;
}

// MARK: - open_capture matching CamillaDSP device.rs:open_capture (lines
// 408-479)

static bool wasapi_capture_open(void* ctx, backend_error_t* err) {
  wasapi_capture_t* capture = (wasapi_capture_t*)ctx;
  if (!capture) return false;

  HRESULT init_hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  capture->com_initialized = SUCCEEDED(init_hr);
  atomic_init(&capture->stopped, false);
  atomic_init(&capture->paused, false);
  atomic_init(&capture->has_pending_rate_change, false);

  HRESULT hr =
      CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                       &IID_IMMDeviceEnumerator, (void**)&capture->enumerator);
  if (FAILED(hr)) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to create MMDeviceEnumerator");
    goto error_cleanup;
  }

  capture->mm_device = wasapi_find_device(capture->enumerator, capture->device,
                                          true, capture->loopback);
  if (!capture->mm_device) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_DEVICE_NOT_FOUND,
                         "WASAPI capture device not found");
    goto error_cleanup;
  }

  hr = IMMDevice_Activate(capture->mm_device, &IID_IAudioClient, CLSCTX_ALL,
                          NULL, (void**)&capture->client);
  if (FAILED(hr)) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to activate IAudioClient");
    goto error_cleanup;
  }
  logger_trace(&g_wasapi_logger, "Got capture iaudioclient.");

  bool exclusive = capture->loopback ? false : capture->exclusive;
  const char* direction_name = capture->loopback ? "Render" : "Capture";

  WAVEFORMATEXTENSIBLE wfx;
  bool is_std_wfx = false;
  wasapi_binary_sample_format_t bin_fmt;
  bool has_format = (capture->format != WASAPI_SAMPLE_FORMAT_INVALID);

  if (!wasapi_get_device_format(capture->client, capture->sample_rate,
                                capture->channels, capture->format, has_format,
                                exclusive, direction_name, &wfx, &is_std_wfx,
                                &bin_fmt, err)) {
    goto error_cleanup;
  }
  capture->bin_fmt = bin_fmt;
  capture->blockalign = (size_t)wfx.Format.nBlockAlign;

  REFERENCE_TIME def_time = 0, min_time = 0;
  IAudioClient_GetDevicePeriod(capture->client, &def_time, &min_time);
  capture->def_period = def_time;
  logger_debug(&g_wasapi_logger,
               "Capture default period %lld, min period %lld.",
               (long long)def_time, (long long)min_time);

  REFERENCE_TIME aligned_time = wasapi_calculate_aligned_period_near(
      capture->client, def_time, 128, capture->sample_rate,
      (int)capture->blockalign);

  AUDCLNT_SHAREMODE mode =
      exclusive ? AUDCLNT_SHAREMODE_EXCLUSIVE : AUDCLNT_SHAREMODE_SHARED;

  DWORD streamflags = 0;
  if (!capture->polling) {
    streamflags |= AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
  }
  if (capture->loopback) {
    streamflags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
  }

  REFERENCE_TIME buffer_duration = 0;
  REFERENCE_TIME period = 0;
  if (exclusive) {
    if (capture->polling) {
      buffer_duration = 8 * aligned_time;
      period = aligned_time;
    } else {
      buffer_duration = aligned_time;
      period = aligned_time;
    }
  } else {
    buffer_duration = 8 * def_time;
    period = 0;
  }

  logger_debug(&g_wasapi_logger,
               "Capture stream mode: polling=%d, exclusive=%d",
               capture->polling, exclusive);

  hr = IAudioClient_Initialize(capture->client, mode, streamflags,
                               buffer_duration, period,
                               (const WAVEFORMATEX*)&wfx, NULL);
  if (FAILED(hr)) {
    if (err) {
      char msg[256];
      snprintf(msg, sizeof(msg),
               "Failed to initialize IAudioClient (Capture): hr=0x%08lX",
               (unsigned long)hr);
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
    goto error_cleanup;
  }
  logger_debug(&g_wasapi_logger, "Initialized capture audio client.");

  if (!capture->polling) {
    capture->event_handle = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!capture->event_handle) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                           "Failed to create event handle");
      goto error_cleanup;
    }
    hr = IAudioClient_SetEventHandle(capture->client, capture->event_handle);
    if (FAILED(hr)) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                           "Failed to set event handle");
      goto error_cleanup;
    }
  } else {
    capture->event_handle = NULL;
  }
  logger_trace(&g_wasapi_logger, "Capture got event handle.");

  hr =
      IAudioClient_GetBufferSize(capture->client, &capture->buffer_frame_count);
  hr = IAudioClient_GetService(capture->client, &IID_IAudioCaptureClient,
                               (void**)&capture->capture_client);
  if (FAILED(hr)) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to get IAudioCaptureClient");
    goto error_cleanup;
  }

  IAudioClient_GetService(capture->client, &IID_IAudioSessionControl,
                          (void**)&capture->session_control);
  if (capture->session_control) {
    capture->session_events_listener =
        wasapi_session_events_create(capture, wasapi_capture_on_format_change);
    if (capture->session_events_listener) {
      IAudioSessionControl_RegisterAudioSessionNotification(
          capture->session_control, capture->session_events_listener);
    }
  }

  logger_debug(&g_wasapi_logger, "Opened Wasapi capture device \"%s\".",
               capture->device[0] != '\0' ? capture->device : "default");

  capture->bytes_per_sample =
      wasapi_binary_format_bytes_per_sample(capture->bin_fmt);
  capture->blockalign = (size_t)capture->channels * capture->bytes_per_sample;

  // Allocate SPSC byte ring buffer for audio samples
  size_t ring_size =
      (size_t)capture->channels * 4 * (2 * (size_t)capture->chunk_size + 2048);
  capture->ring_buffer = spsc_byte_ring_buffer_create(ring_size);

  capture->decode_buf_cap =
      (size_t)capture->chunk_size * capture->blockalign * 2;
  capture->decode_buf = (uint8_t*)malloc(capture->decode_buf_cap);

  atomic_store_explicit(&capture->thread_running, true, memory_order_release);
  if (pthread_create(&capture->inner_thread, NULL, wasapi_capture_loop,
                     capture) != 0) {
    atomic_store_explicit(&capture->thread_running, false,
                          memory_order_release);
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to create inner capture thread");
    goto error_cleanup;
  }

  return true;

error_cleanup:
  if (capture->decode_buf) {
    free(capture->decode_buf);
    capture->decode_buf = NULL;
  }
  if (capture->ring_buffer) {
    spsc_byte_ring_buffer_free(capture->ring_buffer);
    capture->ring_buffer = NULL;
  }
  if (capture->session_control) {
    if (capture->session_events_listener) {
      IAudioSessionControl_UnregisterAudioSessionNotification(
          capture->session_control, capture->session_events_listener);
      SAFE_RELEASE(capture->session_events_listener);
    }
    SAFE_RELEASE(capture->session_control);
  } else if (capture->session_events_listener) {
    SAFE_RELEASE(capture->session_events_listener);
  }
  if (capture->capture_client) {
    SAFE_RELEASE(capture->capture_client);
  }
  if (capture->client) {
    SAFE_RELEASE(capture->client);
  }
  if (capture->mm_device) {
    SAFE_RELEASE(capture->mm_device);
  }
  if (capture->enumerator) {
    SAFE_RELEASE(capture->enumerator);
  }
  if (capture->event_handle) {
    CloseHandle(capture->event_handle);
    capture->event_handle = NULL;
  }
  if (capture->com_initialized) {
    CoUninitialize();
    capture->com_initialized = false;
  }
  return false;
}

// MARK: - wasapi_capture_read matching CamillaDSP device.rs (lines 1381-1420)

static bool wasapi_capture_read(void* ctx, size_t frames, audio_chunk_t* chunk,
                                backend_error_t* err) {
  wasapi_capture_t* capture = (wasapi_capture_t*)ctx;
  if (!capture) return false;

  if (atomic_load_explicit(&capture->has_pending_rate_change,
                           memory_order_acquire)) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_NONE, "Format change pending");
    return false;
  }

  if (atomic_load_explicit(&capture->stopped, memory_order_acquire) ||
      !atomic_load_explicit(&capture->thread_running, memory_order_acquire)) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                         "Capture stream stopped");
    return false;
  }

  if (audio_chunk_get_channels(chunk) < (size_t)capture->channels) {
    if (err) {
      backend_error_init(
          err, BACKEND_ERROR_INVALID_CHANNELS,
          "Chunk channels count does not match capture channels");
    }
    return false;
  }

  size_t bytes_requested = frames * capture->blockalign;
  if (bytes_requested > capture->decode_buf_cap) {
    if (err) {
      backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                         "Frame count exceeds capture buffer capacity");
    }
    return false;
  }

  DWORD start_time = GetTickCount();
  while (spsc_byte_ring_buffer_get_available_to_read(capture->ring_buffer) <
         bytes_requested) {
    if (atomic_load_explicit(&capture->stopped, memory_order_acquire) ||
        !atomic_load_explicit(&capture->thread_running, memory_order_acquire)) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                           "Capture stream stopped");
      return false;
    }
    if (atomic_load_explicit(&capture->has_pending_rate_change,
                             memory_order_acquire)) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_NONE, "Format change pending");
      return false;
    }
    if (GetTickCount() - start_time > 3000) {
      // Stream timeout
      break;
    }
    cdsp_sleep_ms(1);
  }

  size_t consumed_bytes = spsc_byte_ring_buffer_consume(
      capture->ring_buffer, capture->decode_buf, bytes_requested);
  size_t valid_frames =
      (capture->blockalign > 0) ? (consumed_bytes / capture->blockalign) : 0;

  for (size_t f = 0; f < valid_frames; f++) {
    for (int c = 0; c < capture->channels; c++) {
      const uint8_t* src =
          capture->decode_buf + (f * (size_t)capture->channels + (size_t)c) *
                                    capture->bytes_per_sample;
      double sample = 0.0;
      switch (capture->bin_fmt) {
        case WASAPI_BINARY_FORMAT_S16_LE:
          sample = pcm_sample_decode_s16_bytes(src);
          break;
        case WASAPI_BINARY_FORMAT_S24_3_LE:
          sample = pcm_sample_decode_s24_3bytes(src);
          break;
        case WASAPI_BINARY_FORMAT_S24_4_LJ_LE:
          sample = pcm_sample_decode_s24_4_lj_bytes(src);
          break;
        case WASAPI_BINARY_FORMAT_S32_LE:
          sample = pcm_sample_decode_s32_bytes(src);
          break;
        case WASAPI_BINARY_FORMAT_F32_LE:
          sample = pcm_sample_decode_f32_bytes(src);
          break;
        default:
          sample = pcm_sample_decode_s32_bytes(src);
          break;
      }
      audio_chunk_get_channel(chunk, c)[f] = sample;
    }
  }
  audio_chunk_set_valid_frames(chunk, valid_frames);

  return true;
}

static void wasapi_capture_close(void* ctx) {
  wasapi_capture_t* capture = (wasapi_capture_t*)ctx;
  if (!capture) return;

  if (capture->thread_running) {
    atomic_store_explicit(&capture->thread_running, false,
                          memory_order_release);
    atomic_store_explicit(&capture->stopped, true, memory_order_release);
    if (capture->event_handle) SetEvent(capture->event_handle);
    pthread_join(capture->inner_thread, NULL);
  }

  if (capture->decode_buf) {
    free(capture->decode_buf);
    capture->decode_buf = NULL;
  }
  if (capture->ring_buffer) {
    spsc_byte_ring_buffer_free(capture->ring_buffer);
    capture->ring_buffer = NULL;
  }
  if (capture->client) {
    IAudioClient_Stop(capture->client);
  }
  SAFE_RELEASE(capture->capture_client);
  SAFE_RELEASE(capture->client);
  if (capture->session_control) {
    if (capture->session_events_listener) {
      IAudioSessionControl_UnregisterAudioSessionNotification(
          capture->session_control, capture->session_events_listener);
      SAFE_RELEASE(capture->session_events_listener);
    }
    SAFE_RELEASE(capture->session_control);
  }
  if (capture->event_handle) {
    CloseHandle(capture->event_handle);
    capture->event_handle = NULL;
  }
  SAFE_RELEASE(capture->mm_device);
  SAFE_RELEASE(capture->enumerator);

  if (capture->com_initialized) {
    CoUninitialize();
    capture->com_initialized = false;
  }
}

static bool wasapi_capture_get_pending_rate_change(void* ctx,
                                                   double* out_rate) {
  wasapi_capture_t* capture = (wasapi_capture_t*)ctx;
  if (!capture) return false;
  if (atomic_load_explicit(&capture->has_pending_rate_change,
                           memory_order_acquire)) {
    logger_info(&g_wasapi_logger,
                "capture get_pending_rate_change detected flag: "
                "pending_rate=%f, sample_rate=%d",
                capture->pending_rate, capture->sample_rate);
    double rate = capture->pending_rate;
    if (rate <= 0.0) {
      for (int i = 0; i < 100; i++) {
        rate = wasapi_device_get_current_mix_rate(capture->device,
                                                  !capture->loopback);
        if (rate > 0.0) break;
        cdsp_sleep_ms(50);
      }
    }
    atomic_store_explicit(&capture->has_pending_rate_change, false,
                          memory_order_release);
    logger_info(&g_wasapi_logger,
                "capture get_pending_rate_change evaluated final rate=%f",
                rate);
    if (rate > 0.0) {
      if (out_rate) {
        *out_rate = rate;
      }
      logger_info(&g_wasapi_logger,
                  "capture get_pending_rate_change returning true with rate=%f",
                  rate);
      return true;
    }
  }
  return false;
}

static bool wasapi_capture_pitch_control_supported(void* ctx) {
  (void)ctx;
  return false;
}

static void wasapi_capture_set_pitch(void* ctx, double multiplier) {
  (void)ctx;
  (void)multiplier;
}

static bool wasapi_capture_wait(void* ctx, uint32_t timeout_ms) {
  wasapi_capture_t* capture = (wasapi_capture_t*)ctx;
  if (!capture) return false;
  if (capture->polling) {
    cdsp_sleep_ms(1);
    return true;
  }
  if (!capture->event_handle) return false;
  DWORD res = WaitForSingleObject(capture->event_handle, timeout_ms);
  return (res == WAIT_OBJECT_0);
}

static void wasapi_capture_set_is_paused(void* ctx, bool paused) {
  wasapi_capture_t* capture = (wasapi_capture_t*)ctx;
  if (!capture) return;
  atomic_store_explicit(&capture->paused, paused, memory_order_release);
}

static void wasapi_capture_stop(void* ctx) {
  wasapi_capture_t* capture = (wasapi_capture_t*)ctx;
  if (!capture) return;
  atomic_store_explicit(&capture->stopped, true, memory_order_release);
  if (capture->event_handle) {
    SetEvent(capture->event_handle);
  }
}

static void wasapi_capture_destroy(void* ctx) {
  wasapi_capture_t* capture = (wasapi_capture_t*)ctx;
  if (capture) {
    wasapi_capture_close(capture);
    free(capture);
  }
}

static capture_backend_t* wasapi_capture_create(
    const capture_device_config_t* config, int sample_rate, int chunk_size,
    bool full_duplex, processing_parameters_t* params, backend_error_t* err) {
  (void)full_duplex;
  (void)params;
  (void)err;
  wasapi_capture_t* capture =
      (wasapi_capture_t*)calloc(1, sizeof(wasapi_capture_t));
  if (!capture) return NULL;

  if (config->cfg.wasapi.has_device &&
      strcmp(config->cfg.wasapi.device, "default") != 0) {
    snprintf(capture->device, sizeof(capture->device), "%s",
             config->cfg.wasapi.device);
  } else {
    capture->device[0] = '\0';
  }

  capture->sample_rate = sample_rate;
  capture->channels = config->cfg.wasapi.channels;
  capture->chunk_size = chunk_size;
  capture->format = config->cfg.wasapi.format;
  capture->loopback =
      config->cfg.wasapi.has_loopback ? config->cfg.wasapi.loopback : false;
  capture->exclusive =
      config->cfg.wasapi.has_exclusive ? config->cfg.wasapi.exclusive : false;
  capture->polling =
      config->cfg.wasapi.has_polling ? config->cfg.wasapi.polling : false;

  capture_backend_t* backend =
      (capture_backend_t*)calloc(1, sizeof(capture_backend_t));
  if (!backend) {
    free(capture);
    return NULL;
  }
  backend->ctx = capture;
  backend->vtable = &g_wasapi_capture_vtable;
  backend->is_realtime = true;
  return backend;
}

const capture_backend_vtable_t g_wasapi_capture_vtable = {
    .create = wasapi_capture_create,
    .open = wasapi_capture_open,
    .read = wasapi_capture_read,
    .close = wasapi_capture_close,
    .get_pending_rate_change = wasapi_capture_get_pending_rate_change,
    .is_pitch_control_supported = wasapi_capture_pitch_control_supported,
    .set_pitch = wasapi_capture_set_pitch,
    .wait_for_data = wasapi_capture_wait,
    .set_is_paused = wasapi_capture_set_is_paused,
    .stop = wasapi_capture_stop,
    .destroy = wasapi_capture_destroy};

#endif  // ENABLE_WASAPI
