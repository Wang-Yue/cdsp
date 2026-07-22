/**
 * @file wasapi_capture.c
 * @brief WASAPI capture backend implementation.
 */

#if defined(ENABLE_WASAPI)

#include "wasapi_capture.h"

#include <windef.h>
#include <windows.h>
#ifndef CDECL
#define CDECL __cdecl
#endif
#include <initguid.h>
#include <ks.h>
#include <ksmedia.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "Audio/sample_conversion.h"
#include "Engine/cdsp_sem.h"
#include "Utils/cdsp_time.h"
#include "wasapi_capabilities.h"
#include "wasapi_device.h"

struct wasapi_capture {
  char device[256];
  int sample_rate;
  int channels;
  int chunk_size;
  wasapi_sample_format_t format;
  bool loopback;
  bool exclusive;
  bool polling;

  int bits_per_sample;
  int valid_bits;
  bool is_float;
  bool com_initialized;

  IMMDeviceEnumerator* enumerator;
  IMMDevice* mm_device;
  IAudioClient* client;
  IAudioCaptureClient* capture_client;
  IAudioSessionControl* session_control;
  IAudioSessionEvents* session_events_listener;
  UINT32 buffer_frame_count;
  cdsp_sem_t semaphore;
  audio_chunk_t* residual_chunk;
  size_t residual_frames;
  size_t residual_offset;
  _Atomic bool stopped;
  double pending_rate;
  bool has_pending_rate_change;
};

static void wasapi_capture_on_format_change(void* parent, double new_rate) {
  wasapi_capture_t* capture = (wasapi_capture_t*)parent;
  capture->pending_rate = new_rate;
  capture->has_pending_rate_change = true;
}

/**
 * @brief Decodes interleaved audio samples from WASAPI input buffer format to
 * deinterleaved double format.
 */
static inline void decode_samples_from_wasapi(audio_chunk_t* chunk,
                                              size_t chunk_offset,
                                              const BYTE* src, size_t frames,
                                              int channels, DWORD flags,
                                              int bits_per_sample,
                                              int valid_bits, bool is_float) {
  if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
    for (size_t f = 0; f < frames; f++) {
      for (int c = 0; c < channels; c++) {
        audio_chunk_get_channel(chunk, c)[chunk_offset + f] = 0.0;
      }
    }
    return;
  }

  if (is_float) {
    const float* f32 = (const float*)src;
    for (size_t f = 0; f < frames; f++) {
      for (int c = 0; c < channels; c++) {
        audio_chunk_get_channel(chunk, c)[chunk_offset + f] =
            pcm_sample_decode_f32(f32[f * channels + c]);
      }
    }
    return;
  }

  if (bits_per_sample == 16) {
    const int16_t* s16 = (const int16_t*)src;
    for (size_t f = 0; f < frames; f++) {
      for (int c = 0; c < channels; c++) {
        audio_chunk_get_channel(chunk, c)[chunk_offset + f] =
            pcm_sample_decode_s16(s16[f * channels + c]);
      }
    }
  } else if (bits_per_sample == 24) {
    for (size_t f = 0; f < frames; f++) {
      for (int c = 0; c < channels; c++) {
        size_t idx = (f * channels + c) * 3;
        audio_chunk_get_channel(chunk, c)[chunk_offset + f] =
            pcm_sample_decode_s24_3bytes(&src[idx]);
      }
    }
  } else if (bits_per_sample == 32 && valid_bits == 24) {
    const int32_t* s32 = (const int32_t*)src;
    for (size_t f = 0; f < frames; f++) {
      for (int c = 0; c < channels; c++) {
        audio_chunk_get_channel(chunk, c)[chunk_offset + f] =
            pcm_sample_decode_s24(s32[f * channels + c] >> 8);
      }
    }
  } else if (bits_per_sample == 32 && valid_bits == 32) {
    const int32_t* s32 = (const int32_t*)src;
    for (size_t f = 0; f < frames; f++) {
      for (int c = 0; c < channels; c++) {
        audio_chunk_get_channel(chunk, c)[chunk_offset + f] =
            pcm_sample_decode_s32(s32[f * channels + c]);
      }
    }
  }
}

static bool wasapi_capture_open(void* ctx, backend_error_t* err) {
  wasapi_capture_t* capture = (wasapi_capture_t*)ctx;
  if (!capture) return false;
  WAVEFORMATEX* final_wfx = NULL;
  HRESULT init_hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  capture->com_initialized = SUCCEEDED(init_hr);
  atomic_init(&capture->stopped, false);

  HRESULT hr =
      CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                       &IID_IMMDeviceEnumerator, (void**)&capture->enumerator);
  if (FAILED(hr)) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to create MMDeviceEnumerator");
    goto error_cleanup;
  }

  capture->mm_device = wasapi_find_device_by_name(
      capture->enumerator, capture->device, !capture->loopback);

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

  AUDCLNT_SHAREMODE mode = AUDCLNT_SHAREMODE_SHARED;
  if (capture->exclusive && !capture->loopback) {
    mode = AUDCLNT_SHAREMODE_EXCLUSIVE;
  }

  WAVEFORMATEXTENSIBLE wfx = {0};
  wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  wfx.Format.nChannels = capture->channels;
  wfx.Format.nSamplesPerSec = capture->sample_rate;
  wfx.Format.cbSize = 22;
  wfx.dwChannelMask =
      (capture->channels == 2) ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : 0;

  bool format_found = false;
  WAVEFORMATEX std_wfx = {0};
  bool use_ext = true;
  if (mode == AUDCLNT_SHAREMODE_SHARED) {
    format_found = wasapi_setup_shared_format(
        capture->client, capture->sample_rate, &final_wfx,
        &capture->bits_per_sample, &capture->valid_bits, &capture->is_float);
  }

  if (!format_found) {
    if (capture->format == WASAPI_SAMPLE_FORMAT_S16) {
      wfx.Format.wBitsPerSample = 16;
      wfx.Format.nBlockAlign = 2 * capture->channels;
      wfx.Format.nAvgBytesPerSec =
          capture->sample_rate * wfx.Format.nBlockAlign;
      wfx.Samples.wValidBitsPerSample = 16;
      wfx.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
      if (wasapi_check_format_supported(capture->client, mode, &wfx, &std_wfx,
                                        &use_ext)) {
        capture->bits_per_sample = 16;
        capture->valid_bits = 16;
        capture->is_float = false;
        format_found = true;
      }
    } else if (capture->format == WASAPI_SAMPLE_FORMAT_S32) {
      wfx.Format.wBitsPerSample = 32;
      wfx.Format.nBlockAlign = 4 * capture->channels;
      wfx.Format.nAvgBytesPerSec =
          capture->sample_rate * wfx.Format.nBlockAlign;
      wfx.Samples.wValidBitsPerSample = 32;
      wfx.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
      if (wasapi_check_format_supported(capture->client, mode, &wfx, &std_wfx,
                                        &use_ext)) {
        capture->bits_per_sample = 32;
        capture->valid_bits = 32;
        capture->is_float = false;
        format_found = true;
      }
    } else if (capture->format == WASAPI_SAMPLE_FORMAT_F32) {
      wfx.Format.wBitsPerSample = 32;
      wfx.Format.nBlockAlign = 4 * capture->channels;
      wfx.Format.nAvgBytesPerSec =
          capture->sample_rate * wfx.Format.nBlockAlign;
      wfx.Samples.wValidBitsPerSample = 32;
      wfx.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
      if (wasapi_check_format_supported(capture->client, mode, &wfx, &std_wfx,
                                        &use_ext)) {
        capture->bits_per_sample = 32;
        capture->valid_bits = 32;
        capture->is_float = true;
        format_found = true;
      }
    } else if (capture->format == WASAPI_SAMPLE_FORMAT_S24) {
      wfx.Format.wBitsPerSample = 24;
      wfx.Format.nBlockAlign = 3 * capture->channels;
      wfx.Format.nAvgBytesPerSec =
          capture->sample_rate * wfx.Format.nBlockAlign;
      wfx.Samples.wValidBitsPerSample = 24;
      wfx.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
      if (wasapi_check_format_supported(capture->client, mode, &wfx, &std_wfx,
                                        &use_ext)) {
        capture->bits_per_sample = 24;
        capture->valid_bits = 24;
        capture->is_float = false;
        format_found = true;
      } else {
        wfx.Format.wBitsPerSample = 32;
        wfx.Format.nBlockAlign = 4 * capture->channels;
        wfx.Format.nAvgBytesPerSec =
            capture->sample_rate * wfx.Format.nBlockAlign;
        wfx.Samples.wValidBitsPerSample = 24;
        wfx.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        if (wasapi_check_format_supported(capture->client, mode, &wfx, &std_wfx,
                                          &use_ext)) {
          capture->bits_per_sample = 32;
          capture->valid_bits = 24;
          capture->is_float = false;
          format_found = true;
        }
      }
    }
  }

  if (!format_found) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Unsupported sample format");
    goto error_cleanup;
  }

  REFERENCE_TIME duration = 0;
  REFERENCE_TIME periodicity = 0;
  REFERENCE_TIME def_period = 0, min_period = 0;
  IAudioClient_GetDevicePeriod(capture->client, &def_period, &min_period);

  if (mode == AUDCLNT_SHAREMODE_EXCLUSIVE) {
    REFERENCE_TIME aligned_time = wasapi_calculate_aligned_period_near(
        capture->client, def_period, 128,
        final_wfx ? (WAVEFORMATEXTENSIBLE*)final_wfx
                  : (use_ext ? &wfx : (WAVEFORMATEXTENSIBLE*)&std_wfx));
    if (capture->polling) {
      duration = 8 * aligned_time;
      periodicity = aligned_time;
    } else {
      duration = aligned_time;
      periodicity = aligned_time;
    }
  } else {
    duration = 8 * def_period;
    periodicity = 0;
  }

  DWORD flags = (capture->loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0);
  if (!capture->polling) {
    flags |= AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
  }

  hr = IAudioClient_Initialize(
      capture->client, mode, flags, duration, periodicity,
      final_wfx ? final_wfx : (use_ext ? (WAVEFORMATEX*)&wfx : &std_wfx), NULL);
  if (FAILED(hr)) {
    WAVEFORMATEX* mix_wfx = NULL;
    if (SUCCEEDED(IAudioClient_GetMixFormat(capture->client, &mix_wfx)) &&
        mix_wfx) {
      if (mix_wfx->wFormatTag == 65534) {
        WAVEFORMATEXTENSIBLE* wfx_ext = (WAVEFORMATEXTENSIBLE*)mix_wfx;
        LPOLESTR subformat_str = NULL;
        StringFromCLSID(&wfx_ext->SubFormat, &subformat_str);
        char fmt_buf[512];
        snprintf(fmt_buf, sizeof(fmt_buf),
                 "Capture mix format EXT: rate=%u, channels=%u, bits=%u, "
                 "valid_bits=%u, subformat=%ls",
                 (unsigned int)mix_wfx->nSamplesPerSec,
                 (unsigned int)mix_wfx->nChannels,
                 (unsigned int)mix_wfx->wBitsPerSample,
                 (unsigned int)wfx_ext->Samples.wValidBitsPerSample,
                 subformat_str);
        logger_error(&g_wasapi_logger, "%s", fmt_buf);
        CoTaskMemFree(subformat_str);
      } else {
        logger_error(
            &g_wasapi_logger,
            "Capture mix format std: rate=%u, channels=%u, bits=%u, tag=%u",
            (unsigned int)mix_wfx->nSamplesPerSec,
            (unsigned int)mix_wfx->nChannels,
            (unsigned int)mix_wfx->wBitsPerSample,
            (unsigned int)mix_wfx->wFormatTag);
      }
      CoTaskMemFree(mix_wfx);
    }
    REFERENCE_TIME debug_def = 0, debug_min = 0;
    IAudioClient_GetDevicePeriod(capture->client, &debug_def, &debug_min);
    logger_error(&g_wasapi_logger,
                 "Capture periods: def=%lld, min=%lld, "
                 "requested_duration=%lld, flags=0x%08lX",
                 (long long)debug_def, (long long)debug_min,
                 (long long)duration, (unsigned long)flags);
    if (err) {
      char msg[256];
      snprintf(msg, sizeof(msg),
               "Failed to initialize IAudioClient (Capture): hr=0x%08lX",
               (unsigned long)hr);
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
    goto error_cleanup;
  }

  if (!capture->polling) {
    capture->semaphore = cdsp_sem_create();
    if (!capture->semaphore) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                           "Failed to create event handle");
      goto error_cleanup;
    }

    hr = IAudioClient_SetEventHandle(capture->client,
                                     (HANDLE)capture->semaphore);
    if (FAILED(hr)) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                           "Failed to set event handle");
      goto error_cleanup;
    }
  } else {
    capture->semaphore = NULL;
  }

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

  capture->residual_chunk =
      audio_chunk_create(capture->chunk_size * 4, capture->channels);
  if (!capture->residual_chunk) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to allocate WASAPI capture residual buffer");
    goto error_cleanup;
  }
  capture->residual_frames = 0;
  capture->residual_offset = 0;

  IAudioClient_Start(capture->client);

  logger_info(&g_wasapi_logger,
              "Opened WASAPI capture: device=%s, rate=%d, channels=%d",
              capture->device[0] != '\0' ? capture->device : "default",
              capture->sample_rate, capture->channels);
  logger_info(&g_wasapi_logger,
              "WASAPI capture options: loopback=%d, exclusive=%d",
              capture->loopback, capture->exclusive);

  if (final_wfx) CoTaskMemFree(final_wfx);
  return true;

error_cleanup:
  if (final_wfx) CoTaskMemFree(final_wfx);
  if (capture->session_control) {
    SAFE_RELEASE(capture->session_control);
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
  if (capture->semaphore) {
    cdsp_sem_destroy(capture->semaphore);
    capture->semaphore = NULL;
  }
  if (capture->com_initialized) {
    CoUninitialize();
    capture->com_initialized = false;
  }
  if (capture->residual_chunk) {
    audio_chunk_free(capture->residual_chunk);
    capture->residual_chunk = NULL;
  }
  return false;
}

static bool wasapi_capture_read(void* ctx, size_t frames, audio_chunk_t* chunk,
                                backend_error_t* err) {
  wasapi_capture_t* capture = (wasapi_capture_t*)ctx;
  if (!capture) return false;
  if (audio_chunk_get_channels(chunk) < (size_t)capture->channels) {
    if (err) {
      backend_error_init(
          err, BACKEND_ERROR_INVALID_CHANNELS,
          "Chunk channels count does not match capture channels");
    }
    return false;
  }
  size_t frames_read = 0;
  DWORD start_time = GetTickCount();

  if (capture->residual_frames > 0) {
    size_t to_copy = frames - frames_read;
    if (to_copy > capture->residual_frames) {
      to_copy = capture->residual_frames;
    }
    for (size_t ch = 0; ch < (size_t)capture->channels; ch++) {
      double* dst = audio_chunk_get_channel(chunk, ch);
      const double* src = audio_chunk_get_channel(capture->residual_chunk, ch);
      if (dst && src) {
        memcpy(dst + frames_read, src + capture->residual_offset,
               to_copy * sizeof(double));
      }
    }
    frames_read += to_copy;
    capture->residual_frames -= to_copy;
    capture->residual_offset += to_copy;
    if (capture->residual_frames == 0) {
      capture->residual_offset = 0;
    }
  }

  while (frames_read < frames) {
    if (atomic_load_explicit(&capture->stopped, memory_order_acquire)) {
      return false;
    }
    if (GetTickCount() - start_time > 250) {
      audio_chunk_set_valid_frames(chunk, 0);
      return true;
    }

    UINT32 packet_size = 0;
    HRESULT hr = S_OK;
    if (capture->exclusive) {
      if (capture->polling) {
        hr = IAudioClient_GetCurrentPadding(capture->client, &packet_size);
      } else {
        packet_size = capture->buffer_frame_count;
        hr = S_OK;
      }
    } else {
      hr = IAudioCaptureClient_GetNextPacketSize(capture->capture_client,
                                                 &packet_size);
    }
    if (FAILED(hr)) {
      if (hr == AUDCLNT_E_DEVICE_INVALIDATED ||
          hr == AUDCLNT_E_RESOURCES_INVALIDATED || hr == 0x88890010 ||
          hr == 0x88890018) {
        double mix_rate = 0.0;
        for (int i = 0; i < 60; i++) {
          mix_rate = wasapi_device_get_current_mix_rate(capture->device,
                                                        !capture->loopback);
          if (mix_rate > 0.0) break;
          cdsp_sleep_ms(50);
        }
        if (mix_rate > 0.0 && mix_rate != (double)capture->sample_rate) {
          capture->pending_rate = mix_rate;
          capture->has_pending_rate_change = true;
        }
      }
      if (err)
        backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                           "Failed to get packet size");
      return false;
    }

    if (packet_size > 0) {
      BYTE* data = NULL;
      UINT32 num_frames = 0;
      DWORD flags = 0;
      hr = IAudioCaptureClient_GetBuffer(capture->capture_client, &data,
                                         &num_frames, &flags, NULL, NULL);
      if (SUCCEEDED(hr) && data) {
        start_time = GetTickCount();
        UINT32 to_copy = frames - frames_read;
        if (to_copy >= num_frames) {
          decode_samples_from_wasapi(
              chunk, frames_read, data, num_frames, capture->channels, flags,
              capture->bits_per_sample, capture->valid_bits, capture->is_float);
          frames_read += num_frames;
        } else {
          decode_samples_from_wasapi(
              chunk, frames_read, data, to_copy, capture->channels, flags,
              capture->bits_per_sample, capture->valid_bits, capture->is_float);

          size_t sample_size = (capture->bits_per_sample > 0)
                                   ? ((size_t)capture->bits_per_sample / 8)
                                   : 4;
          const BYTE* extra_data =
              data + (size_t)to_copy * capture->channels * sample_size;
          UINT32 extra_frames = num_frames - to_copy;

          if (extra_frames > (UINT32)capture->chunk_size * 4) {
            extra_frames = (UINT32)capture->chunk_size * 4;
          }

          decode_samples_from_wasapi(capture->residual_chunk, 0, extra_data,
                                     extra_frames, capture->channels, flags,
                                     capture->bits_per_sample,
                                     capture->valid_bits, capture->is_float);

          capture->residual_frames = extra_frames;
          capture->residual_offset = 0;
          frames_read += to_copy;
        }
        IAudioCaptureClient_ReleaseBuffer(capture->capture_client, num_frames);
      }
    } else {
      if (capture->polling) {
        cdsp_sleep_ms(1);
      } else {
        if (!cdsp_sem_timedwait(capture->semaphore, 250)) {
          // Timeout or error wait
        }
      }
    }
  }

  audio_chunk_set_valid_frames(chunk, frames);
  return true;
}

static void wasapi_capture_close(void* ctx) {
  wasapi_capture_t* capture = (wasapi_capture_t*)ctx;
  if (!capture) return;
  if (capture->client) {
    IAudioClient_Stop(capture->client);
    SAFE_RELEASE(capture->capture_client);
    SAFE_RELEASE(capture->client);
  }
  if (capture->session_control) {
    if (capture->session_events_listener) {
      IAudioSessionControl_UnregisterAudioSessionNotification(
          capture->session_control, capture->session_events_listener);
      SAFE_RELEASE(capture->session_events_listener);
    }
    SAFE_RELEASE(capture->session_control);
  }
  if (capture->semaphore) {
    cdsp_sem_destroy(capture->semaphore);
    capture->semaphore = NULL;
  }
  SAFE_RELEASE(capture->mm_device);
  SAFE_RELEASE(capture->enumerator);

  if (capture->com_initialized) {
    CoUninitialize();
    capture->com_initialized = false;
  }
  if (capture->residual_chunk) {
    audio_chunk_free(capture->residual_chunk);
    capture->residual_chunk = NULL;
  }
}

static bool wasapi_capture_get_pending_rate_change(void* ctx,
                                                   double* out_rate) {
  wasapi_capture_t* capture = (wasapi_capture_t*)ctx;
  if (!capture) return false;
  if (capture->has_pending_rate_change) {
    logger_info(&g_wasapi_logger,
                "capture get_pending_rate_change detected flag: "
                "pending_rate=%f, sample_rate=%d",
                capture->pending_rate, capture->sample_rate);
    double rate = capture->pending_rate;
    if (rate == 0.0) {
      for (int i = 0; i < 60; i++) {
        rate = wasapi_device_get_current_mix_rate(capture->device,
                                                  !capture->loopback);
        logger_info(
            &g_wasapi_logger,
            "capture get_pending_rate_change: query attempt %d returned %f", i,
            rate);
        if (rate > 0.0) break;
        cdsp_sleep_ms(50);
      }
    }
    capture->has_pending_rate_change = false;
    logger_info(&g_wasapi_logger,
                "capture get_pending_rate_change evaluated final rate=%f",
                rate);
    if (rate > 0.0 && rate != (double)capture->sample_rate) {
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
  if (!capture->semaphore) return false;
  return cdsp_sem_timedwait(capture->semaphore, timeout_ms);
}

static void wasapi_capture_stop(void* ctx) {
  wasapi_capture_t* capture = (wasapi_capture_t*)ctx;
  if (!capture) return;
  if (capture->client) {
    IAudioClient_Stop(capture->client);
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
  capture->loopback = config->cfg.wasapi.loopback;
  capture->exclusive = config->cfg.wasapi.exclusive;
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
    .stop = wasapi_capture_stop,
    .destroy = wasapi_capture_destroy};

#endif  // ENABLE_WASAPI
