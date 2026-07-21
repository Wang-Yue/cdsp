/**
 * @file wasapi_device.c
 * @brief Common WASAPI device helper functions and COM event listeners.
 */

#if defined(ENABLE_WASAPI)

#include <initguid.h>
#include "wasapi_device.h"
#include "wasapi_capture.h"
#include "wasapi_playback.h"
#include "wasapi_capabilities.h"
#include <ks.h>
#include <ksmedia.h>
#include "Utils/cdsp_time.h"

const logger_t g_wasapi_logger = {"dsp.backend.wasapi"};

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
  logger_debug(&g_wasapi_logger, "session_OnSessionDisconnected called, reason=%d",
              (int)DisconnectReason);
  if (DisconnectReason == DisconnectReasonFormatChanged ||
      DisconnectReason == DisconnectReasonServerShutdown) {
    if (self->callback) {
      self->callback(self->parent, 0.0);
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

IAudioSessionEvents* wasapi_session_events_create(void* parent, wasapi_format_change_callback_t callback) {
  CDSPAudioSessionEvents* events =
      (CDSPAudioSessionEvents*)calloc(1, sizeof(CDSPAudioSessionEvents));
  if (!events) return NULL;
  events->lpVtbl = &g_session_events_vtbl;
  events->ref_count = 1;
  events->parent = parent;
  events->callback = callback;
  return (IAudioSessionEvents*)events;
}

bool wasapi_setup_shared_format(IAudioClient* client, int target_sample_rate,
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

double wasapi_device_get_current_mix_rate(const char* device_name, bool is_capture) {
  HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  bool com_ok = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;

  IMMDeviceEnumerator* enumerator = NULL;
  hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                        &IID_IMMDeviceEnumerator, (void**)&enumerator);
  if (FAILED(hr)) {
    if (com_ok) CoUninitialize();
    return 0.0;
  }

  logger_trace(&g_wasapi_logger, "wasapi_device_get_current_mix_rate entered, device=%s, is_capture=%d",
              device_name[0] != '\0' ? device_name : "default", (int)is_capture);
  
  double rate = 0.0;
  for (int i = 0; i < 40; i++) {
    IMMDevice* mm_device = wasapi_find_device_by_name(enumerator, device_name, is_capture);
    if (mm_device) {
      IAudioClient* client = NULL;
      hr = mm_device->lpVtbl->Activate(mm_device, &IID_IAudioClient,
                                       CLSCTX_ALL, NULL, (void**)&client);
      if (SUCCEEDED(hr) && client) {
        WAVEFORMATEX* wfx = NULL;
        hr = IAudioClient_GetMixFormat(client, &wfx);
        if (SUCCEEDED(hr) && wfx) {
          rate = (double)wfx->nSamplesPerSec;
          logger_trace(&g_wasapi_logger, "GetMixFormat succeeded, rate=%f", rate);
          CoTaskMemFree(wfx);
          client->lpVtbl->Release(client);
          mm_device->lpVtbl->Release(mm_device);
          break;
        }
        logger_trace(&g_wasapi_logger, "GetMixFormat failed: hr=0x%08lX", (unsigned long)hr);
        client->lpVtbl->Release(client);
      } else {
        logger_trace(&g_wasapi_logger, "Activate failed: hr=0x%08lX", (unsigned long)hr);
      }
      mm_device->lpVtbl->Release(mm_device);
    } else {
      logger_trace(&g_wasapi_logger, "wasapi_find_device_by_name failed");
    }
    cdsp_sleep_ms(100);
  }

  enumerator->lpVtbl->Release(enumerator);
  if (com_ok) CoUninitialize();
  return rate;
}

#endif // ENABLE_WASAPI
