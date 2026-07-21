#if defined(ENABLE_WASAPI)

#define WIN32_LEAN_AND_MEAN
// clang-format off
#include <initguid.h>
// clang-format on
#include "wasapi_backend.h"

#include <audioclient.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>

#include "Audio/sample_conversion.h"
#include "Engine/cdsp_sem.h"
#include "Logging/app_logger.h"
#include "Utils/cdsp_time.h"
#include "Utils/lock_free_ring_buffer.h"
#include "wasapi_capabilities.h"

static const logger_t g_logger = {"dsp.backend.wasapi"};

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

// COM Release helper
#define SAFE_RELEASE(punk)         \
  if ((punk) != NULL) {            \
    (punk)->lpVtbl->Release(punk); \
    (punk) = NULL;                 \
  }

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
  DWORD last_rate_check_time;
};

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
  DWORD last_rate_check_time;
};

// Custom COM IAudioSessionEvents implementation in C
typedef struct {
  IAudioSessionEventsVtbl* lpVtbl;
  LONG ref_count;
  void* parent;
  bool is_capture;
} CDSPAudioSessionEvents;

static HRESULT STDMETHODCALLTYPE session_QueryInterface(
    IAudioSessionEvents* This, REFIID riid, void** ppvObject) {
  if (IsEqualIID(riid, &IID_IAudioSessionEvents) ||
      IsEqualIID(riid, &IID_IUnknown)) {
    *ppvObject = This;
    This->lpVtbl->AddRef(This);
    return S_OK;
  }
  *ppvObject = NULL;
  return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE session_AddRef(IAudioSessionEvents* This) {
  CDSPAudioSessionEvents* self = (CDSPAudioSessionEvents*)This;
  return InterlockedIncrement(&self->ref_count);
}

static ULONG STDMETHODCALLTYPE session_Release(IAudioSessionEvents* This) {
  CDSPAudioSessionEvents* self = (CDSPAudioSessionEvents*)This;
  ULONG rc = InterlockedDecrement(&self->ref_count);
  if (rc == 0) {
    free(self);
  }
  return rc;
}

static HRESULT STDMETHODCALLTYPE session_OnDisplayNameChanged(IAudioSessionEvents* This, LPCWSTR NewDisplayName, LPCGUID EventContext) { (void)This; (void)NewDisplayName; (void)EventContext; return S_OK; }
static HRESULT STDMETHODCALLTYPE session_OnIconPathChanged(IAudioSessionEvents* This, LPCWSTR NewIconPath, LPCGUID EventContext) { (void)This; (void)NewIconPath; (void)EventContext; return S_OK; }
static HRESULT STDMETHODCALLTYPE session_OnSimpleVolumeChanged(IAudioSessionEvents* This, float NewVolume, BOOL NewMute, LPCGUID EventContext) { (void)This; (void)NewVolume; (void)NewMute; (void)EventContext; return S_OK; }
static HRESULT STDMETHODCALLTYPE session_OnChannelVolumeChanged(IAudioSessionEvents* This, DWORD ChannelCount, float NewChannelVolumeArray[], DWORD ChangedChannel, LPCGUID EventContext) { (void)This; (void)ChannelCount; (void)NewChannelVolumeArray; (void)ChangedChannel; (void)EventContext; return S_OK; }
static HRESULT STDMETHODCALLTYPE session_OnGroupingParamChanged(IAudioSessionEvents* This, LPCGUID NewGroupingParam, LPCGUID EventContext) { (void)This; (void)NewGroupingParam; (void)EventContext; return S_OK; }
static HRESULT STDMETHODCALLTYPE session_OnStateChanged(IAudioSessionEvents* This, AudioSessionState NewState) { (void)This; (void)NewState; return S_OK; }

static HRESULT STDMETHODCALLTYPE session_OnSessionDisconnected(
    IAudioSessionEvents* This, AudioSessionDisconnectReason DisconnectReason) {
  CDSPAudioSessionEvents* self = (CDSPAudioSessionEvents*)This;
  logger_info(&g_logger, "DEBUG: session_OnSessionDisconnected called, reason=%d, is_capture=%d",
              (int)DisconnectReason, (int)self->is_capture);
  if (DisconnectReason == DisconnectReasonFormatChanged ||
      DisconnectReason == DisconnectReasonServerShutdown) {
    if (self->is_capture) {
      struct wasapi_capture* capture = (struct wasapi_capture*)self->parent;
      capture->pending_rate = 0.0;  // 0.0 signals format change reload
      capture->has_pending_rate_change = true;
    } else {
      struct wasapi_playback* playback = (struct wasapi_playback*)self->parent;
      playback->pending_rate = 0.0;
      playback->has_pending_rate_change = true;
    }
  }
  return S_OK;
}

static IAudioSessionEventsVtbl g_session_events_vtbl = {
    session_QueryInterface,
    session_AddRef,
    session_Release,
    session_OnDisplayNameChanged,
    session_OnIconPathChanged,
    session_OnSimpleVolumeChanged,
    session_OnChannelVolumeChanged,
    session_OnGroupingParamChanged,
    session_OnStateChanged,
    session_OnSessionDisconnected};

static IAudioSessionEvents* wasapi_session_events_create(void* parent,
                                                         bool is_capture) {
  CDSPAudioSessionEvents* events =
      (CDSPAudioSessionEvents*)calloc(1, sizeof(CDSPAudioSessionEvents));
  if (!events) return NULL;
  events->lpVtbl = &g_session_events_vtbl;
  events->ref_count = 1;
  events->parent = parent;
  events->is_capture = is_capture;
  return (IAudioSessionEvents*)events;
}

/**
 * @brief Decodes interleaved audio samples from WASAPI input buffer format to
 * deinterleaved double format.
 *
 * Supports conversion from:
 * - 32-bit IEEE float
 * - 16-bit signed integer (scaled to [-1.0, 1.0])
 * - 24-bit signed integer (packed 3 bytes, scaled to [-1.0, 1.0])
 * - 32-bit signed integer with 24 valid bits (shifted and scaled)
 * - 32-bit signed integer with 32 valid bits (scaled)
 *
 * If AUDCLNT_BUFFERFLAGS_SILENT is set in flags, the output chunk is filled
 * with silence (0.0).
 *
 * @param chunk The destination audio_chunk_t structure.
 * @param chunk_offset Starting frame index in the destination chunk.
 * @param src Pointer to the source raw WASAPI byte buffer.
 * @param frames Number of frames to decode.
 * @param channels Number of audio channels.
 * @param flags Buffer status flags from WASAPI (e.g., silence flag).
 * @param bits_per_sample Total bits per sample in the source container (16, 24,
 * 32).
 * @param valid_bits Actual bits of precision (16, 24, 32).
 * @param is_float True if the source is floating-point, false for PCM.
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

/**
 * @brief Encodes deinterleaved double samples from audio chunk to interleaved
 * WASAPI output buffer format.
 *
 * Clamps input double values to [-1.0, 1.0] before encoding to integer formats.
 * Supports conversion to:
 * - 32-bit IEEE float
 * - 16-bit signed integer
 * - 24-bit signed integer (packed 3 bytes)
 * - 32-bit signed integer with 24 valid bits (shifted)
 * - 32-bit signed integer with 32 valid bits
 *
 * @param dst Pointer to the destination raw WASAPI byte buffer.
 * @param chunk The source audio_chunk_t structure.
 * @param chunk_offset Starting frame index in the source chunk.
 * @param frames Number of frames to encode.
 * @param channels Number of audio channels.
 * @param bits_per_sample Total bits per sample in the destination container
 * (16, 24, 32).
 * @param valid_bits Actual bits of precision (16, 24, 32).
 * @param is_float True if the destination format is floating-point.
 */
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

// MARK: - Capture Backend implementation

/**
 * @brief Opens the WASAPI capture stream.
 *
 * @param ctx Pointer to the wasapi_capture_t instance.
 * @param err Pointer to a backend_error_t to receive error details on failure.
 * @return true if successful, false otherwise.
 */

static bool wasapi_setup_shared_format(IAudioClient* client, int target_sample_rate,
                                       WAVEFORMATEX** out_final_wfx,
                                       int* out_bits_per_sample,
                                       int* out_valid_bits,
                                       bool* out_is_float) {
  WAVEFORMATEX* mix_wfx = NULL;
  HRESULT hr = IAudioClient_GetMixFormat(client, &mix_wfx);
  if (FAILED(hr) || !mix_wfx) return false;

  size_t full_size = sizeof(WAVEFORMATEX) + mix_wfx->cbSize;
  WAVEFORMATEX* final_wfx = (WAVEFORMATEX*)CoTaskMemAlloc(full_size);
  if (!final_wfx) {
    CoTaskMemFree(mix_wfx);
    return false;
  }

  memcpy(final_wfx, mix_wfx, full_size);
  final_wfx->nSamplesPerSec = target_sample_rate;
  final_wfx->nAvgBytesPerSec = final_wfx->nSamplesPerSec * final_wfx->nBlockAlign;

  *out_bits_per_sample = final_wfx->wBitsPerSample;
  if (final_wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
    WAVEFORMATEXTENSIBLE* ext = (WAVEFORMATEXTENSIBLE*)final_wfx;
    *out_valid_bits = ext->Samples.wValidBitsPerSample;
    *out_is_float = IsEqualGUID(&ext->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
  } else {
    *out_valid_bits = final_wfx->wBitsPerSample;
    *out_is_float = (final_wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
  }

  CoTaskMemFree(mix_wfx);
  *out_final_wfx = final_wfx;
  return true;
}

static bool wasapi_capture_open(void* ctx, backend_error_t* err) {
  wasapi_capture_t* capture = (wasapi_capture_t*)ctx;
  if (!capture) return false;
  WAVEFORMATEX* final_wfx = NULL;
  // Initialize COM library for multithreaded operations.
  HRESULT init_hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  capture->com_initialized = SUCCEEDED(init_hr);
  atomic_init(&capture->stopped, false);
  capture->last_rate_check_time = GetTickCount();

  HRESULT hr =
      CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                       &IID_IMMDeviceEnumerator, (void**)&capture->enumerator);
  if (FAILED(hr)) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to create MMDeviceEnumerator");
    goto error_cleanup;
  }

  // Retrieve IMMDevice. If no device name is specified, get default.
  // Otherwise, enumerate endpoints and match friendly name or ID.
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

  // Set up the wave format structure.
  // We use WAVEFORMATEXTENSIBLE which is required for formats with >2 channels
  // or high precision sample types (>16-bit).
  WAVEFORMATEXTENSIBLE wfx = {0};
  wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  wfx.Format.nChannels = capture->channels;
  wfx.Format.nSamplesPerSec = capture->sample_rate;
  wfx.Format.cbSize = 22;
  wfx.dwChannelMask =
      (capture->channels == 2) ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : 0;

  bool format_found = false;
  if (mode == AUDCLNT_SHAREMODE_SHARED) {
    format_found = wasapi_setup_shared_format(capture->client, capture->sample_rate,
                                              &final_wfx,
                                              &capture->bits_per_sample,
                                              &capture->valid_bits,
                                              &capture->is_float);
  }

  if (!format_found) {
    if (capture->format == WASAPI_SAMPLE_FORMAT_S16) {
      wfx.Format.wBitsPerSample = 16;
      wfx.Format.nBlockAlign = 2 * capture->channels;
      wfx.Format.nAvgBytesPerSec =
          capture->sample_rate * wfx.Format.nBlockAlign;
      wfx.Samples.wValidBitsPerSample = 16;
      wfx.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
      hr = IAudioClient_IsFormatSupported(capture->client, mode,
                                          (WAVEFORMATEX*)&wfx, NULL);
      if (SUCCEEDED(hr)) {
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
      hr = IAudioClient_IsFormatSupported(capture->client, mode,
                                          (WAVEFORMATEX*)&wfx, NULL);
      if (SUCCEEDED(hr)) {
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
      hr = IAudioClient_IsFormatSupported(capture->client, mode,
                                          (WAVEFORMATEX*)&wfx, NULL);
      if (SUCCEEDED(hr)) {
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
      hr = IAudioClient_IsFormatSupported(capture->client, mode,
                                          (WAVEFORMATEX*)&wfx, NULL);
      if (SUCCEEDED(hr)) {
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
        hr = IAudioClient_IsFormatSupported(capture->client, mode,
                                            (WAVEFORMATEX*)&wfx, NULL);
        if (SUCCEEDED(hr)) {
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

  REFERENCE_TIME duration =
      (REFERENCE_TIME)(((double)capture->chunk_size / capture->sample_rate) *
                       10000000.0);
  if (mode == AUDCLNT_SHAREMODE_SHARED) {
    duration = 0;
  }
  DWORD flags = (capture->loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0);
  if (!capture->polling) {
    flags |= AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
  }
  // Autoconvert disabled to match CamillaDSP bit-exact output

  hr = IAudioClient_Initialize(
      capture->client, mode, flags, duration,
      (mode == AUDCLNT_SHAREMODE_EXCLUSIVE) ? duration : 0,
      final_wfx ? final_wfx : (WAVEFORMATEX*)&wfx, NULL);
  if (FAILED(hr)) {
    WAVEFORMATEX* mix_wfx = NULL;
    if (SUCCEEDED(IAudioClient_GetMixFormat(capture->client, &mix_wfx)) && mix_wfx) {
      if (mix_wfx->wFormatTag == 65534) {
        WAVEFORMATEXTENSIBLE* wfx_ext = (WAVEFORMATEXTENSIBLE*)mix_wfx;
        LPOLESTR subformat_str = NULL;
        StringFromCLSID(&wfx_ext->SubFormat, &subformat_str);
        char fmt_buf[512];
        snprintf(fmt_buf, sizeof(fmt_buf),
                 "Capture mix format EXT: rate=%u, channels=%u, bits=%u, valid_bits=%u, subformat=%ls",
                 (unsigned int)mix_wfx->nSamplesPerSec, (unsigned int)mix_wfx->nChannels,
                 (unsigned int)mix_wfx->wBitsPerSample, (unsigned int)wfx_ext->Samples.wValidBitsPerSample,
                 subformat_str);
        logger_error(&g_logger, "%s", fmt_buf);
        CoTaskMemFree(subformat_str);
      } else {
        logger_error(&g_logger, "Capture mix format std: rate=%u, channels=%u, bits=%u, tag=%u",
                     (unsigned int)mix_wfx->nSamplesPerSec, (unsigned int)mix_wfx->nChannels,
                     (unsigned int)mix_wfx->wBitsPerSample, (unsigned int)mix_wfx->wFormatTag);
      }
      CoTaskMemFree(mix_wfx);
    }
    REFERENCE_TIME debug_def = 0, debug_min = 0;
    IAudioClient_GetDevicePeriod(capture->client, &debug_def, &debug_min);
    logger_error(&g_logger, "Capture periods: def=%lld, min=%lld, requested_duration=%lld, flags=0x%08lX",
                 (long long)debug_def, (long long)debug_min, (long long)duration, (unsigned long)flags);
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
        wasapi_session_events_create(capture, true);
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

  logger_info(&g_logger,
              "Opened WASAPI capture: device=%s, rate=%d, channels=%d",
              capture->device[0] != '\0' ? capture->device : "default",
              capture->sample_rate, capture->channels);
  logger_info(&g_logger, "WASAPI capture options: loopback=%d, exclusive=%d",
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

/**
 * @brief Reads audio data from the WASAPI capture stream.
 *
 * @param ctx Pointer to the wasapi_capture_t instance.
 * @param frames Number of frames to read.
 * @param chunk Pointer to the audio_chunk_t where the read data will be stored.
 * @param err Pointer to a backend_error_t to receive error details on failure.
 * @return true if successful, false otherwise.
 */
static double wasapi_device_get_current_mix_rate(const char* device_name, bool is_capture) {
  IMMDeviceEnumerator* enumerator = NULL;
  HRESULT hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                &IID_IMMDeviceEnumerator, (void**)&enumerator);
  if (FAILED(hr)) return 0.0;

  IMMDevice* mm_device = wasapi_find_device_by_name(enumerator, device_name, is_capture);
  if (!mm_device) {
    enumerator->lpVtbl->Release(enumerator);
    return 0.0;
  }

  IAudioClient* client = NULL;
  logger_info(&g_logger, "DEBUG: wasapi_device_get_current_mix_rate entered, device=%s, is_capture=%d",
              device_name[0] != '\0' ? device_name : "default", (int)is_capture);
  for (int i = 0; i < 40; i++) {
    hr = mm_device->lpVtbl->Activate(mm_device, &IID_IAudioClient,
                                     CLSCTX_ALL, NULL, (void**)&client);
    if (SUCCEEDED(hr) && client) {
      WAVEFORMATEX* wfx = NULL;
      hr = IAudioClient_GetMixFormat(client, &wfx);
      if (SUCCEEDED(hr) && wfx) {
        double rate = (double)wfx->nSamplesPerSec;
        logger_info(&g_logger, "DEBUG: GetMixFormat succeeded, rate=%f", rate);
        CoTaskMemFree(wfx);
        client->lpVtbl->Release(client);
        mm_device->lpVtbl->Release(mm_device);
        enumerator->lpVtbl->Release(enumerator);
        return rate;
      }
      logger_info(&g_logger, "DEBUG: GetMixFormat failed: hr=0x%08lX", (unsigned long)hr);
      client->lpVtbl->Release(client);
    } else {
      logger_info(&g_logger, "DEBUG: Activate failed: hr=0x%08lX", (unsigned long)hr);
    }
    cdsp_sleep_ms(100);
  }

  mm_device->lpVtbl->Release(mm_device);
  enumerator->lpVtbl->Release(enumerator);
  return 0.0;
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

  DWORD now = GetTickCount();
  if (now - capture->last_rate_check_time > 1000) {
    capture->last_rate_check_time = now;
    double mix_rate = wasapi_device_get_current_mix_rate(capture->device, !capture->loopback);
    if (mix_rate > 0.0 && mix_rate != (double)capture->sample_rate) {
      capture->pending_rate = mix_rate;
      capture->has_pending_rate_change = true;
    }
  }

  // 1. Consume any remaining samples from the residual buffer
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

  // 2. Loop to read packets from WASAPI device
  while (frames_read < frames) {
    if (atomic_load_explicit(&capture->stopped, memory_order_acquire)) {
      return false;
    }
    if (GetTickCount() - start_time > 1000) {
      double mix_rate = wasapi_device_get_current_mix_rate(capture->device, !capture->loopback);
      if (mix_rate > 0.0 && mix_rate != (double)capture->sample_rate) {
        capture->pending_rate = mix_rate;
        capture->has_pending_rate_change = true;
      }
      if (err) {
        backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                           "WASAPI capture timeout (device stalled)");
      }
      return false;
    }

    // Check the size of the next available packet in the capture buffer.
    UINT32 packet_size = 0;
    HRESULT hr = IAudioCaptureClient_GetNextPacketSize(capture->capture_client,
                                                       &packet_size);
    if (FAILED(hr)) {
      double mix_rate = wasapi_device_get_current_mix_rate(capture->device, !capture->loopback);
      if (mix_rate > 0.0 && mix_rate != (double)capture->sample_rate) {
        capture->pending_rate = mix_rate;
        capture->has_pending_rate_change = true;
      } else if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == 0x88890010 || hr == 0x88890018) {
        capture->pending_rate = 0.0;
        capture->has_pending_rate_change = true;
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
      // Retrieve the pointer to the data packet in the shared buffer.
      hr = IAudioCaptureClient_GetBuffer(capture->capture_client, &data,
                                         &num_frames, &flags, NULL, NULL);
      if (SUCCEEDED(hr) && data) {
        start_time = GetTickCount();  // progress made, reset timeout
        UINT32 to_copy = frames - frames_read;
        if (to_copy >= num_frames) {
          // Consume the entire packet
          // Decode the raw WASAPI format into the audio chunk.
          decode_samples_from_wasapi(
              chunk, frames_read, data, num_frames, capture->channels, flags,
              capture->bits_per_sample, capture->valid_bits, capture->is_float);
          frames_read += num_frames;
        } else {
          // Consume a portion, buffer the rest
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
        // Release the buffer back to WASAPI.
        IAudioCaptureClient_ReleaseBuffer(capture->capture_client, num_frames);
      }
    } else {
      // If no data is available, wait before checking again.
      if (capture->polling) {
        cdsp_sleep_ms(1);
      } else {
        // Wait for the event to be signaled by WASAPI indicating data is ready.
        if (!cdsp_sem_timedwait(capture->semaphore, 2000)) {
          // Timeout or error wait
        }
      }
    }
  }

  audio_chunk_set_valid_frames(chunk, frames);
  return true;
}

/**
 * @brief Closes the WASAPI capture stream.
 *
 * @param ctx Pointer to the wasapi_capture_t instance.
 */
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

/**
 * @brief Checks if there is a pending rate change for the capture backend.
 *
 * @param ctx Pointer to the wasapi_capture_t instance.
 * @param out_rate Pointer to double to receive the new rate if pending.
 * @return true if there is a pending rate change, false otherwise.
 */
static bool wasapi_capture_get_pending_rate_change(void* ctx,
                                                   double* out_rate) {
  wasapi_capture_t* capture = (wasapi_capture_t*)ctx;
  if (!capture) return false;
  bool changed = capture->has_pending_rate_change;
  if (changed) {
    if (capture->pending_rate == 0.0) {
      double mix_rate = wasapi_device_get_current_mix_rate(capture->device, !capture->loopback);
      if (mix_rate > 0.0) {
        capture->pending_rate = mix_rate;
      }
    }
    if (out_rate) {
      *out_rate = capture->pending_rate;
    }
    capture->has_pending_rate_change = false;
  }
  return changed;
}

/**
 * @brief Checks if pitch control is supported by the capture backend.
 *
 * @param ctx Pointer to the wasapi_capture_t instance.
 * @return true if supported, false otherwise.
 */
static bool wasapi_capture_pitch_control_supported(void* ctx) {
  (void)ctx;
  return false;
}

/**
 * @brief Sets the pitch multiplier for the capture backend.
 *
 * @param ctx Pointer to the wasapi_capture_t instance.
 * @param multiplier The pitch multiplier to set.
 */
static void wasapi_capture_set_pitch(void* ctx, double multiplier) {
  (void)ctx;
  (void)multiplier;
}

/**
 * @brief Waits for audio data to be available for capture.
 *
 * @param ctx Pointer to the wasapi_capture_t instance.
 * @param timeout_ms Timeout in milliseconds.
 * @return true if data is available, false on timeout or error.
 */
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

/**
 * @brief Stops the WASAPI capture stream.
 *
 * @param ctx Pointer to the wasapi_capture_t instance.
 */
static void wasapi_capture_stop(void* ctx) {
  wasapi_capture_t* capture = (wasapi_capture_t*)ctx;
  if (!capture) return;
  if (capture->client) {
    IAudioClient_Stop(capture->client);
  }
}

/**
 * @brief Destroys the WASAPI capture instance and frees resources.
 *
 * @param ctx Pointer to the wasapi_capture_t instance to destroy.
 */
static void wasapi_capture_destroy(void* ctx) {
  wasapi_capture_t* capture = (wasapi_capture_t*)ctx;
  if (capture) {
    wasapi_capture_close(capture);
    free(capture);
  }
}

/**
 * @brief Creates a WASAPI capture backend.
 *
 * @param config Configuration for the capture device.
 * @param sample_rate The sample rate in Hz.
 * @param chunk_size The chunk size in frames.
 * @param full_duplex True if running in full duplex mode.
 * @param params Processing parameters.
 * @param err Pointer to a backend_error_t to receive error details on failure.
 * @return A pointer to the created capture_backend_t, or NULL on failure.
 */
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

// MARK: - Playback Backend implementation

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

    DWORD now = GetTickCount();
    if (now - playback->last_rate_check_time > 1000) {
      playback->last_rate_check_time = now;
      double mix_rate = wasapi_device_get_current_mix_rate(playback->device, false);
      if (mix_rate > 0.0 && mix_rate != (double)playback->sample_rate) {
        playback->pending_rate = mix_rate;
        playback->has_pending_rate_change = true;
      }
    }

    UINT32 padding = 0;
    if (!playback->exclusive) {
      HRESULT hr = IAudioClient_GetCurrentPadding(playback->client, &padding);
      if (FAILED(hr)) {
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED ||
            hr == AUDCLNT_E_RESOURCES_INVALIDATED ||
            hr == AUDCLNT_E_SERVICE_NOT_RUNNING) {
          double mix_rate = wasapi_device_get_current_mix_rate(playback->device, false);
          if (mix_rate > 0.0 && mix_rate != (double)playback->sample_rate) {
            playback->pending_rate = mix_rate;
            playback->has_pending_rate_change = true;
          }
        }
        continue;
      }
    }

    UINT32 to_write = playback->exclusive
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
      double mix_rate = wasapi_device_get_current_mix_rate(playback->device, false);
      if (mix_rate > 0.0 && mix_rate != (double)playback->sample_rate) {
        playback->pending_rate = mix_rate;
        playback->has_pending_rate_change = true;
      } else if (hr == AUDCLNT_E_DEVICE_INVALIDATED ||
                 hr == AUDCLNT_E_RESOURCES_INVALIDATED ||
                 hr == AUDCLNT_E_SERVICE_NOT_RUNNING) {
        playback->pending_rate = 0.0;
        playback->has_pending_rate_change = true;
      }
    }
    if (SUCCEEDED(hr) && data) {
      static DWORD last_write_time = 0;
      DWORD now = GetTickCount();
      DWORD elapsed = (last_write_time == 0) ? 0 : (now - last_write_time);
      last_write_time = now;
      logger_debug(&g_logger,
                   "Thread wrote to WASAPI: frames=%u, ring_avail=%zu, "
                   "padding=%u, elapsed=%u ms",
                   to_write, ring_avail, padding, (unsigned int)elapsed);
      size_t to_read_from_ring =
          (ring_avail < to_write) ? ring_avail : to_write;

      // Expand transfer buffer if needed
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

/**
 * @brief Opens the WASAPI playback stream.
 *
 * @param ctx Pointer to the wasapi_playback_t instance.
 * @param err Pointer to a backend_error_t to receive error details on failure.
 * @return true if successful, false otherwise.
 */
static bool wasapi_playback_open(void* ctx, backend_error_t* err) {
  wasapi_playback_t* playback = (wasapi_playback_t*)ctx;
  if (!playback) return false;
  WAVEFORMATEX* final_wfx = NULL;
  HRESULT init_hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  playback->com_initialized = SUCCEEDED(init_hr);
  playback->last_rate_check_time = GetTickCount();

  HRESULT hr =
      CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                       &IID_IMMDeviceEnumerator, (void**)&playback->enumerator);
  if (FAILED(hr)) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to create MMDeviceEnumerator");
    goto error_cleanup;
  }

  // Retrieve IMMDevice. If no device name is specified, get default.
  // Otherwise, enumerate endpoints and match friendly name or ID.
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

  // Set up the wave format structure.
  // We use WAVEFORMATEXTENSIBLE which is required for formats with >2 channels
  // or high precision sample types (>16-bit).
  WAVEFORMATEXTENSIBLE wfx = {0};
  wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  wfx.Format.nChannels = playback->channels;
  wfx.Format.nSamplesPerSec = playback->sample_rate;
  wfx.Format.cbSize = 22;
  wfx.dwChannelMask = (playback->channels == 2)
                          ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)
                          : 0;

  bool format_found = false;
  if (mode == AUDCLNT_SHAREMODE_SHARED) {
    format_found = wasapi_setup_shared_format(playback->client, playback->sample_rate,
                                              &final_wfx,
                                              &playback->bits_per_sample,
                                              &playback->valid_bits,
                                              &playback->is_float);
  }

  if (!format_found) {
    if (playback->format == WASAPI_SAMPLE_FORMAT_S16) {
      wfx.Format.wBitsPerSample = 16;
      wfx.Format.nBlockAlign = 2 * playback->channels;
      wfx.Format.nAvgBytesPerSec =
          playback->sample_rate * wfx.Format.nBlockAlign;
      wfx.Samples.wValidBitsPerSample = 16;
      wfx.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
      hr = IAudioClient_IsFormatSupported(playback->client, mode,
                                          (WAVEFORMATEX*)&wfx, NULL);
      if (SUCCEEDED(hr)) {
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
      hr = IAudioClient_IsFormatSupported(playback->client, mode,
                                          (WAVEFORMATEX*)&wfx, NULL);
      if (SUCCEEDED(hr)) {
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
      hr = IAudioClient_IsFormatSupported(playback->client, mode,
                                          (WAVEFORMATEX*)&wfx, NULL);
      if (SUCCEEDED(hr)) {
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
      hr = IAudioClient_IsFormatSupported(playback->client, mode,
                                          (WAVEFORMATEX*)&wfx, NULL);
      if (SUCCEEDED(hr)) {
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
        hr = IAudioClient_IsFormatSupported(playback->client, mode,
                                            (WAVEFORMATEX*)&wfx, NULL);
        if (SUCCEEDED(hr)) {
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

  REFERENCE_TIME duration = (REFERENCE_TIME)(((double)playback->chunk_size *
                                              1.0 / playback->sample_rate) *
                                             10000000.0);
  if (mode == AUDCLNT_SHAREMODE_SHARED) {
    duration = 0;
  }
  DWORD flags = 0;
  if (!playback->polling) {
    flags |= AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
  }
  // Autoconvert disabled to match CamillaDSP bit-exact output

  hr = IAudioClient_Initialize(playback->client, mode, flags, duration,
                               playback->exclusive ? duration : 0,
                               final_wfx ? final_wfx : (WAVEFORMATEX*)&wfx, NULL);
  if (FAILED(hr)) {
    WAVEFORMATEX* mix_wfx = NULL;
    if (SUCCEEDED(IAudioClient_GetMixFormat(playback->client, &mix_wfx)) && mix_wfx) {
      if (mix_wfx->wFormatTag == 65534) {
        WAVEFORMATEXTENSIBLE* wfx_ext = (WAVEFORMATEXTENSIBLE*)mix_wfx;
        LPOLESTR subformat_str = NULL;
        StringFromCLSID(&wfx_ext->SubFormat, &subformat_str);
        char fmt_buf[512];
        snprintf(fmt_buf, sizeof(fmt_buf),
                 "Playback mix format EXT: rate=%u, channels=%u, bits=%u, valid_bits=%u, subformat=%ls",
                 (unsigned int)mix_wfx->nSamplesPerSec, (unsigned int)mix_wfx->nChannels,
                 (unsigned int)mix_wfx->wBitsPerSample, (unsigned int)wfx_ext->Samples.wValidBitsPerSample,
                 subformat_str);
        logger_error(&g_logger, "%s", fmt_buf);
        CoTaskMemFree(subformat_str);
      } else {
        logger_error(&g_logger, "Playback mix format std: rate=%u, channels=%u, bits=%u, tag=%u",
                     (unsigned int)mix_wfx->nSamplesPerSec, (unsigned int)mix_wfx->nChannels,
                     (unsigned int)mix_wfx->wBitsPerSample, (unsigned int)mix_wfx->wFormatTag);
      }
      CoTaskMemFree(mix_wfx);
    }
    REFERENCE_TIME debug_def = 0, debug_min = 0;
    IAudioClient_GetDevicePeriod(playback->client, &debug_def, &debug_min);
    logger_error(&g_logger, "Playback periods: def=%lld, min=%lld, requested_duration=%lld, flags=0x%08lX",
                 (long long)debug_def, (long long)debug_min, (long long)duration, (unsigned long)flags);
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
    playback->session_events_listener =
        wasapi_session_events_create(playback, false);
    if (playback->session_events_listener) {
      IAudioSessionControl_RegisterAudioSessionNotification(
          playback->session_control, playback->session_events_listener);
    }
  }

  playback->paused = false;
  playback->started = false;
  atomic_init(&playback->stopped, false);

  logger_info(
      &g_logger,
      "Opened WASAPI playback: device=%s, rate=%d, channels=%d, exclusive=%d",
      playback->device[0] != '\0' ? playback->device : "default",
      playback->sample_rate, playback->channels, playback->exclusive);

  logger_info(&g_logger, "WASAPI allocated buffer size: %u frames",
              playback->buffer_frame_count);

  if (!playback->exclusive) {
    logger_warn(
        &g_logger,
        "WASAPI operating in Shared Mode (32-bit Float). Note: Bit-exact DoP "
        "requires Exclusive Mode ('exclusive': true) to prevent float mixer "
        "bit corruption");
  }

  // Initialize ring buffer and background thread
  size_t ring_size = playback->chunk_size * 8;
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

/**
 * @brief Writes audio data to the WASAPI playback stream.
 *
 * @param ctx Pointer to the wasapi_playback_t instance.
 * @param chunk Pointer to the audio_chunk_t containing data to write.
 * @param err Pointer to a backend_error_t to receive error details on failure.
 * @return true if successful, false otherwise.
 */
static bool wasapi_playback_write(void* ctx, const audio_chunk_t* chunk,
                                  backend_error_t* err) {
  wasapi_playback_t* playback = (wasapi_playback_t*)ctx;
  if (!playback) return false;
  if (atomic_load_explicit(&playback->paused, memory_order_acquire))
    return true;

  DWORD now = GetTickCount();
  if (now - playback->last_rate_check_time > 1000) {
    playback->last_rate_check_time = now;
    double mix_rate = wasapi_device_get_current_mix_rate(playback->device, false);
    if (mix_rate > 0.0 && mix_rate != (double)playback->sample_rate) {
      playback->pending_rate = mix_rate;
      playback->has_pending_rate_change = true;
    }
  }

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
    if (total_frames > playback->buffer_frame_count) {
      logger_error(&g_logger,
                   "Input chunk size %zu exceeds WASAPI buffer capacity %u",
                   total_frames, playback->buffer_frame_count);
      if (err) {
        backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                           "Input chunk size exceeds WASAPI buffer capacity");
      }
      return false;
    }

    size_t frames_written = 0;
    DWORD start_time = GetTickCount();

    while (frames_written < total_frames) {
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
      UINT32 to_write = total_frames - frames_written;

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
        double mix_rate = wasapi_device_get_current_mix_rate(playback->device, false);
        if (mix_rate > 0.0 && mix_rate != (double)playback->sample_rate) {
          playback->pending_rate = mix_rate;
          playback->has_pending_rate_change = true;
        } else if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == 0x88890010 || hr == 0x88890018) {
          playback->pending_rate = 0.0;
          playback->has_pending_rate_change = true;
        }
        logger_error(&g_logger,
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
  } else {
    // Event-driven: write to ring buffer
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
        start_time = GetTickCount();  // progress made
      } else {
        cdsp_sleep_ms(1);
        if (!atomic_load_explicit(&playback->thread_running,
                                  memory_order_acquire)) {
          return false;
        }
      }
    }
  }

  if (!playback->started) {
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

  return true;
}

/**
 * @brief Closes the WASAPI playback stream.
 *
 * @param ctx Pointer to the wasapi_playback_t instance.
 */
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

/**
 * @brief Gets the current buffer level of the playback backend in frames.
 *
 * @param ctx Pointer to the wasapi_playback_t instance.
 * @return The buffer level in frames.
 */
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

/**
 * @brief Checks if there is a pending rate change for the playback backend.
 *
 * @param ctx Pointer to the wasapi_playback_t instance.
 * @param out_rate Pointer to double to receive the new rate if pending.
 * @return true if there is a pending rate change, false otherwise.
 */
static bool wasapi_playback_get_pending_rate_change(void* ctx,
                                                    double* out_rate) {
  wasapi_playback_t* playback = (wasapi_playback_t*)ctx;
  if (!playback) return false;
  bool changed = playback->has_pending_rate_change;
  if (changed) {
    if (playback->pending_rate == 0.0) {
      double mix_rate = wasapi_device_get_current_mix_rate(playback->device, false);
      if (mix_rate > 0.0) {
        playback->pending_rate = mix_rate;
      }
    }
    if (out_rate) {
      *out_rate = playback->pending_rate;
    }
    playback->has_pending_rate_change = false;
  }
  return changed;
}

/**
 * @brief Prefills the playback buffer with silence.
 *
 * @param ctx Pointer to the wasapi_playback_t instance.
 * @param frames Number of frames of silence to prefill.
 * @param err Pointer to a backend_error_t to receive error details on failure.
 * @return true if successful, false otherwise.
 */
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

    // Prefill the ring buffer with silence of buffer_frame_count
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

/**
 * @brief Checks if the playback is currently paused.
 *
 * @param ctx Pointer to the wasapi_playback_t instance.
 * @return true if paused, false otherwise.
 */
static bool wasapi_playback_get_is_paused(void* ctx) {
  wasapi_playback_t* playback = (wasapi_playback_t*)ctx;
  if (!playback) return false;
  return atomic_load_explicit(&playback->paused, memory_order_acquire);
}

/**
 * @brief Sets the paused state of the playback.
 *
 * @param ctx Pointer to the wasapi_playback_t instance.
 * @param paused true to pause, false to resume.
 */
static void wasapi_playback_set_is_paused(void* ctx, bool paused) {
  wasapi_playback_t* playback = (wasapi_playback_t*)ctx;
  if (!playback) return;
  atomic_store_explicit(&playback->paused, paused, memory_order_release);
}

/**
 * @brief Stops the WASAPI playback stream.
 *
 * @param ctx Pointer to the wasapi_playback_t instance.
 */
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

/**
 * @brief Destroys the WASAPI playback instance and frees resources.
 *
 * @param ctx Pointer to the wasapi_playback_t instance to destroy.
 */
static void wasapi_playback_destroy(void* ctx) {
  wasapi_playback_t* playback = (wasapi_playback_t*)ctx;
  if (playback) {
    wasapi_playback_close(playback);
    free(playback);
  }
}

/**
 * @brief Creates a WASAPI playback backend.
 *
 * @param config Configuration for the playback device.
 * @param sample_rate The sample rate in Hz.
 * @param chunk_size The chunk size in frames.
 * @param full_duplex True if running in full duplex mode.
 * @param params Processing parameters.
 * @param err Pointer to a backend_error_t to receive error details on failure.
 * @return A pointer to the created playback_backend_t, or NULL on failure.
 */
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
