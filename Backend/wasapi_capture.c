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

  binary_sample_format_t bin_fmt;
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
  DWORD task_index = 0;
  HANDLE mmcss_handle = AvSetMmThreadCharacteristicsA("Pro Audio", &task_index);
  if (!mmcss_handle) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  }
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

  if (!wasapi_create_device_and_client(
          capture->device, true, capture->loopback, &capture->enumerator,
          &capture->mm_device, &capture->client, err)) {
    goto error_cleanup;
  }
  logger_trace(&g_wasapi_logger, "Got capture iaudioclient.");

  bool exclusive = capture->loopback ? false : capture->exclusive;
  const char* direction_name = capture->loopback ? "Render" : "Capture";

  WAVEFORMATEXTENSIBLE wfx;
  bool is_std_wfx = false;
  binary_sample_format_t bin_fmt;
  bool has_format = (capture->format != WASAPI_SAMPLE_FORMAT_INVALID);

  if (!wasapi_get_device_format(capture->client, capture->sample_rate,
                                capture->channels, capture->format, has_format,
                                exclusive, direction_name, &wfx, &is_std_wfx,
                                &bin_fmt, err)) {
    goto error_cleanup;
  }
  capture->bin_fmt = bin_fmt;
  capture->blockalign = (size_t)wfx.Format.nBlockAlign;

  if (!wasapi_initialize_stream(capture->client, &wfx, capture->sample_rate,
                                capture->blockalign, exclusive,
                                capture->polling, capture->loopback,
                                &capture->def_period, &capture->event_handle,
                                &capture->buffer_frame_count, "Capture", err)) {
    goto error_cleanup;
  }

  HRESULT hr =
      IAudioClient_GetService(capture->client, &IID_IAudioCaptureClient,
                              (void**)&capture->capture_client);
  if (FAILED(hr)) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to get IAudioCaptureClient");
    goto error_cleanup;
  }

  wasapi_register_session_events(
      capture->client, capture, wasapi_capture_on_format_change,
      &capture->session_control, &capture->session_events_listener);

  logger_debug(&g_wasapi_logger, "Opened Wasapi capture device \"%s\".",
               capture->device[0] != '\0' ? capture->device : "default");

  capture->bytes_per_sample = sample_format_bytes_per_sample(capture->bin_fmt);
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
  wasapi_cleanup_device_resources(
      &capture->client, (IUnknown**)&capture->capture_client,
      &capture->session_control, &capture->session_events_listener,
      &capture->event_handle, &capture->mm_device, &capture->enumerator,
      &capture->com_initialized);
  return false;
}

// MARK: - wasapi_capture_read matching CamillaDSP device.rs (lines 1381-1420)

static bool wasapi_capture_read(void* ctx, size_t frames, audio_chunk_t* chunk,
                                backend_error_t* err) {
  wasapi_capture_t* capture = (wasapi_capture_t*)ctx;
  if (!capture) return false;
  return audio_backend_ring_buffer_read(
      capture->ring_buffer, capture->decode_buf, capture->decode_buf_cap,
      capture->blockalign, frames, (binary_sample_format_t)capture->bin_fmt,
      (size_t)capture->channels, 3000, &capture->thread_running,
      &capture->stopped, &capture->has_pending_rate_change, chunk, err);
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
  wasapi_cleanup_device_resources(
      &capture->client, (IUnknown**)&capture->capture_client,
      &capture->session_control, &capture->session_events_listener,
      &capture->event_handle, &capture->mm_device, &capture->enumerator,
      &capture->com_initialized);
}

static bool wasapi_capture_get_pending_rate_change(void* ctx,
                                                   double* out_rate) {
  wasapi_capture_t* capture = (wasapi_capture_t*)ctx;
  if (!capture) return false;
  return wasapi_check_and_resolve_pending_rate(
      capture->device, !capture->loopback, capture->pending_rate,
      &capture->has_pending_rate_change, out_rate);
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

  wasapi_extract_device_name(config->cfg.wasapi.has_device,
                             config->cfg.wasapi.device, capture->device,
                             sizeof(capture->device));

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
