#include "backend/wasapi_device.h"

/**
 * @file wasapi_device.c
 * @brief Common WASAPI device helper functions and COM event listeners.
 */

#if defined(ENABLE_WASAPI)

#include <assert.h>
#include <initguid.h>
#include <ks.h>
#include <ksmedia.h>
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/sample_conversion.h"
#include "utils/cdsp_time.h"

const logger_t g_wasapi_logger = {"dsp.backend.wasapi"};

#ifndef KSAUDIO_SPEAKER_MONO
#define KSAUDIO_SPEAKER_MONO (SPEAKER_FRONT_CENTER)
#endif
#ifndef KSAUDIO_SPEAKER_STEREO
#define KSAUDIO_SPEAKER_STEREO (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)
#endif
#ifndef KSAUDIO_SPEAKER_QUAD
#define KSAUDIO_SPEAKER_QUAD                                                   \
  (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_BACK_LEFT |              \
   SPEAKER_BACK_RIGHT)
#endif
#ifndef KSAUDIO_SPEAKER_SURROUND
#define KSAUDIO_SPEAKER_SURROUND                                               \
  (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER |           \
   SPEAKER_BACK_CENTER)
#endif
#ifndef KSAUDIO_SPEAKER_5POINT1
#define KSAUDIO_SPEAKER_5POINT1                                                \
  (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER |           \
   SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT)
#endif
#ifndef KSAUDIO_SPEAKER_7POINT1
#define KSAUDIO_SPEAKER_7POINT1                                                \
  (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER |           \
   SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT |            \
   SPEAKER_FRONT_LEFT_OF_CENTER | SPEAKER_FRONT_RIGHT_OF_CENTER)
#endif
#ifndef KSAUDIO_SPEAKER_5POINT1_SURROUND
#define KSAUDIO_SPEAKER_5POINT1_SURROUND                                       \
  (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER |           \
   SPEAKER_LOW_FREQUENCY | SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT)
#endif
#ifndef KSAUDIO_SPEAKER_7POINT1_SURROUND
#define KSAUDIO_SPEAKER_7POINT1_SURROUND                                       \
  (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER |           \
   SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT |            \
   SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT)
#endif

#ifndef CUSTOM_SPEAKER_2POINT1
#define CUSTOM_SPEAKER_2POINT1 (KSAUDIO_SPEAKER_STEREO | SPEAKER_LOW_FREQUENCY)
#endif
#ifndef CUSTOM_SPEAKER_4POINT1
#define CUSTOM_SPEAKER_4POINT1 (KSAUDIO_SPEAKER_QUAD | SPEAKER_LOW_FREQUENCY)
#endif
#ifndef CUSTOM_SPEAKER_4POINT1_SURROUND
#define CUSTOM_SPEAKER_4POINT1_SURROUND                                        \
  (KSAUDIO_SPEAKER_SURROUND | SPEAKER_LOW_FREQUENCY)
#endif
#ifndef CUSTOM_SPEAKER_6POINT1
#define CUSTOM_SPEAKER_6POINT1 (KSAUDIO_SPEAKER_5POINT1 | SPEAKER_BACK_CENTER)
#endif
#ifndef CUSTOM_SPEAKER_6POINT1_SURROUND
#define CUSTOM_SPEAKER_6POINT1_SURROUND                                        \
  (KSAUDIO_SPEAKER_5POINT1_SURROUND | SPEAKER_BACK_CENTER)
#endif

#ifndef PKEY_Device_FriendlyName
static const PROPERTYKEY PKEY_Device_FriendlyName = {
    {0xa45c254e,
     0xdf1c,
     0x4efd,
     {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}},
    14};
#endif

static HRESULT
    STDMETHODCALLTYPE session_QueryInterface(IAudioSessionEvents *This,
                                             REFIID riid, void **ppvObject) {
  if (IsEqualIID(riid, &IID_IAudioSessionEvents) ||
      IsEqualIID(riid, &IID_IUnknown)) {
    *ppvObject = This;
    This->lpVtbl->AddRef(This);
    return S_OK;
  }
  *ppvObject = NULL;
  return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE session_AddRef(IAudioSessionEvents *This) {
  CDSPAudioSessionEvents *self = (CDSPAudioSessionEvents *)This;
  return InterlockedIncrement(&self->ref_count);
}

static ULONG STDMETHODCALLTYPE session_Release(IAudioSessionEvents *This) {
  CDSPAudioSessionEvents *self = (CDSPAudioSessionEvents *)This;
  ULONG rc = InterlockedDecrement(&self->ref_count);
  if (rc == 0) {
    free(self);
  }
  return rc;
}

static HRESULT STDMETHODCALLTYPE session_OnDisplayNameChanged(
    IAudioSessionEvents *This, LPCWSTR NewDisplayName, LPCGUID EventContext) {
  (void)This;
  (void)NewDisplayName;
  (void)EventContext;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE session_OnIconPathChanged(
    IAudioSessionEvents *This, LPCWSTR NewIconPath, LPCGUID EventContext) {
  (void)This;
  (void)NewIconPath;
  (void)EventContext;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE
session_OnSimpleVolumeChanged(IAudioSessionEvents *This, float NewVolume,
                              BOOL NewMute, LPCGUID EventContext) {
  (void)This;
  (void)NewVolume;
  (void)NewMute;
  (void)EventContext;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE session_OnChannelVolumeChanged(
    IAudioSessionEvents *This, DWORD ChannelCount,
    float NewChannelVolumeArray[], DWORD ChangedChannel, LPCGUID EventContext) {
  (void)This;
  (void)ChannelCount;
  (void)NewChannelVolumeArray;
  (void)ChangedChannel;
  (void)EventContext;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE session_OnGroupingParamChanged(
    IAudioSessionEvents *This, LPCGUID NewGroupingParam, LPCGUID EventContext) {
  (void)This;
  (void)NewGroupingParam;
  (void)EventContext;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE
session_OnStateChanged(IAudioSessionEvents *This, AudioSessionState NewState) {
  (void)This;
  (void)NewState;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE session_OnSessionDisconnected(
    IAudioSessionEvents *This, AudioSessionDisconnectReason DisconnectReason) {
  CDSPAudioSessionEvents *self = (CDSPAudioSessionEvents *)This;
  logger_debug(&g_wasapi_logger, "Disconnected, reason: %d.",
               (int)DisconnectReason);
  // Upstream device.rs:454-461: FormatChanged -> format change, every other
  // reason -> error. Only atomics owned by the listener are touched here; the
  // COM notification thread never dereferences backend memory.
  if (DisconnectReason == DisconnectReasonFormatChanged) {
    atomic_store_explicit(&self->format_changed, true, memory_order_release);
  } else {
    atomic_store_explicit(&self->disconnect_reason, (int)DisconnectReason,
                          memory_order_relaxed);
    atomic_store_explicit(&self->device_error, true, memory_order_release);
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

IAudioSessionEvents *wasapi_session_events_create(void) {
  CDSPAudioSessionEvents *events =
      (CDSPAudioSessionEvents *)calloc(1, sizeof(CDSPAudioSessionEvents));
  if (!events)
    return NULL;
  events->lpVtbl = &g_session_events_vtbl;
  events->ref_count = 1;
  atomic_init(&events->format_changed, false);
  atomic_init(&events->device_error, false);
  atomic_init(&events->disconnect_reason, 0);
  return (IAudioSessionEvents *)events;
}

bool wasapi_register_session_events(IAudioClient *client,
                                    IAudioSessionControl **out_control,
                                    IAudioSessionEvents **out_listener,
                                    backend_error_t *err) {
  if (!client || !out_control || !out_listener)
    return false;
  *out_control = NULL;
  *out_listener = NULL;

  HRESULT hr = IAudioClient_GetService(client, &IID_IAudioSessionControl,
                                       (void **)out_control);
  if (FAILED(hr) || !*out_control) {
    *out_control = NULL;
    if (err) {
      char msg[128];
      snprintf(msg, sizeof(msg),
               "Failed to get IAudioSessionControl: hr=0x%08lX",
               (unsigned long)hr);
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
    return false;
  }
  *out_listener = wasapi_session_events_create();
  if (!*out_listener) {
    SAFE_RELEASE(*out_control);
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to allocate session event listener");
    return false;
  }
  hr = IAudioSessionControl_RegisterAudioSessionNotification(*out_control,
                                                             *out_listener);
  if (FAILED(hr)) {
    SAFE_RELEASE(*out_listener);
    SAFE_RELEASE(*out_control);
    if (err) {
      char msg[128];
      snprintf(msg, sizeof(msg),
               "Failed to register session notifications: hr=0x%08lX",
               (unsigned long)hr);
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
    return false;
  }
  return true;
}

void wasapi_unregister_session_events(IAudioSessionControl **control,
                                      IAudioSessionEvents **listener) {
  if (control && *control) {
    if (listener && *listener) {
      IAudioSessionControl_UnregisterAudioSessionNotification(*control,
                                                              *listener);
      SAFE_RELEASE(*listener);
    }
    SAFE_RELEASE(*control);
  } else if (listener && *listener) {
    SAFE_RELEASE(*listener);
  }
}

bool wasapi_session_events_format_changed(IAudioSessionEvents *listener) {
  if (!listener)
    return false;
  CDSPAudioSessionEvents *self = (CDSPAudioSessionEvents *)listener;
  return atomic_load_explicit(&self->format_changed, memory_order_acquire);
}

bool wasapi_session_events_device_error(IAudioSessionEvents *listener,
                                        int *out_reason) {
  if (!listener)
    return false;
  CDSPAudioSessionEvents *self = (CDSPAudioSessionEvents *)listener;
  if (!atomic_load_explicit(&self->device_error, memory_order_acquire))
    return false;
  if (out_reason)
    *out_reason =
        atomic_load_explicit(&self->disconnect_reason, memory_order_relaxed);
  return true;
}

bool wasapi_check_and_resolve_pending_rate(const char *device, bool is_capture,
                                           bool exclusive,
                                           double configured_rate,
                                           IAudioSessionEvents *listener,
                                           double *out_rate) {
  if (!listener)
    return false;
  CDSPAudioSessionEvents *self = (CDSPAudioSessionEvents *)listener;
  if (!atomic_load_explicit(&self->format_changed, memory_order_acquire))
    return false;
  if (!atomic_exchange_explicit(&self->format_changed, false,
                                memory_order_acq_rel))
    return false;

  // Single, non-retrying query; never sleeps on the calling (engine) thread.
  // The shared-mode mix rate says nothing about an exclusive-mode stream.
  double rate = 0.0;
  if (!exclusive) {
    double mix_rate = wasapi_device_get_current_mix_rate(device, is_capture);
    if (mix_rate > 0.0 && fabs(mix_rate - configured_rate) >= 0.5) {
      rate = mix_rate;
    }
  }
  logger_debug(&g_wasapi_logger,
               "Session format change resolved, reporting rate=%f (0 = "
               "unknown/unchanged, forces restart).",
               rate);
  if (out_rate)
    *out_rate = rate;
  return true;
}

const char *wasapi_disconnect_reason_name(int reason) {
  switch (reason) {
  case DisconnectReasonDeviceRemoval:
    return "device removed";
  case DisconnectReasonServerShutdown:
    return "audio service shut down";
  case DisconnectReasonFormatChanged:
    return "format changed";
  case DisconnectReasonSessionLogoff:
    return "session logoff";
  case DisconnectReasonSessionDisconnected:
    return "session disconnected";
  case DisconnectReasonExclusiveModeOverride:
    return "exclusive mode override";
  default:
    return "unknown reason";
  }
}

void wasapi_stream_error_reset(wasapi_stream_error_t *e) {
  if (!e)
    return;
  e->message[0] = '\0';
  atomic_store_explicit(&e->has_error, false, memory_order_release);
}

void wasapi_stream_error_set(wasapi_stream_error_t *e, const char *fmt, ...) {
  if (!e || atomic_load_explicit(&e->has_error, memory_order_relaxed))
    return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(e->message, sizeof(e->message), fmt, ap);
  va_end(ap);
  logger_error(&g_wasapi_logger, "%s", e->message);
  atomic_store_explicit(&e->has_error, true, memory_order_release);
}

const char *wasapi_stream_error_get(const wasapi_stream_error_t *e) {
  if (!e || !atomic_load_explicit(&e->has_error, memory_order_acquire))
    return NULL;
  return e->message;
}

uint32_t wasapi_make_simple_channelmask(size_t channels) {
  if (channels >= 1 && channels <= 18) {
    return (uint32_t)((1ULL << channels) - 1);
  }
  return 0;
}

size_t wasapi_make_channelmasks(size_t channels, DWORD masks[8]) {
  switch (channels) {
  case 1:
    masks[0] = KSAUDIO_SPEAKER_MONO;
    masks[1] = wasapi_make_simple_channelmask(1);
    masks[2] = 0;
    return 3;
  case 2:
    masks[0] = KSAUDIO_SPEAKER_STEREO;
    masks[1] = 0;
    return 2;
  case 3:
    masks[0] = CUSTOM_SPEAKER_2POINT1;
    masks[1] = wasapi_make_simple_channelmask(3);
    masks[2] = 0;
    return 3;
  case 4:
    masks[0] = KSAUDIO_SPEAKER_QUAD;
    masks[1] = KSAUDIO_SPEAKER_SURROUND;
    masks[2] = wasapi_make_simple_channelmask(4);
    masks[3] = 0;
    return 4;
  case 5:
    masks[0] = CUSTOM_SPEAKER_4POINT1;
    masks[1] = CUSTOM_SPEAKER_4POINT1_SURROUND;
    masks[2] = wasapi_make_simple_channelmask(5);
    masks[3] = 0;
    return 4;
  case 6:
    masks[0] = KSAUDIO_SPEAKER_5POINT1_SURROUND;
    masks[1] = KSAUDIO_SPEAKER_5POINT1;
    masks[2] = wasapi_make_simple_channelmask(6);
    masks[3] = 0;
    return 4;
  case 7:
    masks[0] = CUSTOM_SPEAKER_6POINT1_SURROUND;
    masks[1] = CUSTOM_SPEAKER_6POINT1;
    masks[2] = wasapi_make_simple_channelmask(7);
    masks[3] = 0;
    return 4;
  case 8:
    masks[0] = KSAUDIO_SPEAKER_7POINT1_SURROUND;
    masks[1] = KSAUDIO_SPEAKER_7POINT1;
    masks[2] = wasapi_make_simple_channelmask(8);
    masks[3] = 0;
    return 4;
  }
  if (channels >= 9 && channels <= 18) {
    masks[0] = wasapi_make_simple_channelmask(channels);
    masks[1] = 0;
    return 2;
  }
  masks[0] = 0;
  return 1;
}

void wasapi_build_wave_format(binary_sample_format_t fmt, int samplerate,
                              int channels, uint32_t channel_mask,
                              bool has_mask, WAVEFORMATEXTENSIBLE *out_wfx) {
  memset(out_wfx, 0, sizeof(WAVEFORMATEXTENSIBLE));
  int storebits = 32;
  int validbits = 32;
  bool is_float = false;

  switch (fmt) {
  case BINARY_SAMPLE_FORMAT_S16_LE:
    storebits = 16;
    validbits = 16;
    is_float = false;
    break;
  case BINARY_SAMPLE_FORMAT_S24_3_LE:
    storebits = 24;
    validbits = 24;
    is_float = false;
    break;
  case BINARY_SAMPLE_FORMAT_S24_4_LJ_LE:
    storebits = 32;
    validbits = 24;
    is_float = false;
    break;
  case BINARY_SAMPLE_FORMAT_S32_LE:
    storebits = 32;
    validbits = 32;
    is_float = false;
    break;
  case BINARY_SAMPLE_FORMAT_F32_LE:
    storebits = 32;
    validbits = 32;
    is_float = true;
    break;
  case BINARY_SAMPLE_FORMAT_INVALID:
  case BINARY_SAMPLE_FORMAT_S24_4_RJ_LE:
  case BINARY_SAMPLE_FORMAT_F64_LE:
  case BINARY_SAMPLE_FORMAT_DSD_U8:
  case BINARY_SAMPLE_FORMAT_DSD_U16_LE:
  case BINARY_SAMPLE_FORMAT_DSD_U16_BE:
  case BINARY_SAMPLE_FORMAT_DSD_U32_LE:
  case BINARY_SAMPLE_FORMAT_DSD_U32_BE:
  case BINARY_SAMPLE_FORMAT_DSD_U32_REVERSED:
    break;
  }

  uint32_t blockalign = (uint32_t)(channels * storebits / 8);
  uint32_t byterate = (uint32_t)(samplerate * blockalign);

  out_wfx->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  out_wfx->Format.nChannels = (WORD)channels;
  out_wfx->Format.nSamplesPerSec = (DWORD)samplerate;
  out_wfx->Format.nAvgBytesPerSec = byterate;
  out_wfx->Format.nBlockAlign = (WORD)blockalign;
  out_wfx->Format.wBitsPerSample = (WORD)storebits;
  out_wfx->Format.cbSize = 22;

  out_wfx->Samples.wValidBitsPerSample = (WORD)validbits;
  out_wfx->dwChannelMask =
      has_mask ? channel_mask
               : wasapi_make_simple_channelmask((size_t)channels);
  out_wfx->SubFormat =
      is_float ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
}

bool wasapi_is_supported_exclusive_with_quirks(
    IAudioClient *client, const WAVEFORMATEXTENSIBLE *in_wfx,
    WAVEFORMATEXTENSIBLE *out_wfx, bool *out_is_std_wfx) {
  WAVEFORMATEXTENSIBLE wave_fmt = *in_wfx;

  // 1. Direct query (In exclusive mode, ppClosestMatch must be NULL per WASAPI
  // spec)
  HRESULT hr =
      IAudioClient_IsFormatSupported(client, AUDCLNT_SHAREMODE_EXCLUSIVE,
                                     (const WAVEFORMATEX *)&wave_fmt, NULL);
  if (SUCCEEDED(hr) && hr == S_OK) {
    *out_wfx = wave_fmt;
    *out_is_std_wfx = false;
    return true;
  }

  // 2. If channels <= 2, repeat query as standard WAVEFORMATEX
  if (wave_fmt.Format.nChannels <= 2) {
    WAVEFORMATEX std_wfx;
    memset(&std_wfx, 0, sizeof(WAVEFORMATEX));
    std_wfx.wFormatTag =
        IsEqualGUID(&wave_fmt.SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)
            ? WAVE_FORMAT_IEEE_FLOAT
            : WAVE_FORMAT_PCM;
    std_wfx.nChannels = wave_fmt.Format.nChannels;
    std_wfx.nSamplesPerSec = wave_fmt.Format.nSamplesPerSec;
    std_wfx.nAvgBytesPerSec = wave_fmt.Format.nAvgBytesPerSec;
    std_wfx.nBlockAlign = wave_fmt.Format.nBlockAlign;
    std_wfx.wBitsPerSample = wave_fmt.Format.wBitsPerSample;
    std_wfx.cbSize = 0;

    hr = IAudioClient_IsFormatSupported(client, AUDCLNT_SHAREMODE_EXCLUSIVE,
                                        &std_wfx, NULL);
    if (SUCCEEDED(hr) && hr == S_OK) {
      memset(out_wfx, 0, sizeof(WAVEFORMATEXTENSIBLE));
      out_wfx->Format = std_wfx;
      *out_is_std_wfx = true;
      return true;
    }
  }

  // 3. Repeat query with candidate channel masks from wasapi-rs
  // make_channelmasks
  DWORD masks[8];
  size_t mask_count =
      wasapi_make_channelmasks((size_t)wave_fmt.Format.nChannels, masks);
  for (size_t i = 0; i < mask_count; i++) {
    wave_fmt.dwChannelMask = masks[i];
    hr = IAudioClient_IsFormatSupported(client, AUDCLNT_SHAREMODE_EXCLUSIVE,
                                        (const WAVEFORMATEX *)&wave_fmt, NULL);
    if (SUCCEEDED(hr) && hr == S_OK) {
      *out_wfx = wave_fmt;
      *out_is_std_wfx = false;
      return true;
    }
  }

  return false;
}

bool wasapi_get_supported_wave_format_with_channel_mask(
    IAudioClient *client, wasapi_sample_format_t sample_format, int samplerate,
    int channels, bool exclusive, uint32_t channel_mask, bool has_mask,
    WAVEFORMATEXTENSIBLE *out_wfx, bool *out_is_std_wfx,
    binary_sample_format_t *out_bin_fmt) {
  if (exclusive) {
    switch (sample_format) {
    case WASAPI_SAMPLE_FORMAT_S16: {
      WAVEFORMATEXTENSIBLE wfx;
      wasapi_build_wave_format(BINARY_SAMPLE_FORMAT_S16_LE, samplerate,
                               channels, channel_mask, has_mask, &wfx);
      if (wasapi_is_supported_exclusive_with_quirks(client, &wfx, out_wfx,
                                                    out_is_std_wfx)) {
        if (out_bin_fmt)
          *out_bin_fmt = BINARY_SAMPLE_FORMAT_S16_LE;
        return true;
      }
      return false;
    }
    case WASAPI_SAMPLE_FORMAT_S32: {
      WAVEFORMATEXTENSIBLE wfx;
      wasapi_build_wave_format(BINARY_SAMPLE_FORMAT_S32_LE, samplerate,
                               channels, channel_mask, has_mask, &wfx);
      if (wasapi_is_supported_exclusive_with_quirks(client, &wfx, out_wfx,
                                                    out_is_std_wfx)) {
        if (out_bin_fmt)
          *out_bin_fmt = BINARY_SAMPLE_FORMAT_S32_LE;
        return true;
      }
      return false;
    }
    case WASAPI_SAMPLE_FORMAT_F32: {
      WAVEFORMATEXTENSIBLE wfx;
      wasapi_build_wave_format(BINARY_SAMPLE_FORMAT_F32_LE, samplerate,
                               channels, channel_mask, has_mask, &wfx);
      if (wasapi_is_supported_exclusive_with_quirks(client, &wfx, out_wfx,
                                                    out_is_std_wfx)) {
        if (out_bin_fmt)
          *out_bin_fmt = BINARY_SAMPLE_FORMAT_F32_LE;
        return true;
      }
      return false;
    }
    case WASAPI_SAMPLE_FORMAT_S24: {
      // Try S24_3_LE first
      WAVEFORMATEXTENSIBLE wfx24_3;
      wasapi_build_wave_format(BINARY_SAMPLE_FORMAT_S24_3_LE, samplerate,
                               channels, channel_mask, has_mask, &wfx24_3);
      if (wasapi_is_supported_exclusive_with_quirks(client, &wfx24_3, out_wfx,
                                                    out_is_std_wfx)) {
        if (out_bin_fmt)
          *out_bin_fmt = BINARY_SAMPLE_FORMAT_S24_3_LE;
        return true;
      }
      // Fallback to S24_4_LJ_LE
      WAVEFORMATEXTENSIBLE wfx24_4;
      wasapi_build_wave_format(BINARY_SAMPLE_FORMAT_S24_4_LJ_LE, samplerate,
                               channels, channel_mask, has_mask, &wfx24_4);
      if (wasapi_is_supported_exclusive_with_quirks(client, &wfx24_4, out_wfx,
                                                    out_is_std_wfx)) {
        if (out_bin_fmt)
          *out_bin_fmt = BINARY_SAMPLE_FORMAT_S24_4_LJ_LE;
        return true;
      }
      return false;
    }
    case WASAPI_SAMPLE_FORMAT_INVALID:
      return false;
    }
    CDSP_UNREACHABLE();
    return false;
  } else {
    // Shared mode: standard 32-bit float extensible
    WAVEFORMATEXTENSIBLE wfx;
    wasapi_build_wave_format(BINARY_SAMPLE_FORMAT_F32_LE, samplerate, channels,
                             0, false, &wfx);
    WAVEFORMATEX *closest = NULL;
    HRESULT hr = IAudioClient_IsFormatSupported(
        client, AUDCLNT_SHAREMODE_SHARED, (const WAVEFORMATEX *)&wfx, &closest);
    if (closest) {
      CoTaskMemFree(closest);
    }
    if (SUCCEEDED(hr) && hr == S_OK) {
      *out_wfx = wfx;
      *out_is_std_wfx = false;
      if (out_bin_fmt)
        *out_bin_fmt = BINARY_SAMPLE_FORMAT_F32_LE;
      return true;
    }
    return false;
  }
}

/**
 * @brief Builds the shared-mode "unsupported format" message, including the
 * closest match proposed by the audio engine (upstream device.rs:178-188).
 * In shared mode this is the main hint that samplerate/channels must match
 * the mix format.
 */
static void wasapi_describe_shared_mismatch(IAudioClient *client,
                                            int samplerate, int channels,
                                            const char *direction_name,
                                            char *msg, size_t msg_len) {
  WAVEFORMATEXTENSIBLE wfx;
  wasapi_build_wave_format(BINARY_SAMPLE_FORMAT_F32_LE, samplerate, channels, 0,
                           false, &wfx);
  WAVEFORMATEX *closest = NULL;
  HRESULT hr = IAudioClient_IsFormatSupported(
      client, AUDCLNT_SHAREMODE_SHARED, (const WAVEFORMATEX *)&wfx, &closest);
  if (closest) {
    snprintf(msg, msg_len,
             "Device doesn't support format for %s: F32, %d channels, %d Hz "
             "(shared mode). Closest match is: %u channels, %lu Hz, %u bits",
             direction_name, channels, samplerate, (unsigned)closest->nChannels,
             (unsigned long)closest->nSamplesPerSec,
             (unsigned)closest->wBitsPerSample);
    CoTaskMemFree(closest);
  } else {
    snprintf(msg, msg_len,
             "Device doesn't support format for %s: F32, %d channels, %d Hz "
             "(shared mode). Error: hr=0x%08lX",
             direction_name, channels, samplerate, (unsigned long)hr);
  }
}

bool wasapi_get_device_format(
    IAudioClient *client, int samplerate, int channels,
    wasapi_sample_format_t requested_format, bool has_requested_format,
    bool exclusive, const char *direction_name, WAVEFORMATEXTENSIBLE *out_wfx,
    bool *out_is_std_wfx, binary_sample_format_t *out_bin_fmt,
    backend_error_t *err) {
  wasapi_sample_format_t temp_format = requested_format;
  bool has_temp = has_requested_format;
  if (!has_temp && !exclusive) {
    // Shared mode defaults to F32
    temp_format = WASAPI_SAMPLE_FORMAT_F32;
    has_temp = true;
  }

  if (has_temp) {
    if (wasapi_get_supported_wave_format_with_channel_mask(
            client, temp_format, samplerate, channels, exclusive, 0, false,
            out_wfx, out_is_std_wfx, out_bin_fmt)) {
      return true;
    }
    if (err) {
      char msg[256];
      if (!exclusive) {
        wasapi_describe_shared_mismatch(client, samplerate, channels,
                                        direction_name, msg, sizeof(msg));
      } else {
        snprintf(msg, sizeof(msg),
                 "Device doesn't support requested format for %s with %d "
                 "channels at %d Hz",
                 direction_name, channels, samplerate);
      }
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
    return false;
  }

  // Probing order in CamillaDSP: S32, S24, S16, F32
  const wasapi_sample_format_t probe_order[] = {
      WASAPI_SAMPLE_FORMAT_S32,
      WASAPI_SAMPLE_FORMAT_S24,
      WASAPI_SAMPLE_FORMAT_S16,
      WASAPI_SAMPLE_FORMAT_F32,
  };
  for (size_t i = 0; i < 4; i++) {
    if (wasapi_get_supported_wave_format_with_channel_mask(
            client, probe_order[i], samplerate, channels, exclusive, 0, false,
            out_wfx, out_is_std_wfx, out_bin_fmt)) {
      return true;
    }
  }

  if (err) {
    char msg[256];
    snprintf(msg, sizeof(msg),
             "Unable to find a supported sample format for %s with %d channels "
             "at %d Hz",
             direction_name, channels, samplerate);
    backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
  }
  return false;
}

bool wasapi_initialize_stream(IAudioClient *client,
                              const WAVEFORMATEXTENSIBLE *wfx, int samplerate,
                              size_t blockalign, bool exclusive, bool polling,
                              bool loopback, REFERENCE_TIME *out_def_period,
                              HANDLE *out_event_handle,
                              UINT32 *out_buffer_frame_count,
                              const char *direction_name,
                              backend_error_t *err) {
  if (!client || !wfx)
    return false;

  if (loopback && exclusive) {
    if (err) {
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Loopback is not supported in exclusive mode");
    }
    return false;
  }

  REFERENCE_TIME def_time = 0, min_time = 0;
  HRESULT hr_period =
      IAudioClient_GetDevicePeriod(client, &def_time, &min_time);
  if (FAILED(hr_period)) {
    // Upstream `get_device_period()?` (device.rs:304, 378). Continuing with
    // def_time = 0 would request a zero-length shared-mode buffer.
    if (err) {
      char msg[256];
      snprintf(msg, sizeof(msg), "Failed to get device period (%s): hr=0x%08lX",
               direction_name ? direction_name : "Audio",
               (unsigned long)hr_period);
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
    return false;
  }
  if (out_def_period) {
    *out_def_period = def_time;
  }

  REFERENCE_TIME aligned_time = wasapi_calculate_aligned_period_near(
      client, def_time, 128, samplerate, (int)blockalign);

  AUDCLNT_SHAREMODE mode =
      exclusive ? AUDCLNT_SHAREMODE_EXCLUSIVE : AUDCLNT_SHAREMODE_SHARED;

  DWORD streamflags = 0;
  if (!polling) {
    streamflags |= AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
  }
  if (loopback) {
    streamflags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
  }

  REFERENCE_TIME buffer_duration = 0;
  REFERENCE_TIME period = 0;
  if (exclusive) {
    if (polling) {
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

  logger_debug(&g_wasapi_logger, "%s stream mode: polling=%d, exclusive=%d",
               direction_name ? direction_name : "Audio", polling, exclusive);

  HRESULT hr =
      IAudioClient_Initialize(client, mode, streamflags, buffer_duration,
                              period, (const WAVEFORMATEX *)wfx, NULL);
  if (FAILED(hr)) {
    if (err) {
      char msg[256];
      snprintf(msg, sizeof(msg),
               "Failed to initialize IAudioClient (%s): hr=0x%08lX",
               direction_name ? direction_name : "Audio", (unsigned long)hr);
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
    return false;
  }

  logger_debug(&g_wasapi_logger,
               "%s default period %lld, min period %lld, aligned period %lld.",
               direction_name ? direction_name : "Audio", (long long)def_time,
               (long long)min_time, (long long)aligned_time);
  logger_debug(&g_wasapi_logger, "Initialized %s audio client.",
               direction_name ? direction_name : "Audio");

  if (!polling) {
    HANDLE event_handle = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!event_handle) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                           "Failed to create event handle");
      return false;
    }
    hr = IAudioClient_SetEventHandle(client, event_handle);
    if (FAILED(hr)) {
      CloseHandle(event_handle);
      if (err)
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                           "Failed to set event handle");
      return false;
    }
    if (out_event_handle) {
      *out_event_handle = event_handle;
    }
  } else {
    if (out_event_handle) {
      *out_event_handle = NULL;
    }
  }

  if (out_buffer_frame_count) {
    hr = IAudioClient_GetBufferSize(client, out_buffer_frame_count);
    if (FAILED(hr)) {
      if (out_event_handle && *out_event_handle) {
        CloseHandle(*out_event_handle);
        *out_event_handle = NULL;
      }
      if (err) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Failed to get buffer size (%s): hr=0x%08lX",
                 direction_name ? direction_name : "Audio", (unsigned long)hr);
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
      }
      return false;
    }
  }

  return true;
}

/**
 * @brief Returns true if @p device is an endpoint with data flow @p flow.
 *
 * Endpoint IDs passed to IMMDeviceEnumerator_GetDevice can name a render or a
 * capture endpoint. Upstream only matches friendly names inside the
 * direction's collection (device.rs:194-221), so the ID path must apply the
 * same direction filter.
 */
static bool wasapi_device_has_flow(IMMDevice *device, EDataFlow flow) {
  IMMEndpoint *endpoint = NULL;
  if (FAILED(IMMDevice_QueryInterface(device, &IID_IMMEndpoint,
                                      (void **)&endpoint)) ||
      !endpoint) {
    return false;
  }
  EDataFlow actual = eAll;
  HRESULT hr = IMMEndpoint_GetDataFlow(endpoint, &actual);
  IMMEndpoint_Release(endpoint);
  return SUCCEEDED(hr) && actual == flow;
}

/**
 * @brief Looks up an endpoint by ID and keeps it only if its data flow
 * matches the requested direction.
 */
static IMMDevice *wasapi_get_device_by_id(IMMDeviceEnumerator *enumerator,
                                          const char *devname, EDataFlow flow) {
  wchar_t w_id[512] = {0};
  if (MultiByteToWideChar(CP_UTF8, 0, devname, -1, w_id,
                          (int)(sizeof(w_id) / sizeof(w_id[0]))) == 0) {
    return NULL;
  }
  IMMDevice *device = NULL;
  HRESULT hr = IMMDeviceEnumerator_GetDevice(enumerator, w_id, &device);
  if (FAILED(hr) || !device) {
    return NULL;
  }
  if (!wasapi_device_has_flow(device, flow)) {
    logger_warn(&g_wasapi_logger,
                "Endpoint \"%s\" exists but is not a %s endpoint; ignoring it.",
                devname, flow == eRender ? "render" : "capture");
    IMMDevice_Release(device);
    return NULL;
  }
  return device;
}

IMMDevice *wasapi_find_device(IMMDeviceEnumerator *enumerator,
                              const char *devname, bool is_capture,
                              bool loopback) {
  EDataFlow flow = (loopback || !is_capture) ? eRender : eCapture;
  if (!devname || devname[0] == '\0' || strcmp(devname, "default") == 0) {
    IMMDevice *device = NULL;
    HRESULT hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, flow,
                                                             eConsole, &device);
    if (SUCCEEDED(hr)) {
      return device;
    }
    return NULL;
  }

  if (devname[0] == '{') {
    IMMDevice *device = wasapi_get_device_by_id(enumerator, devname, flow);
    if (device) {
      return device;
    }
  }

  IMMDeviceCollection *collection = NULL;
  HRESULT hr = IMMDeviceEnumerator_EnumAudioEndpoints(
      enumerator, flow, DEVICE_STATE_ACTIVE, &collection);
  if (FAILED(hr) || !collection)
    return NULL;

  UINT count = 0;
  if (FAILED(IMMDeviceCollection_GetCount(collection, &count))) {
    count = 0;
  }
  IMMDevice *found = NULL;
  for (UINT i = 0; i < count; i++) {
    IMMDevice *dev = NULL;
    if (FAILED(IMMDeviceCollection_Item(collection, i, &dev)) || !dev)
      continue;

    IPropertyStore *properties = NULL;
    HRESULT hr_prop = IMMDevice_OpenPropertyStore(dev, STGM_READ, &properties);
    if (SUCCEEDED(hr_prop)) {
      PROPVARIANT var;
      PropVariantInit(&var);
      hr_prop =
          IPropertyStore_GetValue(properties, &PKEY_Device_FriendlyName, &var);
      if (SUCCEEDED(hr_prop) && var.vt == VT_LPWSTR && var.pwszVal) {
        // Large enough for any friendly name (a truncated conversion
        // returns 0 and would never match).
        char friendly_name[1024] = {0};
        WideCharToMultiByte(CP_UTF8, 0, var.pwszVal, -1, friendly_name,
                            (int)sizeof(friendly_name), NULL, NULL);
        friendly_name[sizeof(friendly_name) - 1] = '\0';
        if (strcmp(friendly_name, devname) == 0) {
          found = dev;
          PropVariantClear(&var);
          SAFE_RELEASE(properties);
          break;
        }
        PropVariantClear(&var);
      }
      SAFE_RELEASE(properties);
    }
    IMMDevice_Release(dev);
  }
  IMMDeviceCollection_Release(collection);

  if (!found && devname[0] != '{') {
    found = wasapi_get_device_by_id(enumerator, devname, flow);
  }

  return found;
}

bool wasapi_create_device_and_client(const char *devname, bool is_capture,
                                     bool loopback,
                                     IMMDeviceEnumerator **out_enumerator,
                                     IMMDevice **out_device,
                                     IAudioClient **out_client,
                                     backend_error_t *err) {
  if (!out_enumerator || !out_device || !out_client)
    return false;
  *out_enumerator = NULL;
  *out_device = NULL;
  *out_client = NULL;

  HRESULT hr =
      CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                       &IID_IMMDeviceEnumerator, (void **)out_enumerator);
  if (FAILED(hr)) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to create MMDeviceEnumerator");
    return false;
  }

  *out_device =
      wasapi_find_device(*out_enumerator, devname, is_capture, loopback);
  if (!*out_device) {
    SAFE_RELEASE(*out_enumerator);
    if (err)
      backend_error_init(err, BACKEND_ERROR_DEVICE_NOT_FOUND,
                         is_capture ? "WASAPI capture device not found"
                                    : "WASAPI playback device not found");
    return false;
  }

  hr = IMMDevice_Activate(*out_device, &IID_IAudioClient, CLSCTX_ALL, NULL,
                          (void **)out_client);
  if (FAILED(hr)) {
    SAFE_RELEASE(*out_device);
    SAFE_RELEASE(*out_enumerator);
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to activate IAudioClient");
    return false;
  }

  return true;
}

void wasapi_cleanup_device_resources(
    IAudioClient **client, IUnknown **sub_client,
    IAudioSessionControl **session_control,
    IAudioSessionEvents **session_events_listener, HANDLE *event_handle,
    IMMDevice **mm_device, IMMDeviceEnumerator **enumerator,
    bool *com_initialized) {
  // Unregister session notifications first, matching upstream where the
  // EventRegistration local is dropped before the AudioClient
  // (device.rs:463-464, 615-616).
  wasapi_unregister_session_events(session_control, session_events_listener);
  if (client && *client) {
    IAudioClient_Stop(*client);
  }
  if (sub_client && *sub_client) {
    SAFE_RELEASE(*sub_client);
  }
  if (client && *client) {
    SAFE_RELEASE(*client);
  }
  if (event_handle && *event_handle) {
    CloseHandle(*event_handle);
    *event_handle = NULL;
  }
  if (mm_device && *mm_device) {
    SAFE_RELEASE(*mm_device);
  }
  if (enumerator && *enumerator) {
    SAFE_RELEASE(*enumerator);
  }
  if (com_initialized && *com_initialized) {
    CoUninitialize();
    *com_initialized = false;
  }
}

void wasapi_extract_device_name(bool has_device, const char *config_device,
                                char *out_device, size_t max_len) {
  if (!out_device || max_len == 0)
    return;
  if (has_device && config_device && config_device[0] != '\0' &&
      strcmp(config_device, "default") != 0) {
    snprintf(out_device, max_len, "%s", config_device);
  } else {
    out_device[0] = '\0';
  }
}

double wasapi_device_get_current_mix_rate(const char *device_name,
                                          bool is_capture) {
  // Single attempt, no retries or sleeps: this runs on engine threads (via
  // get_pending_rate_change). RPC_E_CHANGED_MODE means COM is already
  // initialized in another apartment on this thread and must NOT be paired
  // with CoUninitialize.
  HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  bool com_ok = SUCCEEDED(hr);
  const char *name = device_name ? device_name : "";

  IMMDeviceEnumerator *enumerator = NULL;
  hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                        &IID_IMMDeviceEnumerator, (void **)&enumerator);
  if (FAILED(hr)) {
    if (com_ok)
      CoUninitialize();
    return 0.0;
  }

  logger_trace(
      &g_wasapi_logger,
      "wasapi_device_get_current_mix_rate entered, device=%s, is_capture=%d",
      name[0] != '\0' ? name : "default", (int)is_capture);

  double rate = 0.0;
  IMMDevice *mm_device =
      wasapi_find_device(enumerator, name, is_capture, false);
  if (mm_device) {
    IAudioClient *client = NULL;
    hr = IMMDevice_Activate(mm_device, &IID_IAudioClient, CLSCTX_ALL, NULL,
                            (void **)&client);
    if (SUCCEEDED(hr) && client) {
      WAVEFORMATEX *wfx = NULL;
      hr = IAudioClient_GetMixFormat(client, &wfx);
      if (SUCCEEDED(hr) && wfx) {
        rate = (double)wfx->nSamplesPerSec;
        logger_trace(&g_wasapi_logger, "GetMixFormat succeeded, rate=%f", rate);
        CoTaskMemFree(wfx);
      } else {
        logger_trace(&g_wasapi_logger, "GetMixFormat failed: hr=0x%08lX",
                     (unsigned long)hr);
      }
      SAFE_RELEASE(client);
    } else {
      logger_trace(&g_wasapi_logger, "Activate failed: hr=0x%08lX",
                   (unsigned long)hr);
    }
    SAFE_RELEASE(mm_device);
  } else {
    logger_trace(&g_wasapi_logger, "wasapi_find_device failed");
  }

  SAFE_RELEASE(enumerator);
  if (com_ok)
    CoUninitialize();
  return rate;
}

REFERENCE_TIME wasapi_calculate_aligned_period_near(
    IAudioClient *client, REFERENCE_TIME desired_period, uint32_t align_bytes,
    int samplerate, int block_align) {
  REFERENCE_TIME def_period = 0, min_period = 0;
  HRESULT hr = IAudioClient_GetDevicePeriod(client, &def_period, &min_period);
  if (FAILED(hr)) {
    return desired_period;
  }

  REFERENCE_TIME adjusted_desired_period =
      desired_period > min_period ? desired_period : min_period;
  uint32_t frame_bytes = (uint32_t)block_align;

  uint32_t period_alignment_bytes = frame_bytes;
  if (align_bytes > 0) {
    uint32_t a = frame_bytes, b = align_bytes;
    while (b) {
      uint32_t t = b;
      b = a % b;
      a = t;
    }
    period_alignment_bytes = (frame_bytes * align_bytes) / a;
  }

  int64_t period_alignment_frames =
      (int64_t)period_alignment_bytes / (int64_t)frame_bytes;
  int64_t desired_period_frames = (int64_t)round(
      (double)adjusted_desired_period * (double)samplerate / 10000000.0);
  int64_t min_period_frames =
      (int64_t)ceil((double)min_period * (double)samplerate / 10000000.0);

  int64_t nbr_segments = desired_period_frames / period_alignment_frames;
  if (nbr_segments * period_alignment_frames < min_period_frames) {
    nbr_segments++;
  }

  int64_t total_frames = period_alignment_frames * nbr_segments;
  REFERENCE_TIME aligned_period = (REFERENCE_TIME)round(
      (10000000.0 / (double)samplerate) * (double)total_frames);
  return aligned_period;
}

DWORD wasapi_get_default_channel_mask(int channels) {
  switch (channels) {
  case 1:
    return KSAUDIO_SPEAKER_MONO;
  case 2:
    return KSAUDIO_SPEAKER_STEREO;
  case 4:
    return KSAUDIO_SPEAKER_QUAD;
  case 6:
    return KSAUDIO_SPEAKER_5POINT1_SURROUND;
  case 8:
    return KSAUDIO_SPEAKER_7POINT1_SURROUND;
  }
  return wasapi_make_simple_channelmask((size_t)channels);
}

#endif // ENABLE_WASAPI
