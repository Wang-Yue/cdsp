#include "Backend/wasapi_capabilities.h"

#if defined(ENABLE_WASAPI)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef COBJMACROS
#define COBJMACROS
#endif

#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "Backend/wasapi_device.h"
#include "Logging/app_logger.h"

int wasapi_capabilities_available_device_names(bool is_capture,
                                               char out_names[][256],
                                               int max_names) {
  HRESULT init_hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  bool com_initialized = SUCCEEDED(init_hr);

  IMMDeviceEnumerator* enumerator = NULL;
  IMMDeviceCollection* collection = NULL;
  HRESULT hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                &IID_IMMDeviceEnumerator, (void**)&enumerator);
  if (FAILED(hr)) goto error_cleanup;
  hr = IMMDeviceEnumerator_EnumAudioEndpoints(enumerator,
                                              is_capture ? eCapture : eRender,
                                              DEVICE_STATE_ACTIVE, &collection);
  if (FAILED(hr)) {
    goto error_cleanup;
  }

  UINT count = 0;
  IMMDeviceCollection_GetCount(collection, &count);
  int matched = 0;

  for (UINT i = 0; i < count && matched < max_names; i++) {
    IMMDevice* dev = NULL;
    IMMDeviceCollection_Item(collection, i, &dev);
    if (dev) {
      IPropertyStore* properties = NULL;
      char name_buf[256] = {0};
      bool has_name = false;
      HRESULT hr_prop =
          IMMDevice_OpenPropertyStore(dev, STGM_READ, &properties);
      if (SUCCEEDED(hr_prop)) {
        PROPVARIANT var;
        PropVariantInit(&var);
        hr_prop = IPropertyStore_GetValue(properties, &PKEY_Device_FriendlyName,
                                          &var);
        if (SUCCEEDED(hr_prop) && var.vt == VT_LPWSTR && var.pwszVal) {
          wcstombs(name_buf, var.pwszVal, sizeof(name_buf) - 1);
          name_buf[sizeof(name_buf) - 1] = '\0';
          has_name = true;
        }
        PropVariantClear(&var);
        SAFE_RELEASE(properties);
      }
      if (!has_name) {
        LPWSTR id = NULL;
        IMMDevice_GetId(dev, &id);
        if (id) {
          wcstombs(name_buf, id, sizeof(name_buf) - 1);
          name_buf[sizeof(name_buf) - 1] = '\0';
          CoTaskMemFree(id);
        }
      }
      if (name_buf[0] != '\0') {
        snprintf(out_names[matched++], 256, "%s", name_buf);
      }
      SAFE_RELEASE(dev);
    }
  }

  SAFE_RELEASE(collection);
  SAFE_RELEASE(enumerator);
  if (com_initialized) {
    CoUninitialize();
  }
  return matched;

error_cleanup:
  if (collection) {
    SAFE_RELEASE(collection);
  }
  if (enumerator) {
    SAFE_RELEASE(enumerator);
  }
  if (com_initialized) {
    CoUninitialize();
  }
  return 0;
}

bool wasapi_capabilities_default_device_name(bool is_capture, char* out_name,
                                             size_t max_len) {
  (void)is_capture;
  snprintf(out_name, max_len, "default");
  return true;
}

int wasapi_capabilities_channel_count(const char* device_name,
                                      bool is_capture) {
  HRESULT init_hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  bool com_initialized = SUCCEEDED(init_hr);

  IMMDeviceEnumerator* enumerator = NULL;
  IMMDevice* device = NULL;
  IAudioClient* client = NULL;
  WAVEFORMATEX* wfx = NULL;
  int channels = 2;

  HRESULT hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                &IID_IMMDeviceEnumerator, (void**)&enumerator);
  if (FAILED(hr)) goto cleanup;

  device = wasapi_find_device(enumerator, device_name, is_capture, false);
  if (!device) goto cleanup;

  hr = IMMDevice_Activate(device, &IID_IAudioClient, CLSCTX_ALL, NULL,
                          (void**)&client);
  if (FAILED(hr)) goto cleanup;

  hr = IAudioClient_GetMixFormat(client, &wfx);
  if (SUCCEEDED(hr) && wfx) {
    channels = (int)wfx->nChannels;
    CoTaskMemFree(wfx);
  }

cleanup:
  SAFE_RELEASE(client);
  SAFE_RELEASE(device);
  SAFE_RELEASE(enumerator);
  if (com_initialized) {
    CoUninitialize();
  }
  return channels;
}

static const char* wasapi_format_to_str(wasapi_sample_format_t fmt) {
  switch (fmt) {
    case WASAPI_SAMPLE_FORMAT_S16:
      return "S16";
    case WASAPI_SAMPLE_FORMAT_S24:
      return "S24";
    case WASAPI_SAMPLE_FORMAT_S32:
      return "S32";
    case WASAPI_SAMPLE_FORMAT_F32:
      return "F32";
    default:
      return "F32";
  }
}

static void format_labels_to_str(const wasapi_sample_format_t* formats,
                                 size_t count, char* buf, size_t buf_len) {
  size_t offset = 0;
  offset += snprintf(buf + offset, buf_len - offset, "[");
  for (size_t i = 0; i < count; i++) {
    offset += snprintf(buf + offset, buf_len - offset,
                       (i > 0 ? ", \"%s\"" : "\"%s\""),
                       wasapi_format_to_str(formats[i]));
  }
  snprintf(buf + offset, buf_len - offset, "]");
}

static void int_list_to_str(const int* values, size_t count, char* buf,
                            size_t buf_len) {
  size_t offset = 0;
  offset += snprintf(buf + offset, buf_len - offset, "[");
  for (size_t i = 0; i < count; i++) {
    offset += snprintf(buf + offset, buf_len - offset,
                       (i > 0 ? ", %d" : "%d"), values[i]);
  }
  snprintf(buf + offset, buf_len - offset, "]");
}

#define MAX_CAP_CHANNELS 33
#define MAX_CAP_RATES 32
#define MAX_CAP_FORMATS 4

typedef struct {
  int samplerate;
  char* formats[MAX_CAP_FORMATS];
  size_t formats_count;
} temp_samplerate_cap_t;

typedef struct {
  int channels;
  temp_samplerate_cap_t samplerates[MAX_CAP_RATES];
  size_t samplerates_count;
} temp_channel_cap_t;

typedef struct {
  temp_channel_cap_t channels[MAX_CAP_CHANNELS];
  size_t channels_count;
} temp_capabilities_map_t;

typedef struct {
  int max_supported_channels;
  wasapi_sample_format_t supported_formats[4];
  size_t supported_formats_count;
} rate_probe_result_t;

static void capabilities_map_insert(temp_capabilities_map_t* map, int channels,
                                    int samplerate,
                                    const wasapi_sample_format_t* formats,
                                    size_t formats_count) {
  temp_channel_cap_t* chan_entry = NULL;
  for (size_t c = 0; c < map->channels_count; c++) {
    if (map->channels[c].channels == channels) {
      chan_entry = &map->channels[c];
      break;
    }
  }
  if (!chan_entry) {
    if (map->channels_count >= MAX_CAP_CHANNELS) return;
    chan_entry = &map->channels[map->channels_count++];
    chan_entry->channels = channels;
    chan_entry->samplerates_count = 0;
  }

  temp_samplerate_cap_t* rate_entry = NULL;
  for (size_t r = 0; r < chan_entry->samplerates_count; r++) {
    if (chan_entry->samplerates[r].samplerate == samplerate) {
      rate_entry = &chan_entry->samplerates[r];
      break;
    }
  }
  if (!rate_entry) {
    if (chan_entry->samplerates_count >= MAX_CAP_RATES) return;
    rate_entry = &chan_entry->samplerates[chan_entry->samplerates_count++];
    rate_entry->samplerate = samplerate;
    rate_entry->formats_count = 0;
  }

  for (size_t f = 0; f < formats_count; f++) {
    const char* str = wasapi_format_to_str(formats[f]);
    bool exists = false;
    for (size_t ef = 0; ef < rate_entry->formats_count; ef++) {
      if (rate_entry->formats[ef] &&
          strcmp(rate_entry->formats[ef], str) == 0) {
        exists = true;
        break;
      }
    }
    if (!exists && rate_entry->formats_count < MAX_CAP_FORMATS) {
      rate_entry->formats[rate_entry->formats_count++] = strdup(str);
    }
  }
}

static void free_temp_capabilities_map(temp_capabilities_map_t* map) {
  if (!map) return;
  for (size_t c = 0; c < map->channels_count; c++) {
    for (size_t r = 0; r < map->channels[c].samplerates_count; r++) {
      for (size_t f = 0; f < map->channels[c].samplerates[r].formats_count;
           f++) {
        if (map->channels[c].samplerates[r].formats[f]) {
          free(map->channels[c].samplerates[r].formats[f]);
          map->channels[c].samplerates[r].formats[f] = NULL;
        }
      }
    }
  }
}

static rate_probe_result_t probe_and_store_rate_with_candidates(
    temp_capabilities_map_t* map, IAudioClient* client, int samplerate,
    const int* channel_counts, size_t channel_counts_len,
    const wasapi_sample_format_t* candidate_formats,
    size_t candidate_formats_count, uint32_t* cached_masks,
    bool* has_cached_masks) {
  char cand_str[128];
  format_labels_to_str(candidate_formats, candidate_formats_count, cand_str,
                       sizeof(cand_str));
  logger_trace(&g_wasapi_logger,
               "WASAPI capability probe: probing %d Hz using sample formats "
               "%s.",
               samplerate, cand_str);

  rate_probe_result_t res;
  memset(&res, 0, sizeof(res));
  wasapi_sample_format_t narrowed_formats[4];
  size_t narrowed_formats_count = 0;

  for (size_t c_idx = 0; c_idx < channel_counts_len; c_idx++) {
    int channels = channel_counts[c_idx];
    if (channels < 1 || channels > 32) continue;

    const wasapi_sample_format_t* active_formats =
        narrowed_formats_count > 0 ? narrowed_formats : candidate_formats;
    size_t active_formats_count = narrowed_formats_count > 0
                                      ? narrowed_formats_count
                                      : candidate_formats_count;

    wasapi_sample_format_t supported_for_channel[4];
    size_t supported_for_channel_count = 0;

    uint32_t pref_mask = cached_masks[channels];
    bool has_pref = has_cached_masks[channels];
    if (has_pref) {
      logger_trace(&g_wasapi_logger,
                   "WASAPI capability probe: probing %d Hz, %d ch using "
                   "cached channel mask 0x%08x.",
                   samplerate, channels, (unsigned int)pref_mask);
    }

    logger_trace(&g_wasapi_logger,
                 "WASAPI capability probe: probing %d Hz, %d channels.",
                 samplerate, channels);

    for (size_t f = 0; f < active_formats_count; f++) {
      wasapi_sample_format_t fmt = active_formats[f];
      logger_trace(&g_wasapi_logger,
                   "WASAPI capability probe: testing %d Hz, %d ch, format %s.",
                   samplerate, channels, wasapi_format_to_str(fmt));
      WAVEFORMATEXTENSIBLE out_wfx;
      bool out_is_std = false;
      if (wasapi_get_supported_wave_format_with_channel_mask(
              client, fmt, samplerate, channels, true, pref_mask, has_pref,
              &out_wfx, &out_is_std, NULL)) {
        logger_trace(
            &g_wasapi_logger,
            "WASAPI capability probe: supported %d Hz, %d ch, format %s.",
            samplerate, channels, wasapi_format_to_str(fmt));
        if (!out_is_std) {
          uint32_t prev_mask = pref_mask;
          bool prev_has = has_pref;
          cached_masks[channels] = out_wfx.dwChannelMask;
          has_cached_masks[channels] = true;
          pref_mask = out_wfx.dwChannelMask;
          has_pref = true;
          if (!prev_has || prev_mask != out_wfx.dwChannelMask) {
            logger_trace(&g_wasapi_logger,
                         "WASAPI capability probe: channel count %d will use "
                         "channel mask 0x%08x for subsequent probes.",
                         channels, (unsigned int)out_wfx.dwChannelMask);
          }
        }
        supported_for_channel[supported_for_channel_count++] = fmt;
      } else {
        logger_trace(
            &g_wasapi_logger,
            "WASAPI capability probe: unsupported %d Hz, %d ch, format %s.",
            samplerate, channels, wasapi_format_to_str(fmt));
      }
    }

    if (supported_for_channel_count > 0) {
      char supp_str[128];
      format_labels_to_str(supported_for_channel, supported_for_channel_count,
                           supp_str, sizeof(supp_str));
      logger_trace(&g_wasapi_logger,
                   "WASAPI capability probe: found support at %d Hz, %d ch "
                   "with formats %s.",
                   samplerate, channels, supp_str);

      if (narrowed_formats_count == 0 &&
          supported_for_channel_count < candidate_formats_count) {
        logger_debug(&g_wasapi_logger,
                     "WASAPI capability probe: narrowing sample formats for "
                     "the rest of the %d Hz sweep to %s.",
                     samplerate, supp_str);
        for (size_t i = 0; i < supported_for_channel_count; i++) {
          narrowed_formats[i] = supported_for_channel[i];
        }
        narrowed_formats_count = supported_for_channel_count;
      }

      res.max_supported_channels = channels;
      for (size_t i = 0; i < supported_for_channel_count; i++) {
        bool already = false;
        for (size_t j = 0; j < res.supported_formats_count; j++) {
          if (res.supported_formats[j] == supported_for_channel[i]) {
            already = true;
            break;
          }
        }
        if (!already && res.supported_formats_count < 4) {
          res.supported_formats[res.supported_formats_count++] =
              supported_for_channel[i];
        }
      }

      capabilities_map_insert(map, channels, samplerate, supported_for_channel,
                              supported_for_channel_count);
    } else {
      logger_trace(&g_wasapi_logger,
                   "WASAPI capability probe: no supported formats at %d Hz, "
                   "%d ch.",
                   samplerate, channels);
    }
  }

  if (res.max_supported_channels > 0) {
    logger_trace(&g_wasapi_logger,
                 "WASAPI capability probe: highest supported channel count at "
                 "%d Hz is %d.",
                 samplerate, res.max_supported_channels);
  } else {
    logger_trace(&g_wasapi_logger,
                 "WASAPI capability probe: no support found at %d Hz.",
                 samplerate);
  }

  return res;
}

static int compare_int(const void* a, const void* b) {
  int ia = *(const int*)a;
  int ib = *(const int*)b;
  return (ia > ib) - (ia < ib);
}

static int compare_samplerates(const void* a, const void* b) {
  const samplerate_capability_t* sa = (const samplerate_capability_t*)a;
  const samplerate_capability_t* sb = (const samplerate_capability_t*)b;
  return (sa->samplerate > sb->samplerate) - (sa->samplerate < sb->samplerate);
}

static int compare_channels(const void* a, const void* b) {
  const channel_capability_t* ca = (const channel_capability_t*)a;
  const channel_capability_t* cb = (const channel_capability_t*)b;
  return (ca->channels > cb->channels) - (ca->channels < cb->channels);
}

audio_device_descriptor_t* wasapi_capabilities_describe(const char* device_name,
                                                        bool is_capture,
                                                        device_error_t* err) {
  HRESULT init_hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  bool com_initialized = SUCCEEDED(init_hr);

  IMMDeviceEnumerator* enumerator = NULL;
  IMMDevice* device = NULL;
  IAudioClient* client = NULL;
  audio_device_descriptor_t* desc = NULL;
  temp_capabilities_map_t exclusive_map;
  memset(&exclusive_map, 0, sizeof(exclusive_map));

  HRESULT hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                &IID_IMMDeviceEnumerator, (void**)&enumerator);
  if (FAILED(hr)) {
    if (err) {
      device_error_init(err, DEVICE_ERROR_OTHER,
                        "Failed to create MMDeviceEnumerator");
    }
    goto error_cleanup;
  }

  device = wasapi_find_device(enumerator, device_name, is_capture, false);

  if (!device) {
    if (err) {
      device_error_init(err, DEVICE_ERROR_NOT_FOUND, "Device not found");
    }
    goto error_cleanup;
  }

  hr = IMMDevice_Activate(device, &IID_IAudioClient, CLSCTX_ALL, NULL,
                          (void**)&client);
  if (FAILED(hr)) {
    if (err) {
      if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
        device_error_init(err, DEVICE_ERROR_NOT_FOUND, "Device invalidated");
      } else if (hr == E_ACCESSDENIED ||
                 hr == AUDCLNT_E_RESOURCES_INVALIDATED) {
        device_error_init(err, DEVICE_ERROR_BUSY, "Device is busy");
      } else {
        device_error_init(err, DEVICE_ERROR_OTHER,
                          "Failed to activate WASAPI client");
      }
    }
    goto error_cleanup;
  }

  desc =
      (audio_device_descriptor_t*)calloc(1, sizeof(audio_device_descriptor_t));
  if (!desc) {
    goto error_cleanup;
  }

  if (device_name && device_name[0] != '\0') {
    snprintf(desc->name, sizeof(desc->name), "%s", device_name);
  } else {
    IPropertyStore* prop_store = NULL;
    if (SUCCEEDED(
            IMMDevice_OpenPropertyStore(device, STGM_READ, &prop_store))) {
      PROPVARIANT var;
      PropVariantInit(&var);
      if (SUCCEEDED(IPropertyStore_GetValue(prop_store,
                                            &PKEY_Device_FriendlyName, &var)) &&
          var.vt == VT_LPWSTR) {
        wcstombs(desc->name, var.pwszVal, sizeof(desc->name) - 1);
        desc->name[sizeof(desc->name) - 1] = '\0';
        PropVariantClear(&var);
      }
      SAFE_RELEASE(prop_store);
    }
    if (desc->name[0] == '\0') {
      snprintf(desc->name, sizeof(desc->name), "default");
    }
  }

  logger_debug(&g_wasapi_logger,
               "WASAPI capability probe: starting capability scan for device "
               "\"%s\", input=%s.",
               device_name ? device_name : "default",
               is_capture ? "true" : "false");

  // --- Shared mode: GetMixFormat provides authoritative mixer configuration
  // ---
  WAVEFORMATEX* mix_wfx = NULL;
  bool has_shared = false;
  if (SUCCEEDED(IAudioClient_GetMixFormat(client, &mix_wfx)) && mix_wfx) {
    has_shared = true;
    logger_debug(
        &g_wasapi_logger,
        "WASAPI capability probe: shared mode mix format is %d Hz, %d ch, "
        "format F32.",
        (int)mix_wfx->nSamplesPerSec, (int)mix_wfx->nChannels);
  }

  // --- Exclusive mode: staged search matching CamillaDSP capabilities.rs ---
  const int FAMILY_48_RATES[] = {48000, 96000, 192000, 384000, 768000};
  const size_t FAMILY_48_COUNT =
      sizeof(FAMILY_48_RATES) / sizeof(FAMILY_48_RATES[0]);

  const int FAMILY_44_RATES[] = {44100, 88200, 176400, 352800, 705600};
  const size_t FAMILY_44_COUNT =
      sizeof(FAMILY_44_RATES) / sizeof(FAMILY_44_RATES[0]);

  static const char* FAMILY_NAMES[2] = {"48-kHz family", "44.1-kHz family"};

  const int REMAINING_RATES[] = {24000, 12000, 6000, 22050, 11025,
                                 5512,  16000, 8000, 32000, 64000};
  const size_t REMAINING_RATES_COUNT =
      sizeof(REMAINING_RATES) / sizeof(REMAINING_RATES[0]);

  const wasapi_sample_format_t EXCLUSIVE_SAMPLE_FORMATS[] = {
      WASAPI_SAMPLE_FORMAT_S16, WASAPI_SAMPLE_FORMAT_S24,
      WASAPI_SAMPLE_FORMAT_S32, WASAPI_SAMPLE_FORMAT_F32};
  const size_t EXCLUSIVE_SAMPLE_FORMATS_COUNT =
      sizeof(EXCLUSIVE_SAMPLE_FORMATS) / sizeof(EXCLUSIVE_SAMPLE_FORMATS[0]);

  const size_t MAX_EXCLUSIVE_CHANNELS = 32;
  logger_debug(
      &g_wasapi_logger,
      "WASAPI capability probe: starting exclusive-mode scan with channel "
      "ceiling %zu.",
      MAX_EXCLUSIVE_CHANNELS);

  const int* families[2] = {FAMILY_48_RATES, FAMILY_44_RATES};
  const size_t family_counts[2] = {FAMILY_48_COUNT, FAMILY_44_COUNT};

  int channel_limit = 0;
  bool hit[2] = {false, false};
  bool active[2] = {true, true};
  wasapi_sample_format_t learned_formats[4];
  size_t learned_formats_count = 0;

  uint32_t cached_masks[MAX_CAP_CHANNELS] = {0};
  bool has_cached_masks[MAX_CAP_CHANNELS] = {false};

  size_t max_family_len =
      FAMILY_48_COUNT > FAMILY_44_COUNT ? FAMILY_48_COUNT : FAMILY_44_COUNT;

  // Interleaved upward scan
  for (size_t i = 0; i < max_family_len; i++) {
    if (!active[0] && !active[1]) {
      logger_debug(&g_wasapi_logger,
                   "WASAPI capability probe: stopping upward family scan early "
                   "because all families are inactive.");
      break;
    }
    for (size_t f = 0; f < 2; f++) {
      if (!active[f]) continue;
      if (i < family_counts[f]) {
        int rate = families[f][i];
        int limit =
            channel_limit > 0 ? channel_limit : (int)MAX_EXCLUSIVE_CHANNELS;

        logger_trace(&g_wasapi_logger,
                     "WASAPI capability probe: probing %s rate %d Hz with "
                     "channel limit %d.",
                     FAMILY_NAMES[f], rate, limit);

        int channels_to_probe[32];
        for (int ch = 1; ch <= limit; ch++) {
          channels_to_probe[ch - 1] = ch;
        }

        const wasapi_sample_format_t* candidate_formats =
            learned_formats_count > 0 ? learned_formats
                                      : EXCLUSIVE_SAMPLE_FORMATS;
        size_t candidate_count = learned_formats_count > 0
                                     ? learned_formats_count
                                     : EXCLUSIVE_SAMPLE_FORMATS_COUNT;

        rate_probe_result_t result = probe_and_store_rate_with_candidates(
            &exclusive_map, client, rate, channels_to_probe, (size_t)limit,
            candidate_formats, candidate_count, cached_masks, has_cached_masks);

        if (result.max_supported_channels > 0) {
          hit[f] = true;
          int old_limit = channel_limit;
          if (result.max_supported_channels > channel_limit) {
            channel_limit = result.max_supported_channels;
          }
          logger_debug(
              &g_wasapi_logger,
              "WASAPI capability probe: %s rate %d Hz succeeded with max %d "
              "channels; channel limit changed from %d to %d.",
              FAMILY_NAMES[f], rate, result.max_supported_channels, old_limit,
              channel_limit);
          if (learned_formats_count == 0) {
            char lf_str[128];
            format_labels_to_str(result.supported_formats,
                                 result.supported_formats_count, lf_str,
                                 sizeof(lf_str));
            logger_debug(
                &g_wasapi_logger,
                "WASAPI capability probe: learned supported sample formats %s "
                "from the first successful rate %d; reusing them for "
                "subsequent probes.",
                lf_str, rate);
            for (size_t lf = 0; lf < result.supported_formats_count; lf++) {
              learned_formats[lf] = result.supported_formats[lf];
            }
            learned_formats_count = result.supported_formats_count;
          }
        } else if (hit[f]) {
          active[f] = false;
          logger_debug(&g_wasapi_logger,
                       "WASAPI capability probe: stopping %s after miss at %d "
                       "Hz following earlier hits.",
                       FAMILY_NAMES[f], rate);
        } else {
          logger_trace(&g_wasapi_logger,
                       "WASAPI capability probe: %s rate %d Hz had no hits; "
                       "keeping family active until the first success is "
                       "found.",
                       FAMILY_NAMES[f], rate);
        }
      }
    }
  }

  // Probe remaining rates
  int remaining_channel_counts[32];
  size_t remaining_channel_counts_len = 0;
  for (size_t c = 0; c < exclusive_map.channels_count; c++) {
    remaining_channel_counts[remaining_channel_counts_len++] =
        exclusive_map.channels[c].channels;
  }
  qsort(remaining_channel_counts, remaining_channel_counts_len, sizeof(int),
        compare_int);

  if (remaining_channel_counts_len == 0) {
    logger_debug(
        &g_wasapi_logger,
        "WASAPI capability probe: probing remaining low-rate set with full "
        "channel range because no channel counts were discovered in the "
        "upward scan.");
    for (int ch = 1; ch <= (int)MAX_EXCLUSIVE_CHANNELS; ch++) {
      remaining_channel_counts[ch - 1] = ch;
    }
    remaining_channel_counts_len = MAX_EXCLUSIVE_CHANNELS;
  } else {
    char ch_str[256];
    int_list_to_str(remaining_channel_counts, remaining_channel_counts_len,
                    ch_str, sizeof(ch_str));
    logger_debug(
        &g_wasapi_logger,
        "WASAPI capability probe: probing remaining low-rate set using "
        "previously discovered channel counts %s.",
        ch_str);
  }

  for (size_t r = 0; r < REMAINING_RATES_COUNT; r++) {
    int rate = REMAINING_RATES[r];
    logger_trace(&g_wasapi_logger,
                 "WASAPI capability probe: probing remaining rate %d Hz.",
                 rate);
    const wasapi_sample_format_t* candidate_formats =
        learned_formats_count > 0 ? learned_formats : EXCLUSIVE_SAMPLE_FORMATS;
    size_t candidate_count = learned_formats_count > 0
                                 ? learned_formats_count
                                 : EXCLUSIVE_SAMPLE_FORMATS_COUNT;

    rate_probe_result_t result = probe_and_store_rate_with_candidates(
        &exclusive_map, client, rate, remaining_channel_counts,
        remaining_channel_counts_len, candidate_formats, candidate_count,
        cached_masks, has_cached_masks);

    if (learned_formats_count == 0 && result.max_supported_channels > 0) {
      char lf_str[128];
      format_labels_to_str(result.supported_formats,
                           result.supported_formats_count, lf_str,
                           sizeof(lf_str));
      logger_debug(
          &g_wasapi_logger,
          "WASAPI capability probe: learned supported sample formats %s "
          "from the first successful rate %d; reusing them for subsequent "
          "probes.",
          lf_str, rate);
      for (size_t lf = 0; lf < result.supported_formats_count; lf++) {
        learned_formats[lf] = result.supported_formats[lf];
      }
      learned_formats_count = result.supported_formats_count;
    }
  }

  if (exclusive_map.channels_count > 0) {
    logger_debug(
        &g_wasapi_logger,
        "WASAPI capability probe: exclusive-mode scan found %zu channel "
        "capability entries.",
        exclusive_map.channels_count);
  } else {
    logger_debug(&g_wasapi_logger,
                 "WASAPI capability probe: exclusive-mode scan found no "
                 "supported combinations.");
  }
  logger_debug(
      &g_wasapi_logger,
      "WASAPI capability probe: completed capability scan for device \"%s\".",
      desc->name);

  // Build capability sets
  size_t total_sets = 0;
  if (has_shared) total_sets++;
  if (exclusive_map.channels_count > 0) total_sets++;

  if (total_sets == 0) {
    if (mix_wfx) CoTaskMemFree(mix_wfx);
    free_audio_device_descriptor(desc);
    desc = NULL;
    goto error_cleanup;
  }

  desc->capability_sets_count = total_sets;
  desc->capability_sets = (device_capability_set_t*)calloc(
      total_sets, sizeof(device_capability_set_t));
  if (!desc->capability_sets) {
    if (mix_wfx) CoTaskMemFree(mix_wfx);
    free_audio_device_descriptor(desc);
    desc = NULL;
    goto error_cleanup;
  }

  size_t current_set = 0;
  if (has_shared) {
    device_capability_set_t* shared_set = &desc->capability_sets[current_set++];
    snprintf(shared_set->mode, sizeof(shared_set->mode), "Shared");
    shared_set->capabilities_count = 1;
    shared_set->capabilities =
        (channel_capability_t*)calloc(1, sizeof(channel_capability_t));
    if (shared_set->capabilities) {
      shared_set->capabilities[0].channels = (int)mix_wfx->nChannels;
      shared_set->capabilities[0].samplerates_count = 1;
      shared_set->capabilities[0].samplerates =
          (samplerate_capability_t*)calloc(1, sizeof(samplerate_capability_t));
      if (shared_set->capabilities[0].samplerates) {
        shared_set->capabilities[0].samplerates[0].samplerate =
            (int)mix_wfx->nSamplesPerSec;
        shared_set->capabilities[0].samplerates[0].formats_count = 1;
        shared_set->capabilities[0].samplerates[0].formats =
            (char**)calloc(1, sizeof(char*));
        if (shared_set->capabilities[0].samplerates[0].formats) {
          shared_set->capabilities[0].samplerates[0].formats[0] = strdup("F32");
        }
      }
    }
    CoTaskMemFree(mix_wfx);
    mix_wfx = NULL;
  }

  if (exclusive_map.channels_count > 0) {
    device_capability_set_t* excl_set = &desc->capability_sets[current_set++];
    snprintf(excl_set->mode, sizeof(excl_set->mode), "Exclusive");
    excl_set->capabilities_count = exclusive_map.channels_count;
    excl_set->capabilities = (channel_capability_t*)calloc(
        exclusive_map.channels_count, sizeof(channel_capability_t));
    if (excl_set->capabilities) {
      for (size_t c = 0; c < exclusive_map.channels_count; c++) {
        temp_channel_cap_t* t_chan = &exclusive_map.channels[c];
        channel_capability_t* d_chan = &excl_set->capabilities[c];
        d_chan->channels = t_chan->channels;
        d_chan->samplerates_count = t_chan->samplerates_count;
        d_chan->samplerates = (samplerate_capability_t*)calloc(
            t_chan->samplerates_count, sizeof(samplerate_capability_t));
        if (d_chan->samplerates) {
          for (size_t r = 0; r < t_chan->samplerates_count; r++) {
            temp_samplerate_cap_t* t_rate = &t_chan->samplerates[r];
            samplerate_capability_t* d_rate = &d_chan->samplerates[r];
            d_rate->samplerate = t_rate->samplerate;
            d_rate->formats_count = t_rate->formats_count;
            d_rate->formats =
                (char**)calloc(t_rate->formats_count, sizeof(char*));
            if (d_rate->formats) {
              for (size_t f = 0; f < t_rate->formats_count; f++) {
                d_rate->formats[f] = t_rate->formats[f];
                t_rate->formats[f] = NULL;
              }
            }
          }
          qsort(d_chan->samplerates, d_chan->samplerates_count,
                sizeof(samplerate_capability_t), compare_samplerates);
        }
      }
      qsort(excl_set->capabilities, excl_set->capabilities_count,
            sizeof(channel_capability_t), compare_channels);
    }
  }

  free_temp_capabilities_map(&exclusive_map);
  SAFE_RELEASE(client);
  SAFE_RELEASE(device);
  SAFE_RELEASE(enumerator);
  if (com_initialized) {
    CoUninitialize();
  }
  return desc;

error_cleanup:
  free_temp_capabilities_map(&exclusive_map);
  if (client) {
    SAFE_RELEASE(client);
  }
  if (device) {
    SAFE_RELEASE(device);
  }
  if (enumerator) {
    SAFE_RELEASE(enumerator);
  }
  if (com_initialized) {
    CoUninitialize();
  }
  return NULL;
}

#endif  // ENABLE_WASAPI
