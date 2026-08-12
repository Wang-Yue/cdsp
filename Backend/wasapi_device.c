#include "Backend/wasapi_device.h"

/**
 * @file wasapi_device.c
 * @brief Common WASAPI device helper functions and COM event listeners.
 */

#if defined(ENABLE_WASAPI)

#include <initguid.h>
#include <ks.h>
#include <ksmedia.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Utils/cdsp_time.h"

const logger_t g_wasapi_logger = {"dsp.backend.wasapi"};

#ifndef KSAUDIO_SPEAKER_MONO
#define KSAUDIO_SPEAKER_MONO (SPEAKER_FRONT_CENTER)
#endif
#ifndef KSAUDIO_SPEAKER_STEREO
#define KSAUDIO_SPEAKER_STEREO (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)
#endif
#ifndef KSAUDIO_SPEAKER_QUAD
#define KSAUDIO_SPEAKER_QUAD                                      \
  (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_BACK_LEFT | \
   SPEAKER_BACK_RIGHT)
#endif
#ifndef KSAUDIO_SPEAKER_SURROUND
#define KSAUDIO_SPEAKER_SURROUND                                     \
  (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | \
   SPEAKER_BACK_CENTER)
#endif
#ifndef KSAUDIO_SPEAKER_5POINT1
#define KSAUDIO_SPEAKER_5POINT1                                      \
  (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | \
   SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT)
#endif
#ifndef KSAUDIO_SPEAKER_7POINT1
#define KSAUDIO_SPEAKER_7POINT1                                      \
  (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | \
   SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT |  \
   SPEAKER_FRONT_LEFT_OF_CENTER | SPEAKER_FRONT_RIGHT_OF_CENTER)
#endif
#ifndef KSAUDIO_SPEAKER_5POINT1_SURROUND
#define KSAUDIO_SPEAKER_5POINT1_SURROUND                             \
  (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | \
   SPEAKER_LOW_FREQUENCY | SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT)
#endif
#ifndef KSAUDIO_SPEAKER_7POINT1_SURROUND
#define KSAUDIO_SPEAKER_7POINT1_SURROUND                             \
  (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | \
   SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT |  \
   SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT)
#endif

#ifndef CUSTOM_SPEAKER_2POINT1
#define CUSTOM_SPEAKER_2POINT1 (KSAUDIO_SPEAKER_STEREO | SPEAKER_LOW_FREQUENCY)
#endif
#ifndef CUSTOM_SPEAKER_4POINT1
#define CUSTOM_SPEAKER_4POINT1 (KSAUDIO_SPEAKER_QUAD | SPEAKER_LOW_FREQUENCY)
#endif
#ifndef CUSTOM_SPEAKER_4POINT1_SURROUND
#define CUSTOM_SPEAKER_4POINT1_SURROUND \
  (KSAUDIO_SPEAKER_SURROUND | SPEAKER_LOW_FREQUENCY)
#endif
#ifndef CUSTOM_SPEAKER_6POINT1
#define CUSTOM_SPEAKER_6POINT1 (KSAUDIO_SPEAKER_5POINT1 | SPEAKER_BACK_CENTER)
#endif
#ifndef CUSTOM_SPEAKER_6POINT1_SURROUND
#define CUSTOM_SPEAKER_6POINT1_SURROUND \
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

static HRESULT STDMETHODCALLTYPE session_OnDisplayNameChanged(
    IAudioSessionEvents* This, LPCWSTR NewDisplayName, LPCGUID EventContext) {
  (void)This;
  (void)NewDisplayName;
  (void)EventContext;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE session_OnIconPathChanged(
    IAudioSessionEvents* This, LPCWSTR NewIconPath, LPCGUID EventContext) {
  (void)This;
  (void)NewIconPath;
  (void)EventContext;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE
session_OnSimpleVolumeChanged(IAudioSessionEvents* This, float NewVolume,
                              BOOL NewMute, LPCGUID EventContext) {
  (void)This;
  (void)NewVolume;
  (void)NewMute;
  (void)EventContext;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE session_OnChannelVolumeChanged(
    IAudioSessionEvents* This, DWORD ChannelCount,
    float NewChannelVolumeArray[], DWORD ChangedChannel, LPCGUID EventContext) {
  (void)This;
  (void)ChannelCount;
  (void)NewChannelVolumeArray;
  (void)ChangedChannel;
  (void)EventContext;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE session_OnGroupingParamChanged(
    IAudioSessionEvents* This, LPCGUID NewGroupingParam, LPCGUID EventContext) {
  (void)This;
  (void)NewGroupingParam;
  (void)EventContext;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE
session_OnStateChanged(IAudioSessionEvents* This, AudioSessionState NewState) {
  (void)This;
  (void)NewState;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE session_OnSessionDisconnected(
    IAudioSessionEvents* This, AudioSessionDisconnectReason DisconnectReason) {
  CDSPAudioSessionEvents* self = (CDSPAudioSessionEvents*)This;
  logger_debug(&g_wasapi_logger,
               "session_OnSessionDisconnected called, reason=%d",
               (int)DisconnectReason);
  if (self->callback) {
    self->callback(
        self->parent,
        (DisconnectReason == DisconnectReasonFormatChanged) ? 0.0 : -1.0);
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

IAudioSessionEvents* wasapi_session_events_create(
    void* parent, wasapi_format_change_callback_t callback) {
  CDSPAudioSessionEvents* events =
      (CDSPAudioSessionEvents*)calloc(1, sizeof(CDSPAudioSessionEvents));
  if (!events) return NULL;
  events->lpVtbl = &g_session_events_vtbl;
  events->ref_count = 1;
  events->parent = parent;
  events->callback = callback;
  return (IAudioSessionEvents*)events;
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
    default:
      if (channels >= 9 && channels <= 18) {
        masks[0] = wasapi_make_simple_channelmask(channels);
        masks[1] = 0;
        return 2;
      }
      masks[0] = 0;
      return 1;
  }
}

void wasapi_build_wave_format(wasapi_binary_sample_format_t fmt, int samplerate,
                              int channels, uint32_t channel_mask,
                              bool has_mask, WAVEFORMATEXTENSIBLE* out_wfx) {
  memset(out_wfx, 0, sizeof(WAVEFORMATEXTENSIBLE));
  int storebits = 32;
  int validbits = 32;
  bool is_float = false;

  switch (fmt) {
    case WASAPI_BINARY_FORMAT_S16_LE:
      storebits = 16;
      validbits = 16;
      is_float = false;
      break;
    case WASAPI_BINARY_FORMAT_S24_3_LE:
      storebits = 24;
      validbits = 24;
      is_float = false;
      break;
    case WASAPI_BINARY_FORMAT_S24_4_LJ_LE:
      storebits = 32;
      validbits = 24;
      is_float = false;
      break;
    case WASAPI_BINARY_FORMAT_S32_LE:
      storebits = 32;
      validbits = 32;
      is_float = false;
      break;
    case WASAPI_BINARY_FORMAT_F32_LE:
      storebits = 32;
      validbits = 32;
      is_float = true;
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
    IAudioClient* client, const WAVEFORMATEXTENSIBLE* in_wfx,
    WAVEFORMATEXTENSIBLE* out_wfx, bool* out_is_std_wfx) {
  WAVEFORMATEXTENSIBLE wave_fmt = *in_wfx;

  // 1. Direct query (In exclusive mode, ppClosestMatch must be NULL per WASAPI
  // spec)
  HRESULT hr =
      IAudioClient_IsFormatSupported(client, AUDCLNT_SHAREMODE_EXCLUSIVE,
                                     (const WAVEFORMATEX*)&wave_fmt, NULL);
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
                                        (const WAVEFORMATEX*)&wave_fmt, NULL);
    if (SUCCEEDED(hr) && hr == S_OK) {
      *out_wfx = wave_fmt;
      *out_is_std_wfx = false;
      return true;
    }
  }

  return false;
}

bool wasapi_get_supported_wave_format_with_channel_mask(
    IAudioClient* client, wasapi_sample_format_t sample_format, int samplerate,
    int channels, bool exclusive, uint32_t channel_mask, bool has_mask,
    WAVEFORMATEXTENSIBLE* out_wfx, bool* out_is_std_wfx,
    wasapi_binary_sample_format_t* out_bin_fmt) {
  if (exclusive) {
    switch (sample_format) {
      case WASAPI_SAMPLE_FORMAT_S16: {
        WAVEFORMATEXTENSIBLE wfx;
        wasapi_build_wave_format(WASAPI_BINARY_FORMAT_S16_LE, samplerate,
                                 channels, channel_mask, has_mask, &wfx);
        if (wasapi_is_supported_exclusive_with_quirks(client, &wfx, out_wfx,
                                                      out_is_std_wfx)) {
          if (out_bin_fmt) *out_bin_fmt = WASAPI_BINARY_FORMAT_S16_LE;
          return true;
        }
        return false;
      }
      case WASAPI_SAMPLE_FORMAT_S32: {
        WAVEFORMATEXTENSIBLE wfx;
        wasapi_build_wave_format(WASAPI_BINARY_FORMAT_S32_LE, samplerate,
                                 channels, channel_mask, has_mask, &wfx);
        if (wasapi_is_supported_exclusive_with_quirks(client, &wfx, out_wfx,
                                                      out_is_std_wfx)) {
          if (out_bin_fmt) *out_bin_fmt = WASAPI_BINARY_FORMAT_S32_LE;
          return true;
        }
        return false;
      }
      case WASAPI_SAMPLE_FORMAT_F32: {
        WAVEFORMATEXTENSIBLE wfx;
        wasapi_build_wave_format(WASAPI_BINARY_FORMAT_F32_LE, samplerate,
                                 channels, channel_mask, has_mask, &wfx);
        if (wasapi_is_supported_exclusive_with_quirks(client, &wfx, out_wfx,
                                                      out_is_std_wfx)) {
          if (out_bin_fmt) *out_bin_fmt = WASAPI_BINARY_FORMAT_F32_LE;
          return true;
        }
        return false;
      }
      case WASAPI_SAMPLE_FORMAT_S24: {
        // Try S24_3_LE first
        WAVEFORMATEXTENSIBLE wfx24_3;
        wasapi_build_wave_format(WASAPI_BINARY_FORMAT_S24_3_LE, samplerate,
                                 channels, channel_mask, has_mask, &wfx24_3);
        if (wasapi_is_supported_exclusive_with_quirks(client, &wfx24_3, out_wfx,
                                                      out_is_std_wfx)) {
          if (out_bin_fmt) *out_bin_fmt = WASAPI_BINARY_FORMAT_S24_3_LE;
          return true;
        }
        // Fallback to S24_4_LJ_LE
        WAVEFORMATEXTENSIBLE wfx24_4;
        wasapi_build_wave_format(WASAPI_BINARY_FORMAT_S24_4_LJ_LE, samplerate,
                                 channels, channel_mask, has_mask, &wfx24_4);
        if (wasapi_is_supported_exclusive_with_quirks(client, &wfx24_4, out_wfx,
                                                      out_is_std_wfx)) {
          if (out_bin_fmt) *out_bin_fmt = WASAPI_BINARY_FORMAT_S24_4_LJ_LE;
          return true;
        }
        return false;
      }
      default:
        return false;
    }
  } else {
    // Shared mode: standard 32-bit float extensible
    WAVEFORMATEXTENSIBLE wfx;
    wasapi_build_wave_format(WASAPI_BINARY_FORMAT_F32_LE, samplerate, channels,
                             0, false, &wfx);
    WAVEFORMATEX* closest = NULL;
    HRESULT hr = IAudioClient_IsFormatSupported(
        client, AUDCLNT_SHAREMODE_SHARED, (const WAVEFORMATEX*)&wfx, &closest);
    if (closest) {
      CoTaskMemFree(closest);
    }
    if (SUCCEEDED(hr) && hr == S_OK) {
      *out_wfx = wfx;
      *out_is_std_wfx = false;
      if (out_bin_fmt) *out_bin_fmt = WASAPI_BINARY_FORMAT_F32_LE;
      return true;
    }
    return false;
  }
}

bool wasapi_get_device_format(
    IAudioClient* client, int samplerate, int channels,
    wasapi_sample_format_t requested_format, bool has_requested_format,
    bool exclusive, const char* direction_name, WAVEFORMATEXTENSIBLE* out_wfx,
    bool* out_is_std_wfx, wasapi_binary_sample_format_t* out_bin_fmt,
    backend_error_t* err) {
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
      snprintf(msg, sizeof(msg),
               "Device doesn't support requested format for %s with %d "
               "channels at %d Hz",
               direction_name, channels, samplerate);
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

IMMDevice* wasapi_find_device(IMMDeviceEnumerator* enumerator,
                              const char* devname, bool is_capture,
                              bool loopback) {
  EDataFlow flow = (loopback || !is_capture) ? eRender : eCapture;
  if (!devname || devname[0] == '\0' || strcmp(devname, "default") == 0) {
    IMMDevice* device = NULL;
    HRESULT hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, flow,
                                                             eConsole, &device);
    if (SUCCEEDED(hr)) {
      return device;
    }
    return NULL;
  }

  if (devname[0] == '{') {
    wchar_t w_id[256] = {0};
    mbstowcs(w_id, devname, 255);
    IMMDevice* device = NULL;
    HRESULT hr = IMMDeviceEnumerator_GetDevice(enumerator, w_id, &device);
    if (SUCCEEDED(hr) && device) {
      return device;
    }
  }

  IMMDeviceCollection* collection = NULL;
  HRESULT hr = IMMDeviceEnumerator_EnumAudioEndpoints(
      enumerator, flow, DEVICE_STATE_ACTIVE, &collection);
  if (FAILED(hr) || !collection) return NULL;

  UINT count = 0;
  IMMDeviceCollection_GetCount(collection, &count);
  IMMDevice* found = NULL;
  for (UINT i = 0; i < count; i++) {
    IMMDevice* dev = NULL;
    IMMDeviceCollection_Item(collection, i, &dev);
    if (!dev) continue;

    IPropertyStore* properties = NULL;
    HRESULT hr_prop = IMMDevice_OpenPropertyStore(dev, STGM_READ, &properties);
    if (SUCCEEDED(hr_prop)) {
      PROPVARIANT var;
      PropVariantInit(&var);
      hr_prop =
          IPropertyStore_GetValue(properties, &PKEY_Device_FriendlyName, &var);
      if (SUCCEEDED(hr_prop) && var.vt == VT_LPWSTR) {
        char friendly_name[256] = {0};
        wcstombs(friendly_name, var.pwszVal, sizeof(friendly_name) - 1);
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
    wchar_t w_id[256] = {0};
    mbstowcs(w_id, devname, 255);
    hr = IMMDeviceEnumerator_GetDevice(enumerator, w_id, &found);
    if (SUCCEEDED(hr) && found) {
      return found;
    }
  }

  return found;
}

double wasapi_device_get_current_mix_rate(const char* device_name,
                                          bool is_capture) {
  HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  bool com_ok = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;

  IMMDeviceEnumerator* enumerator = NULL;
  hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                        &IID_IMMDeviceEnumerator, (void**)&enumerator);
  if (FAILED(hr)) {
    if (com_ok) CoUninitialize();
    return 0.0;
  }

  logger_trace(
      &g_wasapi_logger,
      "wasapi_device_get_current_mix_rate entered, device=%s, is_capture=%d",
      device_name[0] != '\0' ? device_name : "default", (int)is_capture);

  double rate = 0.0;
  for (int i = 0; i < 40; i++) {
    IMMDevice* mm_device =
        wasapi_find_device(enumerator, device_name, is_capture, false);
    if (mm_device) {
      IAudioClient* client = NULL;
      hr = IMMDevice_Activate(mm_device, &IID_IAudioClient, CLSCTX_ALL, NULL,
                              (void**)&client);
      if (SUCCEEDED(hr) && client) {
        WAVEFORMATEX* wfx = NULL;
        hr = IAudioClient_GetMixFormat(client, &wfx);
        if (SUCCEEDED(hr) && wfx) {
          rate = (double)wfx->nSamplesPerSec;
          logger_trace(&g_wasapi_logger, "GetMixFormat succeeded, rate=%f",
                       rate);
          CoTaskMemFree(wfx);
          SAFE_RELEASE(client);
          SAFE_RELEASE(mm_device);
          break;
        }
        logger_trace(&g_wasapi_logger, "GetMixFormat failed: hr=0x%08lX",
                     (unsigned long)hr);
        SAFE_RELEASE(client);
      } else {
        logger_trace(&g_wasapi_logger, "Activate failed: hr=0x%08lX",
                     (unsigned long)hr);
      }
      SAFE_RELEASE(mm_device);
    } else {
      logger_trace(&g_wasapi_logger, "wasapi_find_device failed");
    }
    cdsp_sleep_ms(100);
  }

  SAFE_RELEASE(enumerator);
  if (com_ok) CoUninitialize();
  return rate;
}

REFERENCE_TIME wasapi_calculate_aligned_period_near(
    IAudioClient* client, REFERENCE_TIME desired_period, uint32_t align_bytes,
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
    default:
      return wasapi_make_simple_channelmask((size_t)channels);
  }
}

#endif  // ENABLE_WASAPI
