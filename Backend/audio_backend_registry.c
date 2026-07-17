#include "audio_backend_registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#if defined(ENABLE_COREAUDIO)
#include "core_audio_capabilities.h"
#endif
#if defined(ENABLE_ALSA)
#include "alsa_capabilities.h"
#endif
#if defined(ENABLE_WASAPI)
#include "wasapi_capabilities.h"
#endif
#if defined(ENABLE_ASIO)
#include "asio_capabilities.h"
#endif

int audio_backend_registry_get_available_devices(const char* backend,
                                                 bool input,
                                                 audio_device_t* out_devices,
                                                 int max_devices) {
  if (!backend) return 0;
  if (strcasecmp(backend, "coreaudio") == 0) {
#if defined(ENABLE_COREAUDIO)
    char names[32][256];
    int count =
        core_audio_capabilities_available_device_names(input, names, 32);
    if (out_devices && count > max_devices) count = max_devices;
    for (int i = 0; i < count; i++) {
      if (out_devices) {
        memcpy(out_devices[i].name, names[i], sizeof(out_devices[i].name));
        out_devices[i].name[sizeof(out_devices[i].name) - 1] = '\0';
      }
    }
    return count;
#else
    return 0;
#endif
  } else if (strcasecmp(backend, "alsa") == 0) {
#if defined(ENABLE_ALSA)
    char names[32][256];
    int count = alsa_capabilities_available_device_names(input, names, 32);
    if (out_devices && count > max_devices) count = max_devices;
    for (int i = 0; i < count; i++) {
      if (out_devices) {
        memcpy(out_devices[i].name, names[i], sizeof(out_devices[i].name));
        out_devices[i].name[sizeof(out_devices[i].name) - 1] = '\0';
      }
    }
    return count;
#else
    return 0;
#endif
  } else if (strcasecmp(backend, "wasapi") == 0) {
#if defined(ENABLE_WASAPI)
    char names[32][256];
    int count = wasapi_capabilities_available_device_names(input, names, 32);
    if (out_devices && count > max_devices) count = max_devices;
    for (int i = 0; i < count; i++) {
      if (out_devices) {
        memcpy(out_devices[i].name, names[i], sizeof(out_devices[i].name));
        out_devices[i].name[sizeof(out_devices[i].name) - 1] = '\0';
      }
    }
    return count;
#else
    return 0;
#endif
  } else if (strcasecmp(backend, "asio") == 0) {
#if defined(ENABLE_ASIO)
    char names[32][256];
    int count = asio_capabilities_available_device_names(input, names, 32);
    if (out_devices && count > max_devices) count = max_devices;
    for (int i = 0; i < count; i++) {
      if (out_devices) {
        memcpy(out_devices[i].name, names[i], sizeof(out_devices[i].name));
        out_devices[i].name[sizeof(out_devices[i].name) - 1] = '\0';
      }
    }
    return count;
#else
    return 0;
#endif
  }
  return 0;
}

audio_device_descriptor_t* audio_backend_registry_get_device_capabilities(
    const char* backend, const char* device, bool is_capture,
    device_error_t* err) {
  if (!backend || !device) {
    if (err) {
      device_error_init(err, DEVICE_ERROR_OTHER,
                        "Invalid backend or device name");
    }
    return NULL;
  }
  if (strcasecmp(backend, "coreaudio") == 0) {
#if defined(ENABLE_COREAUDIO)
    return core_audio_capabilities_describe(device, is_capture, err);
#else
    if (err) {
      device_error_init(err, DEVICE_ERROR_OTHER,
                        "CoreAudio backend not compiled");
    }
    return NULL;
#endif
  } else if (strcasecmp(backend, "alsa") == 0) {
#if defined(ENABLE_ALSA)
    return alsa_capabilities_describe(device, is_capture, err);
#else
    if (err) {
      device_error_init(err, DEVICE_ERROR_OTHER, "ALSA backend not compiled");
    }
    return NULL;
#endif
  } else if (strcasecmp(backend, "wasapi") == 0) {
#if defined(ENABLE_WASAPI)
    return wasapi_capabilities_describe(device, is_capture, err);
#else
    if (err) {
      device_error_init(err, DEVICE_ERROR_OTHER, "WASAPI backend not compiled");
    }
    return NULL;
#endif
  } else if (strcasecmp(backend, "asio") == 0) {
#if defined(ENABLE_ASIO)
    return asio_capabilities_describe(device, is_capture, err);
#else
    if (err) {
      device_error_init(err, DEVICE_ERROR_OTHER, "ASIO backend not compiled");
    }
    return NULL;
#endif
  }
  if (err) {
    device_error_init(err, DEVICE_ERROR_OTHER, "Unsupported backend");
  }
  return NULL;
}
