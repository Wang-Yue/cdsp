#include "backend/asio_backend.h"

/**
 * @file asio_backend.c
 * @brief ASIO capture and playback backend matching upstream CamillaDSP
 * src/asio_backend/ (driver registry, independent multi-device support,
 * improved rate switching without dummy stream cycle, fixed message selectors,
 * capture stream active gating, latency logging, and native DSD support).
 */

#if defined(ENABLE_ASIO)

#define WIN32_LEAN_AND_MEAN

#include <ctype.h>
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

#include "audio/sample_conversion.h"
#include "config/config_gen.h"
#include "engine/cdsp_sem.h"
#include "logging/app_logger.h"
#include "utils/cdsp_time.h"
#include "utils/device_buffer_estimator.h"
#include "utils/lock_free_ring_buffer.h"

static const logger_t g_logger = {"dsp.backend.asio"};

// MARK: - Driver Registry matching CamillaDSP src/asio_backend/driver.rs

typedef struct asio_driver_entry {
  char devname[256];
  IASIO *iasio;
  CRITICAL_SECTION lock;
  LONG refcount;
  struct asio_driver_entry *next;
} asio_driver_entry_t;

static struct {
  SRWLOCK lock;
  asio_driver_entry_t *head;
  asio_driver_entry_t *unlinked_head;
} g_driver_registry = {
    .lock = SRWLOCK_INIT, .head = NULL, .unlinked_head = NULL};

/**
 * @brief Initialise COM on the calling thread as a Single-Threaded Apartment.
 * Matches driver.rs:com_init_this_thread.
 *
 * ASIO drivers are COM objects that expect an STA. Every thread that calls into
 * a driver calls this first, and nothing ever calls CoUninitialize, so COM
 * stays initialised for as long as the process runs.
 *
 * Safe to call more than once per thread; COM keeps a per-thread reference
 * count.
 */
bool asio_com_init_this_thread(backend_error_t *err) {
  HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  logger_trace(&g_logger, "CoInitializeEx returned 0x%08lX", (unsigned long)hr);
  if (FAILED(hr)) {
    if (err) {
      char msg[256];
      snprintf(msg, sizeof(msg),
               "Failed to initialise COM as a single-threaded apartment, "
               "CoInitializeEx returned 0x%08lX",
               (unsigned long)hr);
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
    return false;
  }
  return true;
}

/**
 * Drivers that ignore `setSampleRate` unless the instance is recreated.
 *
 * The Steinberg built-in (generic) driver returns success and then reports the
 * new rate from `getSampleRate`, while the hardware keeps clocking at the old
 * one. Recreating the instance is the only thing that makes it switch, see the
 * workaround in `open_asio_device`.
 *
 * This is deliberately an allow list rather than something applied to every
 * driver. The quirk appears to be rare: PortAudio's ASIO backend has no
 * handling for it at all, and it has been exercised against far more drivers
 * than we have. Recreating an instance is also not free of risk, ASIO4ALL
 * deadlocks when asked to do it, so the reload is only done for drivers known
 * to need it.
 */
static const char *const NEEDS_RATE_RELOAD[] = {"steinberg built-in"};
static const size_t NEEDS_RATE_RELOAD_COUNT =
    sizeof(NEEDS_RATE_RELOAD) / sizeof(NEEDS_RATE_RELOAD[0]);

/**
 * Drivers that are refused outright.
 *
 * ASIO4ALL tolerates only one instance per process. It keeps the audio device
 * open until `ASIOStop` is called or its DLL is unloaded, which its author has
 * confirmed on the ASIO4ALL forum, so releasing an instance that was
 * initialised but never started leaves the device held, and creating the next
 * one either deadlocks in `ASIOInit` or takes the process down. A failed
 * configuration followed by a corrected one reproduces that crash every time,
 * and `ASIOStop` before the release does not reliably help, it worked once in
 * three attempts.
 *
 * Keeping the instance around to reuse it does not work either: it belongs to
 * the COM apartment of the device thread that created it, and that thread exits
 * at the end of every session. Making this driver safe would mean owning every
 * ASIO instance on a dedicated thread that lives as long as the process.
 *
 * That is a lot of machinery for a driver that gives CamillaDSP nothing. All
 * ASIO4ALL does is make an ordinary WDM device reachable from applications that
 * only speak ASIO, and CamillaDSP already speaks Wasapi, where exclusive mode
 * is just as bit-perfect with one emulation layer less. Anyone pointing this
 * backend at ASIO4ALL is better served by the Wasapi backend, so the driver is
 * refused with a message that says so.
 */
static const char *const UNSUPPORTED_DRIVERS[] = {"asio4all"};
static const size_t UNSUPPORTED_DRIVERS_COUNT =
    sizeof(UNSUPPORTED_DRIVERS) / sizeof(UNSUPPORTED_DRIVERS[0]);

/**
 * Case-insensitive substring match of a device name against a list of driver
 * names.
 *
 * Substring rather than equality because the same driver is named
 * inconsistently: ASIO4ALL appears as `ASIO4ALL v2` in the registry key and
 * from `getDriverName`, but as `Asio4all v2` in the description that device
 * names are taken from.
 */
static bool matches_driver(const char *devname, const char *const *names,
                           size_t count) {
  if (!devname || !names || count == 0)
    return false;
  size_t len = strlen(devname);
  char *lower_devname = (char *)malloc(len + 1);
  if (!lower_devname)
    return false;
  for (size_t i = 0; i < len; i++) {
    lower_devname[i] = (char)tolower((unsigned char)devname[i]);
  }
  lower_devname[len] = '\0';

  bool matched = false;
  for (size_t i = 0; i < count; i++) {
    if (strstr(lower_devname, names[i]) != NULL) {
      matched = true;
      break;
    }
  }
  free(lower_devname);
  return matched;
}

/**
 * @brief Whether this driver needs to be recreated for a sample rate change to
 * take effect. Matches CamillaDSP driver.rs:needs_rate_reload.
 */
bool asio_needs_rate_reload(const char *devname) {
  return matches_driver(devname, NEEDS_RATE_RELOAD, NEEDS_RATE_RELOAD_COUNT);
}

/**
 * @brief Whether this driver is refused outright.
 * Matches CamillaDSP driver.rs:is_unsupported_driver.
 */
bool asio_is_unsupported_driver(const char *devname) {
  return matches_driver(devname, UNSUPPORTED_DRIVERS,
                        UNSUPPORTED_DRIVERS_COUNT);
}

/**
 * @brief Whether only one instance of this driver may be created per process.
 * Matches CamillaDSP driver.rs:is_single_instance_driver.
 * @deprecated Use asio_is_unsupported_driver instead.
 */
bool asio_is_single_instance_driver(const char *devname) {
  return asio_is_unsupported_driver(devname);
}

static asio_driver_entry_t *asio_driver_get_entry(const char *devname) {
  if (!devname)
    return NULL;
  AcquireSRWLockShared(&g_driver_registry.lock);
  asio_driver_entry_t *curr = g_driver_registry.head;
  while (curr) {
    if (strcmp(curr->devname, devname) == 0) {
      InterlockedIncrement(&curr->refcount);
      break;
    }
    curr = curr->next;
  }
  ReleaseSRWLockShared(&g_driver_registry.lock);
  return curr;
}

static void asio_driver_entry_release(asio_driver_entry_t *entry) {
  if (!entry)
    return;
  if (InterlockedDecrement(&entry->refcount) == 0) {
    AcquireSRWLockExclusive(&g_driver_registry.lock);
    asio_driver_entry_t **curr = &g_driver_registry.unlinked_head;
    while (*curr) {
      if (*curr == entry) {
        *curr = entry->next;
        break;
      }
      curr = &(*curr)->next;
    }
    curr = &g_driver_registry.head;
    while (*curr) {
      if (*curr == entry) {
        *curr = entry->next;
        break;
      }
      curr = &(*curr)->next;
    }
    ReleaseSRWLockExclusive(&g_driver_registry.lock);

    DeleteCriticalSection(&entry->lock);
    if (entry->iasio) {
      SAFE_RELEASE(entry->iasio);
    }
    free(entry);
  }
}

/**
 * @brief Lock the per-driver mutex for devname and increment active reference
 * count. Matches upstream driver handle Mutex locking.
 */
bool asio_driver_lock(const char *devname) {
  if (!devname)
    return false;
  asio_driver_entry_t *entry = asio_driver_get_entry(devname);
  if (!entry)
    return false;
  EnterCriticalSection(&entry->lock);
  return true;
}

/**
 * @brief Unlock the per-driver mutex for devname and decrement active reference
 * count.
 */
void asio_driver_unlock(const char *devname) {
  if (!devname)
    return;
  AcquireSRWLockShared(&g_driver_registry.lock);
  asio_driver_entry_t *curr = g_driver_registry.head;
  asio_driver_entry_t *target = NULL;
  while (curr) {
    if (strcmp(curr->devname, devname) == 0) {
      target = curr;
      break;
    }
    curr = curr->next;
  }
  if (!target) {
    curr = g_driver_registry.unlinked_head;
    while (curr) {
      if (strcmp(curr->devname, devname) == 0) {
        target = curr;
        break;
      }
      curr = curr->next;
    }
  }
  ReleaseSRWLockShared(&g_driver_registry.lock);
  if (target) {
    LeaveCriticalSection(&target->lock);
    asio_driver_entry_release(target);
  }
}

/**
 * @brief Run action with the driver loaded for devname, holding the per-driver
 * mutex. Matches CamillaDSP driver.rs:with_driver.
 */
bool asio_with_driver(const char *devname, asio_driver_action_fn action,
                      void *user_data, backend_error_t *err) {
  if (!devname) {
    if (err) {
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "No ASIO device specified");
    }
    return false;
  }

  asio_driver_entry_t *entry = asio_driver_get_entry(devname);
  if (!entry) {
    if (err) {
      char msg[256];
      snprintf(msg, sizeof(msg), "No ASIO driver is loaded for device '%s'",
               devname);
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
    return false;
  }

  EnterCriticalSection(&entry->lock);
  bool result = action ? action(entry->iasio, user_data, err) : false;
  LeaveCriticalSection(&entry->lock);
  asio_driver_entry_release(entry);
  return result;
}

/**
 * @brief Look up a loaded driver by device name in the registry.
 */
IASIO *asio_driver_lookup(const char *devname) {
  if (!devname)
    return NULL;
  AcquireSRWLockShared(&g_driver_registry.lock);
  asio_driver_entry_t *curr = g_driver_registry.head;
  IASIO *result = NULL;
  while (curr) {
    if (strcmp(curr->devname, devname) == 0) {
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
bool asio_driver_is_loaded(const char *devname) {
  return asio_driver_lookup(devname) != NULL;
}

static bool asio_reg_query_string_utf8(HKEY key, const wchar_t *val_name,
                                       char *out_buf, size_t out_size) {
  if (!key || !out_buf || out_size == 0)
    return false;
  out_buf[0] = '\0';

  DWORD datatype = 0;
  DWORD byte_size = 0;
  LONG cr = RegQueryValueExW(key, val_name, NULL, &datatype, NULL, &byte_size);
  if (cr != ERROR_SUCCESS || byte_size == 0)
    return false;
  if (datatype != REG_SZ && datatype != REG_EXPAND_SZ)
    return false;

  wchar_t *wbuf = (wchar_t *)malloc(byte_size + sizeof(wchar_t));
  if (!wbuf)
    return false;

  DWORD read_size = byte_size;
  cr = RegQueryValueExW(key, val_name, NULL, &datatype, (LPBYTE)wbuf,
                        &read_size);
  if (cr != ERROR_SUCCESS) {
    free(wbuf);
    return false;
  }
  wbuf[read_size / sizeof(wchar_t)] = L'\0';

  int written = WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, out_buf,
                                    (int)out_size, NULL, NULL);
  free(wbuf);
  if (written <= 0) {
    out_buf[0] = '\0';
    return false;
  }
  out_buf[out_size - 1] = '\0';
  return true;
}

static bool parse_asio_clsid(const char *clsid_str, CLSID *out_clsid) {
  if (!clsid_str || !out_clsid)
    return false;
  while (isspace((unsigned char)*clsid_str))
    clsid_str++;

  char normalized[64];
  size_t len = strlen(clsid_str);
  while (len > 0 && isspace((unsigned char)clsid_str[len - 1]))
    len--;
  if (len == 0 || len >= sizeof(normalized) - 3)
    return false;

  const char *start = clsid_str;
  if (*start == '{') {
    start++;
    len--;
  }
  if (len > 0 && start[len - 1] == '}') {
    len--;
  }
  if (len != 36)
    return false;

  normalized[0] = '{';
  memcpy(normalized + 1, start, 36);
  normalized[37] = '}';
  normalized[38] = '\0';

  wchar_t wclsid[64];
  if (MultiByteToWideChar(CP_UTF8, 0, normalized, -1, wclsid, 64) <= 0) {
    return false;
  }
  return SUCCEEDED(CLSIDFromString(wclsid, out_clsid));
}

static bool find_asio_driver_clsid(const char *driver_name, CLSID *out_clsid) {
  if (!driver_name || driver_name[0] == '\0' || !out_clsid) {
    return false;
  }

  HKEY hk;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ, &hk) !=
      ERROR_SUCCESS) {
    return false;
  }

  wchar_t subkey_name[256];
  DWORD index = 0;
  bool found = false;

  while (RegEnumKeyW(hk, index++, subkey_name,
                     sizeof(subkey_name) / sizeof(wchar_t)) == ERROR_SUCCESS) {
    HKEY hk_driver;
    if (RegOpenKeyExW(hk, subkey_name, 0, KEY_READ, &hk_driver) ==
        ERROR_SUCCESS) {
      char clsid_str[128];
      char drv_name[512];
      if (asio_reg_query_string_utf8(hk_driver, L"CLSID", clsid_str,
                                     sizeof(clsid_str)) &&
          asio_reg_query_string_utf8(hk_driver, L"description", drv_name,
                                     sizeof(drv_name))) {
        if (drv_name[0] != '\0' && strcmp(drv_name, driver_name) == 0) {
          if (parse_asio_clsid(clsid_str, out_clsid)) {
            found = true;
          }
        }
      }
      RegCloseKey(hk_driver);
    }
    if (found)
      break;
  }
  RegCloseKey(hk);
  return found;
}

static HRESULT create_asio_com_instance(const CLSID *clsid, IASIO **out_iasio) {
  if (!clsid || !out_iasio)
    return E_POINTER;
  *out_iasio = NULL;

  IUnknown *unk = NULL;
  HRESULT hr = CoCreateInstance(clsid, NULL, CLSCTX_SERVER, &IID_IUnknown,
                                (void **)&unk);
  if (FAILED(hr) || !unk) {
    return hr;
  }

  hr = unk->lpVtbl->QueryInterface(unk, clsid, (void **)out_iasio);
  unk->lpVtbl->Release(unk);
  return hr;
}

/**
 * @brief List the names of all registered ASIO drivers.
 * Matches driver.rs:list_device_names.
 */
int asio_list_device_names(char out_names[][256], int max_names) {
  if (!out_names || max_names <= 0)
    return 0;

  HKEY hk;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ, &hk) !=
      ERROR_SUCCESS) {
    return 0;
  }

  wchar_t subkey_name[256];
  DWORD index = 0;
  int count = 0;

  while (RegEnumKeyW(hk, index++, subkey_name,
                     sizeof(subkey_name) / sizeof(wchar_t)) == ERROR_SUCCESS &&
         count < max_names) {
    HKEY hk_driver;
    if (RegOpenKeyExW(hk, subkey_name, 0, KEY_READ, &hk_driver) ==
        ERROR_SUCCESS) {
      char clsid_str[128];
      char drv_name[256];
      CLSID dummy_clsid;
      if (asio_reg_query_string_utf8(hk_driver, L"CLSID", clsid_str,
                                     sizeof(clsid_str)) &&
          asio_reg_query_string_utf8(hk_driver, L"description", drv_name,
                                     sizeof(drv_name))) {
        if (drv_name[0] != '\0' && parse_asio_clsid(clsid_str, &dummy_clsid)) {
          snprintf(out_names[count], 256, "%s", drv_name);
          out_names[count][255] = '\0';
          count++;
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
void asio_driver_teardown(const char *devname) {
  if (!devname)
    return;

  asio_driver_entry_t *entry = NULL;
  AcquireSRWLockExclusive(&g_driver_registry.lock);
  asio_driver_entry_t **curr = &g_driver_registry.head;
  while (*curr) {
    if (strcmp((*curr)->devname, devname) == 0) {
      entry = *curr;
      *curr = entry->next;
      entry->next = g_driver_registry.unlinked_head;
      g_driver_registry.unlinked_head = entry;
      break;
    }
    curr = &(*curr)->next;
  }
  ReleaseSRWLockExclusive(&g_driver_registry.lock);

  if (entry) {
    logger_trace(&g_logger,
                 "asio_driver_teardown: releasing the instance for '%s'",
                 devname);
    asio_driver_entry_release(entry);
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
bool asio_driver_load_by_name(const char *name, IASIO **out_iasio,
                              backend_error_t *err) {
  logger_trace(&g_logger, "asio_driver_load_by_name: loading '%s'", name);
  if (asio_is_unsupported_driver(name)) {
    if (err) {
      char msg[256];
      snprintf(msg, sizeof(msg),
               "The ASIO driver '%s' is not supported, use the Wasapi backend "
               "for this "
               "device instead",
               name);
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
    return false;
  }
  asio_driver_teardown(name);
  if (!asio_com_init_this_thread(err)) {
    return false;
  }

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

  IASIO *iasio = NULL;
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

  if (!iasio->lpVtbl->init(iasio, NULL)) {
    char err_msg[129] = {0};
    iasio->lpVtbl->getErrorMessage(iasio, err_msg);
    err_msg[128] = '\0';
    SAFE_RELEASE(iasio);
    if (err) {
      char msg[256];
      snprintf(msg, sizeof(msg), "Failed to initialise ASIO driver '%s': %s",
               name, err_msg[0] ? err_msg : "driver init returned false");
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
    return false;
  }

  char driver_name[33] = {0};
  iasio->lpVtbl->getDriverName(iasio, driver_name);
  driver_name[32] = '\0';
  long driver_version = iasio->lpVtbl->getDriverVersion(iasio);
  logger_debug(&g_logger, "Loaded ASIO driver '%s', version %ld.",
               driver_name[0] ? driver_name : name, driver_version);

  // Store in registry
  asio_driver_entry_t *entry =
      (asio_driver_entry_t *)calloc(1, sizeof(asio_driver_entry_t));
  if (!entry) {
    SAFE_RELEASE(iasio);
    if (err) {
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Out of memory registering ASIO driver");
    }
    return false;
  }

  snprintf(entry->devname, sizeof(entry->devname), "%s", name);
  entry->iasio = iasio;
  InitializeCriticalSection(&entry->lock);
  entry->refcount = 1;

  AcquireSRWLockExclusive(&g_driver_registry.lock);
  entry->next = g_driver_registry.head;
  g_driver_registry.head = entry;
  ReleaseSRWLockExclusive(&g_driver_registry.lock);

  logger_trace(&g_logger,
               "asio_driver_load_by_name: '%s' loaded and initialised", name);
  if (out_iasio) {
    *out_iasio = iasio;
  }
  return true;
}

// MARK: - ASIO Utils matching CamillaDSP utils.rs

static inline bool asio_ok(long r) { return r == 0 || r == (long)ASE_SUCCESS; }

/**
 * @brief Read the currently active ASIO sample rate in Hz.
 * Matches utils.rs:read_current_asio_sample_rate_hz.
 */
struct asio_rate_data {
  int rate_hz;
};

static bool get_sample_rate_action(IASIO *iasio, void *user_data,
                                   backend_error_t *err) {
  (void)err;
  struct asio_rate_data *d = (struct asio_rate_data *)user_data;
  double rate = 0.0;
  long res = iasio->lpVtbl->getSampleRate(iasio, &rate);
  if (asio_ok(res) && isfinite(rate) && rate > 0.0) {
    d->rate_hz = (int)round(rate);
  }
  return true;
}

static int read_current_asio_sample_rate_hz(const char *devname) {
  struct asio_rate_data d = {0};
  if (!asio_with_driver(devname, get_sample_rate_action, &d, NULL)) {
    return 0;
  }
  return d.rate_hz;
}

/**
 * @brief make_buffer_infos matching utils.rs:make_channel_ids.
 */
static ASIOBufferInfo *make_buffer_infos(size_t num_channels, bool is_input) {
  ASIOBufferInfo *infos =
      (ASIOBufferInfo *)calloc(num_channels, sizeof(ASIOBufferInfo));
  if (!infos)
    return NULL;
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
const char *asio_format_to_str(asio_sample_format_t fmt) {
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
  case ASIO_SAMPLE_FORMAT_INVALID:
    return "Invalid";
  }
  CDSP_UNREACHABLE();
  return "Unknown";
}

/**
 * @brief asio_sample_type_name matching utils.rs:asio_sample_type_name.
 */
const char *asio_sample_type_name(int type_id) {
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
  }
  return "Unknown";
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
  }
  return ASIO_SAMPLE_FORMAT_INVALID;
}

/**
 * @brief query_device_format matching utils.rs:query_device_format.
 */
struct query_format_data {
  bool is_input;
  int type;
};

static bool query_format_action(IASIO *iasio, void *user_data,
                                backend_error_t *err) {
  struct query_format_data *d = (struct query_format_data *)user_data;
  ASIOChannelInfo info = {0};
  info.channel = 0;
  info.isInput = d->is_input ? ASIOTrue : ASIOFalse;
  long res = iasio->lpVtbl->getChannelInfo(iasio, &info);
  if (!asio_ok(res)) {
    const char *direction = d->is_input ? "input" : "output";
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
               d->is_input ? "input" : "output", info.type,
               asio_sample_type_name(info.type));
  d->type = info.type;
  return true;
}

static bool query_device_format(const char *devname, bool is_input,
                                int *out_type, backend_error_t *err) {
  struct query_format_data d = {.is_input = is_input, .type = 0};
  if (!asio_with_driver(devname, query_format_action, &d, err)) {
    return false;
  }
  *out_type = d.type;
  return true;
}

/**
 * @brief resolve_format matching utils.rs:resolve_format.
 */
static bool resolve_format(const char *devname, asio_sample_format_t configured,
                           bool has_configured, bool is_input,
                           asio_sample_format_t *out_format,
                           backend_error_t *err) {
  int device_type = 0;
  if (!query_device_format(devname, is_input, &device_type, err)) {
    return false;
  }
  asio_sample_format_t native_format = asio_sample_type_to_format(device_type);
  const char *direction = is_input ? "capture" : "playback";

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

struct get_buf_size_data {
  long preferred;
};

static bool get_buf_size_action(IASIO *iasio, void *user_data,
                                backend_error_t *err) {
  struct get_buf_size_data *d = (struct get_buf_size_data *)user_data;
  long min_buf = 0, max_buf = 0, preferred_buf = 0, granularity = 0;
  long res = iasio->lpVtbl->getBufferSize(iasio, &min_buf, &max_buf,
                                          &preferred_buf, &granularity);
  if (!asio_ok(res)) {
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
  d->preferred = preferred_buf;
  return true;
}

static bool get_preferred_buffer_size(const char *devname, long *out_preferred,
                                      backend_error_t *err) {
  struct get_buf_size_data d = {0};
  if (!asio_with_driver(devname, get_buf_size_action, &d, err)) {
    return false;
  }
  *out_preferred = d.preferred;
  return true;
}

/**
 * @brief create_asio_buffers matching utils.rs:create_asio_buffers.
 */
struct create_buffers_data {
  ASIOBufferInfo *buffer_infos;
  long num_channels;
  long buffer_size;
  ASIOCallbacks *callbacks;
};

static bool create_buffers_action(IASIO *iasio, void *user_data,
                                  backend_error_t *err) {
  struct create_buffers_data *d = (struct create_buffers_data *)user_data;
  logger_trace(
      &g_logger,
      "Calling createBuffers: infos_ptr=%p, channels=%ld, buffer_size=%ld, "
      "callbacks_ptr=%p",
      (int64_t)(uintptr_t)d->buffer_infos, d->num_channels, d->buffer_size,
      (int64_t)(uintptr_t)d->callbacks);
  long res = iasio->lpVtbl->createBuffers(
      iasio, d->buffer_infos, d->num_channels, d->buffer_size, d->callbacks);
  logger_trace(&g_logger, "createBuffers returned %ld.", res);
  if (!asio_ok(res)) {
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

static bool create_asio_buffers(const char *devname,
                                ASIOBufferInfo *buffer_infos, long num_channels,
                                long buffer_size, ASIOCallbacks *callbacks,
                                backend_error_t *err) {
  struct create_buffers_data d = {
      .buffer_infos = buffer_infos,
      .num_channels = num_channels,
      .buffer_size = buffer_size,
      .callbacks = callbacks,
  };
  return asio_with_driver(devname, create_buffers_action, &d, err);
}

/**
 * @brief dispose_asio_buffers matching utils.rs:dispose_asio_buffers.
 */
static bool dispose_buffers_action(IASIO *iasio, void *user_data,
                                   backend_error_t *err) {
  (void)user_data;
  (void)err;
  return asio_ok(iasio->lpVtbl->disposeBuffers(iasio));
}

static bool dispose_asio_buffers(const char *devname) {
  return asio_with_driver(devname, dispose_buffers_action, NULL, NULL);
}

/**
 * @brief start_asio_stream matching utils.rs:start_asio_stream.
 */
static bool start_stream_action(IASIO *iasio, void *user_data,
                                backend_error_t *err) {
  (void)user_data;
  long res = iasio->lpVtbl->start(iasio);
  if (!asio_ok(res)) {
    if (err) {
      char msg[256];
      snprintf(msg, sizeof(msg), "Failed to start ASIO stream: %ld", res);
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
    return false;
  }
  return true;
}

static bool start_asio_stream(const char *devname, backend_error_t *err) {
  return asio_with_driver(devname, start_stream_action, NULL, err);
}

/**
 * @brief stop_asio_stream matching utils.rs:stop_asio_stream.
 */
static bool stop_stream_action(IASIO *iasio, void *user_data,
                               backend_error_t *err) {
  (void)user_data;
  (void)err;
  return asio_ok(iasio->lpVtbl->stop(iasio));
}

static bool stop_asio_stream(const char *devname) {
  return asio_with_driver(devname, stop_stream_action, NULL, NULL);
}

/**
 * @brief log_asio_latencies matching utils.rs:log_asio_latencies.
 */
struct latencies_data {
  long in_lat;
  long out_lat;
  bool ok;
};

static bool latencies_action(IASIO *iasio, void *user_data,
                             backend_error_t *err) {
  (void)err;
  struct latencies_data *d = (struct latencies_data *)user_data;
  long in_lat = 0, out_lat = 0;
  long res = iasio->lpVtbl->getLatencies(iasio, &in_lat, &out_lat);
  if (asio_ok(res)) {
    d->in_lat = in_lat;
    d->out_lat = out_lat;
    d->ok = true;
  }
  return true;
}

static void log_asio_latencies(const char *devname) {
  struct latencies_data d = {0};
  if (!asio_with_driver(devname, latencies_action, &d, NULL) || !d.ok) {
    logger_debug(&g_logger, "Could not read ASIO latencies");
    return;
  }
  int samplerate = read_current_asio_sample_rate_hz(devname);
  if (samplerate > 0) {
    double in_ms = 1000.0 * (double)d.in_lat / (double)samplerate;
    double out_ms = 1000.0 * (double)d.out_lat / (double)samplerate;
    logger_debug(&g_logger,
                 "ASIO driver reported latencies: capture %ld frames (%.1f "
                 "ms), playback %ld frames (%.1f ms).",
                 d.in_lat, in_ms, d.out_lat, out_ms);
  } else {
    logger_debug(&g_logger,
                 "ASIO driver reported latencies: capture %ld frames, "
                 "playback %ld frames.",
                 d.in_lat, d.out_lat);
  }
}

/**
 * @brief Check if channel 0 uses LSB-first bit ordering for DSD.
 */
struct dsd_lsb_data {
  bool is_input;
  bool is_lsb;
};

static bool check_dsd_lsb_action(IASIO *iasio, void *user_data,
                                 backend_error_t *err) {
  (void)err;
  struct dsd_lsb_data *d = (struct dsd_lsb_data *)user_data;
  ASIOChannelInfo ch_info = {0};
  ch_info.channel = 0;
  ch_info.isInput = d->is_input ? ASIOTrue : ASIOFalse;
  if (asio_ok(iasio->lpVtbl->getChannelInfo(iasio, &ch_info))) {
    if (ch_info.type == ASIO_ST_DSD_INT8_LSB_1) {
      d->is_lsb = true;
    }
  }
  return true;
}

static bool asio_device_is_dsd_lsb(const char *devname, bool is_input) {
  struct dsd_lsb_data d = {.is_input = is_input, .is_lsb = false};
  asio_with_driver(devname, check_dsd_lsb_action, &d, NULL);
  return d.is_lsb;
}

// MARK: - Internal Contexts and Global Atomics matching CamillaDSP device.rs

typedef struct {
  spsc_byte_ring_buffer_t *ring_buffer;
  ASIOBufferInfo *buffer_infos;
  size_t num_channels;
  size_t buffer_size;
  size_t bytes_per_sample;
  uint8_t *read_tmp;
  uint8_t *sample_queue;
  size_t sample_queue_len;
  size_t sample_queue_cap;
  _Atomic size_t target_level;
  uint8_t silence_byte;
  // Total frames still pending playback (callback-local queue plus ring
  // buffer), published by the ASIO callback thread and extrapolated by the
  // engine thread. Mirrors upstream's DeviceBufferEstimator
  // (src/utils/countertimer.rs:23-54).
  device_buffer_estimator_t device_buffer;
  bool running;
} asio_playback_context_t;

typedef struct {
  spsc_byte_ring_buffer_t *ring_buffer;
  cdsp_sem_t semaphore;
  ASIOBufferInfo *buffer_infos;
  size_t num_channels;
  size_t buffer_size;
  size_t bytes_per_sample;
  uint8_t *transfer_buf;
  size_t transfer_buf_size;
} asio_capture_context_t;

static _Atomic(asio_playback_context_t *) PLAYBACK_CONTEXT = NULL;
static _Atomic(asio_capture_context_t *) CAPTURE_CONTEXT = NULL;

/// Gates the capture callback until the capture loop is ready to consume.
/// Matches device.rs:CAPTURE_STREAM_ACTIVE.
static _Atomic bool CAPTURE_STREAM_ACTIVE = false;

static _Atomic bool ASIO_PLAYBACK_RATE_CHANGED = false;
static _Atomic bool ASIO_CAPTURE_RATE_CHANGED = false;

/// Set when a driver asks for a reset with `kAsioResetRequest`.
///
/// The callback answers yes to that request, which commits the host to tearing
/// the stream down and starting over. It cannot do that from the driver's own
/// callback thread, so it raises this instead and the device loop stops the
/// stream, which makes the engine restart the pipeline and reopen the device.
static _Atomic bool ASIO_PLAYBACK_RESET_REQUESTED = false;
static _Atomic bool ASIO_CAPTURE_RESET_REQUESTED = false;

static void clear_playback_driver_events(void) {
  atomic_store_explicit(&ASIO_PLAYBACK_RATE_CHANGED, false,
                        memory_order_release);
  atomic_store_explicit(&ASIO_PLAYBACK_RESET_REQUESTED, false,
                        memory_order_release);
}

static void clear_capture_driver_events(void) {
  atomic_store_explicit(&ASIO_CAPTURE_RATE_CHANGED, false,
                        memory_order_release);
  atomic_store_explicit(&ASIO_CAPTURE_RESET_REQUESTED, false,
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

static bool take_playback_reset_request(void) {
  return atomic_exchange_explicit(&ASIO_PLAYBACK_RESET_REQUESTED, false,
                                  memory_order_acq_rel);
}

static bool take_capture_reset_request(void) {
  return atomic_exchange_explicit(&ASIO_CAPTURE_RESET_REQUESTED, false,
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

static inline bool ensure_sample_queue_cap(asio_playback_context_t *ctx,
                                           size_t needed_cap) {
  if (ctx->sample_queue_cap < needed_cap) {
    size_t new_cap = ctx->sample_queue_cap * 2;
    if (new_cap < needed_cap)
      new_cap = needed_cap;
    uint8_t *new_buf = (uint8_t *)realloc(ctx->sample_queue, new_cap);
    if (!new_buf) {
      return false;
    }
    ctx->sample_queue = new_buf;
    ctx->sample_queue_cap = new_cap;
  }
  return true;
}

/**
 * @brief buffer_switch_playback matching CamillaDSP device.rs.
 */
static void buffer_switch_playback(long buffer_index, ASIOBool direct_process) {
  (void)direct_process;
  asio_playback_context_t *ctx =
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
      if (ensure_sample_queue_cap(ctx, needed_bytes)) {
        memset(ctx->sample_queue + ctx->sample_queue_len, ctx->silence_byte,
               missing);
        ctx->sample_queue_len = needed_bytes;
      } else {
        size_t fit = (ctx->sample_queue_cap > ctx->sample_queue_len)
                         ? (ctx->sample_queue_cap - ctx->sample_queue_len)
                         : 0;
        if (fit > 0) {
          memset(ctx->sample_queue + ctx->sample_queue_len, ctx->silence_byte,
                 fit);
          ctx->sample_queue_len += fit;
        }
      }
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
      size_t target_level =
          atomic_load_explicit(&ctx->target_level, memory_order_acquire);
      size_t prefill_frames =
          (target_level > ctx->buffer_size) ? target_level : ctx->buffer_size;
      size_t prefill_bytes = prefill_frames * bytes_per_frame;
      size_t new_len = ctx->sample_queue_len + prefill_bytes;
      if (ensure_sample_queue_cap(ctx, new_len)) {
        memset(ctx->sample_queue + ctx->sample_queue_len, ctx->silence_byte,
               prefill_bytes);
        ctx->sample_queue_len = new_len;
      } else {
        size_t fit = (ctx->sample_queue_cap > ctx->sample_queue_len)
                         ? (ctx->sample_queue_cap - ctx->sample_queue_len)
                         : 0;
        if (fit > 0) {
          memset(ctx->sample_queue + ctx->sample_queue_len, ctx->silence_byte,
                 fit);
          ctx->sample_queue_len += fit;
        }
      }
    }
    size_t missing = (needed_bytes > ctx->sample_queue_len)
                         ? (needed_bytes - ctx->sample_queue_len)
                         : 0;
    size_t to_read = (available < missing) ? available : missing;
    if (to_read > 0) {
      size_t read_bytes = spsc_byte_ring_buffer_consume(ctx->ring_buffer,
                                                        ctx->read_tmp, to_read);
      if (ensure_sample_queue_cap(ctx, ctx->sample_queue_len + read_bytes)) {
        memcpy(ctx->sample_queue + ctx->sample_queue_len, ctx->read_tmp,
               read_bytes);
        ctx->sample_queue_len += read_bytes;
      } else {
        size_t fit = (ctx->sample_queue_cap > ctx->sample_queue_len)
                         ? (ctx->sample_queue_cap - ctx->sample_queue_len)
                         : 0;
        if (fit > 0) {
          size_t copy_bytes = (read_bytes < fit) ? read_bytes : fit;
          memcpy(ctx->sample_queue + ctx->sample_queue_len, ctx->read_tmp,
                 copy_bytes);
          ctx->sample_queue_len += copy_bytes;
        }
      }
    }
  }

  // Copy interleaved data into per-channel ASIO buffers (de-interleave)
  size_t src_offset = 0;
  for (size_t frame = 0; frame < ctx->buffer_size; frame++) {
    for (size_t ch = 0; ch < ctx->num_channels; ch++) {
      void *out_ptr = ctx->buffer_infos[ch].buffers[buffer_index];
      if (out_ptr) {
        uint8_t *dst = (uint8_t *)out_ptr + frame * ctx->bytes_per_sample;
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
  device_buffer_estimator_add(&ctx->device_buffer, curr_buffer_fill);
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
  asio_capture_context_t *ctx =
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

  size_t total_bytes =
      ctx->buffer_size * ctx->num_channels * ctx->bytes_per_sample;
  if (ctx->transfer_buf_size != total_bytes) {
    logger_error(&g_logger,
                 "ASIO capture callback buffer size mismatch: scratch=%zu, "
                 "expected=%zu",
                 ctx->transfer_buf_size, total_bytes);
    return;
  }

  // Read from per-channel ASIO input buffers and interleave into transfer_buf
  for (size_t frame = 0; frame < ctx->buffer_size; frame++) {
    for (size_t ch = 0; ch < ctx->num_channels; ch++) {
      void *in_ptr = ctx->buffer_infos[ch].buffers[buffer_index];
      if (in_ptr) {
        const uint8_t *src =
            (const uint8_t *)in_ptr + frame * ctx->bytes_per_sample;
        size_t offset =
            (frame * ctx->num_channels + ch) * ctx->bytes_per_sample;
        memcpy(&ctx->transfer_buf[offset], src, ctx->bytes_per_sample);
      }
    }
  }

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

static void buffer_switch_combined(long buffer_index, ASIOBool direct_process) {
  buffer_switch_playback(buffer_index, direct_process);
  buffer_switch_capture(buffer_index, direct_process);
}

static void *buffer_switch_timeinfo_playback(void *params,
                                             long doubleBufferIndex,
                                             ASIOBool directProcess) {
  buffer_switch_playback(doubleBufferIndex, directProcess);
  return params;
}

static void *buffer_switch_timeinfo_capture(void *params,
                                            long doubleBufferIndex,
                                            ASIOBool directProcess) {
  buffer_switch_capture(doubleBufferIndex, directProcess);
  return params;
}

static void *buffer_switch_timeinfo_combined(void *params,
                                             long doubleBufferIndex,
                                             ASIOBool directProcess) {
  buffer_switch_combined(doubleBufferIndex, directProcess);
  return params;
}

/**
 * @brief ASIO sampleRateDidChange callback for a full-duplex stream.
 * Matches CamillaDSP device.rs:sample_rate_changed_combined.
 */
static void sample_rate_changed_combined(ASIOSampleRate s_rate) {
  (void)s_rate;
  atomic_store_explicit(&ASIO_PLAYBACK_RATE_CHANGED, true,
                        memory_order_release);
  atomic_store_explicit(&ASIO_CAPTURE_RATE_CHANGED, true, memory_order_release);
  logger_warn(&g_logger, "ASIO sampleRateDidChange callback received.");
}

/**
 * @brief ASIO sampleRateDidChange callback for a standalone playback stream.
 * Matches CamillaDSP device.rs:sample_rate_changed_playback.
 */
static void sample_rate_changed_playback(ASIOSampleRate s_rate) {
  (void)s_rate;
  atomic_store_explicit(&ASIO_PLAYBACK_RATE_CHANGED, true,
                        memory_order_release);
  logger_warn(
      &g_logger,
      "ASIO sampleRateDidChange callback received for the playback device.");
}

/**
 * @brief ASIO sampleRateDidChange callback for a standalone capture stream.
 * Matches CamillaDSP device.rs:sample_rate_changed_capture.
 */
static void sample_rate_changed_capture(ASIOSampleRate s_rate) {
  (void)s_rate;
  atomic_store_explicit(&ASIO_CAPTURE_RATE_CHANGED, true, memory_order_release);
  logger_warn(
      &g_logger,
      "ASIO sampleRateDidChange callback received for the capture device.");
}

/**
 * @brief Handle a driver message, mostly queries about supported features.
 * Matches CamillaDSP device.rs:handle_asio_message.
 */
static long handle_asio_message(long selector, long value, bool playback,
                                bool capture) {
  switch (selector) {
  case K_ASIO_SELECTOR_SUPPORTED:
    switch (value) {
    case K_ASIO_SELECTOR_SUPPORTED:
    case K_ASIO_ENGINE_VERSION:
    case K_ASIO_RESET_REQUEST:
    case K_ASIO_RESYNC_REQUEST:
    case K_ASIO_LATENCIES_CHANGED:
    case K_ASIO_SUPPORTS_TIME_INFO:
      return 1; // Supported
    case K_ASIO_BUFFER_SIZE_CHANGE:
    case K_ASIO_SUPPORTS_TIME_CODE:
      return 0; // Not supported
    }
    return 0; // Not supported
  case K_ASIO_ENGINE_VERSION:
    return 2; // ASIO 2.0
  case K_ASIO_SUPPORTS_TIME_INFO:
    return 1;
  case K_ASIO_SUPPORTS_TIME_CODE:
    return 0;
  case K_ASIO_RESET_REQUEST:
    // Answering 1 commits us to tearing the stream down and starting over, so
    // raise the flag the device loop watches. Doing the work here is not
    // allowed, this runs on the driver's own callback thread.
    logger_warn(&g_logger,
                "ASIO reset request received, restarting the stream.");
    if (playback) {
      atomic_store_explicit(&ASIO_PLAYBACK_RESET_REQUESTED, true,
                            memory_order_release);
    }
    if (capture) {
      atomic_store_explicit(&ASIO_CAPTURE_RESET_REQUESTED, true,
                            memory_order_release);
      asio_capture_context_t *cap_ctx =
          atomic_load_explicit(&CAPTURE_CONTEXT, memory_order_acquire);
      if (cap_ctx && cap_ctx->semaphore) {
        cdsp_sem_signal(cap_ctx->semaphore);
      }
    }
    return 1;
  case K_ASIO_BUFFER_SIZE_CHANGE:
    logger_warn(
        &g_logger,
        "ASIO buffer size change request received. Dynamic resize is not "
        "implemented in this backend.");
    return 0;
  case K_ASIO_RESYNC_REQUEST:
    // Deliberately nothing to do. This selector says the driver's timestamps
    // have gone invalid and asks the host to resynchronise its transport to
    // them, which matters to a sequencer. This backend never reads the Time
    // struct, it hands it straight back, so there is nothing here that can be
    // out of sync. The selector that asks for the driver to be torn down is
    // RESET_REQUEST, and that one is acted on above. Answering 1 without
    // acting matches what other hosts do, and stopping the stream over a
    // notification the driver considers recoverable would only turn it into a
    // dropout.
    logger_debug(&g_logger,
                 "ASIO resync request received, nothing to resynchronise.");
    return 1;
  case K_ASIO_LATENCIES_CHANGED:
    logger_debug(&g_logger, "ASIO latencies changed notification.");
    return 1;
  }
  logger_trace(&g_logger, "Unhandled ASIO message selector %ld.", selector);
  return 0;
}

/**
 * @brief asioMessage callback for full-duplex stream.
 * Matches CamillaDSP device.rs:asio_message_combined.
 */
static long asio_message_combined(long selector, long value, void *message,
                                  double *opt) {
  (void)message;
  (void)opt;
  return handle_asio_message(selector, value, true, true);
}

/**
 * @brief asioMessage callback for standalone playback stream.
 * Matches CamillaDSP device.rs:asio_message_playback.
 */
static long asio_message_playback(long selector, long value, void *message,
                                  double *opt) {
  (void)message;
  (void)opt;
  return handle_asio_message(selector, value, true, false);
}

/**
 * @brief asioMessage callback for standalone capture stream.
 * Matches CamillaDSP device.rs:asio_message_capture.
 */
static long asio_message_capture(long selector, long value, void *message,
                                 double *opt) {
  (void)message;
  (void)opt;
  return handle_asio_message(selector, value, false, true);
}

// MARK: - Full-Duplex Coordination matching CamillaDSP device.rs

typedef struct {
  char driver_name[256];
  long num_inputs;
  long num_outputs;
  long preferred_buf_size;

  ASIOBufferInfo *pending_output;
  size_t pending_output_channels;

  ASIOBufferInfo *pending_input;
  size_t pending_input_channels;

  bool stream_started;
  char setup_error[256];
  uint8_t active_count;

  ASIOBufferInfo *buffer_infos_for_driver;
  ASIOCallbacks callbacks_for_driver;
} asio_shared_state_t;

static struct {
  SRWLOCK lock;
  CONDITION_VARIABLE cond;
  asio_shared_state_t *state;
} g_asio_shared = {
    .lock = SRWLOCK_INIT, .cond = CONDITION_VARIABLE_INIT, .state = NULL};

static bool open_asio_device(const char *devname, int samplerate, bool is_dsd,
                             long *out_inputs, long *out_outputs,
                             backend_error_t *err);

/**
 * @brief init_shared_asio matching CamillaDSP device.rs:init_shared_asio.
 */
static bool init_shared_asio(const char *devname, int samplerate, bool is_dsd,
                             long *out_inputs, long *out_outputs,
                             long *out_preferred_buf, backend_error_t *err) {
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
    g_asio_shared.state->active_count += 1;
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
      (asio_shared_state_t *)calloc(1, sizeof(asio_shared_state_t));
  if (!g_asio_shared.state) {
    asio_driver_teardown(devname);
    ReleaseSRWLockExclusive(&g_asio_shared.lock);
    if (err) {
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to allocate memory for ASIO shared state");
    }
    return false;
  }
  snprintf(g_asio_shared.state->driver_name,
           sizeof(g_asio_shared.state->driver_name), "%s", devname);
  g_asio_shared.state->num_inputs = num_inputs;
  g_asio_shared.state->num_outputs = num_outputs;
  g_asio_shared.state->preferred_buf_size = preferred_buf;
  g_asio_shared.state->stream_started = false;
  g_asio_shared.state->active_count = 1;

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
                              ASIOBufferInfo **out_buffer_infos,
                              long *out_buf_size, backend_error_t *err) {
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

  ASIOBufferInfo *my_infos = make_buffer_infos(num_channels, is_input);
  if (!my_infos) {
    snprintf(g_asio_shared.state->setup_error,
             sizeof(g_asio_shared.state->setup_error),
             "Failed to allocate buffer infos");
    WakeAllConditionVariable(&g_asio_shared.cond);
    ReleaseSRWLockExclusive(&g_asio_shared.lock);
    if (err) {
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to allocate buffer infos");
    }
    return false;
  }
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
    const char *devname = g_asio_shared.state->driver_name;
    size_t out_ch = g_asio_shared.state->pending_output_channels;
    size_t in_ch = g_asio_shared.state->pending_input_channels;
    long preferred_buf = g_asio_shared.state->preferred_buf_size;
    size_t total_ch = out_ch + in_ch;

    ASIOBufferInfo *combined =
        (ASIOBufferInfo *)calloc(total_ch, sizeof(ASIOBufferInfo));
    if (!combined) {
      snprintf(g_asio_shared.state->setup_error,
               sizeof(g_asio_shared.state->setup_error),
               "Failed to allocate combined buffer infos");
      WakeAllConditionVariable(&g_asio_shared.cond);
      ReleaseSRWLockExclusive(&g_asio_shared.lock);
      if (err) {
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                           "Failed to allocate combined buffer infos");
      }
      return false;
    }
    memcpy(combined, g_asio_shared.state->pending_output,
           out_ch * sizeof(ASIOBufferInfo));
    memcpy(combined + out_ch, g_asio_shared.state->pending_input,
           in_ch * sizeof(ASIOBufferInfo));

    g_asio_shared.state->callbacks_for_driver.bufferSwitch =
        buffer_switch_combined;
    g_asio_shared.state->callbacks_for_driver.sampleRateDidChange =
        sample_rate_changed_combined;
    g_asio_shared.state->callbacks_for_driver.asioMessage =
        asio_message_combined;
    g_asio_shared.state->callbacks_for_driver.bufferSwitchTimeInfo =
        buffer_switch_timeinfo_combined;

    if (!create_asio_buffers(devname, combined, (long)total_ch, preferred_buf,
                             &g_asio_shared.state->callbacks_for_driver, err)) {
      snprintf(g_asio_shared.state->setup_error,
               sizeof(g_asio_shared.state->setup_error),
               "createBuffers failed in full-duplex setup: %s",
               (err && err->message[0]) ? err->message : "unknown error");
      free(combined);
      WakeAllConditionVariable(&g_asio_shared.cond);
      ReleaseSRWLockExclusive(&g_asio_shared.lock);
      return false;
    }

    // Update global playback/capture context buffer_infos
    asio_playback_context_t *pb_ctx =
        atomic_load_explicit(&PLAYBACK_CONTEXT, memory_order_acquire);
    if (pb_ctx) {
      pb_ctx->buffer_infos = combined;
    }
    asio_capture_context_t *cap_ctx =
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
               "Failed to start ASIO stream: %s",
               (err && err->message[0]) ? err->message : "unknown error");
      WakeAllConditionVariable(&g_asio_shared.cond);
      ReleaseSRWLockExclusive(&g_asio_shared.lock);
      return false;
    }

    logger_debug(&g_logger, "Full-duplex ASIO stream started.");
    g_asio_shared.state->stream_started = true;
    g_asio_shared.state->setup_error[0] = '\0';
    WakeAllConditionVariable(&g_asio_shared.cond);
  } else {
    logger_debug(&g_logger,
                 "Waiting for other ASIO side to register for full-duplex...");
    DWORD timeout_ms = 10000;
    DWORD start_tick = GetTickCount();
    while (g_asio_shared.state && !g_asio_shared.state->stream_started &&
           g_asio_shared.state->setup_error[0] == '\0') {
      DWORD elapsed = GetTickCount() - start_tick;
      if (elapsed >= timeout_ms) {
        break;
      }
      SleepConditionVariableSRW(&g_asio_shared.cond, &g_asio_shared.lock,
                                timeout_ms - elapsed, 0);
    }
    if (!g_asio_shared.state || !g_asio_shared.state->stream_started) {
      if (err) {
        char msg[384];
        if (g_asio_shared.state &&
            g_asio_shared.state->setup_error[0] != '\0') {
          snprintf(msg, sizeof(msg), "ASIO full-duplex setup aborted: %s",
                   g_asio_shared.state->setup_error);
        } else if (!g_asio_shared.state) {
          snprintf(msg, sizeof(msg),
                   "ASIO full-duplex setup aborted: the other side gave up "
                   "without reporting why");
        } else {
          snprintf(msg, sizeof(msg),
                   "Timed out after 10 seconds waiting for the other side of "
                   "the full-duplex ASIO stream to finish setting up");
        }
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
 * @brief Give up this side's claim on the shared state. When the last one goes,
 * stop the ASIO stream and clear the shared state so a fresh session can be
 * started later.
 * Matches CamillaDSP device.rs:release_shared_asio.
 */
static void release_shared_asio(void) {
  AcquireSRWLockExclusive(&g_asio_shared.lock);
  if (g_asio_shared.state) {
    if (g_asio_shared.state->active_count > 0) {
      g_asio_shared.state->active_count--;
    }
    if (g_asio_shared.state->active_count == 1) {
      logger_debug(&g_logger, "First ASIO side exiting, stopping stream.");
      atomic_store_explicit(&PLAYBACK_CONTEXT, NULL, memory_order_release);
      atomic_store_explicit(&CAPTURE_CONTEXT, NULL, memory_order_release);
      if (g_asio_shared.state->stream_started) {
        stop_asio_stream(g_asio_shared.state->driver_name);
      }
    } else if (g_asio_shared.state->active_count == 0) {
      logger_debug(
          &g_logger,
          "Last ASIO side exiting, cleaning up driver and shared state.");
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

/**
 * @brief Give up this side's claim on the shared state after a failed
 * full-duplex setup. Matches CamillaDSP device.rs:abort_shared_asio.
 */
static void abort_shared_asio(const char *msg) {
  AcquireSRWLockExclusive(&g_asio_shared.lock);
  if (g_asio_shared.state) {
    if (g_asio_shared.state->setup_error[0] == '\0') {
      snprintf(g_asio_shared.state->setup_error,
               sizeof(g_asio_shared.state->setup_error), "%s",
               (msg && msg[0])
                   ? msg
                   : "the other side gave up without reporting why");
    }
  }
  WakeAllConditionVariable(&g_asio_shared.cond);
  ReleaseSRWLockExclusive(&g_asio_shared.lock);
  release_shared_asio();
}

/**
 * @brief Dispose the buffers and release the driver after a failed stream
 * setup. Matches CamillaDSP device.rs:cleanup_failed_setup.
 */
static void cleanup_failed_setup(const char *devname) {
  dispose_asio_buffers(devname);
  asio_driver_teardown(devname);
}

// MARK: - Low-level ASIO helpers matching CamillaDSP device.rs

/**
 * @brief Log the name and sample format of each channel in one direction.
 * Matches device.rs:log_channel_details.
 */
static void log_channel_details(IASIO *iasio, long num_channels,
                                bool is_input) {
  const char *direction = is_input ? "Input " : "Output";
  for (long ch = 0; ch < num_channels; ch++) {
    ASIOChannelInfo info = {0};
    info.channel = (int32_t)ch;
    info.isInput = is_input ? ASIOTrue : ASIOFalse;
    if (asio_ok(iasio->lpVtbl->getChannelInfo(iasio, &info))) {
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
static bool open_asio_device(const char *devname, int samplerate, bool is_dsd,
                             long *out_inputs, long *out_outputs,
                             backend_error_t *err) {
  logger_trace(&g_logger,
               "open_asio_device: dev='%s', samplerate=%d, is_dsd=%d", devname,
               samplerate, (int)is_dsd);

  char available[64][256];
  int avail_count = asio_list_device_names(available, 64);
  char avail_str[1024] = {0};
  size_t avail_str_offset = 0;
  int n = snprintf(avail_str + avail_str_offset,
                   sizeof(avail_str) - avail_str_offset, "[");
  if (n > 0 && (size_t)n < sizeof(avail_str) - avail_str_offset) {
    avail_str_offset += (size_t)n;
  }
  for (int i = 0; i < avail_count; i++) {
    n = snprintf(avail_str + avail_str_offset,
                 sizeof(avail_str) - avail_str_offset,
                 (i > 0 ? ", \"%s\"" : "\"%s\""), available[i]);
    if (n > 0 && (size_t)n < sizeof(avail_str) - avail_str_offset) {
      avail_str_offset += (size_t)n;
    } else {
      break;
    }
  }
  if (avail_str_offset < sizeof(avail_str) - 1) {
    snprintf(avail_str + avail_str_offset, sizeof(avail_str) - avail_str_offset,
             "]");
  } else {
    avail_str[sizeof(avail_str) - 2] = ']';
    avail_str[sizeof(avail_str) - 1] = '\0';
  }
  logger_debug(&g_logger, "Available ASIO devices: %s", avail_str);

  backend_error_t load_err = {0};
  IASIO *iasio = NULL;
  if (!asio_driver_load_by_name(devname, &iasio, &load_err)) {
    // A refused driver is not a missing one, and its message already says what
    // to do.
    if (asio_is_unsupported_driver(devname)) {
      if (err) {
        *err = load_err;
      }
      return false;
    }
    bool exact_match = false;
    for (int i = 0; i < avail_count; i++) {
      if (strcmp(available[i], devname) == 0) {
        exact_match = true;
        break;
      }
    }
    const char *err_desc =
        load_err.message[0] ? load_err.message : "driver load failed";
    const char *hint =
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
    if (asio_ok(io_res)) {
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
  if (!asio_ok(rate_res)) {
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
    if (asio_ok(iasio->lpVtbl->canSampleRate(iasio, check_rate))) {
      offset += snprintf(supported_str + offset, sizeof(supported_str) - offset,
                         (offset > 1 ? ", %u" : "%u"), STANDARD_RATES[r]);
    }
  }
  snprintf(supported_str + offset, sizeof(supported_str) - offset, "]");
  logger_debug(&g_logger, "ASIO supported sample rates: %s", supported_str);

  // Set the requested sample rate IMMEDIATELY after init, before getChannels.
  // Some drivers lock in the rate once channels or buffers are queried.
  double rate = (double)(is_dsd ? (samplerate * 32) : samplerate);
  if (!asio_ok(iasio->lpVtbl->canSampleRate(iasio, rate))) {
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
    if (!asio_ok(set_res)) {
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

    // Workaround for drivers that accept a new rate but do not act on it. The
    // only one we have seen do this is the Steinberg built-in (generic) driver,
    // version 1.0.9, the latest as of August 2026. It returns success from
    // setSampleRate and then reports the new rate from getSampleRate, while the
    // hardware keeps clocking at the old one. Only measuring the callback rate
    // exposes it, the driver's own report does not. Asking it to go from 48 to
    // 96 kHz gave a measured rate of 48 kHz, and 44.1 to 48 kHz stayed at 44.1
    // kHz, while moving down from 96 kHz worked without any of this. It behaved
    // the same with two different WASAPI devices under it, so this looks like
    // the driver itself rather than the endpoint it wraps. A MOTU M series
    // switches in both directions on its own, so this is not the norm.
    //
    // Recreating the instance is the only thing found that makes the rate
    // stick, the requested value does survive into the next instance. Setting
    // the rate twice, waiting a second, re-running ASIOInit and a start/stop
    // cycle were all tried on the same instance and changed nothing. Disposing
    // and recreating the buffers is not an option here, the rate is set right
    // after ASIOInit and no buffers exist yet.
    //
    // Keep the teardown a real drop: releasing the COM object is what resets
    // the driver, so leaking the handle instead would silently break this.
    //
    // Only done for drivers known to need it. The quirk looks rare, PortAudio
    // has no handling for it at all, and recreating an instance carries its own
    // risk.
    if (asio_needs_rate_reload(devname)) {
      asio_driver_teardown(devname);
      if (!asio_driver_load_by_name(devname, &iasio, err)) {
        return false;
      }
    } else {
      logger_debug(
          &g_logger,
          "Not reinitialising '%s' to apply the sample rate, this driver is "
          "not known to need it.",
          devname);
    }

    if (is_dsd) {
      ASIOIoFormat dsd_format = {0};
      dsd_format.FormatType = kASIOFormatDSD;
      iasio->lpVtbl->future(iasio, kAsioSetIoFormat, &dsd_format);
    }

    double after_set = 0.0;
    long after_res = iasio->lpVtbl->getSampleRate(iasio, &after_set);
    if (!asio_ok(after_res)) {
      asio_driver_teardown(devname);
      if (err) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "Failed to read ASIO sample rate after setting it: %ld",
                 after_res);
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
      }
      return false;
    }
    if (fabs(after_set - rate) > 0.5) {
      asio_driver_teardown(devname);
      if (err) {
        char msg[384];
        snprintf(msg, sizeof(msg),
                 "ASIO device still reports %.0f Hz after being asked for %d "
                 "Hz. The driver may require the rate to be set from its own "
                 "control panel.",
                 after_set, samplerate);
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
      }
      return false;
    }
    logger_debug(&g_logger, "ASIO sample rate %d Hz applied.", samplerate);
  }

  // Query channels AFTER the sample rate has been set
  long num_inputs = 0, num_outputs = 0;
  long channels_res =
      iasio->lpVtbl->getChannels(iasio, &num_inputs, &num_outputs);
  if (!asio_ok(channels_res)) {
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
static bool open_asio_playback(const char *devname, size_t num_channels,
                               int samplerate,
                               asio_sample_format_t configured_format,
                               bool has_format,
                               asio_sample_format_t *out_resolved_format,
                               backend_error_t *err) {
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
static bool open_asio_capture(const char *devname, size_t num_channels,
                              int samplerate,
                              asio_sample_format_t configured_format,
                              bool has_format,
                              asio_sample_format_t *out_resolved_format,
                              backend_error_t *err) {
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
  bool shared_claimed;
  int target_level;

  asio_sample_format_t resolved_format;
  bool is_lsb;
  size_t bytes_per_sample;
  long actual_buffer_size;

  ASIOBufferInfo *buffer_infos;
  bool single_mode_allocated_infos;
  ASIOCallbacks callbacks_for_driver;

  spsc_byte_ring_buffer_t *ring_buffer;
  uint8_t *encode_buf;
  size_t encode_buf_size;

  asio_playback_context_t *context;
  _Atomic bool is_running;
  _Atomic bool stopped;
  _Atomic bool paused;
  bool com_initialized;
};

static void asio_playback_close(void *ctx) {
  asio_playback_t *playback = (asio_playback_t *)ctx;
  if (!playback)
    return;

  logger_debug(&g_logger, "Stopping ASIO playback.");
  if (playback->full_duplex) {
    if (playback->shared_claimed) {
      release_shared_asio();
      playback->shared_claimed = false;
    }
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
}

/**
 * @brief open matching CamillaDSP device.rs AsioPlaybackDevice::start.
 */
static bool asio_playback_open(void *ctx, backend_error_t *err) {
  asio_playback_t *playback = (asio_playback_t *)ctx;
  if (!playback)
    return false;

  if (!asio_com_init_this_thread(err)) {
    return false;
  }
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
    playback->shared_claimed = true;
    if (playback->channels > (size_t)outputs) {
      if (err) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "Requested %zu output channels but device only has %ld",
                 playback->channels, outputs);
        backend_error_init(err, BACKEND_ERROR_INVALID_CHANNELS, msg);
        abort_shared_asio(msg);
      } else {
        abort_shared_asio("Invalid output channels");
      }
      playback->shared_claimed = false;
      goto error_cleanup;
    }
    if (!resolve_format(playback->device, playback->format,
                        playback->has_format, false, &resolved_format, err)) {
      abort_shared_asio(err ? err->message : "Format resolution failed");
      playback->shared_claimed = false;
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
  bool is_lsb = asio_device_is_dsd_lsb(playback->device, false);

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
  playback->encode_buf = (uint8_t *)malloc(playback->encode_buf_size);

  clear_playback_driver_events();
  reset_playback_callback_seen();

  size_t target_level = (playback->target_level > 0)
                            ? (size_t)playback->target_level
                            : (size_t)playback->chunk_size;

  size_t bytes_per_frame = playback->bytes_per_sample * playback->channels;
  size_t asio_buf_frames = (size_t)asio_buffer_size;

  playback->context =
      (asio_playback_context_t *)calloc(1, sizeof(asio_playback_context_t));
  if (!playback->ring_buffer || !playback->encode_buf || !playback->context) {
    if (err) {
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to allocate playback buffers or context");
    }
    if (playback->full_duplex && playback->shared_claimed) {
      abort_shared_asio("Failed to allocate playback buffers or context");
      playback->shared_claimed = false;
    }
    goto error_cleanup;
  }

  playback->context->ring_buffer = playback->ring_buffer;
  playback->context->num_channels = playback->channels;
  playback->context->buffer_size = asio_buf_frames;
  playback->context->bytes_per_sample = playback->bytes_per_sample;
  playback->context->read_tmp =
      (uint8_t *)calloc(1, asio_buf_frames * bytes_per_frame);

  size_t initial_queue_cap =
      (16 * ring_frames + target_level + asio_buf_frames * 2) * bytes_per_frame;
  if (initial_queue_cap < asio_buf_frames * bytes_per_frame * 4) {
    initial_queue_cap = asio_buf_frames * bytes_per_frame * 4;
  }
  playback->context->sample_queue = (uint8_t *)malloc(initial_queue_cap);
  if (!playback->context->read_tmp || !playback->context->sample_queue) {
    if (err) {
      backend_error_init(
          err, BACKEND_ERROR_INITIALIZATION_FAILED,
          "Failed to allocate playback sample queue or temporary buffer");
    }
    if (playback->full_duplex && playback->shared_claimed) {
      abort_shared_asio(
          "Failed to allocate playback sample queue or temporary buffer");
      playback->shared_claimed = false;
    }
    goto error_cleanup;
  }

  playback->context->sample_queue_len = 0;
  playback->context->sample_queue_cap = initial_queue_cap;
  atomic_init(&playback->context->target_level, target_level);
  playback->context->running = false;
  playback->context->silence_byte =
      (resolved_format == ASIO_SAMPLE_FORMAT_DSD_INT8) ? 0x69 : 0x00;
  device_buffer_estimator_init(&playback->context->device_buffer,
                               (double)playback->sample_rate);

  if (playback->full_duplex) {
    atomic_store_explicit(&PLAYBACK_CONTEXT, playback->context,
                          memory_order_release);
    if (!register_and_wait(false, playback->channels, &playback->buffer_infos,
                           &playback->actual_buffer_size, err)) {
      atomic_store_explicit(&PLAYBACK_CONTEXT, NULL, memory_order_release);
      abort_shared_asio(err ? err->message
                            : "Full-duplex playback setup aborted");
      playback->shared_claimed = false;
      goto error_cleanup;
    }
    if (resolved_format == ASIO_SAMPLE_FORMAT_DSD_INT8) {
      playback->actual_buffer_size /= 32;
    }
  } else {
    playback->buffer_infos = make_buffer_infos(playback->channels, false);
    if (!playback->buffer_infos) {
      if (err) {
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                           "Failed to allocate ASIO buffer infos");
      }
      cleanup_failed_setup(playback->device);
      goto error_cleanup;
    }
    playback->single_mode_allocated_infos = true;
    playback->callbacks_for_driver.bufferSwitch = buffer_switch_playback;
    playback->callbacks_for_driver.sampleRateDidChange =
        sample_rate_changed_playback;
    playback->callbacks_for_driver.asioMessage = asio_message_playback;
    playback->callbacks_for_driver.bufferSwitchTimeInfo =
        buffer_switch_timeinfo_playback;

    if (!create_asio_buffers(playback->device, playback->buffer_infos,
                             (long)playback->channels, driver_buffer_size,
                             &playback->callbacks_for_driver, err)) {
      cleanup_failed_setup(playback->device);
      goto error_cleanup;
    }

    playback->context->buffer_infos = playback->buffer_infos;
    atomic_store_explicit(&PLAYBACK_CONTEXT, playback->context,
                          memory_order_release);

    log_asio_latencies(playback->device);

    logger_trace(&g_logger, "Playback: starting the stream");
    if (!start_asio_stream(playback->device, err)) {
      atomic_store_explicit(&PLAYBACK_CONTEXT, NULL, memory_order_release);
      cleanup_failed_setup(playback->device);
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
static bool asio_playback_write(void *ctx, const audio_chunk_t *chunk,
                                backend_error_t *err) {
  asio_playback_t *playback = (asio_playback_t *)ctx;
  if (!playback)
    return false;

  // The driver asked for a reset and the callback promised one, so stop
  // here and let the engine reopen the device. Matches CamillaDSP device.rs.
  if (take_playback_reset_request()) {
    logger_warn(&g_logger,
                "The ASIO driver requested a reset of the playback stream.");
    atomic_store_explicit(&playback->stopped, true, memory_order_release);
    if (err) {
      backend_error_init(
          err, BACKEND_ERROR_WRITE_ERROR,
          "The ASIO driver requested a reset of the playback stream");
    }
    return false;
  }

  size_t blockalign = playback->channels * playback->bytes_per_sample;
  size_t sleep_duration_us =
      (size_t)(1000000ULL * (unsigned long long)playback->chunk_size /
               (unsigned long long)playback->sample_rate / 2ULL);
  uint32_t sleep_ms = (uint32_t)(sleep_duration_us / 1000);
  if (sleep_ms == 0)
    sleep_ms = 1;

  return audio_backend_ring_buffer_write(
      playback->ring_buffer, playback->encode_buf, playback->encode_buf_size,
      blockalign, chunk,
      asio_sample_format_to_binary_format(playback->resolved_format,
                                          playback->is_lsb),
      playback->channels, sleep_ms, 8, &playback->is_running,
      &playback->stopped, &playback->paused, &ASIO_PLAYBACK_RATE_CHANGED, err);
}

static size_t asio_playback_get_buffer_level(void *ctx) {
  asio_playback_t *playback = (asio_playback_t *)ctx;
  if (!playback || !playback->context)
    return 0;
  // The published value already covers the ring buffer as well as the
  // callback-local queue, so it is the complete level on its own.
  return device_buffer_estimator_estimate(&playback->context->device_buffer);
}

static bool asio_playback_get_pending_rate_change(void *ctx, double *out_rate) {
  asio_playback_t *playback = (asio_playback_t *)ctx;
  if (!playback)
    return false;
  if (take_playback_rate_change_event()) {
    int new_rate = read_current_asio_sample_rate_hz(playback->device);
    if (out_rate) {
      *out_rate = (double)new_rate;
    }
    return true;
  }
  return false;
}

static bool asio_playback_prefill_silence(void *ctx, size_t frames,
                                          backend_error_t *err) {
  (void)err;
  asio_playback_t *playback = (asio_playback_t *)ctx;
  if (!playback)
    return false;
  playback->target_level = (int)frames;
  if (playback->context) {
    atomic_store_explicit(&playback->context->target_level, frames,
                          memory_order_release);
  }
  return true;
}

static void asio_playback_stop(void *ctx) {
  asio_playback_t *playback = (asio_playback_t *)ctx;
  if (!playback)
    return;
  atomic_store_explicit(&playback->stopped, true, memory_order_release);
}

static bool asio_playback_get_is_paused(void *ctx) {
  asio_playback_t *playback = (asio_playback_t *)ctx;
  if (!playback)
    return false;
  return atomic_load_explicit(&playback->paused, memory_order_acquire);
}

static void asio_playback_set_is_paused(void *ctx, bool paused) {
  asio_playback_t *playback = (asio_playback_t *)ctx;
  if (!playback)
    return;
  atomic_store_explicit(&playback->paused, paused, memory_order_release);
}

static void asio_playback_destroy(void *ctx) {
  asio_playback_t *playback = (asio_playback_t *)ctx;
  if (playback) {
    asio_playback_close(playback);
    free(playback);
  }
}

static playback_backend_t *
asio_playback_create(const playback_device_config_t *config, int sample_rate,
                     int chunk_size, bool full_duplex,
                     processing_parameters_t *params, backend_error_t *err) {
  (void)params;
  (void)err;
  asio_playback_t *playback =
      (asio_playback_t *)calloc(1, sizeof(asio_playback_t));
  if (!playback)
    return NULL;

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

  playback_backend_t *backend =
      (playback_backend_t *)calloc(1, sizeof(playback_backend_t));
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
  bool shared_claimed;

  asio_sample_format_t resolved_format;
  bool is_lsb;
  size_t bytes_per_sample;
  long actual_buffer_size;

  ASIOBufferInfo *buffer_infos;
  bool single_mode_allocated_infos;
  ASIOCallbacks callbacks_for_driver;

  spsc_byte_ring_buffer_t *ring_buffer;
  cdsp_sem_t semaphore;
  uint8_t *decode_buf;
  size_t decode_buf_size;

  asio_capture_context_t *context;
  _Atomic bool is_running;
  _Atomic bool stopped;
  bool com_initialized;
};

static void asio_capture_close(void *ctx) {
  asio_capture_t *capture = (asio_capture_t *)ctx;
  if (!capture)
    return;

  // Close the gate first, so callbacks arriving during teardown do no work.
  // Matches device.rs:CAPTURE_STREAM_ACTIVE.
  atomic_store_explicit(&CAPTURE_STREAM_ACTIVE, false, memory_order_release);

  logger_debug(&g_logger, "Stopping ASIO capture.");
  if (capture->full_duplex) {
    if (capture->shared_claimed) {
      release_shared_asio();
      capture->shared_claimed = false;
    }
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
    cdsp_sem_signal(capture->semaphore);
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
}

/**
 * @brief open matching CamillaDSP device.rs AsioCaptureDevice::start.
 */
static bool asio_capture_open(void *ctx, backend_error_t *err) {
  asio_capture_t *capture = (asio_capture_t *)ctx;
  if (!capture)
    return false;

  if (!asio_com_init_this_thread(err)) {
    return false;
  }
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
    capture->shared_claimed = true;
    if (capture->channels > (size_t)inputs) {
      char msg[256];
      snprintf(msg, sizeof(msg),
               "Requested %zu input channels but device only has %ld",
               capture->channels, inputs);
      if (err) {
        backend_error_init(err, BACKEND_ERROR_INVALID_CHANNELS, msg);
      }
      abort_shared_asio(msg);
      capture->shared_claimed = false;
      goto error_cleanup;
    }
    if (!resolve_format(capture->device, capture->format, capture->has_format,
                        true, &resolved_format, err)) {
      abort_shared_asio(err ? err->message : "Format resolution failed");
      capture->shared_claimed = false;
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
  bool is_lsb = asio_device_is_dsd_lsb(capture->device, true);

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
  capture->decode_buf = (uint8_t *)malloc(capture->decode_buf_size);

  clear_capture_driver_events();
  // Keep the callback from pushing until the loop is ready to consume
  atomic_store_explicit(&CAPTURE_STREAM_ACTIVE, false, memory_order_release);

  capture->context =
      (asio_capture_context_t *)calloc(1, sizeof(asio_capture_context_t));
  if (capture->context) {
    capture->context->ring_buffer = capture->ring_buffer;
    capture->context->semaphore = capture->semaphore;
    capture->context->num_channels = capture->channels;
    capture->context->buffer_size = (size_t)asio_buffer_size;
    capture->context->bytes_per_sample = capture->bytes_per_sample;
    capture->context->transfer_buf_size = (size_t)asio_buffer_size *
                                          capture->bytes_per_sample *
                                          capture->channels;
    capture->context->transfer_buf =
        (uint8_t *)malloc(capture->context->transfer_buf_size);
  }

  if (!capture->ring_buffer || !capture->semaphore || !capture->decode_buf ||
      !capture->context || !capture->context->transfer_buf) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to allocate capture buffers or semaphore");
    if (capture->full_duplex && capture->shared_claimed) {
      abort_shared_asio("Failed to allocate capture buffers or semaphore");
      capture->shared_claimed = false;
    }
    goto error_cleanup;
  }

  if (capture->full_duplex) {
    atomic_store_explicit(&CAPTURE_CONTEXT, capture->context,
                          memory_order_release);
    if (!register_and_wait(true, capture->channels, &capture->buffer_infos,
                           &capture->actual_buffer_size, err)) {
      atomic_store_explicit(&CAPTURE_CONTEXT, NULL, memory_order_release);
      abort_shared_asio(err ? err->message
                            : "Full-duplex capture setup aborted");
      capture->shared_claimed = false;
      goto error_cleanup;
    }
    if (resolved_format == ASIO_SAMPLE_FORMAT_DSD_INT8) {
      capture->actual_buffer_size /= 32;
    }
  } else {
    capture->buffer_infos = make_buffer_infos(capture->channels, true);
    if (!capture->buffer_infos) {
      if (err) {
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                           "Failed to allocate ASIO buffer infos");
      }
      cleanup_failed_setup(capture->device);
      goto error_cleanup;
    }
    capture->single_mode_allocated_infos = true;
    capture->callbacks_for_driver.bufferSwitch = buffer_switch_capture;
    capture->callbacks_for_driver.sampleRateDidChange =
        sample_rate_changed_capture;
    capture->callbacks_for_driver.asioMessage = asio_message_capture;
    capture->callbacks_for_driver.bufferSwitchTimeInfo =
        buffer_switch_timeinfo_capture;

    if (!create_asio_buffers(capture->device, capture->buffer_infos,
                             (long)capture->channels, driver_buffer_size,
                             &capture->callbacks_for_driver, err)) {
      cleanup_failed_setup(capture->device);
      goto error_cleanup;
    }

    capture->context->buffer_infos = capture->buffer_infos;
    atomic_store_explicit(&CAPTURE_CONTEXT, capture->context,
                          memory_order_release);

    logger_trace(&g_logger, "Capture: starting the stream");
    if (!start_asio_stream(capture->device, err)) {
      atomic_store_explicit(&CAPTURE_CONTEXT, NULL, memory_order_release);
      cleanup_failed_setup(capture->device);
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
static bool asio_capture_read(void *ctx, size_t frames, audio_chunk_t *chunk,
                              backend_error_t *err) {
  asio_capture_t *capture = (asio_capture_t *)ctx;
  if (!capture)
    return false;

  // The driver asked for a reset and the callback promised one, so stop
  // here and let the engine reopen the device. Matches CamillaDSP device.rs.
  if (take_capture_reset_request()) {
    logger_warn(&g_logger,
                "The ASIO driver requested a reset of the capture stream.");
    atomic_store_explicit(&capture->stopped, true, memory_order_release);
    if (err) {
      backend_error_init(
          err, BACKEND_ERROR_READ_ERROR,
          "The ASIO driver requested a reset of the capture stream");
    }
    return false;
  }

  size_t blockalign = capture->channels * capture->bytes_per_sample;
  return audio_backend_ring_buffer_read(
      capture->ring_buffer, capture->decode_buf, capture->decode_buf_size,
      blockalign, frames,
      asio_sample_format_to_binary_format(capture->resolved_format,
                                          capture->is_lsb),
      capture->channels, &capture->is_running, &capture->stopped,
      &ASIO_CAPTURE_RATE_CHANGED, chunk, err);
}

static bool asio_capture_wait_for_data(void *ctx, uint32_t timeout_ms) {
  asio_capture_t *capture = (asio_capture_t *)ctx;
  if (!capture || !capture->semaphore)
    return false;
  if (atomic_load_explicit(&capture->stopped, memory_order_acquire))
    return false;
  return cdsp_sem_timedwait(capture->semaphore, timeout_ms);
}

static bool asio_capture_get_pending_rate_change(void *ctx, double *out_rate) {
  asio_capture_t *capture = (asio_capture_t *)ctx;
  if (!capture)
    return false;
  if (take_capture_rate_change_event()) {
    int new_rate = read_current_asio_sample_rate_hz(capture->device);
    if (out_rate) {
      *out_rate = (double)new_rate;
    }
    return true;
  }
  return false;
}

static void asio_capture_stop(void *ctx) {
  asio_capture_t *capture = (asio_capture_t *)ctx;
  if (!capture)
    return;
  atomic_store_explicit(&capture->stopped, true, memory_order_release);
  if (capture->semaphore) {
    cdsp_sem_signal(capture->semaphore);
  }
}

static void asio_capture_destroy(void *ctx) {
  asio_capture_t *capture = (asio_capture_t *)ctx;
  if (capture) {
    asio_capture_close(capture);
    free(capture);
  }
}

static capture_backend_t *
asio_capture_create(const capture_device_config_t *config, int sample_rate,
                    int chunk_size, bool full_duplex,
                    processing_parameters_t *params, backend_error_t *err) {
  (void)params;
  (void)err;
  asio_capture_t *capture = (asio_capture_t *)calloc(1, sizeof(asio_capture_t));
  if (!capture)
    return NULL;

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

  capture_backend_t *backend =
      (capture_backend_t *)calloc(1, sizeof(capture_backend_t));
  if (!backend) {
    free(capture);
    return NULL;
  }
  backend->ctx = capture;
  backend->vtable = &g_asio_capture_vtable;
  backend->is_realtime = true;
  return backend;
}

static void asio_capture_set_is_paused(void *ctx, bool paused) {
  asio_capture_t *capture = (asio_capture_t *)ctx;
  if (!capture)
    return;
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

#endif // ENABLE_ASIO
