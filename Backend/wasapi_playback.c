/**
 * @file wasapi_playback.c
 * @brief WASAPI playback backend implementation.
 */

#if defined(ENABLE_WASAPI)

#include "wasapi_playback.h"

#include <windef.h>
#include <windows.h>
#ifndef CDECL
#define CDECL __cdecl
#endif
#include <initguid.h>
#include <ks.h>
#include <ksmedia.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "Audio/sample_conversion.h"
#include "Engine/cdsp_sem.h"
#include "Utils/cdsp_time.h"
#include "Utils/lock_free_ring_buffer.h"
#include "wasapi_capabilities.h"
#include "wasapi_device.h"

struct wasapi_playback {
  char device[256];
  int sample_rate;
  int channels;
  int chunk_size;
  wasapi_sample_format_t format;
  bool exclusive;
  bool polling;

  int bits_per_sample;
  int valid_bits;
  bool is_float;
  bool com_initialized;

  IMMDeviceEnumerator* enumerator;
  IMMDevice* mm_device;
  IAudioClient* client;
  IAudioRenderClient* render_client;
  IAudioSessionControl* session_control;
  IAudioSessionEvents* session_events_listener;
  UINT32 buffer_frame_count;
  _Atomic bool paused;
  cdsp_sem_t semaphore;
  bool started;

  spsc_audio_ring_buffer_t* ring_buffer;
  pthread_t thread;
  _Atomic bool thread_running;
  float* transfer_buf;
  size_t transfer_buf_cap;
  float* write_buf;
  size_t write_buf_cap;
  _Atomic bool stopped;
  double pending_rate;
  bool has_pending_rate_change;
};

static void wasapi_playback_on_format_change(void* parent, double new_rate) {
  wasapi_playback_t* playback = (wasapi_playback_t*)parent;
  playback->pending_rate = new_rate;
  playback->has_pending_rate_change = true;
}

static inline void encode_samples_to_wasapi(BYTE* dst,
                                            const audio_chunk_t* chunk,
                                            size_t chunk_offset, size_t frames,
                                            int channels, int bits_per_sample,
                                            int valid_bits, bool is_float) {
  if (is_float) {
    float* f32 = (float*)dst;
    for (size_t f = 0; f < frames; f++) {
      for (int c = 0; c < channels; c++) {
        f32[f * channels + c] = pcm_sample_encode_f32(
            audio_chunk_get_channel(chunk, c)[chunk_offset + f]);
      }
    }
    return;
  }

  if (bits_per_sample == 16) {
    int16_t* s16 = (int16_t*)dst;
    for (size_t f = 0; f < frames; f++) {
      for (int c = 0; c < channels; c++) {
        double val = audio_chunk_get_channel(chunk, c)[chunk_offset + f];
        s16[f * channels + c] = pcm_sample_encode_s16(val);
      }
    }
  } else if (bits_per_sample == 24) {
    for (size_t f = 0; f < frames; f++) {
      for (int c = 0; c < channels; c++) {
        double val = audio_chunk_get_channel(chunk, c)[chunk_offset + f];
        size_t idx = (f * channels + c) * 3;
        pcm_sample_encode_s24_3bytes(val, &dst[idx]);
      }
    }
  } else if (bits_per_sample == 32 && valid_bits == 24) {
    int32_t* s32 = (int32_t*)dst;
    for (size_t f = 0; f < frames; f++) {
      for (int c = 0; c < channels; c++) {
        double val = audio_chunk_get_channel(chunk, c)[chunk_offset + f];
        s32[f * channels + c] = pcm_sample_encode_s24_msb(val);
      }
    }
  } else if (bits_per_sample == 32 && valid_bits == 32) {
    int32_t* s32 = (int32_t*)dst;
    for (size_t f = 0; f < frames; f++) {
      for (int c = 0; c < channels; c++) {
        double val = audio_chunk_get_channel(chunk, c)[chunk_offset + f];
        s32[f * channels + c] = pcm_sample_encode_s32(val);
      }
    }
  }
}

static inline void encode_float_samples_to_wasapi(BYTE* dst, const float* src,
                                                  size_t frames, int channels,
                                                  int bits_per_sample,
                                                  int valid_bits,
                                                  bool is_float) {
  if (is_float) {
    memcpy(dst, src, frames * channels * sizeof(float));
    return;
  }
  if (bits_per_sample == 16) {
    int16_t* s16 = (int16_t*)dst;
    for (size_t i = 0; i < frames * channels; i++) {
      s16[i] = pcm_sample_encode_s16(src[i]);
    }
  } else if (bits_per_sample == 24) {
    for (size_t i = 0; i < frames * channels; i++) {
      pcm_sample_encode_s24_3bytes(src[i], &dst[i * 3]);
    }
  } else if (bits_per_sample == 32 && valid_bits == 24) {
    int32_t* s32 = (int32_t*)dst;
    for (size_t i = 0; i < frames * channels; i++) {
      s32[i] = pcm_sample_encode_s24_msb(src[i]);
    }
  } else if (bits_per_sample == 32 && valid_bits == 32) {
    int32_t* s32 = (int32_t*)dst;
    for (size_t i = 0; i < frames * channels; i++) {
      s32[i] = pcm_sample_encode_s32(src[i]);
    }
  }
}

static void* wasapi_playback_thread_func(void* arg) {
  wasapi_playback_t* playback = (wasapi_playback_t*)arg;
  HRESULT init_hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  bool com_ok = SUCCEEDED(init_hr);
#ifdef _WIN32
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#endif

  while (
      atomic_load_explicit(&playback->thread_running, memory_order_acquire)) {
    if (!cdsp_sem_timedwait(playback->semaphore, 1000)) {
      continue;
    }

    if (!atomic_load_explicit(&playback->thread_running,
                              memory_order_acquire)) {
      break;
    }

    if (atomic_load_explicit(&playback->paused, memory_order_acquire)) {
      continue;
    }

    UINT32 padding = 0;
    if (!playback->exclusive || playback->polling) {
      HRESULT hr = IAudioClient_GetCurrentPadding(playback->client, &padding);
      if (FAILED(hr)) {
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED ||
            hr == AUDCLNT_E_RESOURCES_INVALIDATED ||
            hr == AUDCLNT_E_SERVICE_NOT_RUNNING) {
          double mix_rate = 0.0;
          for (int i = 0; i < 60; i++) {
            mix_rate =
                wasapi_device_get_current_mix_rate(playback->device, false);
            if (mix_rate > 0.0) break;
            cdsp_sleep_ms(50);
          }
          if (mix_rate > 0.0 && mix_rate != (double)playback->sample_rate) {
            playback->pending_rate = mix_rate;
            playback->has_pending_rate_change = true;
          }
        }
        continue;
      }
    }

    UINT32 to_write = (playback->exclusive && !playback->polling)
                          ? playback->buffer_frame_count
                          : (playback->buffer_frame_count - padding);
    if (to_write == 0) continue;

    size_t ring_avail =
        spsc_audio_ring_buffer_get_available_to_read(playback->ring_buffer) /
        playback->channels;

    BYTE* data = NULL;
    HRESULT hr =
        IAudioRenderClient_GetBuffer(playback->render_client, to_write, &data);
    if (FAILED(hr)) {
      if (hr == AUDCLNT_E_DEVICE_INVALIDATED ||
          hr == AUDCLNT_E_RESOURCES_INVALIDATED ||
          hr == AUDCLNT_E_SERVICE_NOT_RUNNING) {
        double mix_rate = 0.0;
        for (int i = 0; i < 60; i++) {
          mix_rate =
              wasapi_device_get_current_mix_rate(playback->device, false);
          if (mix_rate > 0.0) break;
          cdsp_sleep_ms(50);
        }
        if (mix_rate > 0.0 && mix_rate != (double)playback->sample_rate) {
          playback->pending_rate = mix_rate;
          playback->has_pending_rate_change = true;
        }
      }
    }
    if (SUCCEEDED(hr) && data) {
      static DWORD last_write_time = 0;
      DWORD now = GetTickCount();
      DWORD elapsed = (last_write_time == 0) ? 0 : (now - last_write_time);
      last_write_time = now;
      logger_trace(&g_wasapi_logger,
                   "Thread wrote to WASAPI: frames=%u, ring_avail=%zu, "
                   "padding=%u, elapsed=%u ms",
                   to_write, ring_avail, padding, (unsigned int)elapsed);
      size_t to_read_from_ring =
          (ring_avail < to_write) ? ring_avail : to_write;

      if (to_write * playback->channels > playback->transfer_buf_cap) {
        playback->transfer_buf_cap = to_write * playback->channels;
        playback->transfer_buf = (float*)realloc(
            playback->transfer_buf, playback->transfer_buf_cap * sizeof(float));
      }

      size_t consumed = spsc_audio_ring_buffer_consume(
          playback->ring_buffer, playback->transfer_buf,
          to_read_from_ring * playback->channels);
      size_t consumed_frames = consumed / playback->channels;

      encode_float_samples_to_wasapi(
          data, playback->transfer_buf, consumed_frames, playback->channels,
          playback->bits_per_sample, playback->valid_bits, playback->is_float);

      if (consumed_frames < to_write) {
        size_t silent_frames = to_write - consumed_frames;
        BYTE* silence_start = data + consumed_frames * playback->channels *
                                         (playback->bits_per_sample / 8);
        memset(silence_start, 0,
               silent_frames * playback->channels *
                   (playback->bits_per_sample / 8));
      }

      IAudioRenderClient_ReleaseBuffer(playback->render_client, to_write, 0);
    }
  }

  if (com_ok) {
    CoUninitialize();
  }
  return NULL;
}

static bool wasapi_playback_open(void* ctx, backend_error_t* err) {
  wasapi_playback_t* playback = (wasapi_playback_t*)ctx;
  if (!playback) return false;
  WAVEFORMATEX* final_wfx = NULL;
  HRESULT init_hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  playback->com_initialized = SUCCEEDED(init_hr);

  HRESULT hr =
      CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                       &IID_IMMDeviceEnumerator, (void**)&playback->enumerator);
  if (FAILED(hr)) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to create MMDeviceEnumerator");
    goto error_cleanup;
  }

  playback->mm_device =
      wasapi_find_device_by_name(playback->enumerator, playback->device, false);

  if (!playback->mm_device) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_DEVICE_NOT_FOUND,
                         "WASAPI playback device not found");
    goto error_cleanup;
  }

  hr = IMMDevice_Activate(playback->mm_device, &IID_IAudioClient, CLSCTX_ALL,
                          NULL, (void**)&playback->client);

  if (FAILED(hr)) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to activate IAudioClient");
    goto error_cleanup;
  }

  AUDCLNT_SHAREMODE mode = playback->exclusive ? AUDCLNT_SHAREMODE_EXCLUSIVE
                                               : AUDCLNT_SHAREMODE_SHARED;

  WAVEFORMATEXTENSIBLE wfx = {0};
  wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  wfx.Format.nChannels = playback->channels;
  wfx.Format.nSamplesPerSec = playback->sample_rate;
  wfx.Format.cbSize = 22;
  wfx.dwChannelMask = (playback->channels == 2)
                          ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)
                          : 0;

  bool format_found = false;
  WAVEFORMATEX std_wfx = {0};
  bool use_ext = true;
  if (mode == AUDCLNT_SHAREMODE_SHARED) {
    format_found = wasapi_setup_shared_format(
        playback->client, playback->sample_rate, &final_wfx,
        &playback->bits_per_sample, &playback->valid_bits, &playback->is_float);
  }

  if (!format_found) {
    if (playback->format == WASAPI_SAMPLE_FORMAT_S16) {
      wfx.Format.wBitsPerSample = 16;
      wfx.Format.nBlockAlign = 2 * playback->channels;
      wfx.Format.nAvgBytesPerSec =
          playback->sample_rate * wfx.Format.nBlockAlign;
      wfx.Samples.wValidBitsPerSample = 16;
      wfx.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
      if (wasapi_check_format_supported(playback->client, mode, &wfx, &std_wfx,
                                        &use_ext)) {
        playback->bits_per_sample = 16;
        playback->valid_bits = 16;
        playback->is_float = false;
        format_found = true;
      }
    } else if (playback->format == WASAPI_SAMPLE_FORMAT_S32) {
      wfx.Format.wBitsPerSample = 32;
      wfx.Format.nBlockAlign = 4 * playback->channels;
      wfx.Format.nAvgBytesPerSec =
          playback->sample_rate * wfx.Format.nBlockAlign;
      wfx.Samples.wValidBitsPerSample = 32;
      wfx.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
      if (wasapi_check_format_supported(playback->client, mode, &wfx, &std_wfx,
                                        &use_ext)) {
        playback->bits_per_sample = 32;
        playback->valid_bits = 32;
        playback->is_float = false;
        format_found = true;
      }
    } else if (playback->format == WASAPI_SAMPLE_FORMAT_F32) {
      wfx.Format.wBitsPerSample = 32;
      wfx.Format.nBlockAlign = 4 * playback->channels;
      wfx.Format.nAvgBytesPerSec =
          playback->sample_rate * wfx.Format.nBlockAlign;
      wfx.Samples.wValidBitsPerSample = 32;
      wfx.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
      if (wasapi_check_format_supported(playback->client, mode, &wfx, &std_wfx,
                                        &use_ext)) {
        playback->bits_per_sample = 32;
        playback->valid_bits = 32;
        playback->is_float = true;
        format_found = true;
      }
    } else if (playback->format == WASAPI_SAMPLE_FORMAT_S24) {
      wfx.Format.wBitsPerSample = 24;
      wfx.Format.nBlockAlign = 3 * playback->channels;
      wfx.Format.nAvgBytesPerSec =
          playback->sample_rate * wfx.Format.nBlockAlign;
      wfx.Samples.wValidBitsPerSample = 24;
      wfx.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
      if (wasapi_check_format_supported(playback->client, mode, &wfx, &std_wfx,
                                        &use_ext)) {
        playback->bits_per_sample = 24;
        playback->valid_bits = 24;
        playback->is_float = false;
        format_found = true;
      } else {
        wfx.Format.wBitsPerSample = 32;
        wfx.Format.nBlockAlign = 4 * playback->channels;
        wfx.Format.nAvgBytesPerSec =
            playback->sample_rate * wfx.Format.nBlockAlign;
        wfx.Samples.wValidBitsPerSample = 24;
        wfx.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        if (wasapi_check_format_supported(playback->client, mode, &wfx,
                                          &std_wfx, &use_ext)) {
          playback->bits_per_sample = 32;
          playback->valid_bits = 24;
          playback->is_float = false;
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
  IAudioClient_GetDevicePeriod(playback->client, &def_period, &min_period);

  if (playback->exclusive) {
    REFERENCE_TIME aligned_time = wasapi_calculate_aligned_period_near(
        playback->client, def_period, 128,
        final_wfx ? (WAVEFORMATEXTENSIBLE*)final_wfx
                  : (use_ext ? &wfx : (WAVEFORMATEXTENSIBLE*)&std_wfx));
    if (playback->polling) {
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

  DWORD flags = 0;
  if (!playback->polling) {
    flags |= AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
  }

  hr = IAudioClient_Initialize(
      playback->client, mode, flags, duration, periodicity,
      final_wfx ? final_wfx : (use_ext ? (WAVEFORMATEX*)&wfx : &std_wfx), NULL);
  if (FAILED(hr)) {
    WAVEFORMATEX* mix_wfx = NULL;
    if (SUCCEEDED(IAudioClient_GetMixFormat(playback->client, &mix_wfx)) &&
        mix_wfx) {
      if (mix_wfx->wFormatTag == 65534) {
        WAVEFORMATEXTENSIBLE* wfx_ext = (WAVEFORMATEXTENSIBLE*)mix_wfx;
        LPOLESTR subformat_str = NULL;
        StringFromCLSID(&wfx_ext->SubFormat, &subformat_str);
        char fmt_buf[512];
        snprintf(fmt_buf, sizeof(fmt_buf),
                 "Playback mix format EXT: rate=%u, channels=%u, bits=%u, "
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
            "Playback mix format std: rate=%u, channels=%u, bits=%u, tag=%u",
            (unsigned int)mix_wfx->nSamplesPerSec,
            (unsigned int)mix_wfx->nChannels,
            (unsigned int)mix_wfx->wBitsPerSample,
            (unsigned int)mix_wfx->wFormatTag);
      }
      CoTaskMemFree(mix_wfx);
    }
    REFERENCE_TIME debug_def = 0, debug_min = 0;
    IAudioClient_GetDevicePeriod(playback->client, &debug_def, &debug_min);
    logger_error(&g_wasapi_logger,
                 "Playback periods: def=%lld, min=%lld, "
                 "requested_duration=%lld, flags=0x%08lX",
                 (long long)debug_def, (long long)debug_min,
                 (long long)duration, (unsigned long)flags);
    if (err) {
      char msg[256];
      snprintf(msg, sizeof(msg),
               "Failed to initialize IAudioClient (Playback): hr=0x%08lX",
               (unsigned long)hr);
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
    goto error_cleanup;
  }

  if (!playback->polling) {
    playback->semaphore = cdsp_sem_create();
    if (!playback->semaphore) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                           "Failed to create event handle");
      goto error_cleanup;
    }

    hr = IAudioClient_SetEventHandle(playback->client,
                                     (HANDLE)playback->semaphore);
    if (FAILED(hr)) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                           "Failed to set event handle");
      goto error_cleanup;
    }
  } else {
    playback->semaphore = NULL;
  }

  hr = IAudioClient_GetBufferSize(playback->client,
                                  &playback->buffer_frame_count);
  hr = IAudioClient_GetService(playback->client, &IID_IAudioRenderClient,
                               (void**)&playback->render_client);
  if (FAILED(hr)) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to get IAudioRenderClient");
    goto error_cleanup;
  }

  IAudioClient_GetService(playback->client, &IID_IAudioSessionControl,
                          (void**)&playback->session_control);
  if (playback->session_control) {
    playback->session_events_listener = wasapi_session_events_create(
        playback, wasapi_playback_on_format_change);
    if (playback->session_events_listener) {
      IAudioSessionControl_RegisterAudioSessionNotification(
          playback->session_control, playback->session_events_listener);
    }
  }

  playback->paused = false;
  playback->started = false;
  atomic_init(&playback->stopped, false);

  logger_info(
      &g_wasapi_logger,
      "Opened WASAPI playback: device=%s, rate=%d, channels=%d, exclusive=%d",
      playback->device[0] != '\0' ? playback->device : "default",
      playback->sample_rate, playback->channels, playback->exclusive);

  logger_info(&g_wasapi_logger, "WASAPI allocated buffer size: %u frames",
              playback->buffer_frame_count);

  if (!playback->exclusive) {
    logger_warn(
        &g_wasapi_logger,
        "WASAPI operating in Shared Mode (32-bit Float). Note: Bit-exact DoP "
        "requires Exclusive Mode ('exclusive': true) to prevent float mixer "
        "bit corruption");
  }

  size_t max_frames = playback->chunk_size;
  if ((size_t)playback->buffer_frame_count > max_frames) {
    max_frames = (size_t)playback->buffer_frame_count;
  }
  size_t ring_size = max_frames * 8;
  playback->ring_buffer =
      spsc_audio_ring_buffer_create(ring_size * playback->channels);
  if (!playback->ring_buffer) {
    if (err) {
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to create ring buffer");
    }
    goto error_cleanup;
  }

  playback->transfer_buf_cap =
      playback->buffer_frame_count * playback->channels;
  playback->transfer_buf =
      (float*)malloc(playback->transfer_buf_cap * sizeof(float));
  playback->write_buf_cap = playback->buffer_frame_count * playback->channels;
  playback->write_buf = (float*)malloc(playback->write_buf_cap * sizeof(float));
  if (!playback->transfer_buf || !playback->write_buf) {
    if (err) {
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to allocate temporary buffers");
    }
    goto error_cleanup;
  }

  if (!playback->polling) {
    atomic_store_explicit(&playback->thread_running, true,
                          memory_order_release);
    int ret = pthread_create(&playback->thread, NULL,
                             wasapi_playback_thread_func, playback);
    if (ret != 0) {
      atomic_store_explicit(&playback->thread_running, false,
                            memory_order_release);
      if (err) {
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                           "Failed to create playback thread");
      }
      goto error_cleanup;
    }
  }
  if (final_wfx) CoTaskMemFree(final_wfx);
  return true;

error_cleanup:
  if (final_wfx) CoTaskMemFree(final_wfx);
  if (playback->thread_running) {
    atomic_store_explicit(&playback->thread_running, false,
                          memory_order_release);
    if (playback->semaphore) cdsp_sem_signal(playback->semaphore);
    pthread_join(playback->thread, NULL);
  }
  if (playback->ring_buffer) {
    spsc_audio_ring_buffer_free(playback->ring_buffer);
    playback->ring_buffer = NULL;
  }
  if (playback->transfer_buf) {
    free(playback->transfer_buf);
    playback->transfer_buf = NULL;
  }
  if (playback->write_buf) {
    free(playback->write_buf);
    playback->write_buf = NULL;
  }
  if (playback->session_control) {
    SAFE_RELEASE(playback->session_control);
  }
  if (playback->render_client) {
    SAFE_RELEASE(playback->render_client);
  }
  if (playback->client) {
    SAFE_RELEASE(playback->client);
  }
  if (playback->mm_device) {
    SAFE_RELEASE(playback->mm_device);
  }
  if (playback->enumerator) {
    SAFE_RELEASE(playback->enumerator);
  }
  if (playback->semaphore) {
    cdsp_sem_destroy(playback->semaphore);
    playback->semaphore = NULL;
  }
  if (playback->com_initialized) {
    CoUninitialize();
    playback->com_initialized = false;
  }
  return false;
}

static bool wasapi_playback_write(void* ctx, const audio_chunk_t* chunk,
                                  backend_error_t* err) {
  wasapi_playback_t* playback = (wasapi_playback_t*)ctx;
  if (!playback) return false;
  if (atomic_load_explicit(&playback->paused, memory_order_acquire))
    return true;

  if (audio_chunk_get_channels(chunk) < (size_t)playback->channels) {
    if (err) {
      backend_error_init(
          err, BACKEND_ERROR_INVALID_CHANNELS,
          "Chunk channels count does not match playback channels");
    }
    return false;
  }

  size_t total_frames = audio_chunk_get_valid_frames(chunk);

  if (playback->polling || !playback->started) {
    size_t direct_write_limit = playback->buffer_frame_count;
    if (playback->polling) {
      if (total_frames > playback->buffer_frame_count) {
        logger_error(&g_wasapi_logger,
                     "Input chunk size %zu exceeds WASAPI buffer capacity %u",
                     total_frames, playback->buffer_frame_count);
        if (err) {
          backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                             "Input chunk size exceeds WASAPI buffer capacity");
        }
        return false;
      }
      direct_write_limit = total_frames;
    } else {
      // Event-driven exclusive mode prefill:
      // Limit direct write to the hardware buffer capacity.
      if (direct_write_limit > total_frames) {
        direct_write_limit = total_frames;
      }
    }

    size_t frames_written = 0;
    DWORD start_time = GetTickCount();

    while (frames_written < direct_write_limit) {
      if (GetTickCount() - start_time > 3000) {
        if (err) {
          backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                             "WASAPI playback timeout (device stalled)");
        }
        return false;
      }

      UINT32 padding = 0;
      HRESULT hr = IAudioClient_GetCurrentPadding(playback->client, &padding);
      if (FAILED(hr)) {
        if (err)
          backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                             "Failed to get padding");
        return false;
      }

      UINT32 available_frames = playback->buffer_frame_count - padding;
      UINT32 to_write = (UINT32)(direct_write_limit - frames_written);

      if (available_frames < to_write) {
        cdsp_sleep_ms(1);
        continue;
      }

      BYTE* data = NULL;
      hr = IAudioRenderClient_GetBuffer(playback->render_client, to_write,
                                        &data);
      if (SUCCEEDED(hr) && data) {
        start_time = GetTickCount();
        encode_samples_to_wasapi(data, chunk, frames_written, to_write,
                                 playback->channels, playback->bits_per_sample,
                                 playback->valid_bits, playback->is_float);
        IAudioRenderClient_ReleaseBuffer(playback->render_client, to_write, 0);
        frames_written += to_write;
      } else {
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED ||
            hr == AUDCLNT_E_RESOURCES_INVALIDATED ||
            hr == AUDCLNT_E_SERVICE_NOT_RUNNING ||
            hr == AUDCLNT_E_BUFFER_ERROR) {
          double mix_rate = 0.0;
          for (int i = 0; i < 60; i++) {
            mix_rate =
                wasapi_device_get_current_mix_rate(playback->device, false);
            if (mix_rate > 0.0) break;
            cdsp_sleep_ms(50);
          }
          if (mix_rate > 0.0 && mix_rate != (double)playback->sample_rate) {
            playback->pending_rate = mix_rate;
            playback->has_pending_rate_change = true;
          }
        }
        logger_error(&g_wasapi_logger,
                     "IAudioRenderClient_GetBuffer failed: hr=0x%08lX "
                     "(to_write=%u, available=%u)",
                     (unsigned long)hr, to_write, available_frames);
        if (err) {
          char msg[256];
          snprintf(msg, sizeof(msg),
                   "IAudioRenderClient_GetBuffer failed: hr=0x%08lX",
                   (unsigned long)hr);
          backend_error_init(err, BACKEND_ERROR_WRITE_ERROR, msg);
        }
        return false;
      }
    }

    if (!playback->started && !playback->polling) {
      HRESULT hr = IAudioClient_Start(playback->client);
      if (FAILED(hr)) {
        if (err) {
          char msg[256];
          snprintf(msg, sizeof(msg),
                   "Failed to start IAudioClient in write: hr=0x%08lX",
                   (unsigned long)hr);
          backend_error_init(err, BACKEND_ERROR_WRITE_ERROR, msg);
        }
        return false;
      }
      playback->started = true;
    }

    if (!playback->polling && frames_written < total_frames) {
      size_t remaining_frames = total_frames - frames_written;
      if (remaining_frames * playback->channels > playback->write_buf_cap) {
        playback->write_buf_cap = remaining_frames * playback->channels;
        playback->write_buf = (float*)realloc(
            playback->write_buf, playback->write_buf_cap * sizeof(float));
        if (!playback->write_buf) {
          if (err) {
            backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                               "Failed to allocate write buffer");
          }
          return false;
        }
      }

      for (size_t f = 0; f < remaining_frames; f++) {
        for (int c = 0; c < playback->channels; c++) {
          playback->write_buf[f * playback->channels + c] =
              (float)audio_chunk_get_channel(chunk, c)[frames_written + f];
        }
      }

      size_t written = 0;
      size_t requested = remaining_frames * playback->channels;
      start_time = GetTickCount();

      while (written < requested) {
        if (atomic_load_explicit(&playback->stopped, memory_order_acquire)) {
          if (err) {
            backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                               "Playback stream stopped");
          }
          return false;
        }
        if (GetTickCount() - start_time > 3000) {
          if (err) {
            backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                               "WASAPI write timeout (ring buffer full)");
          }
          return false;
        }

        size_t available_space =
            spsc_audio_ring_buffer_get_capacity(playback->ring_buffer) -
            spsc_audio_ring_buffer_get_available_to_read(playback->ring_buffer);
        size_t to_write = requested - written;
        if (to_write > available_space) to_write = available_space;

        if (to_write > 0) {
          spsc_audio_ring_buffer_write(playback->ring_buffer,
                                       playback->write_buf + written, to_write,
                                       1);
          written += to_write;
          start_time = GetTickCount();
        } else {
          cdsp_sleep_ms(1);
          if (!atomic_load_explicit(&playback->thread_running,
                                    memory_order_acquire)) {
            return false;
          }
        }
      }
    }
  } else {
    if (total_frames * playback->channels > playback->write_buf_cap) {
      playback->write_buf_cap = total_frames * playback->channels;
      playback->write_buf = (float*)realloc(
          playback->write_buf, playback->write_buf_cap * sizeof(float));
      if (!playback->write_buf) {
        if (err) {
          backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                             "Failed to allocate write buffer");
        }
        return false;
      }
    }

    for (size_t f = 0; f < total_frames; f++) {
      for (int c = 0; c < playback->channels; c++) {
        playback->write_buf[f * playback->channels + c] =
            (float)audio_chunk_get_channel(chunk, c)[f];
      }
    }

    size_t written = 0;
    size_t requested = total_frames * playback->channels;
    DWORD start_time = GetTickCount();

    while (written < requested) {
      if (atomic_load_explicit(&playback->stopped, memory_order_acquire)) {
        if (err) {
          backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                             "Playback stream stopped");
        }
        return false;
      }
      if (GetTickCount() - start_time > 3000) {
        if (err) {
          backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                             "WASAPI write timeout (ring buffer full)");
        }
        return false;
      }

      size_t available_space =
          spsc_audio_ring_buffer_get_capacity(playback->ring_buffer) -
          spsc_audio_ring_buffer_get_available_to_read(playback->ring_buffer);
      size_t to_write = requested - written;
      if (to_write > available_space) to_write = available_space;

      if (to_write > 0) {
        spsc_audio_ring_buffer_write(
            playback->ring_buffer, playback->write_buf + written, to_write, 1);
        written += to_write;
        start_time = GetTickCount();
      } else {
        cdsp_sleep_ms(1);
        if (!atomic_load_explicit(&playback->thread_running,
                                  memory_order_acquire)) {
          return false;
        }
      }
    }
  }

  return true;
}

static void wasapi_playback_close(void* ctx) {
  wasapi_playback_t* playback = (wasapi_playback_t*)ctx;
  if (!playback) return;
  if (playback->thread_running) {
    atomic_store_explicit(&playback->thread_running, false,
                          memory_order_release);
    if (playback->semaphore) cdsp_sem_signal(playback->semaphore);
    pthread_join(playback->thread, NULL);
  }
  if (playback->ring_buffer) {
    spsc_audio_ring_buffer_free(playback->ring_buffer);
    playback->ring_buffer = NULL;
  }
  if (playback->transfer_buf) {
    free(playback->transfer_buf);
    playback->transfer_buf = NULL;
  }
  if (playback->write_buf) {
    free(playback->write_buf);
    playback->write_buf = NULL;
  }
  if (playback->client) {
    IAudioClient_Stop(playback->client);
    SAFE_RELEASE(playback->render_client);
    SAFE_RELEASE(playback->client);
  }
  if (playback->session_control) {
    if (playback->session_events_listener) {
      IAudioSessionControl_UnregisterAudioSessionNotification(
          playback->session_control, playback->session_events_listener);
      SAFE_RELEASE(playback->session_events_listener);
    }
    SAFE_RELEASE(playback->session_control);
  }
  if (playback->semaphore) {
    cdsp_sem_destroy(playback->semaphore);
    playback->semaphore = NULL;
  }
  SAFE_RELEASE(playback->mm_device);
  SAFE_RELEASE(playback->enumerator);

  playback->started = false;

  if (playback->com_initialized) {
    CoUninitialize();
    playback->com_initialized = false;
  }
}

static size_t wasapi_playback_get_buffer_level(void* ctx) {
  wasapi_playback_t* playback = (wasapi_playback_t*)ctx;
  if (!playback || !playback->client) return 0;
  UINT32 padding = 0;
  IAudioClient_GetCurrentPadding(playback->client, &padding);
  if (playback->polling) {
    return padding;
  }
  size_t ring_frames = 0;
  if (playback->ring_buffer) {
    ring_frames =
        spsc_audio_ring_buffer_get_available_to_read(playback->ring_buffer) /
        playback->channels;
  }
  return padding + ring_frames;
}

static bool wasapi_playback_get_pending_rate_change(void* ctx,
                                                    double* out_rate) {
  wasapi_playback_t* playback = (wasapi_playback_t*)ctx;
  if (!playback) return false;
  if (playback->has_pending_rate_change) {
    logger_info(&g_wasapi_logger,
                "get_pending_rate_change detected flag: pending_rate=%f, "
                "sample_rate=%d",
                playback->pending_rate, playback->sample_rate);
    double rate = playback->pending_rate;
    if (rate == 0.0) {
      for (int i = 0; i < 60; i++) {
        rate = wasapi_device_get_current_mix_rate(playback->device, false);
        logger_info(&g_wasapi_logger,
                    "get_pending_rate_change: query attempt %d returned %f", i,
                    rate);
        if (rate > 0.0) break;
        cdsp_sleep_ms(50);
      }
    }
    playback->has_pending_rate_change = false;
    logger_info(&g_wasapi_logger,
                "get_pending_rate_change evaluated final rate=%f", rate);
    if (rate > 0.0 && rate != (double)playback->sample_rate) {
      if (out_rate) {
        *out_rate = rate;
      }
      logger_info(&g_wasapi_logger,
                  "get_pending_rate_change returning true with rate=%f", rate);
      return true;
    }
  }
  return false;
}

static bool wasapi_playback_prefill_silence(void* ctx, size_t frames,
                                            backend_error_t* err) {
  wasapi_playback_t* playback = (wasapi_playback_t*)ctx;
  (void)frames;
  if (!playback) return false;
  if (playback->polling || !playback->started) {
    if (!playback->render_client) return false;
    BYTE* data = NULL;
    UINT32 prefill_frames = playback->buffer_frame_count;
    HRESULT hr = IAudioRenderClient_GetBuffer(playback->render_client,
                                              prefill_frames, &data);
    if (SUCCEEDED(hr) && data) {
      memset(data, 0,
             prefill_frames * playback->channels *
                 (playback->bits_per_sample / 8));
      IAudioRenderClient_ReleaseBuffer(playback->render_client, prefill_frames,
                                       0);
      if (!playback->started) {
        hr = IAudioClient_Start(playback->client);
        if (FAILED(hr)) {
          if (err) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "Failed to start IAudioClient after prefill: hr=0x%08lX",
                     (unsigned long)hr);
            backend_error_init(err, BACKEND_ERROR_WRITE_ERROR, msg);
          }
          return false;
        }
        playback->started = true;
      }
      return true;
    }
    if (err)
      backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                         "Failed to prefill silence");
    return false;
  } else {
    if (!playback->ring_buffer) return false;

    spsc_audio_ring_buffer_write_silence(
        playback->ring_buffer,
        playback->buffer_frame_count * playback->channels);

    if (!playback->started) {
      HRESULT hr = IAudioClient_Start(playback->client);
      if (FAILED(hr)) {
        if (err) {
          char msg[256];
          snprintf(msg, sizeof(msg),
                   "Failed to start IAudioClient after prefill: hr=0x%08lX",
                   (unsigned long)hr);
          backend_error_init(err, BACKEND_ERROR_WRITE_ERROR, msg);
        }
        return false;
      }
      playback->started = true;
    }
    return true;
  }
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

static void wasapi_playback_stop(void* ctx) {
  wasapi_playback_t* playback = (wasapi_playback_t*)ctx;
  if (!playback) return;
  atomic_store_explicit(&playback->stopped, true, memory_order_release);
  if (playback->client) {
    IAudioClient_Stop(playback->client);
  }
  if (playback->semaphore) {
    cdsp_sem_signal(playback->semaphore);
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

  if (config->cfg.wasapi.has_device &&
      strcmp(config->cfg.wasapi.device, "default") != 0) {
    snprintf(playback->device, sizeof(playback->device), "%s",
             config->cfg.wasapi.device);
  } else {
    playback->device[0] = '\0';
  }

  playback->sample_rate = sample_rate;
  playback->channels = config->cfg.wasapi.channels;
  playback->chunk_size = chunk_size;
  playback->format = config->cfg.wasapi.format;
  playback->exclusive = config->cfg.wasapi.exclusive;
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
    .stop = wasapi_playback_stop,
    .destroy = wasapi_playback_destroy};

#endif  // ENABLE_WASAPI
