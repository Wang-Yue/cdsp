#include "Backend/asio_backend.h"

/**
 * @file asio_backend.c
 * @brief ASIO capture and playback backend matching upstream CamillaDSP
 * src/asio_backend/ (driver registry, independent multi-device support,
 * improved rate switching without dummy stream cycle, fixed message selectors,
 * capture stream active gating, latency logging, and native DSD support).
 */

#if defined(ENABLE_ASIO)

#define WIN32_LEAN_AND_MEAN

#include <initguid.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unknwn.h>
#include <windows.h>

#include "Audio/sample_conversion.h"
#include "Engine/cdsp_sem.h"
#include "Logging/app_logger.h"
#include "Utils/cdsp_time.h"
#include "Utils/lock_free_ring_buffer.h"

static const logger_t g_logger = {"dsp.backend.asio"};

static const GUID g_IID_IASIO_VAL = {
    0x9333b620,
    0x1f0b,
    0x11d2,
    {0x98, 0xbc, 0x00, 0x00, 0xf8, 0x75, 0xac, 0x12}};

// MARK: - Driver Registry matching CamillaDSP src/asio_backend/driver.rs

typedef struct asio_driver_entry {
  char devname[256];
  IASIO* iasio;
  struct asio_driver_entry* next;
} asio_driver_entry_t;

static struct {
  SRWLOCK lock;
  asio_driver_entry_t* head;
} g_driver_registry = {.lock = SRWLOCK_INIT, .head = NULL};

/**
 * @brief Initialise COM on the calling thread as a Single-Threaded Apartment.
 * Matches driver.rs:com_init_this_thread.
 */
void asio_com_init_this_thread(void) {
  HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  logger_trace(&g_logger, "CoInitializeEx returned 0x%08lX", (unsigned long)hr);
}

/**
 * @brief Look up a loaded driver by device name in the registry.
 */
IASIO* asio_driver_lookup(const char* devname) {
  if (!devname) return NULL;
  AcquireSRWLockShared(&g_driver_registry.lock);
  asio_driver_entry_t* curr = g_driver_registry.head;
  IASIO* result = NULL;
  while (curr) {
    if (strcasecmp(curr->devname, devname) == 0) {
      result = curr->iasio;
      break;
    }
    curr = curr->next;
  }
  ReleaseSRWLockShared(&g_driver_registry.lock);
  return result;
}

/**
 * @brief Whether a driver is currently loaded for `devname`.
 * Matches driver.rs:driver_is_loaded.
 */
bool asio_driver_is_loaded(const char* devname) {
  return asio_driver_lookup(devname) != NULL;
}

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
      char expanded[MAX_PATH];
      if (ExpandEnvironmentStringsA(out_dll_path, expanded, MAX_PATH) > 0) {
        if (GetFileAttributesA(expanded) != INVALID_FILE_ATTRIBUTES) {
          strncpy(out_dll_path, expanded, max_path - 1);
          out_dll_path[max_path - 1] = '\0';
          return true;
        }
      } else if (GetFileAttributesA(out_dll_path) != INVALID_FILE_ATTRIBUTES) {
        return true;
      }
    }
  }
  return false;
}

static bool find_asio_driver_clsid(const char* driver_name, CLSID* out_clsid) {
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

static HRESULT create_asio_com_instance(const CLSID* clsid, IASIO** out_iasio) {
  HRESULT hr = CoCreateInstance(clsid, NULL, CLSCTX_INPROC_SERVER, clsid,
                                (void**)out_iasio);
  if (FAILED(hr)) {
    hr = CoCreateInstance(clsid, NULL, CLSCTX_INPROC_SERVER, &g_IID_IASIO_VAL,
                          (void**)out_iasio);
  }
  if (FAILED(hr)) {
    hr = CoCreateInstance(clsid, NULL, CLSCTX_INPROC_SERVER, &IID_IUnknown,
                          (void**)out_iasio);
  }
  return hr;
}

/**
 * @brief List the names of all registered ASIO drivers.
 * Matches driver.rs:list_device_names.
 */
int asio_list_device_names(char out_names[][256], int max_names) {
  HKEY hk;
  if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\ASIO", 0, KEY_READ, &hk) !=
      ERROR_SUCCESS) {
    return 0;
  }

  char subkey_name[256];
  DWORD index = 0;
  int count = 0;

  while (RegEnumKeyA(hk, index++, subkey_name, sizeof(subkey_name)) ==
             ERROR_SUCCESS &&
         count < max_names) {
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
          snprintf(out_names[count++], 256, "%s", drv_name);
        }
      }
      RegCloseKey(hk_driver);
    }
  }
  RegCloseKey(hk);
  return count;
}

/**
 * @brief Release the driver loaded for `devname`, if any.
 * Matches driver.rs:teardown_asio_driver.
 */
void asio_driver_teardown(const char* devname) {
  if (!devname) return;

  IASIO* to_release = NULL;
  AcquireSRWLockExclusive(&g_driver_registry.lock);
  asio_driver_entry_t** curr = &g_driver_registry.head;
  while (*curr) {
    if (strcasecmp((*curr)->devname, devname) == 0) {
      asio_driver_entry_t* entry = *curr;
      *curr = entry->next;
      to_release = entry->iasio;
      free(entry);
      break;
    }
    curr = &(*curr)->next;
  }
  ReleaseSRWLockExclusive(&g_driver_registry.lock);

  if (to_release) {
    logger_trace(&g_logger,
                 "asio_driver_teardown: releasing the instance for '%s'",
                 devname);
    SAFE_RELEASE(to_release);
    CoUninitialize();
  } else {
    logger_trace(
        &g_logger,
        "asio_driver_teardown: no driver loaded for '%s', nothing to do",
        devname);
  }
}

/**
 * @brief Load an ASIO driver by name and initialise it.
 * Matches driver.rs:load_driver_by_name.
 */
bool asio_driver_load_by_name(const char* name, IASIO** out_iasio,
                              backend_error_t* err) {
  logger_trace(&g_logger, "asio_driver_load_by_name: loading '%s'", name);
  asio_driver_teardown(name);
  asio_com_init_this_thread();

  CLSID clsid;
  if (!find_asio_driver_clsid(name, &clsid)) {
    if (err) {
      char msg[256];
      snprintf(msg, sizeof(msg), "No ASIO driver named '%s' is registered",
               name);
      backend_error_init(err, BACKEND_ERROR_DEVICE_NOT_FOUND, msg);
    }
    return false;
  }

  IASIO* iasio = NULL;
  HRESULT hr = create_asio_com_instance(&clsid, &iasio);
  if (FAILED(hr) || !iasio) {
    if (err) {
      char msg[256];
      snprintf(msg, sizeof(msg),
               "Failed to create an instance of ASIO driver '%s': 0x%08lX",
               name, (unsigned long)hr);
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
    return false;
  }

  if (!iasio->lpVtbl->init(iasio, GetDesktopWindow())) {
    char err_msg[128] = {0};
    iasio->lpVtbl->getErrorMessage(iasio, err_msg);
    SAFE_RELEASE(iasio);
    CoUninitialize();
    if (err) {
      char msg[256];
      snprintf(msg, sizeof(msg), "Failed to initialise ASIO driver '%s': %s",
               name, err_msg[0] ? err_msg : "driver init returned false");
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
    return false;
  }

  char driver_name[32] = {0};
  iasio->lpVtbl->getDriverName(iasio, driver_name);
  long driver_version = iasio->lpVtbl->getDriverVersion(iasio);
  logger_debug(&g_logger, "Loaded ASIO driver '%s', version %ld.",
               driver_name[0] ? driver_name : name, driver_version);

  // Store in registry
  asio_driver_entry_t* entry =
      (asio_driver_entry_t*)calloc(1, sizeof(asio_driver_entry_t));
  if (entry) {
    snprintf(entry->devname, sizeof(entry->devname), "%s", name);
    entry->iasio = iasio;

    AcquireSRWLockExclusive(&g_driver_registry.lock);
    entry->next = g_driver_registry.head;
    g_driver_registry.head = entry;
    ReleaseSRWLockExclusive(&g_driver_registry.lock);
  }

  logger_trace(&g_logger,
               "asio_driver_load_by_name: '%s' loaded and initialised", name);
  if (out_iasio) {
    *out_iasio = iasio;
  }
  return true;
}

// MARK: - ASIO Utils matching CamillaDSP utils.rs

/**
 * @brief Read the currently active ASIO sample rate in Hz.
 * Matches utils.rs:read_current_asio_sample_rate_hz.
 */
static int read_current_asio_sample_rate_hz(const char* devname) {
  IASIO* iasio = asio_driver_lookup(devname);
  if (!iasio) return 0;
  double rate = 0.0;
  long res = iasio->lpVtbl->getSampleRate(iasio, &rate);
  if (res == 0 && isfinite(rate) && rate > 0.0) {
    return (int)round(rate);
  }
  return 0;
}

/**
 * @brief make_buffer_infos matching utils.rs:make_channel_ids.
 */
static ASIOBufferInfo* make_buffer_infos(size_t num_channels, bool is_input) {
  ASIOBufferInfo* infos =
      (ASIOBufferInfo*)calloc(num_channels, sizeof(ASIOBufferInfo));
  if (!infos) return NULL;
  for (size_t ch = 0; ch < num_channels; ch++) {
    infos[ch].isInput = is_input ? ASIOTrue : ASIOFalse;
    infos[ch].channelNum = (int32_t)ch;
    infos[ch].buffers[0] = NULL;
    infos[ch].buffers[1] = NULL;
  }
  return infos;
}

/**
 * @brief asio_format_to_str matching utils.rs:asio_format_to_str.
 */
const char* asio_format_to_str(asio_sample_format_t fmt) {
  switch (fmt) {
    case ASIO_SAMPLE_FORMAT_S16_LE:
      return "S16_LE";
    case ASIO_SAMPLE_FORMAT_S24_4_LE:
      return "S24_4_LE";
    case ASIO_SAMPLE_FORMAT_S24_3_LE:
      return "S24_3_LE";
    case ASIO_SAMPLE_FORMAT_S32_LE:
      return "S32_LE";
    case ASIO_SAMPLE_FORMAT_F32_LE:
      return "F32_LE";
    case ASIO_SAMPLE_FORMAT_F64_LE:
      return "F64_LE";
    case ASIO_SAMPLE_FORMAT_DSD_INT8:
      return "DSD_INT8";
    default:
      return "Unknown";
  }
}

/**
 * @brief asio_sample_type_name matching utils.rs:asio_sample_type_name.
 */
const char* asio_sample_type_name(int type_id) {
  switch (type_id) {
    case ASIO_ST_INT16_MSB:
      return "Int16 MSB (big-endian)";
    case ASIO_ST_INT24_MSB:
      return "Int24 MSB (3-byte packed, big-endian)";
    case ASIO_ST_INT32_MSB:
      return "Int32 MSB (big-endian)";
    case ASIO_ST_FLOAT32_MSB:
      return "Float32 MSB (big-endian)";
    case ASIO_ST_FLOAT64_MSB:
      return "Float64 MSB (big-endian)";
    case ASIO_ST_INT32_MSB_16:
      return "Int32 MSB 16-bit (big-endian)";
    case ASIO_ST_INT32_MSB_18:
      return "Int32 MSB 18-bit (big-endian)";
    case ASIO_ST_INT32_MSB_20:
      return "Int32 MSB 20-bit (big-endian)";
    case ASIO_ST_INT32_MSB_24:
      return "Int32 MSB 24-bit (big-endian)";
    case ASIO_ST_INT16_LSB:
      return "Int16 LSB";
    case ASIO_ST_INT24_LSB:
      return "Int24 LSB (3-byte packed)";
    case ASIO_ST_INT32_LSB:
      return "Int32 LSB";
    case ASIO_ST_FLOAT32_LSB:
      return "Float32 LSB";
    case ASIO_ST_FLOAT64_LSB:
      return "Float64 LSB";
    case ASIO_ST_INT32_LSB_16:
      return "Int32 LSB 16-bit";
    case ASIO_ST_INT32_LSB_18:
      return "Int32 LSB 18-bit";
    case ASIO_ST_INT32_LSB_20:
      return "Int32 LSB 20-bit";
    case ASIO_ST_INT32_LSB_24:
      return "Int32 LSB 24-bit";
    case ASIO_ST_DSD_INT8_LSB_1:
      return "DSD Int8 LSB 1";
    case ASIO_ST_DSD_INT8_MSB_1:
      return "DSD Int8 MSB 1";
    case ASIO_ST_DSD_INT8_NER8:
      return "DSD Int8 NER8";
    default:
      return "Unknown";
  }
}

/**
 * @brief asio_sample_type_to_format matching
 * utils.rs:asio_sample_type_to_format.
 */
asio_sample_format_t asio_sample_type_to_format(int type_id) {
  switch (type_id) {
    case ASIO_ST_INT16_LSB:
      return ASIO_SAMPLE_FORMAT_S16_LE;
    case ASIO_ST_INT24_LSB:
      return ASIO_SAMPLE_FORMAT_S24_3_LE;
    case ASIO_ST_INT32_LSB:
    case ASIO_ST_INT32_LSB_16:
    case ASIO_ST_INT32_LSB_18:
    case ASIO_ST_INT32_LSB_20:
      return ASIO_SAMPLE_FORMAT_S32_LE;
    case ASIO_ST_INT32_LSB_24:
      return ASIO_SAMPLE_FORMAT_S24_4_LE;
    case ASIO_ST_FLOAT32_LSB:
      return ASIO_SAMPLE_FORMAT_F32_LE;
    case ASIO_ST_FLOAT64_LSB:
      return ASIO_SAMPLE_FORMAT_F64_LE;
    case ASIO_ST_DSD_INT8_LSB_1:
    case ASIO_ST_DSD_INT8_MSB_1:
    case ASIO_ST_DSD_INT8_NER8:
      return ASIO_SAMPLE_FORMAT_DSD_INT8;
    default:
      return ASIO_SAMPLE_FORMAT_INVALID;
  }
}

/**
 * @brief query_device_format matching utils.rs:query_device_format.
 */
static bool query_device_format(const char* devname, bool is_input,
                                int* out_type, backend_error_t* err) {
  IASIO* iasio = asio_driver_lookup(devname);
  if (!iasio) {
    if (err) {
      char msg[256];
      snprintf(msg, sizeof(msg), "No ASIO driver is loaded for device '%s'",
               devname);
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
    return false;
  }

  ASIOChannelInfo info = {0};
  info.channel = 0;
  info.isInput = is_input ? ASIOTrue : ASIOFalse;
  long res = iasio->lpVtbl->getChannelInfo(iasio, &info);
  if (res != 0) {
    const char* direction = is_input ? "input" : "output";
    if (err) {
      char msg[256];
      snprintf(msg, sizeof(msg),
               "getChannelInfo failed for %s channel 0 (error code %ld)",
               direction, res);
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
    return false;
  }
  logger_debug(&g_logger, "ASIO channel 0 (%s): type=%d (%s)",
               is_input ? "input" : "output", info.type,
               asio_sample_type_name(info.type));
  *out_type = info.type;
  return true;
}

/**
 * @brief resolve_format matching utils.rs:resolve_format.
 */
static bool resolve_format(const char* devname, asio_sample_format_t configured,
                           bool has_configured, bool is_input,
                           asio_sample_format_t* out_format,
                           backend_error_t* err) {
  int device_type = 0;
  if (!query_device_format(devname, is_input, &device_type, err)) {
    return false;
  }
  asio_sample_format_t native_format = asio_sample_type_to_format(device_type);
  const char* direction = is_input ? "capture" : "playback";

  if (native_format == ASIO_SAMPLE_FORMAT_INVALID) {
    if (err) {
      char msg[256];
      snprintf(msg, sizeof(msg),
               "ASIO %s: device uses unsupported sample type %d (%s)",
               direction, device_type, asio_sample_type_name(device_type));
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
    return false;
  }

  if (has_configured && configured != ASIO_SAMPLE_FORMAT_INVALID) {
    if (configured != native_format) {
      if (err) {
        char msg[512];
        snprintf(
            msg, sizeof(msg),
            "ASIO %s: configured format %s does not match device native "
            "format %s (%s). ASIO drivers do not convert sample formats. "
            "Please remove the format setting to auto-detect, or set it to %s",
            direction, asio_format_to_str(configured),
            asio_format_to_str(native_format),
            asio_sample_type_name(device_type),
            asio_format_to_str(native_format));
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
      }
      return false;
    }
    logger_debug(&g_logger,
                 "ASIO %s: configured format %s matches device native format.",
                 direction, asio_format_to_str(configured));
  } else {
    logger_debug(&g_logger, "ASIO %s: auto-detected format %s from device.",
                 direction, asio_format_to_str(native_format));
  }

  *out_format = native_format;
  return true;
}

/**
 * @brief get_preferred_buffer_size matching utils.rs:get_preferred_buffer_size.
 */
static bool get_preferred_buffer_size(const char* devname, long* out_preferred,
                                      backend_error_t* err) {
  IASIO* iasio = asio_driver_lookup(devname);
  if (!iasio) {
    if (err) {
      char msg[256];
      snprintf(msg, sizeof(msg), "No ASIO driver is loaded for device '%s'",
               devname);
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
    return false;
  }

  long min_buf = 0, max_buf = 0, preferred_buf = 0, granularity = 0;
  long res = iasio->lpVtbl->getBufferSize(iasio, &min_buf, &max_buf,
                                          &preferred_buf, &granularity);
  if (res != 0) {
    if (err) {
      char msg[256];
      snprintf(msg, sizeof(msg), "getBufferSize failed with error code %ld",
               res);
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
    return false;
  }
  logger_trace(&g_logger,
               "getBufferSize: min=%ld, max=%ld, preferred=%ld, "
               "granularity=%ld",
               min_buf, max_buf, preferred_buf, granularity);
  *out_preferred = preferred_buf;
  return true;
}

/**
 * @brief create_asio_buffers matching utils.rs:create_asio_buffers.
 */
static bool create_asio_buffers(const char* devname,
                                ASIOBufferInfo* buffer_infos, long num_channels,
                                long buffer_size, ASIOCallbacks* callbacks,
                                backend_error_t* err) {
  IASIO* iasio = asio_driver_lookup(devname);
  if (!iasio) {
    if (err) {
      char msg[256];
      snprintf(msg, sizeof(msg), "No ASIO driver is loaded for device '%s'",
               devname);
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
    return false;
  }

  logger_trace(
      &g_logger,
      "Calling createBuffers: infos_ptr=%p, channels=%ld, buffer_size=%ld, "
      "callbacks_ptr=%p",
      (int64_t)(uintptr_t)buffer_infos, num_channels, buffer_size,
      (int64_t)(uintptr_t)callbacks);
  long res = iasio->lpVtbl->createBuffers(iasio, buffer_infos, num_channels,
                                          buffer_size, callbacks);
  logger_trace(&g_logger, "createBuffers returned %ld.", res);
  if (res != 0) {
    if (err) {
      char msg[256];
      snprintf(msg, sizeof(msg), "createBuffers failed with error code %ld",
               res);
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
    return false;
  }
  return true;
}

/**
 * @brief dispose_asio_buffers matching utils.rs:dispose_asio_buffers.
 */
static bool dispose_asio_buffers(const char* devname) {
  IASIO* iasio = asio_driver_lookup(devname);
  if (!iasio) return false;
  return iasio->lpVtbl->disposeBuffers(iasio) == 0;
}

/**
 * @brief start_asio_stream matching utils.rs:start_asio_stream.
 */
static bool start_asio_stream(const char* devname, backend_error_t* err) {
  IASIO* iasio = asio_driver_lookup(devname);
  if (!iasio) {
    if (err) {
      char msg[256];
      snprintf(msg, sizeof(msg), "No ASIO driver is loaded for device '%s'",
               devname);
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
    return false;
  }
  long res = iasio->lpVtbl->start(iasio);
  if (res != 0) {
    if (err) {
      char msg[256];
      snprintf(msg, sizeof(msg), "Failed to start ASIO stream: %ld", res);
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
    return false;
  }
  return true;
}

/**
 * @brief stop_asio_stream matching utils.rs:stop_asio_stream.
 */
static bool stop_asio_stream(const char* devname) {
  IASIO* iasio = asio_driver_lookup(devname);
  if (!iasio) return false;
  return iasio->lpVtbl->stop(iasio) == 0;
}

/**
 * @brief log_asio_latencies matching utils.rs:log_asio_latencies.
 */
static void log_asio_latencies(const char* devname) {
  IASIO* iasio = asio_driver_lookup(devname);
  if (!iasio) return;
  long in_lat = 0, out_lat = 0;
  long res = iasio->lpVtbl->getLatencies(iasio, &in_lat, &out_lat);
  int samplerate = read_current_asio_sample_rate_hz(devname);
  if (res == 0 && samplerate > 0) {
    double in_ms = 1000.0 * (double)in_lat / (double)samplerate;
    double out_ms = 1000.0 * (double)out_lat / (double)samplerate;
    logger_debug(&g_logger,
                 "ASIO driver reported latencies: capture %ld frames (%.1f "
                 "ms), playback %ld frames (%.1f ms).",
                 in_lat, in_ms, out_lat, out_ms);
  } else if (res == 0) {
    logger_debug(&g_logger,
                 "ASIO driver reported latencies: capture %ld frames, "
                 "playback %ld frames.",
                 in_lat, out_lat);
  }
}

// MARK: - Internal Contexts and Global Atomics matching CamillaDSP device.rs

typedef struct {
  spsc_byte_ring_buffer_t* ring_buffer;
  ASIOBufferInfo* buffer_infos;
  size_t num_channels;
  size_t buffer_size;
  size_t bytes_per_sample;
  uint8_t* read_tmp;
  uint8_t* sample_queue;
  size_t sample_queue_len;
  size_t sample_queue_cap;
  size_t target_level;
  uint8_t silence_byte;
  _Atomic double buffer_fill;
  bool running;
} asio_playback_context_t;

typedef struct {
  spsc_byte_ring_buffer_t* ring_buffer;
  cdsp_sem_t semaphore;
  ASIOBufferInfo* buffer_infos;
  size_t num_channels;
  size_t buffer_size;
  size_t bytes_per_sample;
  uint8_t* transfer_buf;
} asio_capture_context_t;

static _Atomic(asio_playback_context_t*) PLAYBACK_CONTEXT = NULL;
static _Atomic(asio_capture_context_t*) CAPTURE_CONTEXT = NULL;

/// Gates the capture callback until the capture loop is ready to consume.
/// Matches device.rs:CAPTURE_STREAM_ACTIVE.
static _Atomic bool CAPTURE_STREAM_ACTIVE = false;

static _Atomic bool ASIO_PLAYBACK_RATE_CHANGED = false;
static _Atomic bool ASIO_CAPTURE_RATE_CHANGED = false;

static void clear_playback_rate_change_event(void) {
  atomic_store_explicit(&ASIO_PLAYBACK_RATE_CHANGED, false,
                        memory_order_release);
}

static void clear_capture_rate_change_event(void) {
  atomic_store_explicit(&ASIO_CAPTURE_RATE_CHANGED, false,
                        memory_order_release);
}

static bool take_playback_rate_change_event(void) {
  return atomic_exchange_explicit(&ASIO_PLAYBACK_RATE_CHANGED, false,
                                  memory_order_acq_rel);
}

static bool take_capture_rate_change_event(void) {
  return atomic_exchange_explicit(&ASIO_CAPTURE_RATE_CHANGED, false,
                                  memory_order_acq_rel);
}

// MARK: - Startup callback gate (PLAYBACK_CALLBACK_SEEN) matching device.rs

static struct {
  SRWLOCK lock;
  CONDITION_VARIABLE cond;
  bool seen;
} g_playback_callback_seen = {
    .lock = SRWLOCK_INIT, .cond = CONDITION_VARIABLE_INIT, .seen = false};

static void reset_playback_callback_seen(void) {
  AcquireSRWLockExclusive(&g_playback_callback_seen.lock);
  g_playback_callback_seen.seen = false;
  ReleaseSRWLockExclusive(&g_playback_callback_seen.lock);
}

static void mark_playback_callback_seen(void) {
  AcquireSRWLockExclusive(&g_playback_callback_seen.lock);
  if (!g_playback_callback_seen.seen) {
    g_playback_callback_seen.seen = true;
    WakeAllConditionVariable(&g_playback_callback_seen.cond);
  }
  ReleaseSRWLockExclusive(&g_playback_callback_seen.lock);
}

static bool wait_for_playback_callback(DWORD timeout_ms) {
  AcquireSRWLockExclusive(&g_playback_callback_seen.lock);
  if (g_playback_callback_seen.seen) {
    ReleaseSRWLockExclusive(&g_playback_callback_seen.lock);
    return true;
  }
  SleepConditionVariableSRW(&g_playback_callback_seen.cond,
                            &g_playback_callback_seen.lock, timeout_ms, 0);
  bool seen = g_playback_callback_seen.seen;
  ReleaseSRWLockExclusive(&g_playback_callback_seen.lock);
  return seen;
}

// MARK: - ASIO Callbacks matching CamillaDSP device.rs

static void buffer_switch_combined(long buffer_index, ASIOBool direct_process);

static inline void ensure_sample_queue_cap(asio_playback_context_t* ctx,
                                           size_t needed_cap) {
  if (ctx->sample_queue_cap < needed_cap) {
    size_t new_cap = ctx->sample_queue_cap * 2;
    if (new_cap < needed_cap) new_cap = needed_cap;
    uint8_t* new_buf = (uint8_t*)realloc(ctx->sample_queue, new_cap);
    if (new_buf) {
      ctx->sample_queue = new_buf;
      ctx->sample_queue_cap = new_cap;
    }
  }
}

/**
 * @brief buffer_switch_playback matching CamillaDSP device.rs.
 */
static void buffer_switch_playback(long buffer_index, ASIOBool direct_process) {
  (void)direct_process;
  asio_playback_context_t* ctx =
      atomic_load_explicit(&PLAYBACK_CONTEXT, memory_order_acquire);
  if (!ctx) {
    return;
  }
  if (buffer_index < 0 || buffer_index > 1) {
    logger_debug(
        &g_logger,
        "ASIO playback callback got invalid buffer index %ld, ignoring.",
        buffer_index);
    return;
  }
  if (!ctx->buffer_infos) {
    return;
  }
  mark_playback_callback_seen();

  size_t bytes_per_frame = ctx->bytes_per_sample * ctx->num_channels;
  size_t needed_bytes = ctx->buffer_size * bytes_per_frame;

  // Fill the sample queue from the ring buffer
  while (ctx->sample_queue_len < needed_bytes) {
    size_t available =
        spsc_byte_ring_buffer_get_available_to_read(ctx->ring_buffer);
    if (available == 0) {
      // No data — fill remainder with silence
      size_t missing = needed_bytes - ctx->sample_queue_len;
      logger_warn(
          &g_logger,
          "ASIO playback callback: underrun, filled %zu bytes of silence.",
          missing);
      ensure_sample_queue_cap(ctx, needed_bytes);
      memset(ctx->sample_queue + ctx->sample_queue_len, ctx->silence_byte,
             missing);
      ctx->sample_queue_len = needed_bytes;
      if (ctx->running) {
        ctx->running = false;
      }
      break;
    }
    if (!ctx->running) {
      ctx->running = true;
      // Prefill at least one full callback's worth of frames so the loop
      // below doesn't immediately re-drain the ring buffer to empty and
      // re-trigger an underrun when target_level is smaller than the
      // driver's actual buffer size (see issue #498).
      size_t prefill_frames = (ctx->target_level > ctx->buffer_size)
                                  ? ctx->target_level
                                  : ctx->buffer_size;
      size_t prefill_bytes = prefill_frames * bytes_per_frame;
      size_t new_len = ctx->sample_queue_len + prefill_bytes;
      ensure_sample_queue_cap(ctx, new_len);
      memset(ctx->sample_queue + ctx->sample_queue_len, ctx->silence_byte,
             prefill_bytes);
      ctx->sample_queue_len = new_len;
    }
    size_t missing = (needed_bytes > ctx->sample_queue_len)
                         ? (needed_bytes - ctx->sample_queue_len)
                         : 0;
    size_t to_read = (available < missing) ? available : missing;
    if (to_read > 0) {
      size_t read_bytes = spsc_byte_ring_buffer_consume(ctx->ring_buffer,
                                                        ctx->read_tmp, to_read);
      ensure_sample_queue_cap(ctx, ctx->sample_queue_len + read_bytes);
      memcpy(ctx->sample_queue + ctx->sample_queue_len, ctx->read_tmp,
             read_bytes);
      ctx->sample_queue_len += read_bytes;
    }
  }

  // Copy interleaved data into per-channel ASIO buffers (de-interleave)
  size_t src_offset = 0;
  for (size_t frame = 0; frame < ctx->buffer_size; frame++) {
    for (size_t ch = 0; ch < ctx->num_channels; ch++) {
      void* out_ptr = ctx->buffer_infos[ch].buffers[buffer_index];
      if (out_ptr) {
        uint8_t* dst = (uint8_t*)out_ptr + frame * ctx->bytes_per_sample;
        memcpy(dst, ctx->sample_queue + src_offset, ctx->bytes_per_sample);
      } else if (frame == 0) {
        logger_trace(
            &g_logger,
            "ASIO playback callback: null output buffer pointer at channel "
            "%zu, index %ld.",
            ch, buffer_index);
      }
      src_offset += ctx->bytes_per_sample;
    }
  }
  if (needed_bytes > 0 && ctx->sample_queue_len >= needed_bytes) {
    size_t remaining = ctx->sample_queue_len - needed_bytes;
    if (remaining > 0) {
      memmove(ctx->sample_queue, ctx->sample_queue + needed_bytes, remaining);
    }
    ctx->sample_queue_len = remaining;
  }

  // Update buffer fill estimate.
  // Include both the callback-local queue and the remaining ringbuffer data
  // to represent total pending playback frames.
  size_t curr_buffer_fill =
      (ctx->sample_queue_len +
       spsc_byte_ring_buffer_get_available_to_read(ctx->ring_buffer)) /
      bytes_per_frame;
  atomic_store_explicit(&ctx->buffer_fill, (double)curr_buffer_fill,
                        memory_order_relaxed);
}

/**
 * @brief buffer_switch_capture matching CamillaDSP device.rs.
 */
static void buffer_switch_capture(long buffer_index, ASIOBool direct_process) {
  (void)direct_process;
  if (!atomic_load_explicit(&CAPTURE_STREAM_ACTIVE, memory_order_acquire)) {
    // The capture loop is not consuming yet, drop this buffer instead of
    // filling the ring buffer with audio that would only be discarded. Matches
    // CAPTURE_STREAM_ACTIVE.
    return;
  }
  asio_capture_context_t* ctx =
      atomic_load_explicit(&CAPTURE_CONTEXT, memory_order_acquire);
  if (!ctx) {
    return;
  }
  if (buffer_index < 0 || buffer_index > 1) {
    logger_debug(
        &g_logger,
        "ASIO capture callback got invalid buffer index %ld, ignoring.",
        buffer_index);
    return;
  }
  if (!ctx->buffer_infos || !ctx->transfer_buf) {
    return;
  }

  // Read from per-channel ASIO input buffers and interleave into transfer_buf
  for (size_t frame = 0; frame < ctx->buffer_size; frame++) {
    for (size_t ch = 0; ch < ctx->num_channels; ch++) {
      void* in_ptr = ctx->buffer_infos[ch].buffers[buffer_index];
      if (in_ptr) {
        const uint8_t* src =
            (const uint8_t*)in_ptr + frame * ctx->bytes_per_sample;
        size_t offset =
            (frame * ctx->num_channels + ch) * ctx->bytes_per_sample;
        memcpy(&ctx->transfer_buf[offset], src, ctx->bytes_per_sample);
      }
    }
  }

  size_t total_bytes =
      ctx->buffer_size * ctx->num_channels * ctx->bytes_per_sample;
  size_t pushed_bytes = spsc_byte_ring_buffer_write(
      ctx->ring_buffer, ctx->transfer_buf, total_bytes);
  if (pushed_bytes < total_bytes) {
    logger_warn(
        &g_logger,
        "ASIO capture callback: ringbuffer full, dropped %zu of %zu bytes.",
        total_bytes - pushed_bytes, total_bytes);
  }
  if (ctx->semaphore) {
    cdsp_sem_signal(ctx->semaphore);
  }
}

static void* buffer_switch_timeinfo_playback(void* params,
                                             long doubleBufferIndex,
                                             ASIOBool directProcess) {
  buffer_switch_playback(doubleBufferIndex, directProcess);
  return params;
}

static void* buffer_switch_timeinfo_capture(void* params,
                                            long doubleBufferIndex,
                                            ASIOBool directProcess) {
  buffer_switch_capture(doubleBufferIndex, directProcess);
  return params;
}

static void* buffer_switch_timeinfo_combined(void* params,
                                             long doubleBufferIndex,
                                             ASIOBool directProcess) {
  buffer_switch_combined(doubleBufferIndex, directProcess);
  return params;
}

static void buffer_switch_combined(long buffer_index, ASIOBool direct_process) {
  buffer_switch_playback(buffer_index, direct_process);
  buffer_switch_capture(buffer_index, direct_process);
}

/**
 * @brief sample_rate_changed_callback matching CamillaDSP device.rs.
 */
static void sample_rate_changed_callback(ASIOSampleRate s_rate) {
  (void)s_rate;
  atomic_store_explicit(&ASIO_PLAYBACK_RATE_CHANGED, true,
                        memory_order_release);
  atomic_store_explicit(&ASIO_CAPTURE_RATE_CHANGED, true, memory_order_release);
  logger_warn(&g_logger, "ASIO sampleRateDidChange callback received.");
}

/**
 * @brief asio_message_callback matching CamillaDSP device.rs / azo-sys
 * MessageSelector.
 */
static long asio_message_callback(long selector, long value, void* message,
                                  double* opt) {
  (void)message;
  (void)opt;
  switch (selector) {
    case K_ASIO_SELECTOR_SUPPORTED:
      switch (value) {
        case K_ASIO_SELECTOR_SUPPORTED:
        case K_ASIO_ENGINE_VERSION:
        case K_ASIO_RESET_REQUEST:
        case K_ASIO_RESYNC_REQUEST:
        case K_ASIO_LATENCIES_CHANGED:
        case K_ASIO_SUPPORTS_TIME_INFO:
          return 1;  // Supported
        case K_ASIO_BUFFER_SIZE_CHANGE:
        case K_ASIO_SUPPORTS_TIME_CODE:
        default:
          return 0;  // Not supported
      }
    case K_ASIO_ENGINE_VERSION:
      return 2;  // ASIO 2.0
    case K_ASIO_SUPPORTS_TIME_INFO:
      return 1;
    case K_ASIO_SUPPORTS_TIME_CODE:
      return 0;
    case K_ASIO_RESET_REQUEST:
      logger_warn(&g_logger,
                  "ASIO reset request received. A stream restart may be "
                  "required by the driver.");
      return 1;
    case K_ASIO_BUFFER_SIZE_CHANGE:
      logger_warn(&g_logger,
                  "ASIO buffer size change request received. Dynamic resize is "
                  "not implemented in this backend.");
      return 0;
    case K_ASIO_RESYNC_REQUEST:
      logger_debug(&g_logger, "ASIO resync request received.");
      return 1;
    case K_ASIO_LATENCIES_CHANGED:
      logger_debug(&g_logger, "ASIO latencies changed notification.");
      return 1;
    default:
      logger_trace(&g_logger, "Unhandled ASIO message selector %ld.", selector);
      return 0;
  }
}

// MARK: - Full-Duplex Coordination matching CamillaDSP device.rs

typedef struct {
  char driver_name[256];
  long num_inputs;
  long num_outputs;
  long preferred_buf_size;

  ASIOBufferInfo* pending_output;
  size_t pending_output_channels;

  ASIOBufferInfo* pending_input;
  size_t pending_input_channels;

  bool stream_started;
  char setup_error[256];
  uint8_t active_count;

  ASIOBufferInfo* buffer_infos_for_driver;
  ASIOCallbacks callbacks_for_driver;
} asio_shared_state_t;

static struct {
  SRWLOCK lock;
  CONDITION_VARIABLE cond;
  asio_shared_state_t* state;
} g_asio_shared = {
    .lock = SRWLOCK_INIT, .cond = CONDITION_VARIABLE_INIT, .state = NULL};

static bool open_asio_device(const char* devname, int samplerate, bool is_dsd,
                             long* out_inputs, long* out_outputs,
                             backend_error_t* err);

/**
 * @brief init_shared_asio matching CamillaDSP device.rs:init_shared_asio.
 */
static bool init_shared_asio(const char* devname, int samplerate, bool is_dsd,
                             long* out_inputs, long* out_outputs,
                             long* out_preferred_buf, backend_error_t* err) {
  logger_trace(&g_logger,
               "init_shared_asio: dev='%s', samplerate=%d, is_dsd=%d", devname,
               samplerate, (int)is_dsd);
  AcquireSRWLockExclusive(&g_asio_shared.lock);

  if (g_asio_shared.state) {
    if (strcmp(g_asio_shared.state->driver_name, devname) != 0) {
      ReleaseSRWLockExclusive(&g_asio_shared.lock);
      if (err) {
        char msg[512];
        snprintf(msg, sizeof(msg),
                 "Full-duplex ASIO state is still held by device '%s' while "
                 "opening '%s'",
                 g_asio_shared.state->driver_name, devname);
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
      }
      return false;
    }
    logger_trace(&g_logger,
                 "init_shared_asio: reusing existing shared state for '%s'",
                 g_asio_shared.state->driver_name);
    *out_inputs = g_asio_shared.state->num_inputs;
    *out_outputs = g_asio_shared.state->num_outputs;
    *out_preferred_buf = g_asio_shared.state->preferred_buf_size;
    ReleaseSRWLockExclusive(&g_asio_shared.lock);
    return true;
  }

  long num_inputs = 0, num_outputs = 0;
  if (!open_asio_device(devname, samplerate, is_dsd, &num_inputs, &num_outputs,
                        err)) {
    ReleaseSRWLockExclusive(&g_asio_shared.lock);
    return false;
  }

  long preferred_buf = 0;
  if (!get_preferred_buffer_size(devname, &preferred_buf, err)) {
    asio_driver_teardown(devname);
    ReleaseSRWLockExclusive(&g_asio_shared.lock);
    return false;
  }

  g_asio_shared.state =
      (asio_shared_state_t*)calloc(1, sizeof(asio_shared_state_t));
  snprintf(g_asio_shared.state->driver_name,
           sizeof(g_asio_shared.state->driver_name), "%s", devname);
  g_asio_shared.state->num_inputs = num_inputs;
  g_asio_shared.state->num_outputs = num_outputs;
  g_asio_shared.state->preferred_buf_size = preferred_buf;
  g_asio_shared.state->stream_started = false;
  g_asio_shared.state->active_count = 0;

  *out_inputs = num_inputs;
  *out_outputs = num_outputs;
  *out_preferred_buf = preferred_buf;

  ReleaseSRWLockExclusive(&g_asio_shared.lock);
  return true;
}

/**
 * @brief register_and_wait matching CamillaDSP device.rs:register_and_wait.
 */
static bool register_and_wait(bool is_input, size_t num_channels,
                              ASIOBufferInfo** out_buffer_infos,
                              long* out_buf_size, backend_error_t* err) {
  logger_trace(&g_logger, "register_and_wait: is_input=%d, num_channels=%zu",
               is_input, num_channels);
  AcquireSRWLockExclusive(&g_asio_shared.lock);

  if (!g_asio_shared.state) {
    ReleaseSRWLockExclusive(&g_asio_shared.lock);
    if (err) {
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "ASIO shared state not initialized");
    }
    return false;
  }

  if (g_asio_shared.state->setup_error[0] != '\0') {
    if (err) {
      char msg[384];
      snprintf(msg, sizeof(msg), "ASIO full-duplex setup aborted: %s",
               g_asio_shared.state->setup_error);
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
    ReleaseSRWLockExclusive(&g_asio_shared.lock);
    return false;
  }

  ASIOBufferInfo* my_infos = make_buffer_infos(num_channels, is_input);
  if (is_input) {
    g_asio_shared.state->pending_input = my_infos;
    g_asio_shared.state->pending_input_channels = num_channels;
  } else {
    g_asio_shared.state->pending_output = my_infos;
    g_asio_shared.state->pending_output_channels = num_channels;
  }

  bool both_ready = (g_asio_shared.state->pending_input != NULL &&
                     g_asio_shared.state->pending_output != NULL);

  if (both_ready) {
    const char* devname = g_asio_shared.state->driver_name;
    size_t out_ch = g_asio_shared.state->pending_output_channels;
    size_t in_ch = g_asio_shared.state->pending_input_channels;
    long preferred_buf = g_asio_shared.state->preferred_buf_size;
    size_t total_ch = out_ch + in_ch;

    ASIOBufferInfo* combined =
        (ASIOBufferInfo*)calloc(total_ch, sizeof(ASIOBufferInfo));
    memcpy(combined, g_asio_shared.state->pending_output,
           out_ch * sizeof(ASIOBufferInfo));
    memcpy(combined + out_ch, g_asio_shared.state->pending_input,
           in_ch * sizeof(ASIOBufferInfo));

    g_asio_shared.state->callbacks_for_driver.bufferSwitch =
        buffer_switch_combined;
    g_asio_shared.state->callbacks_for_driver.sampleRateDidChange =
        sample_rate_changed_callback;
    g_asio_shared.state->callbacks_for_driver.asioMessage =
        asio_message_callback;
    g_asio_shared.state->callbacks_for_driver.bufferSwitchTimeInfo =
        buffer_switch_timeinfo_combined;

    if (!create_asio_buffers(devname, combined, (long)total_ch, preferred_buf,
                             &g_asio_shared.state->callbacks_for_driver, err)) {
      snprintf(g_asio_shared.state->setup_error,
               sizeof(g_asio_shared.state->setup_error),
               "createBuffers failed in full-duplex setup");
      WakeAllConditionVariable(&g_asio_shared.cond);
      ReleaseSRWLockExclusive(&g_asio_shared.lock);
      return false;
    }

    // Update global playback/capture context buffer_infos
    asio_playback_context_t* pb_ctx =
        atomic_load_explicit(&PLAYBACK_CONTEXT, memory_order_acquire);
    if (pb_ctx) {
      pb_ctx->buffer_infos = combined;
    }
    asio_capture_context_t* cap_ctx =
        atomic_load_explicit(&CAPTURE_CONTEXT, memory_order_acquire);
    if (cap_ctx) {
      cap_ctx->buffer_infos = combined + out_ch;
    }

    g_asio_shared.state->buffer_infos_for_driver = combined;

    log_asio_latencies(devname);

    // Start the stream
    if (!start_asio_stream(devname, err)) {
      snprintf(g_asio_shared.state->setup_error,
               sizeof(g_asio_shared.state->setup_error),
               "Failed to start ASIO stream");
      WakeAllConditionVariable(&g_asio_shared.cond);
      ReleaseSRWLockExclusive(&g_asio_shared.lock);
      return false;
    }

    logger_debug(&g_logger, "Full-duplex ASIO stream started.");
    g_asio_shared.state->stream_started = true;
    g_asio_shared.state->setup_error[0] = '\0';
    g_asio_shared.state->active_count = 2;
    WakeAllConditionVariable(&g_asio_shared.cond);
  } else {
    logger_debug(&g_logger,
                 "Waiting for other ASIO side to register for full-duplex...");
    while (!g_asio_shared.state->stream_started &&
           g_asio_shared.state->setup_error[0] == '\0') {
      SleepConditionVariableSRW(&g_asio_shared.cond, &g_asio_shared.lock,
                                INFINITE, 0);
    }
    if (g_asio_shared.state->setup_error[0] != '\0') {
      if (err) {
        char msg[384];
        snprintf(msg, sizeof(msg), "ASIO full-duplex setup aborted: %s",
                 g_asio_shared.state->setup_error);
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
      }
      ReleaseSRWLockExclusive(&g_asio_shared.lock);
      return false;
    }
    logger_debug(&g_logger, "Full-duplex ASIO setup complete, proceeding.");
  }

  size_t out_ch = g_asio_shared.state->pending_output_channels;
  *out_buffer_infos =
      is_input ? (g_asio_shared.state->buffer_infos_for_driver + out_ch)
               : g_asio_shared.state->buffer_infos_for_driver;
  *out_buf_size = g_asio_shared.state->preferred_buf_size;

  ReleaseSRWLockExclusive(&g_asio_shared.lock);
  return true;
}

/**
 * @brief release_shared_asio matching CamillaDSP device.rs:release_shared_asio.
 */
static void release_shared_asio(void) {
  AcquireSRWLockExclusive(&g_asio_shared.lock);
  if (g_asio_shared.state) {
    if (!g_asio_shared.state->stream_started &&
        g_asio_shared.state->setup_error[0] == '\0') {
      strncpy(g_asio_shared.state->setup_error, "ASIO setup aborted",
              sizeof(g_asio_shared.state->setup_error) - 1);
      g_asio_shared.state
          ->setup_error[sizeof(g_asio_shared.state->setup_error) - 1] = '\0';
      WakeAllConditionVariable(&g_asio_shared.cond);
    }
    if (g_asio_shared.state->active_count > 0) {
      g_asio_shared.state->active_count--;
    }
    if (g_asio_shared.state->active_count == 1) {
      logger_debug(&g_logger, "First ASIO side exiting, stopping stream.");
      atomic_store_explicit(&PLAYBACK_CONTEXT, NULL, memory_order_release);
      atomic_store_explicit(&CAPTURE_CONTEXT, NULL, memory_order_release);
      stop_asio_stream(g_asio_shared.state->driver_name);
    } else if (g_asio_shared.state->active_count == 0) {
      logger_debug(&g_logger, "Last ASIO side exiting, disposing driver.");
      dispose_asio_buffers(g_asio_shared.state->driver_name);
      asio_driver_teardown(g_asio_shared.state->driver_name);

      if (g_asio_shared.state->buffer_infos_for_driver) {
        free(g_asio_shared.state->buffer_infos_for_driver);
      }
      if (g_asio_shared.state->pending_output) {
        free(g_asio_shared.state->pending_output);
      }
      if (g_asio_shared.state->pending_input) {
        free(g_asio_shared.state->pending_input);
      }
      free(g_asio_shared.state);
      g_asio_shared.state = NULL;
    }
  }
  ReleaseSRWLockExclusive(&g_asio_shared.lock);
}

// MARK: - Low-level ASIO helpers matching CamillaDSP device.rs

/**
 * @brief Log the name and sample format of each channel in one direction.
 * Matches device.rs:log_channel_details.
 */
static void log_channel_details(IASIO* iasio, long num_channels,
                                bool is_input) {
  const char* direction = is_input ? "Input " : "Output";
  for (long ch = 0; ch < num_channels; ch++) {
    ASIOChannelInfo info = {0};
    info.channel = (int32_t)ch;
    info.isInput = is_input ? ASIOTrue : ASIOFalse;
    if (iasio->lpVtbl->getChannelInfo(iasio, &info) == 0) {
      char fmt_buf[128] = {0};
      snprintf(fmt_buf, sizeof(fmt_buf), "%d (%s)", (int)info.type,
               asio_sample_type_name(info.type));
      logger_debug(&g_logger, "  %s channel %ld: name='%s', format=%s",
                   direction, ch, info.name, fmt_buf);
    }
  }
}

/**
 * @brief Open an ASIO device: load driver, init, set sample rate, query
 * channels. Matches CamillaDSP device.rs:open_asio_device.
 *
 * The sample rate is set immediately after init, before getChannels.
 * Recreating the driver instance makes the new rate take effect cleanly
 * without needing a dummy stream cycle.
 */
static bool open_asio_device(const char* devname, int samplerate, bool is_dsd,
                             long* out_inputs, long* out_outputs,
                             backend_error_t* err) {
  logger_trace(&g_logger,
               "open_asio_device: dev='%s', samplerate=%d, is_dsd=%d", devname,
               samplerate, (int)is_dsd);

  char available[64][256];
  int avail_count = asio_list_device_names(available, 64);
  char avail_str[1024] = {0};
  size_t avail_str_offset = 0;
  avail_str_offset += snprintf(avail_str + avail_str_offset,
                               sizeof(avail_str) - avail_str_offset, "[");
  for (int i = 0; i < avail_count; i++) {
    avail_str_offset += snprintf(avail_str + avail_str_offset,
                                 sizeof(avail_str) - avail_str_offset,
                                 (i > 0 ? ", \"%s\"" : "\"%s\""), available[i]);
  }
  snprintf(avail_str + avail_str_offset, sizeof(avail_str) - avail_str_offset,
           "]");
  logger_debug(&g_logger, "Available ASIO devices: %s", avail_str);

  backend_error_t load_err = {0};
  IASIO* iasio = NULL;
  if (!asio_driver_load_by_name(devname, &iasio, &load_err)) {
    bool exact_match = false;
    for (int i = 0; i < avail_count; i++) {
      if (strcmp(available[i], devname) == 0) {
        exact_match = true;
        break;
      }
    }
    const char* err_desc =
        load_err.message[0] ? load_err.message : "driver load failed";
    const char* hint =
        exact_match
            ? " A driver matching the provided name was found, so the device "
              "may be turned off or disconnected."
            : " No driver matching the provided name was found.";
    if (err) {
      char msg[2048];
      if (exact_match) {
        snprintf(msg, sizeof(msg), "Failed to load ASIO driver '%s': %s%s",
                 devname, err_desc, hint);
      } else {
        snprintf(msg, sizeof(msg),
                 "Failed to load ASIO driver '%s': %s Available devices: %s.%s",
                 devname, err_desc, avail_str, hint);
      }
      backend_error_init(err,
                         load_err.type != BACKEND_ERROR_NONE
                             ? load_err.type
                             : BACKEND_ERROR_INITIALIZATION_FAILED,
                         msg);
    }
    return false;
  }

  // Set DSD mode if requested
  if (is_dsd) {
    ASIOIoFormat dsd_format = {0};
    dsd_format.FormatType = kASIOFormatDSD;
    ASIOError io_res = (ASIOError)(uintptr_t)iasio->lpVtbl->future(
        iasio, kAsioSetIoFormat, &dsd_format);
    if (io_res == 0 || io_res == (ASIOError)ASE_SUCCESS) {
      logger_info(&g_logger,
                  "ASIO driver successfully set to Native DSD format via "
                  "kAsioSetIoFormat");
    } else {
      logger_warn(
          &g_logger,
          "ASIO driver kAsioSetIoFormat DSD returned %ld (FormatType=%ld)",
          io_res, (long)dsd_format.FormatType);
    }
  }

  // Log current sample rate before any changes
  double current_rate = 0.0;
  long rate_res = iasio->lpVtbl->getSampleRate(iasio, &current_rate);
  if (rate_res != 0) {
    asio_driver_teardown(devname);
    if (err) {
      char msg[256];
      snprintf(msg, sizeof(msg),
               "Failed to read ASIO sample rate (error code %ld)", rate_res);
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
    return false;
  }
  logger_debug(&g_logger, "ASIO current sample rate: %.1f Hz", current_rate);

  // Log supported sample rates
  char supported_str[256] = {0};
  size_t offset = 0;
  offset +=
      snprintf(supported_str + offset, sizeof(supported_str) - offset, "[");
  for (size_t r = 0; r < STANDARD_RATES_COUNT; r++) {
    double check_rate =
        (double)(is_dsd ? (STANDARD_RATES[r] * 32) : STANDARD_RATES[r]);
    if (iasio->lpVtbl->canSampleRate(iasio, check_rate) == 0) {
      offset += snprintf(supported_str + offset, sizeof(supported_str) - offset,
                         (offset > 1 ? ", %u" : "%u"), STANDARD_RATES[r]);
    }
  }
  snprintf(supported_str + offset, sizeof(supported_str) - offset, "]");
  logger_debug(&g_logger, "ASIO supported sample rates: %s", supported_str);

  // Set the requested sample rate IMMEDIATELY after init, before getChannels.
  // Some drivers lock in the rate once channels or buffers are queried.
  double rate = (double)(is_dsd ? (samplerate * 32) : samplerate);
  if (iasio->lpVtbl->canSampleRate(iasio, rate) != 0) {
    asio_driver_teardown(devname);
    if (err) {
      char msg[512];
      snprintf(
          msg, sizeof(msg),
          "ASIO device does not support sample rate %d Hz. Supported rates: %s",
          samplerate, supported_str);
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
    return false;
  }

  // Check if rate is already correct
  bool already_correct = (fabs(current_rate - rate) <= 0.5);
  if (already_correct) {
    logger_debug(&g_logger,
                 "ASIO sample rate already at %.0f Hz, no change needed.",
                 rate);
  } else {
    // Try setting on the current driver instance
    long set_res = iasio->lpVtbl->setSampleRate(iasio, rate);
    if (set_res != 0) {
      asio_driver_teardown(devname);
      if (err) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "Failed to set ASIO sample rate to %.0f Hz (error code %ld)",
                 rate, set_res);
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
      }
      return false;
    }

    // Recreating the driver instance is what makes the new rate take effect.
    // Matches CamillaDSP device.rs lines 866-880.
    asio_driver_teardown(devname);
    if (!asio_driver_load_by_name(devname, &iasio, err)) {
      return false;
    }

    if (is_dsd) {
      ASIOIoFormat dsd_format = {0};
      dsd_format.FormatType = kASIOFormatDSD;
      iasio->lpVtbl->future(iasio, kAsioSetIoFormat, &dsd_format);
    }

    double after_reload = 0.0;
    if (iasio->lpVtbl->getSampleRate(iasio, &after_reload) != 0 ||
        fabs(after_reload - rate) > 0.5) {
      asio_driver_teardown(devname);
      if (err) {
        char msg[384];
        snprintf(msg, sizeof(msg),
                 "ASIO device still reports %.1f Hz after being asked for %.0f "
                 "Hz and reinitialised. The driver may require the rate to be "
                 "set from its own control panel.",
                 after_reload, rate);
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
      }
      return false;
    }
    logger_debug(&g_logger,
                 "ASIO sample rate %.0f Hz applied after reinitialising the "
                 "driver.",
                 rate);
  }

  // Query channels AFTER the sample rate has been set
  long num_inputs = 0, num_outputs = 0;
  long channels_res =
      iasio->lpVtbl->getChannels(iasio, &num_inputs, &num_outputs);
  if (channels_res != 0) {
    asio_driver_teardown(devname);
    if (err) {
      char msg[256];
      snprintf(msg, sizeof(msg), "getChannels failed: error code %ld",
               channels_res);
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
    return false;
  }
  logger_debug(&g_logger,
               "ASIO device opened: %ld input channels, %ld output channels.",
               num_inputs, num_outputs);

  // Log per-channel details
  log_channel_details(iasio, num_inputs, true);
  log_channel_details(iasio, num_outputs, false);

  *out_inputs = num_inputs;
  *out_outputs = num_outputs;
  return true;
}

/**
 * @brief open_asio_playback matching CamillaDSP device.rs:open_asio_playback.
 */
static bool open_asio_playback(const char* devname, size_t num_channels,
                               int samplerate,
                               asio_sample_format_t configured_format,
                               bool has_format,
                               asio_sample_format_t* out_resolved_format,
                               backend_error_t* err) {
  long inputs = 0, outputs = 0;
  bool is_dsd = (configured_format == ASIO_SAMPLE_FORMAT_DSD_INT8);
  if (!open_asio_device(devname, samplerate, is_dsd, &inputs, &outputs, err)) {
    return false;
  }
  if (num_channels > (size_t)outputs) {
    asio_driver_teardown(devname);
    if (err) {
      char msg[256];
      snprintf(msg, sizeof(msg),
               "Requested %zu output channels but device only has %ld",
               num_channels, outputs);
      backend_error_init(err, BACKEND_ERROR_INVALID_CHANNELS, msg);
    }
    return false;
  }
  if (!resolve_format(devname, configured_format, has_format, false,
                      out_resolved_format, err)) {
    asio_driver_teardown(devname);
    return false;
  }
  return true;
}

/**
 * @brief open_asio_capture matching CamillaDSP device.rs:open_asio_capture.
 */
static bool open_asio_capture(const char* devname, size_t num_channels,
                              int samplerate,
                              asio_sample_format_t configured_format,
                              bool has_format,
                              asio_sample_format_t* out_resolved_format,
                              backend_error_t* err) {
  long inputs = 0, outputs = 0;
  bool is_dsd = (configured_format == ASIO_SAMPLE_FORMAT_DSD_INT8);
  if (!open_asio_device(devname, samplerate, is_dsd, &inputs, &outputs, err)) {
    return false;
  }
  if (num_channels > (size_t)inputs) {
    asio_driver_teardown(devname);
    if (err) {
      char msg[256];
      snprintf(msg, sizeof(msg),
               "Requested %zu input channels but device only has %ld",
               num_channels, inputs);
      backend_error_init(err, BACKEND_ERROR_INVALID_CHANNELS, msg);
    }
    return false;
  }
  if (!resolve_format(devname, configured_format, has_format, true,
                      out_resolved_format, err)) {
    asio_driver_teardown(devname);
    return false;
  }
  return true;
}

// MARK: - Playback Backend Struct and VTable Methods

struct asio_playback {
  char device[256];
  int sample_rate;
  size_t channels;
  int chunk_size;
  asio_sample_format_t format;
  bool has_format;
  bool full_duplex;
  int target_level;

  asio_sample_format_t resolved_format;
  bool is_lsb;
  size_t bytes_per_sample;
  long actual_buffer_size;

  ASIOBufferInfo* buffer_infos;
  bool single_mode_allocated_infos;
  ASIOCallbacks callbacks_for_driver;

  spsc_byte_ring_buffer_t* ring_buffer;
  uint8_t* encode_buf;
  size_t encode_buf_size;

  asio_playback_context_t* context;
  _Atomic bool is_running;
  _Atomic bool stopped;
  _Atomic bool paused;
  bool com_initialized;
};

static void asio_playback_close(void* ctx) {
  asio_playback_t* playback = (asio_playback_t*)ctx;
  if (!playback) return;

  logger_debug(&g_logger, "Stopping ASIO playback.");
  if (playback->full_duplex) {
    release_shared_asio();
  } else {
    atomic_store_explicit(&PLAYBACK_CONTEXT, NULL, memory_order_release);
    logger_trace(
        &g_logger,
        "Playback: stopping the stream, disposing buffers and tearing down");
    stop_asio_stream(playback->device);
    dispose_asio_buffers(playback->device);
    asio_driver_teardown(playback->device);
  }
  atomic_store_explicit(&PLAYBACK_CONTEXT, NULL, memory_order_release);

  if (playback->context) {
    if (playback->context->read_tmp) {
      free(playback->context->read_tmp);
    }
    if (playback->context->sample_queue) {
      free(playback->context->sample_queue);
    }
    free(playback->context);
    playback->context = NULL;
  }

  if (playback->single_mode_allocated_infos && playback->buffer_infos) {
    free(playback->buffer_infos);
    playback->buffer_infos = NULL;
  }

  if (playback->ring_buffer) {
    spsc_byte_ring_buffer_free(playback->ring_buffer);
    playback->ring_buffer = NULL;
  }
  if (playback->encode_buf) {
    free(playback->encode_buf);
    playback->encode_buf = NULL;
    playback->encode_buf_size = 0;
  }

  if (playback->com_initialized) {
    CoUninitialize();
    playback->com_initialized = false;
  }
}

/**
 * @brief open matching CamillaDSP device.rs AsioPlaybackDevice::start.
 */
static bool asio_playback_open(void* ctx, backend_error_t* err) {
  asio_playback_t* playback = (asio_playback_t*)ctx;
  if (!playback) return false;

  asio_com_init_this_thread();
  playback->com_initialized = true;

  asio_sample_format_t resolved_format = ASIO_SAMPLE_FORMAT_S32_LE;
  long asio_buffer_size = 0;

  if (playback->full_duplex) {
    long inputs = 0, outputs = 0, preferred_buf = 0;
    bool is_dsd = (playback->format == ASIO_SAMPLE_FORMAT_DSD_INT8);
    if (!init_shared_asio(playback->device, playback->sample_rate, is_dsd,
                          &inputs, &outputs, &preferred_buf, err)) {
      goto error_cleanup;
    }
    if (playback->channels > (size_t)outputs) {
      if (err) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "Requested %zu output channels but device only has %ld",
                 playback->channels, outputs);
        backend_error_init(err, BACKEND_ERROR_INVALID_CHANNELS, msg);
      }
      goto error_cleanup;
    }
    if (!resolve_format(playback->device, playback->format,
                        playback->has_format, false, &resolved_format, err)) {
      goto error_cleanup;
    }
    asio_buffer_size = preferred_buf;
  } else {
    if (!open_asio_playback(playback->device, playback->channels,
                            playback->sample_rate, playback->format,
                            playback->has_format, &resolved_format, err)) {
      goto error_cleanup;
    }
    long preferred_buf = 0;
    if (!get_preferred_buffer_size(playback->device, &preferred_buf, err)) {
      goto error_cleanup;
    }
    asio_buffer_size = preferred_buf;
  }

  // Detect whether channel 0 uses LSB-first bit ordering for DSD
  bool is_lsb = false;
  IASIO* active_iasio = asio_driver_lookup(playback->device);
  if (active_iasio) {
    ASIOChannelInfo ch_info = {0};
    ch_info.channel = 0;
    ch_info.isInput = ASIOFalse;
    if (active_iasio->lpVtbl->getChannelInfo(active_iasio, &ch_info) == 0) {
      if (ch_info.type == ASIO_ST_DSD_INT8_LSB_1) {
        is_lsb = true;
      }
    }
  }

  long driver_buffer_size = asio_buffer_size;
  if (resolved_format == ASIO_SAMPLE_FORMAT_DSD_INT8) {
    asio_buffer_size /= 32;
  }

  playback->resolved_format = resolved_format;
  playback->is_lsb = is_lsb;
  playback->bytes_per_sample = sample_format_bytes_per_sample(
      asio_sample_format_to_binary_format(resolved_format, is_lsb));
  playback->actual_buffer_size = asio_buffer_size;

  // Size ring buffer to fit at least driver's buffer size (issue #498)
  size_t ring_frames = ((size_t)playback->chunk_size > (size_t)asio_buffer_size)
                           ? (size_t)playback->chunk_size
                           : (size_t)asio_buffer_size;
  size_t ring_bytes = playback->channels * playback->bytes_per_sample *
                      (2 * ring_frames + 2048);
  playback->ring_buffer = spsc_byte_ring_buffer_create(ring_bytes);

  playback->encode_buf_size = playback->channels *
                              (size_t)playback->chunk_size *
                              playback->bytes_per_sample * 2;
  playback->encode_buf = (uint8_t*)malloc(playback->encode_buf_size);

  clear_playback_rate_change_event();
  reset_playback_callback_seen();

  size_t target_level = (playback->target_level > 0)
                            ? (size_t)playback->target_level
                            : (size_t)playback->chunk_size;

  size_t bytes_per_frame = playback->bytes_per_sample * playback->channels;
  size_t asio_buf_frames = (size_t)asio_buffer_size;

  playback->context =
      (asio_playback_context_t*)calloc(1, sizeof(asio_playback_context_t));
  playback->context->ring_buffer = playback->ring_buffer;
  playback->context->num_channels = playback->channels;
  playback->context->buffer_size = asio_buf_frames;
  playback->context->bytes_per_sample = playback->bytes_per_sample;
  playback->context->read_tmp =
      (uint8_t*)calloc(1, asio_buf_frames * bytes_per_frame);

  size_t initial_queue_cap =
      (16 * ring_frames + target_level + asio_buf_frames * 2) * bytes_per_frame;
  if (initial_queue_cap < asio_buf_frames * bytes_per_frame * 4) {
    initial_queue_cap = asio_buf_frames * bytes_per_frame * 4;
  }
  playback->context->sample_queue = (uint8_t*)malloc(initial_queue_cap);
  playback->context->sample_queue_len = 0;
  playback->context->sample_queue_cap = initial_queue_cap;
  playback->context->target_level = target_level;
  playback->context->running = false;
  playback->context->silence_byte =
      (resolved_format == ASIO_SAMPLE_FORMAT_DSD_INT8) ? 0x69 : 0x00;
  atomic_init(&playback->context->buffer_fill, 0.0);

  if (playback->full_duplex) {
    atomic_store_explicit(&PLAYBACK_CONTEXT, playback->context,
                          memory_order_release);
    if (!register_and_wait(false, playback->channels, &playback->buffer_infos,
                           &playback->actual_buffer_size, err)) {
      atomic_store_explicit(&PLAYBACK_CONTEXT, NULL, memory_order_release);
      goto error_cleanup;
    }
    if (resolved_format == ASIO_SAMPLE_FORMAT_DSD_INT8) {
      playback->actual_buffer_size /= 32;
    }
  } else {
    playback->buffer_infos = make_buffer_infos(playback->channels, false);
    playback->single_mode_allocated_infos = true;
    playback->callbacks_for_driver.bufferSwitch = buffer_switch_playback;
    playback->callbacks_for_driver.sampleRateDidChange =
        sample_rate_changed_callback;
    playback->callbacks_for_driver.asioMessage = asio_message_callback;
    playback->callbacks_for_driver.bufferSwitchTimeInfo =
        buffer_switch_timeinfo_playback;

    if (!create_asio_buffers(playback->device, playback->buffer_infos,
                             (long)playback->channels, driver_buffer_size,
                             &playback->callbacks_for_driver, err)) {
      goto error_cleanup;
    }

    playback->context->buffer_infos = playback->buffer_infos;
    atomic_store_explicit(&PLAYBACK_CONTEXT, playback->context,
                          memory_order_release);

    log_asio_latencies(playback->device);

    logger_trace(&g_logger, "Playback: starting the stream");
    if (!start_asio_stream(playback->device, err)) {
      atomic_store_explicit(&PLAYBACK_CONTEXT, NULL, memory_order_release);
      goto error_cleanup;
    }
    logger_trace(&g_logger, "Playback: stream started");
  }

  logger_debug(&g_logger, "Playback device ready and waiting.");
  bool got_callback = wait_for_playback_callback(500);
  logger_trace(&g_logger,
               "Playback startup callback gate: first_callback_received=%d",
               got_callback);
  logger_debug(&g_logger, "Playback device starts now!");

  atomic_store_explicit(&playback->is_running, true, memory_order_release);
  return true;

error_cleanup:
  asio_playback_close(playback);
  return false;
}

/**
 * @brief write matching CamillaDSP device.rs.
 */
static bool asio_playback_write(void* ctx, const audio_chunk_t* chunk,
                                backend_error_t* err) {
  asio_playback_t* playback = (asio_playback_t*)ctx;
  if (!playback) return false;

  size_t blockalign = playback->channels * playback->bytes_per_sample;
  size_t sleep_duration_us =
      (size_t)(1000000ULL * (unsigned long long)playback->chunk_size /
               (unsigned long long)playback->sample_rate / 2ULL);
  uint32_t sleep_ms = (uint32_t)(sleep_duration_us / 1000);
  if (sleep_ms == 0) sleep_ms = 1;

  return audio_backend_ring_buffer_write(
      playback->ring_buffer, playback->encode_buf, playback->encode_buf_size,
      blockalign, chunk,
      asio_sample_format_to_binary_format(playback->resolved_format,
                                          playback->is_lsb),
      playback->channels, sleep_ms, 8, &playback->is_running,
      &playback->stopped, &playback->paused, &ASIO_PLAYBACK_RATE_CHANGED, err);
}

static size_t asio_playback_get_buffer_level(void* ctx) {
  asio_playback_t* playback = (asio_playback_t*)ctx;
  if (!playback || !playback->context) return 0;
  return (size_t)atomic_load_explicit(&playback->context->buffer_fill,
                                      memory_order_relaxed);
}

static bool asio_playback_get_pending_rate_change(void* ctx, double* out_rate) {
  asio_playback_t* playback = (asio_playback_t*)ctx;
  if (!playback) return false;
  if (take_playback_rate_change_event()) {
    int new_rate = read_current_asio_sample_rate_hz(playback->device);
    if (out_rate) {
      *out_rate = (double)new_rate;
    }
    return true;
  }
  return false;
}

static bool asio_playback_prefill_silence(void* ctx, size_t frames,
                                          backend_error_t* err) {
  (void)err;
  asio_playback_t* playback = (asio_playback_t*)ctx;
  if (!playback) return false;
  playback->target_level = (int)frames;
  return true;
}

static void asio_playback_stop(void* ctx) {
  asio_playback_t* playback = (asio_playback_t*)ctx;
  if (!playback) return;
  atomic_store_explicit(&playback->stopped, true, memory_order_release);
}

static bool asio_playback_get_is_paused(void* ctx) {
  asio_playback_t* playback = (asio_playback_t*)ctx;
  if (!playback) return false;
  return atomic_load_explicit(&playback->paused, memory_order_acquire);
}

static void asio_playback_set_is_paused(void* ctx, bool paused) {
  asio_playback_t* playback = (asio_playback_t*)ctx;
  if (!playback) return;
  atomic_store_explicit(&playback->paused, paused, memory_order_release);
}

static void asio_playback_destroy(void* ctx) {
  asio_playback_t* playback = (asio_playback_t*)ctx;
  if (playback) {
    asio_playback_close(playback);
    free(playback);
  }
}

static playback_backend_t* asio_playback_create(
    const playback_device_config_t* config, int sample_rate, int chunk_size,
    bool full_duplex, processing_parameters_t* params, backend_error_t* err) {
  (void)params;
  (void)err;
  asio_playback_t* playback =
      (asio_playback_t*)calloc(1, sizeof(asio_playback_t));
  if (!playback) return NULL;

  snprintf(playback->device, sizeof(playback->device), "%s",
           config->cfg.asio.device);

  playback->sample_rate = sample_rate;
  playback->channels = config->cfg.asio.channels;
  playback->chunk_size = chunk_size;
  playback->format = config->cfg.asio.format;
  playback->has_format =
      (config->cfg.asio.format != ASIO_SAMPLE_FORMAT_INVALID);
  playback->full_duplex = full_duplex;

  atomic_init(&playback->is_running, false);
  atomic_init(&playback->stopped, false);
  atomic_init(&playback->paused, false);

  playback_backend_t* backend =
      (playback_backend_t*)calloc(1, sizeof(playback_backend_t));
  if (!backend) {
    free(playback);
    return NULL;
  }
  backend->ctx = playback;
  backend->vtable = &g_asio_playback_vtable;
  return backend;
}

const playback_backend_vtable_t g_asio_playback_vtable = {
    .create = asio_playback_create,
    .open = asio_playback_open,
    .write = asio_playback_write,
    .close = asio_playback_close,
    .get_buffer_level = asio_playback_get_buffer_level,
    .get_pending_rate_change = asio_playback_get_pending_rate_change,
    .prefill_silence = asio_playback_prefill_silence,
    .get_is_paused = asio_playback_get_is_paused,
    .set_is_paused = asio_playback_set_is_paused,
    .pitch_control_supported = NULL,
    .set_pitch = NULL,
    .stop = asio_playback_stop,
    .destroy = asio_playback_destroy,
};

// MARK: - Capture Backend Struct and VTable Methods

struct asio_capture {
  char device[256];
  int sample_rate;
  size_t channels;
  int chunk_size;
  asio_sample_format_t format;
  bool has_format;
  bool full_duplex;

  asio_sample_format_t resolved_format;
  bool is_lsb;
  size_t bytes_per_sample;
  long actual_buffer_size;

  ASIOBufferInfo* buffer_infos;
  bool single_mode_allocated_infos;
  ASIOCallbacks callbacks_for_driver;

  spsc_byte_ring_buffer_t* ring_buffer;
  cdsp_sem_t semaphore;
  uint8_t* decode_buf;
  size_t decode_buf_size;

  asio_capture_context_t* context;
  _Atomic bool is_running;
  _Atomic bool stopped;
  bool com_initialized;
};

static void asio_capture_close(void* ctx) {
  asio_capture_t* capture = (asio_capture_t*)ctx;
  if (!capture) return;

  // Close the gate first, so callbacks arriving during teardown do no work.
  // Matches device.rs:CAPTURE_STREAM_ACTIVE.
  atomic_store_explicit(&CAPTURE_STREAM_ACTIVE, false, memory_order_release);

  logger_debug(&g_logger, "Stopping ASIO capture.");
  if (capture->full_duplex) {
    release_shared_asio();
  } else {
    atomic_store_explicit(&CAPTURE_CONTEXT, NULL, memory_order_release);
    logger_trace(
        &g_logger,
        "Capture: stopping the stream, disposing buffers and tearing down");
    stop_asio_stream(capture->device);
    dispose_asio_buffers(capture->device);
    asio_driver_teardown(capture->device);
  }
  atomic_store_explicit(&CAPTURE_CONTEXT, NULL, memory_order_release);

  if (capture->context) {
    if (capture->context->transfer_buf) {
      free(capture->context->transfer_buf);
    }
    free(capture->context);
    capture->context = NULL;
  }

  if (capture->single_mode_allocated_infos && capture->buffer_infos) {
    free(capture->buffer_infos);
    capture->buffer_infos = NULL;
  }

  if (capture->semaphore) {
    cdsp_sem_destroy(capture->semaphore);
    capture->semaphore = NULL;
  }
  if (capture->ring_buffer) {
    spsc_byte_ring_buffer_free(capture->ring_buffer);
    capture->ring_buffer = NULL;
  }
  if (capture->decode_buf) {
    free(capture->decode_buf);
    capture->decode_buf = NULL;
    capture->decode_buf_size = 0;
  }

  if (capture->com_initialized) {
    CoUninitialize();
    capture->com_initialized = false;
  }
}

/**
 * @brief open matching CamillaDSP device.rs AsioCaptureDevice::start.
 */
static bool asio_capture_open(void* ctx, backend_error_t* err) {
  asio_capture_t* capture = (asio_capture_t*)ctx;
  if (!capture) return false;

  asio_com_init_this_thread();
  capture->com_initialized = true;

  asio_sample_format_t resolved_format = ASIO_SAMPLE_FORMAT_S32_LE;
  long asio_buffer_size = 0;

  if (capture->full_duplex) {
    long inputs = 0, outputs = 0, preferred_buf = 0;
    bool is_dsd = (capture->format == ASIO_SAMPLE_FORMAT_DSD_INT8);
    if (!init_shared_asio(capture->device, capture->sample_rate, is_dsd,
                          &inputs, &outputs, &preferred_buf, err)) {
      goto error_cleanup;
    }
    if (capture->channels > (size_t)inputs) {
      if (err) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "Requested %zu input channels but device only has %ld",
                 capture->channels, inputs);
        backend_error_init(err, BACKEND_ERROR_INVALID_CHANNELS, msg);
      }
      goto error_cleanup;
    }
    if (!resolve_format(capture->device, capture->format, capture->has_format,
                        true, &resolved_format, err)) {
      goto error_cleanup;
    }
    asio_buffer_size = preferred_buf;
  } else {
    if (!open_asio_capture(capture->device, capture->channels,
                           capture->sample_rate, capture->format,
                           capture->has_format, &resolved_format, err)) {
      goto error_cleanup;
    }
    long preferred_buf = 0;
    if (!get_preferred_buffer_size(capture->device, &preferred_buf, err)) {
      goto error_cleanup;
    }
    asio_buffer_size = preferred_buf;
  }

  // Detect whether channel 0 uses LSB-first bit ordering for DSD
  bool is_lsb = false;
  IASIO* active_iasio = asio_driver_lookup(capture->device);
  if (active_iasio) {
    ASIOChannelInfo ch_info = {0};
    ch_info.channel = 0;
    ch_info.isInput = ASIOTrue;
    if (active_iasio->lpVtbl->getChannelInfo(active_iasio, &ch_info) == 0) {
      if (ch_info.type == ASIO_ST_DSD_INT8_LSB_1) {
        is_lsb = true;
      }
    }
  }

  long driver_buffer_size = asio_buffer_size;
  if (resolved_format == ASIO_SAMPLE_FORMAT_DSD_INT8) {
    asio_buffer_size /= 32;
  }

  capture->resolved_format = resolved_format;
  capture->is_lsb = is_lsb;
  capture->bytes_per_sample = sample_format_bytes_per_sample(
      asio_sample_format_to_binary_format(resolved_format, is_lsb));
  capture->actual_buffer_size = asio_buffer_size;

  size_t ring_frames = ((size_t)capture->chunk_size > (size_t)asio_buffer_size)
                           ? (size_t)capture->chunk_size
                           : (size_t)asio_buffer_size;
  size_t ring_bytes =
      capture->channels * capture->bytes_per_sample * (2 * ring_frames + 2048);
  capture->ring_buffer = spsc_byte_ring_buffer_create(ring_bytes);
  capture->semaphore = cdsp_sem_create();

  capture->decode_buf_size = capture->channels * (size_t)capture->chunk_size *
                             capture->bytes_per_sample * 2;
  capture->decode_buf = (uint8_t*)malloc(capture->decode_buf_size);

  clear_capture_rate_change_event();
  // Keep the callback from pushing until the loop is ready to consume
  atomic_store_explicit(&CAPTURE_STREAM_ACTIVE, false, memory_order_release);

  capture->context =
      (asio_capture_context_t*)calloc(1, sizeof(asio_capture_context_t));
  capture->context->ring_buffer = capture->ring_buffer;
  capture->context->semaphore = capture->semaphore;
  capture->context->num_channels = capture->channels;
  capture->context->buffer_size = (size_t)asio_buffer_size;
  capture->context->bytes_per_sample = capture->bytes_per_sample;
  capture->context->transfer_buf = (uint8_t*)malloc(
      (size_t)asio_buffer_size * capture->bytes_per_sample * capture->channels);

  if (capture->full_duplex) {
    atomic_store_explicit(&CAPTURE_CONTEXT, capture->context,
                          memory_order_release);
    if (!register_and_wait(true, capture->channels, &capture->buffer_infos,
                           &capture->actual_buffer_size, err)) {
      atomic_store_explicit(&CAPTURE_CONTEXT, NULL, memory_order_release);
      goto error_cleanup;
    }
    if (resolved_format == ASIO_SAMPLE_FORMAT_DSD_INT8) {
      capture->actual_buffer_size /= 32;
    }
  } else {
    capture->buffer_infos = make_buffer_infos(capture->channels, true);
    capture->single_mode_allocated_infos = true;
    capture->callbacks_for_driver.bufferSwitch = buffer_switch_capture;
    capture->callbacks_for_driver.sampleRateDidChange =
        sample_rate_changed_callback;
    capture->callbacks_for_driver.asioMessage = asio_message_callback;
    capture->callbacks_for_driver.bufferSwitchTimeInfo =
        buffer_switch_timeinfo_capture;

    if (!create_asio_buffers(capture->device, capture->buffer_infos,
                             (long)capture->channels, driver_buffer_size,
                             &capture->callbacks_for_driver, err)) {
      goto error_cleanup;
    }

    capture->context->buffer_infos = capture->buffer_infos;
    atomic_store_explicit(&CAPTURE_CONTEXT, capture->context,
                          memory_order_release);

    logger_trace(&g_logger, "Capture: starting the stream");
    if (!start_asio_stream(capture->device, err)) {
      atomic_store_explicit(&CAPTURE_CONTEXT, NULL, memory_order_release);
      goto error_cleanup;
    }
    logger_trace(&g_logger, "Capture: stream started");
  }

  // Discard anything queued before the loop was ready, then open the gate.
  // Matches CamillaDSP device.rs lines 1821-1829.
  size_t discarded =
      spsc_byte_ring_buffer_get_available_to_read(capture->ring_buffer);
  if (discarded > 0) {
    logger_debug(&g_logger,
                 "Discarding %zu bytes captured before the loop was ready.",
                 discarded);
    spsc_byte_ring_buffer_drain(capture->ring_buffer);
  }
  atomic_store_explicit(&CAPTURE_STREAM_ACTIVE, true, memory_order_release);

  logger_debug(&g_logger, "Capture device ready and waiting.");
  logger_debug(&g_logger, "Capture device starts now!");

  atomic_store_explicit(&capture->is_running, true, memory_order_release);
  return true;

error_cleanup:
  asio_capture_close(capture);
  return false;
}

/**
 * @brief read matching CamillaDSP device.rs.
 */
static bool asio_capture_read(void* ctx, size_t frames, audio_chunk_t* chunk,
                              backend_error_t* err) {
  asio_capture_t* capture = (asio_capture_t*)ctx;
  if (!capture) return false;

  size_t blockalign = capture->channels * capture->bytes_per_sample;
  return audio_backend_ring_buffer_read(
      capture->ring_buffer, capture->decode_buf, capture->decode_buf_size,
      blockalign, frames,
      asio_sample_format_to_binary_format(capture->resolved_format,
                                          capture->is_lsb),
      capture->channels, 250, &capture->is_running, &capture->stopped,
      &ASIO_CAPTURE_RATE_CHANGED, chunk, err);
}

static bool asio_capture_wait_for_data(void* ctx, uint32_t timeout_ms) {
  asio_capture_t* capture = (asio_capture_t*)ctx;
  if (!capture || !capture->semaphore) return false;
  return cdsp_sem_timedwait(capture->semaphore, timeout_ms);
}

static bool asio_capture_get_pending_rate_change(void* ctx, double* out_rate) {
  asio_capture_t* capture = (asio_capture_t*)ctx;
  if (!capture) return false;
  if (take_capture_rate_change_event()) {
    int new_rate = read_current_asio_sample_rate_hz(capture->device);
    if (out_rate) {
      *out_rate = (double)new_rate;
    }
    return true;
  }
  return false;
}

static void asio_capture_stop(void* ctx) {
  asio_capture_t* capture = (asio_capture_t*)ctx;
  if (!capture) return;
  atomic_store_explicit(&capture->stopped, true, memory_order_release);
  if (capture->semaphore) {
    cdsp_sem_signal(capture->semaphore);
  }
}

static void asio_capture_destroy(void* ctx) {
  asio_capture_t* capture = (asio_capture_t*)ctx;
  if (capture) {
    asio_capture_close(capture);
    free(capture);
  }
}

static capture_backend_t* asio_capture_create(
    const capture_device_config_t* config, int sample_rate, int chunk_size,
    bool full_duplex, processing_parameters_t* params, backend_error_t* err) {
  (void)params;
  (void)err;
  asio_capture_t* capture = (asio_capture_t*)calloc(1, sizeof(asio_capture_t));
  if (!capture) return NULL;

  snprintf(capture->device, sizeof(capture->device), "%s",
           config->cfg.asio.device);

  capture->sample_rate = sample_rate;
  capture->channels = config->cfg.asio.channels;
  capture->chunk_size = chunk_size;
  capture->format = config->cfg.asio.format;
  capture->has_format = (config->cfg.asio.format != ASIO_SAMPLE_FORMAT_INVALID);
  capture->full_duplex = full_duplex;

  atomic_init(&capture->is_running, false);
  atomic_init(&capture->stopped, false);

  capture_backend_t* backend =
      (capture_backend_t*)calloc(1, sizeof(capture_backend_t));
  if (!backend) {
    free(capture);
    return NULL;
  }
  backend->ctx = capture;
  backend->vtable = &g_asio_capture_vtable;
  backend->is_realtime = true;
  return backend;
}

static void asio_capture_set_is_paused(void* ctx, bool paused) {
  asio_capture_t* capture = (asio_capture_t*)ctx;
  if (!capture) return;
  (void)paused;
}

const capture_backend_vtable_t g_asio_capture_vtable = {
    .create = asio_capture_create,
    .open = asio_capture_open,
    .read = asio_capture_read,
    .close = asio_capture_close,
    .get_pending_rate_change = asio_capture_get_pending_rate_change,
    .is_pitch_control_supported = NULL,
    .set_pitch = NULL,
    .wait_for_data = asio_capture_wait_for_data,
    .set_is_paused = asio_capture_set_is_paused,
    .stop = asio_capture_stop,
    .destroy = asio_capture_destroy,
};

#endif  // ENABLE_ASIO
