#include "Backend/asio_capabilities.h"

#if defined(ENABLE_ASIO)

#define WIN32_LEAN_AND_MEAN

#include <initguid.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unknwn.h>
#include <windows.h>

#include "Backend/asio_backend.h"

// COM Release helper
#define SAFE_RELEASE(punk)         \
  if ((punk) != NULL) {            \
    (punk)->lpVtbl->Release(punk); \
    (punk) = NULL;                 \
  }

// ASIO type definitions
typedef int32_t ASIOBool;
#define ASIOFalse 0
#define ASIOTrue 1

typedef double ASIOSampleRate;
typedef long ASIOError;

typedef enum {
  ASIOSTInt16MSB = 0,
  ASIOSTInt24MSB = 1,
  ASIOSTInt32MSB = 2,
  ASIOSTFloat32MSB = 3,
  ASIOSTFloat64MSB = 4,
  ASIOSTInt32MSB16 = 8,
  ASIOSTInt32MSB18 = 9,
  ASIOSTInt32MSB20 = 10,
  ASIOSTInt32MSB24 = 11,
  ASIOSTInt16LSB = 16,
  ASIOSTInt24LSB = 17,
  ASIOSTInt32LSB = 18,
  ASIOSTFloat32LSB = 19,
  ASIOSTFloat64LSB = 20,
  ASIOSTInt32LSB16 = 24,
  ASIOSTInt32LSB18 = 25,
  ASIOSTInt32LSB20 = 26,
  ASIOSTInt32LSB24 = 27,
  ASIOTSDSDInt8LSB = 32,
  ASIOTSDSDInt8MSB = 33,
  ASIOTSDSDInt8NER8 = 40,
} ASIOSampleType;

typedef struct {
  int32_t channel;
  ASIOBool isInput;
  ASIOBool isActive;
  int32_t channelGroup;
  int32_t type;
  char name[32];
} ASIOChannelInfo;

typedef struct {
  long asioVersion;
  long driverVersion;
  char name[32];
  char errorMessage[124];
  void* sysRef;
} ASIODriverInfo;

// Forward declaration of COM interface
typedef struct IASIO IASIO;
typedef struct IASIOVtbl {
  HRESULT(STDMETHODCALLTYPE* QueryInterface)(IASIO* This, REFIID riid,
                                             void** ppvObject);
  ULONG(STDMETHODCALLTYPE* AddRef)(IASIO* This);
  ULONG(STDMETHODCALLTYPE* Release)(IASIO* This);
  ASIOBool(STDMETHODCALLTYPE* init)(IASIO* This, void* sysHandle);
  void(STDMETHODCALLTYPE* getDriverName)(IASIO* This, char* name);
  long(STDMETHODCALLTYPE* getDriverVersion)(IASIO* This);
  void(STDMETHODCALLTYPE* getErrorMessage)(IASIO* This, char* string);
  ASIOError(STDMETHODCALLTYPE* start)(IASIO* This);
  ASIOError(STDMETHODCALLTYPE* stop)(IASIO* This);
  ASIOError(STDMETHODCALLTYPE* getChannels)(IASIO* This, long* numInputChannels,
                                            long* numOutputChannels);
  ASIOError(STDMETHODCALLTYPE* getLatencies)(IASIO* This, long* inputLatency,
                                             long* outputLatency);
  ASIOError(STDMETHODCALLTYPE* getBufferSize)(IASIO* This, long* minSize,
                                              long* maxSize,
                                              long* preferredSize,
                                              long* granularity);
  ASIOError(STDMETHODCALLTYPE* canSampleRate)(IASIO* This, double sampleRate);
  ASIOError(STDMETHODCALLTYPE* getSampleRate)(IASIO* This, double* sampleRate);
  ASIOError(STDMETHODCALLTYPE* setSampleRate)(IASIO* This, double sampleRate);
  ASIOError(STDMETHODCALLTYPE* getClockSources)(IASIO* This, void* clocks,
                                                long* numSources);
  ASIOError(STDMETHODCALLTYPE* setClockSource)(IASIO* This, long reference);
  ASIOError(STDMETHODCALLTYPE* getSamplePosition)(IASIO* This, int64_t* sPos,
                                                  int64_t* tStamp);
  ASIOError(STDMETHODCALLTYPE* getChannelInfo)(IASIO* This, void* info);
  ASIOError(STDMETHODCALLTYPE* createBuffers)(IASIO* This, void* bufferInfos,
                                              long numChannels, long bufferSize,
                                              void* callbacks);
  ASIOError(STDMETHODCALLTYPE* disposeBuffers)(IASIO* This);
  ASIOError(STDMETHODCALLTYPE* controlPanel)(IASIO* This);
  ASIOError(STDMETHODCALLTYPE* future)(IASIO* This, long selector, void* opt);
  ASIOError(STDMETHODCALLTYPE* outputReady)(IASIO* This);
} IASIOVtbl;

struct IASIO {
  const IASIOVtbl* lpVtbl;
};

static const GUID g_IID_IASIO_VAL = {
    0x9333b620,
    0x1f0b,
    0x11d2,
    {0x98, 0xbc, 0x00, 0x00, 0xf8, 0x75, 0xac, 0x12}};

#define STANDARD_RATES_COUNT 17
static const uint32_t STANDARD_RATES[STANDARD_RATES_COUNT] = {
    5512,  8000,  11025,  16000,  22050,  32000,  44100,  48000, 64000,
    88200, 96000, 176400, 192000, 352800, 384000, 705600, 768000};

static bool asio_check_drv_path(const char* clsid_str, char* out_dll_path,
                                size_t max_path) {
  char clsid_key[384];
  snprintf(clsid_key, sizeof(clsid_key), "clsid\\%s\\InprocServer32",
           clsid_str);
  HKEY hkpath;
  if (RegOpenKeyExA(HKEY_CLASSES_ROOT, clsid_key, 0, KEY_READ, &hkpath) ==
      ERROR_SUCCESS) {
    DWORD datatype = REG_SZ;
    DWORD datasize = (DWORD)max_path;
    LONG cr = RegQueryValueExA(hkpath, NULL, 0, &datatype, (LPBYTE)out_dll_path,
                               &datasize);
    RegCloseKey(hkpath);
    if (cr == ERROR_SUCCESS) {
      if (GetFileAttributesA(out_dll_path) != INVALID_FILE_ATTRIBUTES) {
        return true;
      }
    }
  }
  return false;
}

static bool find_asio_driver_caps_clsid(const char* driver_name,
                                        CLSID* out_clsid) {
  HKEY hk;
  if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\ASIO", 0, KEY_READ, &hk) !=
      ERROR_SUCCESS) {
    return false;
  }

  char subkey_name[256];
  DWORD index = 0;
  bool found = false;

  while (RegEnumKeyA(hk, index++, subkey_name, sizeof(subkey_name)) ==
         ERROR_SUCCESS) {
    HKEY hk_driver;
    if (RegOpenKeyExA(hk, subkey_name, 0, KEY_READ, &hk_driver) ==
        ERROR_SUCCESS) {
      char clsid_str[128];
      DWORD size = sizeof(clsid_str);
      if (RegQueryValueExA(hk_driver, "CLSID", NULL, NULL, (LPBYTE)clsid_str,
                           &size) == ERROR_SUCCESS) {
        char dllpath[MAX_PATH];
        if (asio_check_drv_path(clsid_str, dllpath, sizeof(dllpath))) {
          char drv_name[256];
          DWORD desc_size = sizeof(drv_name);
          if (RegQueryValueExA(hk_driver, "description", NULL, NULL,
                               (LPBYTE)drv_name, &desc_size) != ERROR_SUCCESS ||
              drv_name[0] == '\0') {
            snprintf(drv_name, sizeof(drv_name), "%s", subkey_name);
          }

          if (!driver_name || driver_name[0] == '\0' ||
              strcasecmp(driver_name, "default") == 0 ||
              strcasecmp(drv_name, driver_name) == 0 ||
              strcasecmp(subkey_name, driver_name) == 0) {
            wchar_t wclsid_str[128];
            mbstowcs(wclsid_str, clsid_str, 128);
            if (SUCCEEDED(CLSIDFromString(wclsid_str, out_clsid))) {
              found = true;
            }
          }
        }
      }
      RegCloseKey(hk_driver);
    }
    if (found) break;
  }
  RegCloseKey(hk);
  return found;
}

static const char* asio_sample_type_to_format_str(int type_id) {
  switch (type_id) {
    case ASIOSTInt16LSB:
      return "S16_LE";
    case ASIOSTInt24LSB:
      return "S24_3_LE";
    case ASIOSTInt32LSB:
    case ASIOSTInt32LSB16:
    case ASIOSTInt32LSB18:
    case ASIOSTInt32LSB20:
      return "S32_LE";
    case ASIOSTInt32LSB24:
      return "S24_4_LE";
    case ASIOSTFloat32LSB:
      return "F32_LE";
    case ASIOSTFloat64LSB:
      return "F64_LE";
    default:
      return NULL;
  }
}

/**
 * @brief list_device_names matching CamillaDSP device.rs:list_device_names
 * (lines 1195-1211).
 */
int asio_capabilities_available_device_names(bool is_capture,
                                             char out_names[][256],
                                             int max_names) {
  (void)is_capture;
  HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  bool com_initialized = SUCCEEDED(hr);

  HKEY hk;
  if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\ASIO", 0, KEY_READ, &hk) !=
      ERROR_SUCCESS) {
    if (com_initialized) CoUninitialize();
    return 0;
  }

  char subkey_name[256];
  DWORD index = 0;
  int matched = 0;

  while (RegEnumKeyA(hk, index++, subkey_name, sizeof(subkey_name)) ==
             ERROR_SUCCESS &&
         matched < max_names) {
    HKEY hk_driver;
    if (RegOpenKeyExA(hk, subkey_name, 0, KEY_READ, &hk_driver) ==
        ERROR_SUCCESS) {
      char clsid_str[128];
      DWORD size = sizeof(clsid_str);
      if (RegQueryValueExA(hk_driver, "CLSID", NULL, NULL, (LPBYTE)clsid_str,
                           &size) == ERROR_SUCCESS) {
        char dllpath[MAX_PATH];
        if (asio_check_drv_path(clsid_str, dllpath, sizeof(dllpath))) {
          char drv_name[256];
          DWORD desc_size = sizeof(drv_name);
          if (RegQueryValueExA(hk_driver, "description", NULL, NULL,
                               (LPBYTE)drv_name, &desc_size) != ERROR_SUCCESS ||
              drv_name[0] == '\0') {
            snprintf(drv_name, sizeof(drv_name), "%s", subkey_name);
          }
          snprintf(out_names[matched++], 256, "%s", drv_name);
        }
      }
      RegCloseKey(hk_driver);
    }
  }
  RegCloseKey(hk);
  if (com_initialized) CoUninitialize();
  return matched;
}

bool asio_capabilities_default_device_name(bool is_capture, char* out_name,
                                           size_t max_len) {
  (void)is_capture;
  char names[1][256];
  int count = asio_capabilities_available_device_names(is_capture, names, 1);
  if (count > 0) {
    snprintf(out_name, max_len, "%s", names[0]);
    return true;
  }
  out_name[0] = '\0';
  return false;
}

/**
 * @brief get_device_capabilities matching CamillaDSP
 * device.rs:get_device_capabilities (lines 1219-1315).
 */
audio_device_descriptor_t* asio_capabilities_describe(const char* device_name,
                                                      bool is_capture,
                                                      device_error_t* err) {
  // Refuse to probe if an in-process ASIO driver is already loaded (live
  // stream). Matches CamillaDSP device.rs lines 1223-1230.
  if (asio_driver_is_initialized()) {
    if (err) {
      char msg[256];
      snprintf(msg, sizeof(msg),
               "ASIO driver is already in use; cannot probe '%s' while a "
               "stream is active",
               device_name ? device_name : "default");
      device_error_init(err, DEVICE_ERROR_BUSY, msg);
    }
    return NULL;
  }

  char target_dev_name[256] = {0};
  if (device_name && device_name[0] != '\0') {
    snprintf(target_dev_name, sizeof(target_dev_name), "%s", device_name);
  } else {
    if (!asio_capabilities_default_device_name(is_capture, target_dev_name,
                                               sizeof(target_dev_name))) {
      if (err) {
        device_error_init(err, DEVICE_ERROR_NOT_FOUND,
                          "No ASIO driver available");
      }
      return NULL;
    }
  }

  // Check if device name exists in list_device_names (lines 1232-1235)
  char available_names[64][256];
  int available_count =
      asio_capabilities_available_device_names(is_capture, available_names, 64);
  bool found_name = false;
  for (int i = 0; i < available_count; i++) {
    if (strcasecmp(available_names[i], target_dev_name) == 0) {
      found_name = true;
      break;
    }
  }
  if (!found_name && strcasecmp(target_dev_name, "default") != 0) {
    if (err) {
      device_error_init(err, DEVICE_ERROR_NOT_FOUND, target_dev_name);
    }
    return NULL;
  }

  HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  bool com_initialized = SUCCEEDED(hr);
  audio_device_descriptor_t* desc = NULL;
  IASIO* iasio = NULL;

  CLSID clsid;
  if (!find_asio_driver_caps_clsid(target_dev_name, &clsid)) {
    if (err) {
      device_error_init(err, DEVICE_ERROR_NOT_FOUND, target_dev_name);
    }
    goto error_cleanup;
  }

  hr = CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER, &clsid,
                        (void**)&iasio);
  if (FAILED(hr)) {
    hr = CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER, &g_IID_IASIO_VAL,
                          (void**)&iasio);
  }
  if (FAILED(hr)) {
    hr = CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER, &IID_IUnknown,
                          (void**)&iasio);
  }
  if (FAILED(hr) || !iasio) {
    if (err) {
      char msg[256];
      snprintf(msg, sizeof(msg), "Failed to load ASIO driver '%s'",
               target_dev_name);
      device_error_init(err, DEVICE_ERROR_OTHER, msg);
    }
    goto error_cleanup;
  }

  if (!iasio->lpVtbl->init(iasio, GetDesktopWindow())) {
    if (err) {
      char msg[256];
      snprintf(msg, sizeof(msg), "ASIOInit failed for driver '%s'",
               target_dev_name);
      device_error_init(err, DEVICE_ERROR_OTHER, msg);
    }
    SAFE_RELEASE(iasio);
    goto error_cleanup;
  }

  // Supported rates probe (lines 1242-1247)
  size_t supported_rates_indices[STANDARD_RATES_COUNT];
  size_t supported_rates_count = 0;
  for (size_t r = 0; r < STANDARD_RATES_COUNT; r++) {
    if (iasio->lpVtbl->canSampleRate(iasio, (double)STANDARD_RATES[r]) == 0) {
      supported_rates_indices[supported_rates_count++] = r;
    }
  }

  // Probe native sample format (lines 1249-1260)
  ASIOChannelInfo chan_info = {0};
  chan_info.channel = 0;
  chan_info.isInput = is_capture ? ASIOTrue : ASIOFalse;
  if (iasio->lpVtbl->getChannelInfo(iasio, &chan_info) != 0) {
    chan_info.isInput = is_capture ? ASIOFalse : ASIOTrue;
    chan_info.channel = 0;
    iasio->lpVtbl->getChannelInfo(iasio, &chan_info);
  }

  const char* fmt_str = asio_sample_type_to_format_str(chan_info.type);
  if (!fmt_str) {
    if (err) {
      const char* direction_name = is_capture ? "capture" : "playback";
      char msg[256];
      snprintf(msg, sizeof(msg),
               "Failed to detect %s sample format for ASIO device '%s'",
               direction_name, target_dev_name);
      device_error_init(err, DEVICE_ERROR_OTHER, msg);
    }
    SAFE_RELEASE(iasio);
    goto error_cleanup;
  }

  // Get channel count (lines 1262-1274)
  long num_inputs = 0, num_outputs = 0;
  if (iasio->lpVtbl->getChannels(iasio, &num_inputs, &num_outputs) != 0) {
    if (err) {
      char msg[256];
      snprintf(msg, sizeof(msg), "ASIOGetChannels failed for '%s'",
               target_dev_name);
      device_error_init(err, DEVICE_ERROR_OTHER, msg);
    }
    SAFE_RELEASE(iasio);
    goto error_cleanup;
  }

  SAFE_RELEASE(iasio);

  long target_channels = is_capture ? num_inputs : num_outputs;

  desc =
      (audio_device_descriptor_t*)calloc(1, sizeof(audio_device_descriptor_t));
  if (!desc) {
    if (err) device_error_init(err, DEVICE_ERROR_OTHER, "Out of memory");
    goto error_cleanup;
  }
  snprintf(desc->name, sizeof(desc->name), "%s", target_dev_name);

  // Filter 0 channels or empty supported rates (lines 1283-1292)
  if (target_channels <= 0 || supported_rates_count == 0) {
    desc->capability_sets_count = 0;
    desc->capability_sets = NULL;
    if (com_initialized) CoUninitialize();
    return desc;
  }

  desc->capability_sets_count = 1;
  desc->capability_sets =
      (device_capability_set_t*)calloc(1, sizeof(device_capability_set_t));
  if (!desc->capability_sets) {
    goto error_cleanup;
  }

  device_capability_set_t* set = &desc->capability_sets[0];
  snprintf(set->mode, sizeof(set->mode), "Unified");
  set->capabilities_count = 1;
  set->capabilities =
      (channel_capability_t*)calloc(1, sizeof(channel_capability_t));
  if (!set->capabilities) {
    goto error_cleanup;
  }

  channel_capability_t* cap = &set->capabilities[0];
  cap->channels = (int)target_channels;
  cap->samplerates_count = supported_rates_count;
  cap->samplerates = (samplerate_capability_t*)calloc(
      supported_rates_count, sizeof(samplerate_capability_t));
  if (!cap->samplerates) {
    goto error_cleanup;
  }

  for (size_t i = 0; i < supported_rates_count; i++) {
    samplerate_capability_t* rate_cap = &cap->samplerates[i];
    rate_cap->samplerate = (int)STANDARD_RATES[supported_rates_indices[i]];
    rate_cap->formats_count = 1;
    rate_cap->formats = (char**)calloc(1, sizeof(char*));
    if (rate_cap->formats) {
      rate_cap->formats[0] = strdup(fmt_str);
    }
  }

  if (com_initialized) CoUninitialize();
  return desc;

error_cleanup:
  if (desc) {
    free_audio_device_descriptor(desc);
    desc = NULL;
  }
  if (com_initialized) CoUninitialize();
  return NULL;
}

#endif  // ENABLE_ASIO
