#include "backend/wasapi_capture.h"

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

#include "audio/sample_conversion.h"
#include "backend/backend_buffer.h"
#include "backend/wasapi_capabilities.h"
#include "backend/wasapi_device.h"
#include "config/config_gen.h"
#include "utils/cdsp_time.h"

struct wasapi_capture {
  char device[256];
  int sample_rate;
  size_t channels;
  int chunk_size;
  wasapi_sample_format_t format;
  bool has_format;
  bool loopback;
  bool exclusive;
  bool polling;

  binary_sample_format_t bin_fmt;
  size_t bytes_per_sample;
  size_t blockalign;
  bool com_initialized;

  IMMDeviceEnumerator *enumerator;
  IMMDevice *mm_device;
  IAudioClient *client;
  IAudioCaptureClient *capture_client;
  IAudioSessionControl *session_control;
  IAudioSessionEvents *session_events_listener;
  UINT32 buffer_frame_count;
  REFERENCE_TIME def_period;
  HANDLE event_handle;

  pthread_t inner_thread;
  bool inner_thread_created;
  backend_buffer_t *buffer;
  processing_parameters_t *params;
  /** First fatal error of the inner thread, reported by read(). */
  wasapi_stream_error_t error;
};

/**
 * @brief Polls the session listener from the inner device thread.
 *
 * A FormatChanged disconnect sets the backend "pending rate change" flag (so
 * the engine stops reading obsolete-format data) and stops the inner loop. Any
 * other disconnect reason is a device error (upstream device.rs:606-613 ->
 * CaptureError): the error is recorded, the loop stops and the stream state
 * becomes STOPPED, which the engine reads as a capture error. The backend
 * buffer is only touched from this thread, never from the COM notification
 * thread.
 *
 * @return true if the inner loop must stop.
 */
static bool wasapi_capture_check_session(wasapi_capture_t *capture) {
  if (wasapi_session_events_format_changed(capture->session_events_listener)) {
    logger_debug(&g_wasapi_logger,
                 "Stopping inner capture loop due to session format change.");
    backend_buffer_set_pending_rate_change(capture->buffer, true);
    return true;
  }
  int reason = 0;
  if (wasapi_session_events_device_error(capture->session_events_listener,
                                         &reason)) {
    wasapi_stream_error_set(&capture->error,
                            "Capture failed with error: session disconnected "
                            "(%s, reason %d)",
                            wasapi_disconnect_reason_name(reason), reason);
    return true;
  }
  return false;
}

/**
 * @brief On WASAPI call error, checks if a format change notification is in
 * flight. Mirrors upstream CamillaDSP send_error_or_captureformatchange
 * (device.rs:1072-1088).
 */
static bool wasapi_capture_check_session_on_error(wasapi_capture_t *capture) {
  int reason = 0;
  for (int retry = 0; retry < 10; retry++) {
    if (wasapi_session_events_format_changed(
            capture->session_events_listener)) {
      break;
    }
    if (wasapi_session_events_device_error(capture->session_events_listener,
                                           &reason)) {
      break;
    }
    cdsp_sleep_ms(5);
  }
  return wasapi_capture_check_session(capture);
}

/**
 * @brief get_next_packet_size matching wasapi-rs api.rs (shared mode only).
 */
static inline HRESULT
wasapi_capture_get_next_packet_size(IAudioCaptureClient *capture_client,
                                    UINT32 *out_frames) {
  UINT32 frames = 0;
  HRESULT hr = IAudioCaptureClient_GetNextPacketSize(capture_client, &frames);
  *out_frames = SUCCEEDED(hr) ? frames : 0;
  return hr;
}

/**
 * Read-only zero block used to push AUDCLNT_BUFFERFLAGS_SILENT packets into
 * the ring without a staging buffer. All negotiated WASAPI formats (S16, S24,
 * S32, F32) encode silence as zero bytes.
 */
static const uint8_t k_wasapi_capture_silence[16384];

/**
 * @brief Pushes @p frames frames of silence into the capture ring (whole
 * frames, drop-on-full like backend_buffer_push).
 */
static void wasapi_capture_push_silence(backend_buffer_t *buffer, size_t frames,
                                        size_t blockalign) {
  if (blockalign == 0)
    return;
  size_t per_push = sizeof(k_wasapi_capture_silence) / blockalign;
  if (per_push == 0)
    return;
  while (frames > 0) {
    size_t n = frames < per_push ? frames : per_push;
    size_t pushed = backend_buffer_push(buffer, k_wasapi_capture_silence, n);
    frames -= n;
    if (pushed < n)
      break; // Ring full: drop the rest (drop-on-full policy).
  }
}

/**
 * @brief read_from_device matching wasapi-rs api.rs, but zero-copy.
 *
 * Upstream (device.rs:728, api.rs:1940-1975) copies each packet into a
 * heap-allocated staging Vec and then pushes that into the ring. Per AGENTS.md
 * §3.4, cdsp pushes the device packet straight into the SPSC ring between
 * GetBuffer and ReleaseBuffer: no staging buffer, one copy, and no upper
 * bound on the packet size (upstream returns DataLengthTooShort when a
 * packet does not fit its staging buffer; here the ring applies its normal
 * drop-on-full policy instead).
 *
 * @return The HRESULT of GetBuffer (on failure) or ReleaseBuffer.
 */
static inline HRESULT wasapi_capture_read_from_device(
    IAudioCaptureClient *capture_client, backend_buffer_t *buffer,
    size_t bytes_per_frame, UINT32 *out_frames_read, DWORD *out_flags) {
  BYTE *buffer_ptr = NULL;
  UINT32 nbr_frames_returned = 0;
  DWORD flags = 0;
  UINT64 index = 0;
  UINT64 timestamp = 0;

  *out_frames_read = 0;
  *out_flags = 0;
  HRESULT hr = IAudioCaptureClient_GetBuffer(capture_client, &buffer_ptr,
                                             &nbr_frames_returned, &flags,
                                             &index, &timestamp);
  if (FAILED(hr)) {
    return hr;
  }

  *out_frames_read = nbr_frames_returned;
  *out_flags = flags;

  if (nbr_frames_returned == 0) {
    return hr;
  }

  if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) || !buffer_ptr) {
    logger_debug(&g_wasapi_logger, "Captured a buffer marked as silent.");
    wasapi_capture_push_silence(buffer, (size_t)nbr_frames_returned,
                                bytes_per_frame);
  } else {
    backend_buffer_push(buffer, buffer_ptr, (size_t)nbr_frames_returned);
  }

  return IAudioCaptureClient_ReleaseBuffer(capture_client, nbr_frames_returned);
}

/**
 * @brief capture_loop matching CamillaDSP device.rs:capture_loop (lines
 * 634-838).
 */
static void *wasapi_capture_loop(void *arg) {
  wasapi_capture_t *capture = (wasapi_capture_t *)arg;
  bool com_ok = SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED));

  size_t blockalign = capture->blockalign;
  bool inactive = false;

  // Device period as queried (and checked) by wasapi_initialize_stream in
  // open(); it is fixed for the lifetime of the initialized client.
  REFERENCE_TIME def_time = capture->def_period;
  uint64_t poll_delay_us = (uint64_t)(def_time / 10);
  if (poll_delay_us == 0)
    poll_delay_us = 1000;

  if (!capture->event_handle) {
    logger_debug(&g_wasapi_logger,
                 "Capture uses polling mode, delay is %lu us.",
                 (unsigned long)(def_time / 10));
  }

  int no_frames_counter = 0;

  DWORD task_index = 0;
  HANDLE mmcss_handle = AvSetMmThreadCharacteristicsA("Pro Audio", &task_index);
  if (!mmcss_handle) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  }

  bool fatal = false;
  logger_trace(&g_wasapi_logger, "Starting capture stream.");
  HRESULT hr = IAudioClient_Start(capture->client);
  if (FAILED(hr)) {
    wasapi_stream_error_set(&capture->error,
                            "Capture failed with error: start stream failed "
                            "(hr=0x%08lX)",
                            (unsigned long)hr);
    fatal = true;
  } else {
    logger_trace(&g_wasapi_logger, "Started capture stream.");
  }

  // Every WASAPI call below is fatal on failure, matching the `?` operators
  // in upstream capture_loop (device.rs:673, 705-718, 727-728, 748): the
  // error is recorded, the loop ends and the stream state becomes STOPPED, so
  // read() reports BACKEND_ERROR_READ_ERROR with the HRESULT (upstream:
  // DeviceState::Error -> CaptureError, device.rs:1219-1223, 1294-1298).
  while (!fatal &&
         backend_buffer_get_state(capture->buffer) != BACKEND_STREAM_STOPPED) {
    logger_trace(&g_wasapi_logger, "Capturing.");
    if (wasapi_capture_check_session(capture)) {
      break;
    }

    if (capture->event_handle) {
      DWORD wait_res = WaitForSingleObject(capture->event_handle, 250);
      if (backend_buffer_get_state(capture->buffer) == BACKEND_STREAM_STOPPED) {
        logger_debug(&g_wasapi_logger,
                     "Stopping inner capture loop on request.");
        break;
      }
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
      cdsp_sleep_us(poll_delay_us);
      if (backend_buffer_get_state(capture->buffer) == BACKEND_STREAM_STOPPED) {
        logger_debug(&g_wasapi_logger,
                     "Stopping inner capture loop on request.");
        break;
      }
      UINT32 frames_ready = 0;
      hr = IAudioClient_GetCurrentPadding(capture->client, &frames_ready);
      if (FAILED(hr)) {
        if (wasapi_capture_check_session_on_error(capture)) {
          break;
        }
        wasapi_stream_error_set(&capture->error,
                                "Capture failed with error: GetCurrentPadding "
                                "failed (hr=0x%08lX)",
                                (unsigned long)hr);
        fatal = true;
        break;
      }
      logger_trace(&g_wasapi_logger,
                   "Capture, nbr frames ready after sleep: %u.", frames_ready);
      if (frames_ready > 0) {
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
    const char *failed_call = NULL;
    if (!capture->exclusive) {
      hr = wasapi_capture_get_next_packet_size(capture->capture_client,
                                               &available_frames);
      failed_call = "GetNextPacketSize";
    } else if (capture->event_handle) {
      available_frames = capture->buffer_frame_count;
      hr = S_OK;
    } else {
      hr = IAudioClient_GetCurrentPadding(capture->client, &available_frames);
      failed_call = "GetCurrentPadding";
    }
    if (FAILED(hr)) {
      if (wasapi_capture_check_session_on_error(capture)) {
        break;
      }
      wasapi_stream_error_set(&capture->error,
                              "Capture failed with error: %s failed "
                              "(hr=0x%08lX)",
                              failed_call, (unsigned long)hr);
      fatal = true;
      break;
    }

    logger_trace(&g_wasapi_logger, "Capture, available frames from dev: %u.",
                 available_frames);

    while (available_frames > 0 &&
           backend_buffer_get_state(capture->buffer) !=
               BACKEND_STREAM_STOPPED) {
      UINT32 nbr_frames_read = 0;
      DWORD flags = 0;
      hr = wasapi_capture_read_from_device(capture->capture_client,
                                           capture->buffer, blockalign,
                                           &nbr_frames_read, &flags);
      if (FAILED(hr)) {
        if (wasapi_capture_check_session_on_error(capture)) {
          break;
        }
        wasapi_stream_error_set(&capture->error,
                                "Capture failed with error: reading from "
                                "device failed (hr=0x%08lX)",
                                (unsigned long)hr);
        fatal = true;
        break;
      }

      if (nbr_frames_read < available_frames) {
        logger_debug(&g_wasapi_logger, "Expected %u frames, got %u.",
                     available_frames, nbr_frames_read);
      }
      if (nbr_frames_read == 0) {
        break;
      }

      if (capture->exclusive && capture->event_handle) {
        break;
      }

      UINT32 more_frames = 0;
      if (!capture->exclusive) {
        hr = wasapi_capture_get_next_packet_size(capture->capture_client,
                                                 &more_frames);
        failed_call = "GetNextPacketSize";
      } else {
        hr = IAudioClient_GetCurrentPadding(capture->client, &more_frames);
        failed_call = "GetCurrentPadding";
      }
      if (FAILED(hr)) {
        if (wasapi_capture_check_session_on_error(capture)) {
          break;
        }
        wasapi_stream_error_set(&capture->error,
                                "Capture failed with error: %s failed "
                                "(hr=0x%08lX)",
                                failed_call, (unsigned long)hr);
        fatal = true;
        break;
      }
      if (more_frames > 0) {
        logger_trace(&g_wasapi_logger,
                     "Capture, more frames available: %u frames.", more_frames);
      }
      available_frames = more_frames;
    }
  }

  backend_buffer_set_state(capture->buffer, BACKEND_STREAM_STOPPED);
  IAudioClient_Stop(capture->client);
  if (mmcss_handle) {
    AvRevertMmThreadCharacteristics(mmcss_handle);
  }
  if (com_ok) {
    CoUninitialize();
  }
  return NULL;
}

// MARK: - open_capture matching CamillaDSP device.rs:open_capture (lines
// 408-479)

static bool wasapi_capture_open(void *ctx, backend_error_t *err) {
  wasapi_capture_t *capture = (wasapi_capture_t *)ctx;
  if (!capture)
    return false;

  HRESULT init_hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  capture->com_initialized = SUCCEEDED(init_hr);

  if (!wasapi_create_device_and_client(
          capture->device, true, capture->loopback, &capture->enumerator,
          &capture->mm_device, &capture->client, err)) {
    goto error_cleanup;
  }
  logger_trace(&g_wasapi_logger, "Got capture iaudioclient.");

  if (capture->loopback && capture->exclusive) {
    if (err) {
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Loopback is not supported in exclusive mode");
    }
    goto error_cleanup;
  }
  bool exclusive = capture->exclusive;
  const char *direction_name = capture->loopback ? "Render" : "Capture";

  WAVEFORMATEXTENSIBLE wfx;
  bool is_std_wfx = false;
  binary_sample_format_t bin_fmt;
  bool has_format = capture->has_format;

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
                              (void **)&capture->capture_client);
  if (FAILED(hr)) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to get IAudioCaptureClient");
    goto error_cleanup;
  }

  if (!wasapi_register_session_events(capture->client,
                                      &capture->session_control,
                                      &capture->session_events_listener, err)) {
    goto error_cleanup;
  }

  logger_debug(&g_wasapi_logger, "Opened Wasapi capture device \"%s\".",
               capture->device[0] != '\0' ? capture->device : "default");

  capture->bytes_per_sample = sample_format_bytes_per_sample(capture->bin_fmt);
  capture->blockalign = (size_t)capture->channels * capture->bytes_per_sample;

  // Allocate backend buffer for audio samples
  size_t ring_frames = 2 * (size_t)capture->chunk_size + 2048;
  capture->buffer = backend_buffer_create(
      ring_frames, capture->bin_fmt, capture->channels,
      (double)capture->sample_rate, false, capture->params);
  if (!capture->buffer) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to allocate capture buffer");
    goto error_cleanup;
  }
  backend_buffer_set_state(capture->buffer, BACKEND_STREAM_RUNNING);
  wasapi_stream_error_reset(&capture->error);

  if (pthread_create(&capture->inner_thread, NULL, wasapi_capture_loop,
                     capture) != 0) {
    backend_buffer_set_state(capture->buffer, BACKEND_STREAM_STOPPED);
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to create inner capture thread");
    goto error_cleanup;
  }
  capture->inner_thread_created = true;

  return true;

error_cleanup:
  wasapi_cleanup_device_resources(
      &capture->client, (IUnknown **)&capture->capture_client,
      &capture->session_control, &capture->session_events_listener,
      &capture->event_handle, &capture->mm_device, &capture->enumerator,
      &capture->com_initialized);
  backend_buffer_free(capture->buffer);
  capture->buffer = NULL;
  return false;
}

// MARK: - wasapi_capture_read matching CamillaDSP device.rs (lines 1381-1420)

static bool wasapi_capture_read(void *ctx, size_t frames, audio_chunk_t *chunk,
                                backend_error_t *err) {
  wasapi_capture_t *capture = (wasapi_capture_t *)ctx;
  if (!capture)
    return false;
  bool ok = backend_buffer_read_chunk(capture->buffer, frames, chunk, err);
  if (!ok && err && err->type == BACKEND_ERROR_READ_ERROR) {
    // Replace the generic "stream stopped" text with the HRESULT / disconnect
    // reason recorded by the inner thread (upstream CaptureError(msg)).
    const char *msg = wasapi_stream_error_get(&capture->error);
    if (msg)
      backend_error_init(err, BACKEND_ERROR_READ_ERROR, msg);
  }
  return ok;
}

static void wasapi_capture_close(void *ctx) {
  wasapi_capture_t *capture = (wasapi_capture_t *)ctx;
  if (!capture)
    return;

  if (capture->inner_thread_created) {
    backend_buffer_set_state(capture->buffer, BACKEND_STREAM_STOPPED);
    if (capture->event_handle)
      SetEvent(capture->event_handle);
    pthread_join(capture->inner_thread, NULL);
    capture->inner_thread_created = false;
  }

  // Unregister the session listener and release the device before freeing
  // the buffer. The inner thread (the only user of the buffer besides the
  // engine) has been joined above.
  wasapi_cleanup_device_resources(
      &capture->client, (IUnknown **)&capture->capture_client,
      &capture->session_control, &capture->session_events_listener,
      &capture->event_handle, &capture->mm_device, &capture->enumerator,
      &capture->com_initialized);
  backend_buffer_free(capture->buffer);
  capture->buffer = NULL;
}

static bool wasapi_capture_get_pending_rate_change(void *ctx,
                                                   double *out_rate) {
  wasapi_capture_t *capture = (wasapi_capture_t *)ctx;
  if (!capture)
    return false;
  return wasapi_check_and_resolve_pending_rate(
      capture->device, !capture->loopback, capture->exclusive,
      (double)capture->sample_rate, capture->session_events_listener, out_rate);
}

static bool wasapi_capture_pitch_control_supported(void *ctx) {
  (void)ctx;
  return false;
}

static void wasapi_capture_set_pitch(void *ctx, double multiplier) {
  (void)ctx;
  (void)multiplier;
}

static bool wasapi_capture_wait(void *ctx, uint32_t timeout_ms) {
  wasapi_capture_t *capture = (wasapi_capture_t *)ctx;
  if (!capture)
    return false;
  return backend_buffer_wait(capture->buffer, timeout_ms);
}

static void wasapi_capture_stop(void *ctx) {
  wasapi_capture_t *capture = (wasapi_capture_t *)ctx;
  if (!capture)
    return;
  backend_buffer_set_state(capture->buffer, BACKEND_STREAM_STOPPED);
  if (capture->event_handle) {
    SetEvent(capture->event_handle);
  }
}

static void wasapi_capture_destroy(void *ctx) {
  wasapi_capture_t *capture = (wasapi_capture_t *)ctx;
  if (capture) {
    wasapi_capture_close(capture);
    free(capture);
  }
}

static capture_backend_t *
wasapi_capture_create(const capture_device_config_t *config, int sample_rate,
                      int chunk_size, bool full_duplex,
                      processing_parameters_t *params, backend_error_t *err) {
  (void)full_duplex;
  (void)params;
  (void)err;
  wasapi_capture_t *capture =
      (wasapi_capture_t *)calloc(1, sizeof(wasapi_capture_t));
  if (!capture)
    return NULL;

  wasapi_extract_device_name(config->cfg.wasapi.has_device,
                             config->cfg.wasapi.device, capture->device,
                             sizeof(capture->device));

  capture->sample_rate = sample_rate;
  capture->channels = config->cfg.wasapi.channels;
  capture->chunk_size = chunk_size;
  capture->format = config->cfg.wasapi.format;
  capture->has_format = config->cfg.wasapi.has_format;
  capture->loopback =
      config->cfg.wasapi.has_loopback ? config->cfg.wasapi.loopback : false;
  capture->exclusive =
      config->cfg.wasapi.has_exclusive ? config->cfg.wasapi.exclusive : false;
  capture->polling =
      config->cfg.wasapi.has_polling ? config->cfg.wasapi.polling : false;
  capture->params = params;

  capture_backend_t *backend =
      (capture_backend_t *)calloc(1, sizeof(capture_backend_t));
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
    .stop = wasapi_capture_stop,
    .destroy = wasapi_capture_destroy};

#endif // ENABLE_WASAPI
