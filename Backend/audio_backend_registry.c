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
#include "Logging/app_logger.h"

static const logger_t g_logger = {"dsp.backend.registry"};

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

static void log_device_capabilities_result(const char* backend, const char* device,
                                            bool is_capture,
                                            const audio_device_descriptor_t* desc) {
  if (!desc) {
    logger_warn(&g_logger,
                "Query device capabilities failed: backend=%s, device=%s, capture=%d",
                backend ? backend : "(null)", device ? device : "(null)", is_capture);
    return;
  }

  logger_info(&g_logger,
              "Device capabilities discovered: backend=%s, device='%s', capture=%d, sets_count=%zu",
              backend, desc->name, is_capture, desc->capability_sets_count);

  for (size_t s = 0; s < desc->capability_sets_count; s++) {
    const device_capability_set_t* set = &desc->capability_sets[s];
    logger_info(&g_logger,
                "  Capability Set [%zu]: %zu channel config(s)",
                s, set->capabilities_count);

    for (size_t c = 0; c < set->capabilities_count; c++) {
      const channel_capability_t* ch = &set->capabilities[c];
      for (size_t r = 0; r < ch->samplerates_count; r++) {
        const samplerate_capability_t* sr = &ch->samplerates[r];
        char fmt_buf[256] = {0};
        size_t pos = 0;
        for (size_t f = 0; f < sr->formats_count; f++) {
          pos += (size_t)snprintf(fmt_buf + pos, sizeof(fmt_buf) - pos,
                                  "%s%s", (f == 0 ? "" : ", "), sr->formats[f]);
        }
        logger_info(&g_logger,
                    "    - Channels: %d, SampleRate: %d Hz, Formats: [%s]",
                    ch->channels, sr->samplerate, fmt_buf);
      }
    }
  }
}

audio_device_descriptor_t* audio_backend_registry_get_device_capabilities(
    const char* backend, const char* device, bool is_capture,
    device_error_t* err) {
  logger_info(&g_logger,
              "Querying device capabilities: backend=%s, device=%s, capture=%d",
              backend ? backend : "(null)", device ? device : "(null)", is_capture);
  if (!backend || !device) {
    if (err) {
      device_error_init(err, DEVICE_ERROR_OTHER,
                        "Invalid backend or device name");
    }
    return NULL;
  }
  audio_device_descriptor_t* desc = NULL;
  if (strcasecmp(backend, "coreaudio") == 0) {
#if defined(ENABLE_COREAUDIO)
    desc = core_audio_capabilities_describe(device, is_capture, err);
#else
    if (err) {
      device_error_init(err, DEVICE_ERROR_OTHER,
                        "CoreAudio backend not compiled");
    }
#endif
  } else if (strcasecmp(backend, "alsa") == 0) {
#if defined(ENABLE_ALSA)
    desc = alsa_capabilities_describe(device, is_capture, err);
#else
    if (err) {
      device_error_init(err, DEVICE_ERROR_OTHER, "ALSA backend not compiled");
    }
#endif
  } else if (strcasecmp(backend, "wasapi") == 0) {
#if defined(ENABLE_WASAPI)
    desc = wasapi_capabilities_describe(device, is_capture, err);
#else
    if (err) {
      device_error_init(err, DEVICE_ERROR_OTHER, "WASAPI backend not compiled");
    }
#endif
  } else if (strcasecmp(backend, "asio") == 0) {
#if defined(ENABLE_ASIO)
    desc = asio_capabilities_describe(device, is_capture, err);
#else
    if (err) {
      device_error_init(err, DEVICE_ERROR_OTHER, "ASIO backend not compiled");
    }
#endif
  } else {
    if (err) {
      device_error_init(err, DEVICE_ERROR_OTHER, "Unsupported backend");
    }
  }

  log_device_capabilities_result(backend, device, is_capture, desc);
  return desc;
}
