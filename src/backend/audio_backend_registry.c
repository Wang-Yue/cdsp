#include "backend/audio_backend_registry.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "backend/audio_backend.h"

#if defined(ENABLE_COREAUDIO)
#include "backend/core_audio_capabilities.h"
#endif
#if defined(ENABLE_ALSA)
#include "backend/alsa_capabilities.h"
#endif
#if defined(ENABLE_WASAPI)
#include "backend/wasapi_capabilities.h"
#endif
#if defined(ENABLE_ASIO)
#include "backend/asio_capabilities.h"
#endif
#if defined(ENABLE_WEBAUDIO)
#include "backend/webaudio_backend.h"
#endif

void audio_backend_registry_init(void) {
#if defined(ENABLE_WEBAUDIO) && defined(__EMSCRIPTEN__)
  // The browser audio device behind the WebAudio backends (idempotent).
  webaudio_device_start();
#endif
}

#if defined(ENABLE_COREAUDIO) || defined(ENABLE_ALSA) ||                       \
    defined(ENABLE_WASAPI) || defined(ENABLE_ASIO)
typedef int (*device_names_fn_t)(bool is_capture, char out_names[][256],
                                 int max_names);

// `audio_device_t` is a single `char name[256]`, so an array of them has the
// same layout as `char[][256]` and the backend can fill it directly.
_Static_assert(sizeof(audio_device_t) == 256,
               "audio_device_t must be layout-compatible with char[256]");

/// Upper bound when only counting (out_devices == NULL).
#define REGISTRY_COUNT_ONLY_CAPACITY 1024

/**
 * List device names straight into the caller's array, bounded only by
 * @p max_devices. Upstream returns an unbounded Vec (lib.rs:872-884); the
 * previous fixed 32-entry staging array silently truncated longer lists.
 */
static int registry_list_names(device_names_fn_t fn, bool input,
                               audio_device_t *out_devices, int max_devices) {
  if (out_devices) {
    if (max_devices <= 0)
      return 0;
    int count = fn(input, (char (*)[256])out_devices, max_devices);
    if (count > max_devices)
      count = max_devices;
    for (int i = 0; i < count; i++) {
      out_devices[i].name[sizeof(out_devices[i].name) - 1] = '\0';
    }
    return count;
  }
  // Count only: list into a temporary heap buffer (control path, not RT).
  char (*tmp)[256] = calloc(REGISTRY_COUNT_ONLY_CAPACITY, sizeof(*tmp));
  if (!tmp)
    return -1;
  int count = fn(input, tmp, REGISTRY_COUNT_ONLY_CAPACITY);
  free(tmp);
  return count;
}
#endif

int audio_backend_registry_get_available_devices(const char *backend,
                                                 bool input,
                                                 audio_device_t *out_devices,
                                                 int max_devices) {
  if (!backend)
    return 0;
  (void)input;
  (void)out_devices;
  (void)max_devices;
  if (strcasecmp(backend, "coreaudio") == 0) {
#if defined(ENABLE_COREAUDIO)
    return registry_list_names(core_audio_capabilities_available_device_names,
                               input, out_devices, max_devices);
#else
    return 0;
#endif
  } else if (strcasecmp(backend, "alsa") == 0) {
#if defined(ENABLE_ALSA)
    return registry_list_names(alsa_capabilities_available_device_names, input,
                               out_devices, max_devices);
#else
    return 0;
#endif
  } else if (strcasecmp(backend, "wasapi") == 0) {
#if defined(ENABLE_WASAPI)
    return registry_list_names(wasapi_capabilities_available_device_names,
                               input, out_devices, max_devices);
#else
    return 0;
#endif
  } else if (strcasecmp(backend, "asio") == 0) {
#if defined(ENABLE_ASIO)
    return registry_list_names(asio_capabilities_available_device_names, input,
                               out_devices, max_devices);
#else
    return 0;
#endif
  } else if (strcasecmp(backend, "webaudio") == 0) {
#if defined(ENABLE_WEBAUDIO)
    return webaudio_get_available_devices(input, out_devices, max_devices);
#else
    return 0;
#endif
  }
  return 0;
}

audio_device_descriptor_t *audio_backend_registry_get_device_capabilities(
    const char *backend, const char *device, bool is_capture,
    device_error_t *err) {
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
  } else if (strcasecmp(backend, "webaudio") == 0) {
#if defined(ENABLE_WEBAUDIO)
    return webaudio_describe(device, is_capture, err);
#else
    if (err) {
      device_error_init(err, DEVICE_ERROR_OTHER,
                        "WebAudio backend not compiled");
    }
    return NULL;
#endif
  }
  if (err) {
    device_error_init(err, DEVICE_ERROR_OTHER, "Unsupported backend");
  }
  return NULL;
}
