// Audio backend protocols.
//
// `ProcessingState` and `ProcessingStopReason` — used by both the
// engine internals and the public actor — live in `Engine/DSPEngine.swift`.

#include "backend/audio_backend.h"

#include <assert.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "wav/wav_reader.h"

#if defined(ENABLE_COREAUDIO)
#include "backend/core_audio_capture.h"
#include "backend/core_audio_playback.h"
#endif
#if defined(ENABLE_ALSA)
#include "backend/alsa_capture.h"
#include "backend/alsa_playback.h"
#endif
#if defined(ENABLE_PIPEWIRE)
#include "backend/pipewire_backend.h"
#endif
#if defined(ENABLE_ASIO)
#include "backend/asio_backend.h"
#endif
#if defined(ENABLE_WASAPI)
#include "backend/wasapi_capture.h"
#include "backend/wasapi_playback.h"
#endif

#include "audio/audio_chunk.h"
#include "audio/sample_format.h"
#include "backend/file_backend.h"
#include "backend/generator_capture.h"
#include "config/config_gen.h"
#include "logging/app_logger.h"
#include "utils/cdsp_time.h"

const uint32_t STANDARD_RATES[STANDARD_RATES_COUNT] = {
    5512,  8000,  11025,  16000,  22050,  32000,  44100,  48000,  64000,
    88200, 96000, 176400, 192000, 352800, 384000, 705600, 768000,
};

void audio_backend_error_description(const audio_backend_error_t *err,
                                     char *out_buf, size_t buf_len) {
  if (!err || !out_buf || buf_len == 0)
    return;
  switch (err->type) {
  case AUDIO_BACKEND_ERR_CONFIG_PARSE:
    snprintf(out_buf, buf_len, "Config parse error: %s", err->message);
    break;
  case AUDIO_BACKEND_ERR_COMMAND_SEND:
    snprintf(out_buf, buf_len, "Command send error: %s", err->message);
    break;
  case AUDIO_BACKEND_ERR_INVALID_SAMPLERATE:
    snprintf(out_buf, buf_len, "Invalid samplerate: %s", err->message);
    break;
  case AUDIO_BACKEND_ERR_SPECTRUM_COMPUTE:
    snprintf(out_buf, buf_len, "Spectrum compute error: %s", err->message);
    break;
  case AUDIO_BACKEND_ERR_ENGINE_NOT_RUNNING:
    snprintf(out_buf, buf_len, "Engine not running");
    break;
  case AUDIO_BACKEND_ERR_BUFFER_EMPTY:
    snprintf(out_buf, buf_len, "Audio history buffer is empty");
    break;
  case AUDIO_BACKEND_ERR_DEVICE_NOT_FOUND:
    snprintf(out_buf, buf_len, "Device not found: %s", err->message);
    break;
  case AUDIO_BACKEND_ERR_DEVICE_BUSY:
    snprintf(out_buf, buf_len, "Device busy: %s", err->message);
    break;
  case AUDIO_BACKEND_ERR_CONFIG_READ:
    snprintf(out_buf, buf_len, "Config read error: %s", err->message);
    break;
  }
}

void free_audio_device_descriptor(audio_device_descriptor_t *desc) {
  if (!desc)
    return;
  if (desc->capability_sets) {
    for (size_t s = 0; s < desc->capability_sets_count; s++) {
      device_capability_set_t *set = &desc->capability_sets[s];
      if (set->capabilities) {
        for (size_t c = 0; c < set->capabilities_count; c++) {
          channel_capability_t *ch_cap = &set->capabilities[c];
          if (ch_cap->samplerates) {
            for (size_t r = 0; r < ch_cap->samplerates_count; r++) {
              samplerate_capability_t *rate_cap = &ch_cap->samplerates[r];
              if (rate_cap->formats) {
                for (size_t f = 0; f < rate_cap->formats_count; f++) {
                  free(rate_cap->formats[f]);
                }
                free(rate_cap->formats);
              }
            }
            free(ch_cap->samplerates);
          }
        }
        free(set->capabilities);
      }
    }
    free(desc->capability_sets);
  }
  free(desc);
}

#if defined(ENABLE_COREAUDIO)
static binary_sample_format_t
coreaudio_sample_format_to_binary_format(coreaudio_sample_format_t fmt) {
  switch (fmt) {
  case COREAUDIO_SAMPLE_FORMAT_S16:
    return BINARY_SAMPLE_FORMAT_S16_LE;
  case COREAUDIO_SAMPLE_FORMAT_S24:
    return BINARY_SAMPLE_FORMAT_S24_4_LJ_LE;
  case COREAUDIO_SAMPLE_FORMAT_S32:
    return BINARY_SAMPLE_FORMAT_S32_LE;
  case COREAUDIO_SAMPLE_FORMAT_F32:
    return BINARY_SAMPLE_FORMAT_F32_LE;
  case COREAUDIO_SAMPLE_FORMAT_INVALID:
    return BINARY_SAMPLE_FORMAT_INVALID;
  }
  CDSP_UNREACHABLE();
  return BINARY_SAMPLE_FORMAT_INVALID;
}

static coreaudio_sample_format_t
coreaudio_sample_format_from_binary_format(binary_sample_format_t fmt) {
  switch (fmt) {
  case BINARY_SAMPLE_FORMAT_S16_LE:
    return COREAUDIO_SAMPLE_FORMAT_S16;
  case BINARY_SAMPLE_FORMAT_S24_4_LJ_LE:
  case BINARY_SAMPLE_FORMAT_S24_4_RJ_LE:
  case BINARY_SAMPLE_FORMAT_S24_3_LE:
    return COREAUDIO_SAMPLE_FORMAT_S24;
  case BINARY_SAMPLE_FORMAT_S32_LE:
    return COREAUDIO_SAMPLE_FORMAT_S32;
  case BINARY_SAMPLE_FORMAT_F32_LE:
    return COREAUDIO_SAMPLE_FORMAT_F32;
  case BINARY_SAMPLE_FORMAT_INVALID:
  case BINARY_SAMPLE_FORMAT_F64_LE:
  case BINARY_SAMPLE_FORMAT_DSD_U8:
  case BINARY_SAMPLE_FORMAT_DSD_U16_LE:
  case BINARY_SAMPLE_FORMAT_DSD_U16_BE:
  case BINARY_SAMPLE_FORMAT_DSD_U32_LE:
  case BINARY_SAMPLE_FORMAT_DSD_U32_BE:
  case BINARY_SAMPLE_FORMAT_DSD_U32_REVERSED:
    return COREAUDIO_SAMPLE_FORMAT_INVALID;
  }
  CDSP_UNREACHABLE();
  return COREAUDIO_SAMPLE_FORMAT_INVALID;
}
#endif

#if defined(ENABLE_ALSA)
static binary_sample_format_t
alsa_sample_format_to_binary_format(alsa_sample_format_t fmt) {
  switch (fmt) {
  case ALSA_SAMPLE_FORMAT_S16_LE:
    return BINARY_SAMPLE_FORMAT_S16_LE;
  case ALSA_SAMPLE_FORMAT_S24_3_LE:
    return BINARY_SAMPLE_FORMAT_S24_3_LE;
  case ALSA_SAMPLE_FORMAT_S24_4_LE:
    return BINARY_SAMPLE_FORMAT_S24_4_RJ_LE;
  case ALSA_SAMPLE_FORMAT_S32_LE:
    return BINARY_SAMPLE_FORMAT_S32_LE;
  case ALSA_SAMPLE_FORMAT_F32_LE:
    return BINARY_SAMPLE_FORMAT_F32_LE;
  case ALSA_SAMPLE_FORMAT_F64_LE:
    return BINARY_SAMPLE_FORMAT_F64_LE;
  case ALSA_SAMPLE_FORMAT_DSD_U8:
    return BINARY_SAMPLE_FORMAT_DSD_U8;
  case ALSA_SAMPLE_FORMAT_DSD_U16_LE:
    return BINARY_SAMPLE_FORMAT_DSD_U16_LE;
  case ALSA_SAMPLE_FORMAT_DSD_U16_BE:
    return BINARY_SAMPLE_FORMAT_DSD_U16_BE;
  case ALSA_SAMPLE_FORMAT_DSD_U32_LE:
    return BINARY_SAMPLE_FORMAT_DSD_U32_LE;
  case ALSA_SAMPLE_FORMAT_DSD_U32_BE:
    return BINARY_SAMPLE_FORMAT_DSD_U32_BE;
  case ALSA_SAMPLE_FORMAT_INVALID:
    return BINARY_SAMPLE_FORMAT_INVALID;
  }
  CDSP_UNREACHABLE();
  return BINARY_SAMPLE_FORMAT_INVALID;
}

static alsa_sample_format_t
alsa_sample_format_from_binary_format(binary_sample_format_t fmt) {
  switch (fmt) {
  case BINARY_SAMPLE_FORMAT_S16_LE:
    return ALSA_SAMPLE_FORMAT_S16_LE;
  case BINARY_SAMPLE_FORMAT_S24_3_LE:
    return ALSA_SAMPLE_FORMAT_S24_3_LE;
  case BINARY_SAMPLE_FORMAT_S24_4_LJ_LE:
  case BINARY_SAMPLE_FORMAT_S24_4_RJ_LE:
    return ALSA_SAMPLE_FORMAT_S24_4_LE;
  case BINARY_SAMPLE_FORMAT_S32_LE:
    return ALSA_SAMPLE_FORMAT_S32_LE;
  case BINARY_SAMPLE_FORMAT_F32_LE:
    return ALSA_SAMPLE_FORMAT_F32_LE;
  case BINARY_SAMPLE_FORMAT_F64_LE:
    return ALSA_SAMPLE_FORMAT_F64_LE;
  case BINARY_SAMPLE_FORMAT_DSD_U8:
    return ALSA_SAMPLE_FORMAT_DSD_U8;
  case BINARY_SAMPLE_FORMAT_DSD_U16_LE:
    return ALSA_SAMPLE_FORMAT_DSD_U16_LE;
  case BINARY_SAMPLE_FORMAT_DSD_U16_BE:
    return ALSA_SAMPLE_FORMAT_DSD_U16_BE;
  case BINARY_SAMPLE_FORMAT_DSD_U32_LE:
    return ALSA_SAMPLE_FORMAT_DSD_U32_LE;
  case BINARY_SAMPLE_FORMAT_DSD_U32_BE:
    return ALSA_SAMPLE_FORMAT_DSD_U32_BE;
  case BINARY_SAMPLE_FORMAT_INVALID:
  case BINARY_SAMPLE_FORMAT_DSD_U32_REVERSED:
    return ALSA_SAMPLE_FORMAT_INVALID;
  }
  CDSP_UNREACHABLE();
  return ALSA_SAMPLE_FORMAT_INVALID;
}
#endif

#if defined(ENABLE_WASAPI)
static binary_sample_format_t
wasapi_sample_format_to_binary_format(wasapi_sample_format_t fmt) {
  switch (fmt) {
  case WASAPI_SAMPLE_FORMAT_S16:
    return BINARY_SAMPLE_FORMAT_S16_LE;
  case WASAPI_SAMPLE_FORMAT_S24:
    return BINARY_SAMPLE_FORMAT_S24_4_LJ_LE;
  case WASAPI_SAMPLE_FORMAT_S32:
    return BINARY_SAMPLE_FORMAT_S32_LE;
  case WASAPI_SAMPLE_FORMAT_F32:
    return BINARY_SAMPLE_FORMAT_F32_LE;
  case WASAPI_SAMPLE_FORMAT_INVALID:
    return BINARY_SAMPLE_FORMAT_INVALID;
  }
  CDSP_UNREACHABLE();
  return BINARY_SAMPLE_FORMAT_INVALID;
}

static wasapi_sample_format_t
wasapi_sample_format_from_binary_format(binary_sample_format_t fmt) {
  switch (fmt) {
  case BINARY_SAMPLE_FORMAT_S16_LE:
    return WASAPI_SAMPLE_FORMAT_S16;
  case BINARY_SAMPLE_FORMAT_S24_3_LE:
  case BINARY_SAMPLE_FORMAT_S24_4_LJ_LE:
  case BINARY_SAMPLE_FORMAT_S24_4_RJ_LE:
    return WASAPI_SAMPLE_FORMAT_S24;
  case BINARY_SAMPLE_FORMAT_S32_LE:
    return WASAPI_SAMPLE_FORMAT_S32;
  case BINARY_SAMPLE_FORMAT_F32_LE:
    return WASAPI_SAMPLE_FORMAT_F32;
  case BINARY_SAMPLE_FORMAT_INVALID:
  case BINARY_SAMPLE_FORMAT_F64_LE:
  case BINARY_SAMPLE_FORMAT_DSD_U8:
  case BINARY_SAMPLE_FORMAT_DSD_U16_LE:
  case BINARY_SAMPLE_FORMAT_DSD_U16_BE:
  case BINARY_SAMPLE_FORMAT_DSD_U32_LE:
  case BINARY_SAMPLE_FORMAT_DSD_U32_BE:
  case BINARY_SAMPLE_FORMAT_DSD_U32_REVERSED:
    return WASAPI_SAMPLE_FORMAT_INVALID;
  }
  CDSP_UNREACHABLE();
  return WASAPI_SAMPLE_FORMAT_INVALID;
}
#endif

#if defined(ENABLE_ASIO)
binary_sample_format_t
asio_sample_format_to_binary_format(asio_sample_format_t fmt, bool is_lsb) {
  switch (fmt) {
  case ASIO_SAMPLE_FORMAT_S16_LE:
    return BINARY_SAMPLE_FORMAT_S16_LE;
  case ASIO_SAMPLE_FORMAT_S24_3_LE:
    return BINARY_SAMPLE_FORMAT_S24_3_LE;
  case ASIO_SAMPLE_FORMAT_S24_4_LE:
    return BINARY_SAMPLE_FORMAT_S24_4_LJ_LE;
  case ASIO_SAMPLE_FORMAT_S32_LE:
    return BINARY_SAMPLE_FORMAT_S32_LE;
  case ASIO_SAMPLE_FORMAT_F32_LE:
    return BINARY_SAMPLE_FORMAT_F32_LE;
  case ASIO_SAMPLE_FORMAT_F64_LE:
    return BINARY_SAMPLE_FORMAT_F64_LE;
  case ASIO_SAMPLE_FORMAT_DSD_INT8:
    return is_lsb ? BINARY_SAMPLE_FORMAT_DSD_U32_REVERSED
                  : BINARY_SAMPLE_FORMAT_DSD_U32_BE;
  case ASIO_SAMPLE_FORMAT_INVALID:
    return BINARY_SAMPLE_FORMAT_INVALID;
  }
  CDSP_UNREACHABLE();
  return BINARY_SAMPLE_FORMAT_INVALID;
}

static asio_sample_format_t
asio_sample_format_from_binary_format(binary_sample_format_t fmt) {
  switch (fmt) {
  case BINARY_SAMPLE_FORMAT_S16_LE:
    return ASIO_SAMPLE_FORMAT_S16_LE;
  case BINARY_SAMPLE_FORMAT_S24_3_LE:
    return ASIO_SAMPLE_FORMAT_S24_3_LE;
  case BINARY_SAMPLE_FORMAT_S24_4_LJ_LE:
  case BINARY_SAMPLE_FORMAT_S24_4_RJ_LE:
    return ASIO_SAMPLE_FORMAT_S24_4_LE;
  case BINARY_SAMPLE_FORMAT_S32_LE:
    return ASIO_SAMPLE_FORMAT_S32_LE;
  case BINARY_SAMPLE_FORMAT_F32_LE:
    return ASIO_SAMPLE_FORMAT_F32_LE;
  case BINARY_SAMPLE_FORMAT_F64_LE:
    return ASIO_SAMPLE_FORMAT_F64_LE;
  case BINARY_SAMPLE_FORMAT_DSD_U32_LE:
  case BINARY_SAMPLE_FORMAT_DSD_U32_BE:
  case BINARY_SAMPLE_FORMAT_DSD_U32_REVERSED:
    return ASIO_SAMPLE_FORMAT_DSD_INT8;
  case BINARY_SAMPLE_FORMAT_INVALID:
  case BINARY_SAMPLE_FORMAT_DSD_U8:
  case BINARY_SAMPLE_FORMAT_DSD_U16_LE:
  case BINARY_SAMPLE_FORMAT_DSD_U16_BE:
    return ASIO_SAMPLE_FORMAT_INVALID;
  }
  CDSP_UNREACHABLE();
  return ASIO_SAMPLE_FORMAT_INVALID;
}
#endif

// MARK: - Device Config Accessors

size_t
capture_device_config_get_channels(const capture_device_config_t *config) {
  if (!config)
    return 0;
  switch (config->type) {
#if defined(ENABLE_COREAUDIO)
  case AUDIO_BACKEND_TYPE_CORE_AUDIO:
    return config->cfg.coreaudio.channels;
#endif
#if defined(ENABLE_ALSA)
  case AUDIO_BACKEND_TYPE_ALSA:
    return config->cfg.alsa.channels;
#endif
#if defined(ENABLE_PIPEWIRE)
  case AUDIO_BACKEND_TYPE_PIPEWIRE:
    return config->cfg.pipewire.channels;
#endif
  case AUDIO_BACKEND_TYPE_FILE:
    return config->is_wav ? config->cfg.wav_file.channels
                          : config->cfg.raw_file.channels;
  case AUDIO_BACKEND_TYPE_STDIN_OUT:
    return config->cfg.stdin_in.channels;
  case AUDIO_BACKEND_TYPE_GENERATOR:
    return config->cfg.generator.channels;
#if defined(ENABLE_WASAPI)
  case AUDIO_BACKEND_TYPE_WASAPI:
    return config->cfg.wasapi.channels;
#endif
#if defined(ENABLE_ASIO)
  case AUDIO_BACKEND_TYPE_ASIO:
    return config->cfg.asio.channels;
#endif
  case AUDIO_BACKEND_TYPE_INVALID:
    return 0;
  }
  CDSP_UNREACHABLE();
  return 0;
}

void capture_device_config_set_channels(capture_device_config_t *config,
                                        size_t channels) {
  if (!config)
    return;
  switch (config->type) {
#if defined(ENABLE_COREAUDIO)
  case AUDIO_BACKEND_TYPE_CORE_AUDIO:
    config->cfg.coreaudio.channels = channels;
    break;
#endif
#if defined(ENABLE_ALSA)
  case AUDIO_BACKEND_TYPE_ALSA:
    config->cfg.alsa.channels = channels;
    break;
#endif
#if defined(ENABLE_PIPEWIRE)
  case AUDIO_BACKEND_TYPE_PIPEWIRE:
    config->cfg.pipewire.channels = channels;
    break;
#endif
  case AUDIO_BACKEND_TYPE_FILE:
    if (config->is_wav) {
      config->cfg.wav_file.channels = channels;
    } else {
      config->cfg.raw_file.channels = channels;
    }
    break;
  case AUDIO_BACKEND_TYPE_STDIN_OUT:
    config->cfg.stdin_in.channels = channels;
    break;
  case AUDIO_BACKEND_TYPE_GENERATOR:
    config->cfg.generator.channels = channels;
    break;
#if defined(ENABLE_WASAPI)
  case AUDIO_BACKEND_TYPE_WASAPI:
    config->cfg.wasapi.channels = channels;
    break;
#endif
#if defined(ENABLE_ASIO)
  case AUDIO_BACKEND_TYPE_ASIO:
    config->cfg.asio.channels = channels;
    break;
#endif
  case AUDIO_BACKEND_TYPE_INVALID:
    break;
  }
}

size_t
playback_device_config_get_channels(const playback_device_config_t *config) {
  if (!config)
    return 0;
  switch (config->type) {
#if defined(ENABLE_COREAUDIO)
  case AUDIO_BACKEND_TYPE_CORE_AUDIO:
    return config->cfg.coreaudio.channels;
#endif
#if defined(ENABLE_ALSA)
  case AUDIO_BACKEND_TYPE_ALSA:
    return config->cfg.alsa.channels;
#endif
#if defined(ENABLE_PIPEWIRE)
  case AUDIO_BACKEND_TYPE_PIPEWIRE:
    return config->cfg.pipewire.channels;
#endif
  case AUDIO_BACKEND_TYPE_FILE:
    return config->cfg.raw_file.channels;
  case AUDIO_BACKEND_TYPE_STDIN_OUT:
    return config->cfg.stdout_out.channels;
  case AUDIO_BACKEND_TYPE_GENERATOR:
    return 0;
#if defined(ENABLE_WASAPI)
  case AUDIO_BACKEND_TYPE_WASAPI:
    return config->cfg.wasapi.channels;
#endif
#if defined(ENABLE_ASIO)
  case AUDIO_BACKEND_TYPE_ASIO:
    return config->cfg.asio.channels;
#endif
  case AUDIO_BACKEND_TYPE_INVALID:
    return 0;
  }
  CDSP_UNREACHABLE();
  return 0;
}

const char *
capture_device_config_get_device(const capture_device_config_t *config) {
  if (!config)
    return "";
  switch (config->type) {
#if defined(ENABLE_COREAUDIO)
  case AUDIO_BACKEND_TYPE_CORE_AUDIO:
    return config->cfg.coreaudio.device;
#endif
#if defined(ENABLE_ALSA)
  case AUDIO_BACKEND_TYPE_ALSA:
    return config->cfg.alsa.device;
#endif
#if defined(ENABLE_PIPEWIRE)
  case AUDIO_BACKEND_TYPE_PIPEWIRE:
    return "";
#endif
#if defined(ENABLE_WASAPI)
  case AUDIO_BACKEND_TYPE_WASAPI:
    return config->cfg.wasapi.device;
#endif
#if defined(ENABLE_ASIO)
  case AUDIO_BACKEND_TYPE_ASIO:
    return config->cfg.asio.device;
#endif
  case AUDIO_BACKEND_TYPE_FILE:
  case AUDIO_BACKEND_TYPE_STDIN_OUT:
  case AUDIO_BACKEND_TYPE_GENERATOR:
  case AUDIO_BACKEND_TYPE_INVALID:
    return "";
  }
  CDSP_UNREACHABLE();
  return "";
}

const char *
playback_device_config_get_device(const playback_device_config_t *config) {
  if (!config)
    return "";
  switch (config->type) {
#if defined(ENABLE_COREAUDIO)
  case AUDIO_BACKEND_TYPE_CORE_AUDIO:
    return config->cfg.coreaudio.device;
#endif
#if defined(ENABLE_ALSA)
  case AUDIO_BACKEND_TYPE_ALSA:
    return config->cfg.alsa.device;
#endif
#if defined(ENABLE_PIPEWIRE)
  case AUDIO_BACKEND_TYPE_PIPEWIRE:
    return "";
#endif
#if defined(ENABLE_WASAPI)
  case AUDIO_BACKEND_TYPE_WASAPI:
    return config->cfg.wasapi.device;
#endif
#if defined(ENABLE_ASIO)
  case AUDIO_BACKEND_TYPE_ASIO:
    return config->cfg.asio.device;
#endif
  case AUDIO_BACKEND_TYPE_FILE:
  case AUDIO_BACKEND_TYPE_STDIN_OUT:
  case AUDIO_BACKEND_TYPE_GENERATOR:
  case AUDIO_BACKEND_TYPE_INVALID:
    return "";
  }
  CDSP_UNREACHABLE();
  return "";
}

binary_sample_format_t
capture_device_config_get_binary_format(const capture_device_config_t *config) {
  if (!config)
    return BINARY_SAMPLE_FORMAT_INVALID;
  switch (config->type) {
#if defined(ENABLE_COREAUDIO)
  case AUDIO_BACKEND_TYPE_CORE_AUDIO:
    return config->cfg.coreaudio.has_format
               ? coreaudio_sample_format_to_binary_format(
                     config->cfg.coreaudio.format)
               : BINARY_SAMPLE_FORMAT_F32_LE;
#endif
#if defined(ENABLE_ALSA)
  case AUDIO_BACKEND_TYPE_ALSA:
    return config->cfg.alsa.has_format
               ? alsa_sample_format_to_binary_format(config->cfg.alsa.format)
               : BINARY_SAMPLE_FORMAT_INVALID;
#endif
#if defined(ENABLE_PIPEWIRE)
  case AUDIO_BACKEND_TYPE_PIPEWIRE:
    return BINARY_SAMPLE_FORMAT_F32_LE;
#endif
#if defined(ENABLE_WASAPI)
  case AUDIO_BACKEND_TYPE_WASAPI:
    return config->cfg.wasapi.has_format
               ? wasapi_sample_format_to_binary_format(
                     config->cfg.wasapi.format)
               : BINARY_SAMPLE_FORMAT_INVALID;
#endif
#if defined(ENABLE_ASIO)
  case AUDIO_BACKEND_TYPE_ASIO:
    return config->cfg.asio.has_format ? asio_sample_format_to_binary_format(
                                             config->cfg.asio.format, false)
                                       : BINARY_SAMPLE_FORMAT_INVALID;
#endif
  case AUDIO_BACKEND_TYPE_FILE:
    if (config->is_wav) {
      return BINARY_SAMPLE_FORMAT_INVALID;
    }
    return config->cfg.raw_file.has_format ? config->cfg.raw_file.format
                                           : BINARY_SAMPLE_FORMAT_INVALID;
  case AUDIO_BACKEND_TYPE_STDIN_OUT:
    return config->cfg.stdin_in.format;
  case AUDIO_BACKEND_TYPE_GENERATOR:
  case AUDIO_BACKEND_TYPE_INVALID:
    return BINARY_SAMPLE_FORMAT_INVALID;
  }
  CDSP_UNREACHABLE();
  return BINARY_SAMPLE_FORMAT_INVALID;
}

binary_sample_format_t playback_device_config_get_binary_format(
    const playback_device_config_t *config) {
  if (!config)
    return BINARY_SAMPLE_FORMAT_INVALID;
  switch (config->type) {
#if defined(ENABLE_COREAUDIO)
  case AUDIO_BACKEND_TYPE_CORE_AUDIO:
    return config->cfg.coreaudio.has_format
               ? coreaudio_sample_format_to_binary_format(
                     config->cfg.coreaudio.format)
               : BINARY_SAMPLE_FORMAT_F32_LE;
#endif
#if defined(ENABLE_ALSA)
  case AUDIO_BACKEND_TYPE_ALSA:
    return config->cfg.alsa.has_format
               ? alsa_sample_format_to_binary_format(config->cfg.alsa.format)
               : BINARY_SAMPLE_FORMAT_INVALID;
#endif
#if defined(ENABLE_PIPEWIRE)
  case AUDIO_BACKEND_TYPE_PIPEWIRE:
    return BINARY_SAMPLE_FORMAT_F32_LE;
#endif
#if defined(ENABLE_WASAPI)
  case AUDIO_BACKEND_TYPE_WASAPI:
    return config->cfg.wasapi.has_format
               ? wasapi_sample_format_to_binary_format(
                     config->cfg.wasapi.format)
               : BINARY_SAMPLE_FORMAT_INVALID;
#endif
#if defined(ENABLE_ASIO)
  case AUDIO_BACKEND_TYPE_ASIO:
    return config->cfg.asio.has_format ? asio_sample_format_to_binary_format(
                                             config->cfg.asio.format, false)
                                       : BINARY_SAMPLE_FORMAT_INVALID;
#endif
  case AUDIO_BACKEND_TYPE_FILE:
    return config->cfg.raw_file.has_format ? config->cfg.raw_file.format
                                           : BINARY_SAMPLE_FORMAT_INVALID;
  case AUDIO_BACKEND_TYPE_STDIN_OUT:
    return config->cfg.stdout_out.format;
  case AUDIO_BACKEND_TYPE_GENERATOR:
  case AUDIO_BACKEND_TYPE_INVALID:
    return BINARY_SAMPLE_FORMAT_INVALID;
  }
  CDSP_UNREACHABLE();
  return BINARY_SAMPLE_FORMAT_INVALID;
}

#if defined(ENABLE_COREAUDIO)
coreaudio_sample_format_t
capture_device_config_get_format(const capture_device_config_t *config) {
  switch (config->type) {
  case AUDIO_BACKEND_TYPE_CORE_AUDIO:
    return config->cfg.coreaudio.has_format ? config->cfg.coreaudio.format
                                            : COREAUDIO_SAMPLE_FORMAT_INVALID;
  case AUDIO_BACKEND_TYPE_INVALID:
#if defined(ENABLE_ALSA)
  case AUDIO_BACKEND_TYPE_ALSA:
#endif
#if defined(ENABLE_PIPEWIRE)
  case AUDIO_BACKEND_TYPE_PIPEWIRE:
#endif
#if defined(ENABLE_WASAPI)
  case AUDIO_BACKEND_TYPE_WASAPI:
#endif
#if defined(ENABLE_ASIO)
  case AUDIO_BACKEND_TYPE_ASIO:
#endif
  case AUDIO_BACKEND_TYPE_FILE:
  case AUDIO_BACKEND_TYPE_STDIN_OUT:
  case AUDIO_BACKEND_TYPE_GENERATOR:
    return COREAUDIO_SAMPLE_FORMAT_INVALID;
  }
  CDSP_UNREACHABLE();
  return COREAUDIO_SAMPLE_FORMAT_INVALID;
}

coreaudio_sample_format_t
playback_device_config_get_format(const playback_device_config_t *config) {
  switch (config->type) {
  case AUDIO_BACKEND_TYPE_CORE_AUDIO:
    return config->cfg.coreaudio.has_format ? config->cfg.coreaudio.format
                                            : COREAUDIO_SAMPLE_FORMAT_INVALID;
  case AUDIO_BACKEND_TYPE_INVALID:
#if defined(ENABLE_ALSA)
  case AUDIO_BACKEND_TYPE_ALSA:
#endif
#if defined(ENABLE_PIPEWIRE)
  case AUDIO_BACKEND_TYPE_PIPEWIRE:
#endif
#if defined(ENABLE_WASAPI)
  case AUDIO_BACKEND_TYPE_WASAPI:
#endif
#if defined(ENABLE_ASIO)
  case AUDIO_BACKEND_TYPE_ASIO:
#endif
  case AUDIO_BACKEND_TYPE_FILE:
  case AUDIO_BACKEND_TYPE_STDIN_OUT:
  case AUDIO_BACKEND_TYPE_GENERATOR:
    return COREAUDIO_SAMPLE_FORMAT_INVALID;
  }
  CDSP_UNREACHABLE();
  return COREAUDIO_SAMPLE_FORMAT_INVALID;
}
#endif

bool capture_device_config_get_bypass_dop(
    const capture_device_config_t *config) {
  if (!config)
    return true;
  return config->bypass_dop;
}

double
capture_device_config_get_dop_cutoff_hz(const capture_device_config_t *config) {
  if (!config)
    return 20000.0;
  return config->dop_cutoff_hz;
}

size_t capture_device_config_calculate_carrier_bits(
    const capture_device_config_t *config) {
  if (!config)
    return 16;
  binary_sample_format_t fmt = capture_device_config_get_binary_format(config);
  if (sample_format_is_dsd(fmt)) {
    return sample_format_bytes_per_sample(fmt) * 8;
  }
  return 16;
}

size_t playback_device_config_calculate_carrier_bits(
    const playback_device_config_t *config) {
  if (!config)
    return 16;
  binary_sample_format_t fmt = playback_device_config_get_binary_format(config);
  if (sample_format_is_dsd(fmt)) {
    return sample_format_bytes_per_sample(fmt) * 8;
  }
  return 16;
}

dsd_mode_t
capture_device_config_get_dsd_mode(const capture_device_config_t *config) {
  if (!config)
    return DSD_MODE_PCM;
  binary_sample_format_t fmt = capture_device_config_get_binary_format(config);
  if (sample_format_is_dsd(fmt)) {
    return DSD_MODE_NATIVE;
  }
  if (!config->bypass_dop) {
    return DSD_MODE_DOP;
  }
  return DSD_MODE_PCM;
}

dsd_mode_t
playback_device_config_get_dsd_mode(const playback_device_config_t *config) {
  if (!config)
    return DSD_MODE_PCM;
  binary_sample_format_t fmt = playback_device_config_get_binary_format(config);
  if (sample_format_is_dsd(fmt)) {
    return DSD_MODE_NATIVE;
  }
  if (config->output_dop) {
    return DSD_MODE_DOP;
  }
  return DSD_MODE_PCM;
}

bool playback_device_config_get_exclusive(
    const playback_device_config_t *config) {
  if (!config)
    return false;
  switch (config->type) {
#if defined(ENABLE_COREAUDIO)
  case AUDIO_BACKEND_TYPE_CORE_AUDIO:
    return config->cfg.coreaudio.exclusive;
#endif
#if defined(ENABLE_WASAPI)
  case AUDIO_BACKEND_TYPE_WASAPI:
    return config->cfg.wasapi.exclusive;
#endif
  case AUDIO_BACKEND_TYPE_INVALID:
#if defined(ENABLE_ALSA)
  case AUDIO_BACKEND_TYPE_ALSA:
#endif
#if defined(ENABLE_PIPEWIRE)
  case AUDIO_BACKEND_TYPE_PIPEWIRE:
#endif
#if defined(ENABLE_ASIO)
  case AUDIO_BACKEND_TYPE_ASIO:
#endif
  case AUDIO_BACKEND_TYPE_FILE:
  case AUDIO_BACKEND_TYPE_STDIN_OUT:
  case AUDIO_BACKEND_TYPE_GENERATOR:
    return false;
  }
  CDSP_UNREACHABLE();
  return false;
}

sdm_filter_t playback_device_config_get_dsd_encoder_filter(
    const playback_device_config_t *config) {
  if (!config)
    return SDM_FILTER_INVALID;
  return config->dsd_encoder_filter;
}

static const logger_t g_logger = {"dsp.backend"};

static const capture_backend_vtable_t *
get_capture_vtable(audio_backend_type_t type) {
  switch (type) {
#if defined(ENABLE_COREAUDIO)
  case AUDIO_BACKEND_TYPE_CORE_AUDIO:
    return &g_core_audio_capture_vtable;
#endif
#if defined(ENABLE_ALSA)
  case AUDIO_BACKEND_TYPE_ALSA:
    return &g_alsa_capture_vtable;
#endif
#if defined(ENABLE_PIPEWIRE)
  case AUDIO_BACKEND_TYPE_PIPEWIRE:
    return &g_pipewire_capture_vtable;
#endif
#if defined(ENABLE_WASAPI)
  case AUDIO_BACKEND_TYPE_WASAPI:
    return &g_wasapi_capture_vtable;
#endif
#if defined(ENABLE_ASIO)
  case AUDIO_BACKEND_TYPE_ASIO:
    return &g_asio_capture_vtable;
#endif
  case AUDIO_BACKEND_TYPE_GENERATOR:
    return &g_generator_capture_vtable;
  case AUDIO_BACKEND_TYPE_FILE:
  case AUDIO_BACKEND_TYPE_STDIN_OUT:
    return &g_file_capture_vtable;
  case AUDIO_BACKEND_TYPE_INVALID:
    return NULL;
  }
  CDSP_UNREACHABLE();
  return NULL;
}

static const playback_backend_vtable_t *
get_playback_vtable(audio_backend_type_t type) {
  switch (type) {
#if defined(ENABLE_COREAUDIO)
  case AUDIO_BACKEND_TYPE_CORE_AUDIO:
    return &g_core_audio_playback_vtable;
#endif
#if defined(ENABLE_ALSA)
  case AUDIO_BACKEND_TYPE_ALSA:
    return &g_alsa_playback_vtable;
#endif
#if defined(ENABLE_PIPEWIRE)
  case AUDIO_BACKEND_TYPE_PIPEWIRE:
    return &g_pipewire_playback_vtable;
#endif
#if defined(ENABLE_WASAPI)
  case AUDIO_BACKEND_TYPE_WASAPI:
    return &g_wasapi_playback_vtable;
#endif
#if defined(ENABLE_ASIO)
  case AUDIO_BACKEND_TYPE_ASIO:
    return &g_asio_playback_vtable;
#endif
  case AUDIO_BACKEND_TYPE_FILE:
  case AUDIO_BACKEND_TYPE_STDIN_OUT:
    return &g_file_playback_vtable;
  case AUDIO_BACKEND_TYPE_GENERATOR:
  case AUDIO_BACKEND_TYPE_INVALID:
    return NULL;
  }
  CDSP_UNREACHABLE();
  return NULL;
}

capture_backend_t *create_capture_backend(const capture_device_config_t *config,
                                          int sample_rate, int chunk_size,
                                          bool full_duplex,
                                          processing_parameters_t *params,
                                          backend_error_t *err) {
  if (!config) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Config is NULL");
    return NULL;
  }
  const capture_backend_vtable_t *vtable = get_capture_vtable(config->type);
  if (!vtable || !vtable->create) {
    logger_error(&g_logger, "Unsupported capture backend type: %s",
                 audio_backend_type_to_string(config->type));
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Unsupported capture backend type");
    return NULL;
  }
  return vtable->create(config, sample_rate, chunk_size, full_duplex, params,
                        err);
}

playback_backend_t *
create_playback_backend(const playback_device_config_t *config, int sample_rate,
                        int chunk_size, bool full_duplex,
                        processing_parameters_t *params, backend_error_t *err) {
  if (!config) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Config is NULL");
    return NULL;
  }
  const playback_backend_vtable_t *vtable = get_playback_vtable(config->type);
  if (!vtable || !vtable->create) {
    logger_error(&g_logger, "Unsupported playback backend type: %s",
                 audio_backend_type_to_string(config->type));
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Unsupported playback backend type");
    return NULL;
  }
  return vtable->create(config, sample_rate, chunk_size, full_duplex, params,
                        err);
}

/// Open the capture device
bool capture_backend_open(capture_backend_t *backend, backend_error_t *err) {
  if (!backend || !backend->vtable || !backend->vtable->open)
    return false;
  return backend->vtable->open(backend->ctx, err);
}

/// Read a chunk of audio into the provided buffer. Returns false on
/// end-of-stream or no data.
bool capture_backend_read(capture_backend_t *backend, size_t frames,
                          audio_chunk_t *chunk, backend_error_t *err) {
  if (!backend || !backend->vtable || !backend->vtable->read)
    return false;
  return backend->vtable->read(backend->ctx, frames, chunk, err);
}

/// Close the capture device
void capture_backend_close(capture_backend_t *backend) {
  if (!backend || !backend->vtable || !backend->vtable->close)
    return;
  backend->vtable->close(backend->ctx);
}

/// Get pending sample rate change detected on the capture device.
bool capture_backend_get_pending_rate_change(capture_backend_t *backend,
                                             double *out_rate) {
  if (!backend || !backend->vtable || !backend->vtable->get_pending_rate_change)
    return false;
  return backend->vtable->get_pending_rate_change(backend->ctx, out_rate);
}

/// Check if the capture device supports clock-pitch correction.
bool capture_backend_pitch_control_supported(capture_backend_t *backend) {
  if (!backend || !backend->vtable ||
      !backend->vtable->is_pitch_control_supported)
    return false;
  return backend->vtable->is_pitch_control_supported(backend->ctx);
}

/// Apply a clock-pitch correction to the capture device.
void capture_backend_set_pitch(capture_backend_t *backend, double multiplier) {
  if (!backend || !backend->vtable || !backend->vtable->set_pitch)
    return;
  backend->vtable->set_pitch(backend->ctx, multiplier);
}

/// Wait for new samples to become available, up to the given timeout.
bool capture_backend_wait(capture_backend_t *backend, uint32_t timeout_ms) {
  if (!backend || !backend->vtable || !backend->vtable->wait_for_data)
    return false;
  return backend->vtable->wait_for_data(backend->ctx, timeout_ms);
}

/// Notify the capture backend of the paused state.
void capture_backend_set_is_paused(capture_backend_t *backend, bool paused) {
  if (!backend || !backend->vtable || !backend->vtable->set_is_paused)
    return;
  backend->vtable->set_is_paused(backend->ctx, paused);
}

void capture_backend_stop(capture_backend_t *backend) {
  if (!backend || !backend->vtable || !backend->vtable->stop)
    return;
  backend->vtable->stop(backend->ctx);
}

/// Destroy and free the capture backend.
void capture_backend_free(capture_backend_t *backend) {
  if (!backend)
    return;
  if (backend->vtable && backend->vtable->destroy) {
    backend->vtable->destroy(backend->ctx);
  }
  free(backend);
}

bool capture_backend_is_realtime(const capture_backend_t *backend) {
  return backend ? backend->is_realtime : false;
}

/// Open the playback device
bool playback_backend_open(playback_backend_t *backend, backend_error_t *err) {
  if (!backend || !backend->vtable || !backend->vtable->open)
    return false;
  return backend->vtable->open(backend->ctx, err);
}

/// Write a chunk of audio
bool playback_backend_write(playback_backend_t *backend,
                            const audio_chunk_t *chunk, backend_error_t *err) {
  if (!backend || !backend->vtable || !backend->vtable->write)
    return false;
  return backend->vtable->write(backend->ctx, chunk, err);
}

/// Close the playback device
void playback_backend_close(playback_backend_t *backend) {
  if (!backend || !backend->vtable || !backend->vtable->close)
    return;
  backend->vtable->close(backend->ctx);
}

/// Get the current playback buffer level in samples
size_t playback_backend_get_buffer_level(playback_backend_t *backend) {
  if (!backend || !backend->vtable || !backend->vtable->get_buffer_level)
    return 0;
  return backend->vtable->get_buffer_level(backend->ctx);
}

/// Get pending sample rate change detected on the playback device.
bool playback_backend_get_pending_rate_change(playback_backend_t *backend,
                                              double *out_rate) {
  if (!backend || !backend->vtable || !backend->vtable->get_pending_rate_change)
    return false;
  return backend->vtable->get_pending_rate_change(backend->ctx, out_rate);
}

/// Push zero samples per channel into the output ring before first real chunk
/// arrives.
bool playback_backend_prefill_silence(playback_backend_t *backend,
                                      size_t frames, backend_error_t *err) {
  if (!backend || !backend->vtable || !backend->vtable->prefill_silence)
    return false;
  return backend->vtable->prefill_silence(backend->ctx, frames, err);
}

/// Get paused flag status.
bool playback_backend_get_is_paused(playback_backend_t *backend) {
  if (!backend || !backend->vtable || !backend->vtable->get_is_paused)
    return false;
  return backend->vtable->get_is_paused(backend->ctx);
}

/// Set paused flag status.
void playback_backend_set_is_paused(playback_backend_t *backend, bool paused) {
  if (!backend || !backend->vtable || !backend->vtable->set_is_paused)
    return;
  backend->vtable->set_is_paused(backend->ctx, paused);
}

bool playback_backend_pitch_control_supported(playback_backend_t *backend) {
  if (!backend || !backend->vtable || !backend->vtable->pitch_control_supported)
    return false;
  return backend->vtable->pitch_control_supported(backend->ctx);
}

void playback_backend_set_pitch(playback_backend_t *backend,
                                double multiplier) {
  if (!backend || !backend->vtable || !backend->vtable->set_pitch)
    return;
  backend->vtable->set_pitch(backend->ctx, multiplier);
}

void playback_backend_drain(playback_backend_t *backend) {
  if (!backend || !backend->vtable || !backend->vtable->drain)
    return;
  backend->vtable->drain(backend->ctx);
}

void playback_backend_stop(playback_backend_t *backend) {
  if (!backend || !backend->vtable || !backend->vtable->stop)
    return;
  backend->vtable->stop(backend->ctx);
}

/// Destroy and free the playback backend.
void playback_backend_free(playback_backend_t *backend) {
  if (!backend)
    return;
  if (backend->vtable && backend->vtable->destroy) {
    backend->vtable->destroy(backend->ctx);
  }
  free(backend);
}

bool audio_backend_ring_buffer_read(spsc_byte_ring_buffer_t *ring_buffer,
                                    size_t blockalign, size_t frames_requested,
                                    binary_sample_format_t fmt, size_t channels,
                                    _Atomic bool *thread_running,
                                    _Atomic bool *stopped,
                                    _Atomic bool *has_pending_rate_change,
                                    audio_chunk_t *chunk,
                                    backend_error_t *err) {
  if (!ring_buffer || !chunk || channels == 0 || blockalign == 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_READ_ERROR, "Invalid parameters");
    return false;
  }

  // Hardware sample rate or format changes must be handled immediately without
  // decoding further frames under obsolete parameters to prevent filter
  // corruption.
  if (has_pending_rate_change &&
      atomic_load_explicit(has_pending_rate_change, memory_order_acquire)) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_NONE, "Format change pending");
    return false;
  }

  if (frames_requested == 0) {
    audio_chunk_set_valid_frames(chunk, 0);
    return true;
  }

  if (audio_chunk_get_channels(chunk) < channels) {
    if (err)
      backend_error_init(
          err, BACKEND_ERROR_INVALID_CHANNELS,
          "Chunk channels count does not match capture channels");
    return false;
  }

  if (audio_chunk_get_frames(chunk) < frames_requested) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                         "Chunk frame capacity too small for requested frames");
    return false;
  }

  if (frames_requested > SIZE_MAX / blockalign) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                         "Requested frame count exceeds maximum size");
    return false;
  }

  size_t bytes_requested = frames_requested * blockalign;

  // Draining semantics: Only check `stopped` / `thread_running` flags after
  // confirming that the ring buffer lacks sufficient bytes. If the capture
  // worker thread wrote its final batch of samples and stopped, those remaining
  // frames must still be consumed to avoid dropping audio at EOF / stream
  // shutdown.
  if (spsc_byte_ring_buffer_get_available_to_read(ring_buffer) <
      bytes_requested) {
    if ((stopped && atomic_load_explicit(stopped, memory_order_acquire)) ||
        (thread_running &&
         !atomic_load_explicit(thread_running, memory_order_acquire))) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                           "Capture stream stopped");
      return false;
    }
    if (err)
      backend_error_init(err, BACKEND_ERROR_NONE, "");
    return false;
  }

  const uint8_t *s1 = NULL, *s2 = NULL;
  size_t l1 = 0, l2 = 0;
  size_t read_avail = spsc_byte_ring_buffer_get_read_slices(
      ring_buffer, bytes_requested, &s1, &l1, &s2, &l2);
  if (read_avail < bytes_requested || !s1) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                         "Failed to get read slices from ring buffer");
    return false;
  }

  if (l1 % blockalign == 0) {
    size_t f1 = l1 / blockalign;
    if (f1 > 0) {
      if (!audio_chunk_decode_interleaved_offset(s1, fmt, channels, f1, chunk,
                                                 0)) {
        if (err)
          backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                             "Failed to decode captured audio data");
        return false;
      }
    }
    size_t f2 = l2 / blockalign;
    if (f2 > 0 && s2) {
      if (!audio_chunk_decode_interleaved_offset(s2, fmt, channels, f2, chunk,
                                                 f1)) {
        if (err)
          backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                             "Failed to decode captured audio data");
        return false;
      }
    }
  } else {
    size_t f1 = l1 / blockalign;
    if (f1 > 0) {
      if (!audio_chunk_decode_interleaved_offset(s1, fmt, channels, f1, chunk,
                                                 0)) {
        if (err)
          backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                             "Failed to decode captured audio data");
        return false;
      }
    }
    size_t rem_l1 = l1 % blockalign;
    size_t rem_l2 = blockalign - rem_l1;
    uint8_t split_frame[256];
    if (blockalign <= sizeof(split_frame) && s2 && l2 >= rem_l2) {
      memcpy(split_frame, s1 + f1 * blockalign, rem_l1);
      memcpy(split_frame + rem_l1, s2, rem_l2);
      if (!audio_chunk_decode_interleaved_offset(split_frame, fmt, channels, 1,
                                                 chunk, f1)) {
        if (err)
          backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                             "Failed to decode split audio frame");
        return false;
      }
      size_t f2 = (l2 - rem_l2) / blockalign;
      if (f2 > 0) {
        if (!audio_chunk_decode_interleaved_offset(s2 + rem_l2, fmt, channels,
                                                   f2, chunk, f1 + 1)) {
          if (err)
            backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                               "Failed to decode captured audio data");
          return false;
        }
      }
    }
  }

  spsc_byte_ring_buffer_advance_read(ring_buffer, bytes_requested);
  audio_chunk_set_valid_frames(chunk, frames_requested);
  return true;
}

bool audio_backend_ring_buffer_write(
    spsc_byte_ring_buffer_t *ring_buffer, size_t blockalign,
    const audio_chunk_t *chunk, binary_sample_format_t fmt, size_t channels,
    uint32_t sleep_ms, uint32_t max_retries, _Atomic bool *thread_running,
    _Atomic bool *stopped, _Atomic bool *is_paused,
    _Atomic bool *has_pending_rate_change, backend_error_t *err) {
  if (!ring_buffer || !chunk || channels == 0 || blockalign == 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_WRITE_ERROR, "Invalid parameters");
    return false;
  }

  if (is_paused && atomic_load_explicit(is_paused, memory_order_acquire)) {
    return true;
  }

  if (has_pending_rate_change &&
      atomic_load_explicit(has_pending_rate_change, memory_order_acquire)) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_NONE, "Format change pending");
    return false;
  }

  if ((stopped && atomic_load_explicit(stopped, memory_order_acquire)) ||
      (thread_running &&
       !atomic_load_explicit(thread_running, memory_order_acquire))) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                         "Playback stream stopped");
    return false;
  }

  if (audio_chunk_get_channels(chunk) < channels) {
    if (err)
      backend_error_init(
          err, BACKEND_ERROR_INVALID_CHANNELS,
          "Chunk channels count does not match playback channels");
    return false;
  }

  size_t frames = audio_chunk_get_valid_frames(chunk);
  if (frames == 0)
    return true;

  if (frames > SIZE_MAX / blockalign) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                         "Frame count exceeds maximum size");
    return false;
  }

  size_t bytes_to_write = frames * blockalign;

  uint32_t retries = (max_retries > 0) ? max_retries : 1;
  for (uint32_t retry = 0; retry < retries; retry++) {
    if (spsc_byte_ring_buffer_get_available_to_write(ring_buffer) >=
        bytes_to_write) {
      break;
    }
    if (has_pending_rate_change &&
        atomic_load_explicit(has_pending_rate_change, memory_order_acquire)) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_NONE, "Format change pending");
      return false;
    }
    if ((stopped && atomic_load_explicit(stopped, memory_order_acquire)) ||
        (thread_running &&
         !atomic_load_explicit(thread_running, memory_order_acquire))) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                           "Playback stream stopped");
      return false;
    }
    if (is_paused && atomic_load_explicit(is_paused, memory_order_acquire)) {
      return true;
    }
    cdsp_sleep_ms(sleep_ms > 0 ? sleep_ms : 1);
  }

  // Audio chunks must be written as atomic units. If the ring buffer cannot fit
  // the complete chunk after backoff, drop the entire chunk rather than pushing
  // a fractured sub-chunk to prevent time-domain waveform discontinuity.
  if (spsc_byte_ring_buffer_get_available_to_write(ring_buffer) <
      bytes_to_write) {
    logger_debug(
        &g_logger,
        "Playback ring buffer is full after %u retries, dropped entire "
        "chunk of %zu bytes to preserve audio framing",
        retries, bytes_to_write);
    if (err)
      backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                         "Playback ring buffer full");
    return false;
  }

  uint8_t *s1 = NULL, *s2 = NULL;
  size_t l1 = 0, l2 = 0;
  size_t write_avail = spsc_byte_ring_buffer_get_write_slices(
      ring_buffer, bytes_to_write, &s1, &l1, &s2, &l2);
  if (write_avail < bytes_to_write || !s1) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                         "Failed to get write slices from ring buffer");
    return false;
  }

  if (l1 % blockalign == 0) {
    size_t f1 = l1 / blockalign;
    if (f1 > 0) {
      if (!audio_chunk_encode_interleaved_offset(chunk, fmt, channels, f1, s1,
                                                 0)) {
        if (err)
          backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                             "Failed to encode audio samples");
        return false;
      }
    }
    size_t f2 = l2 / blockalign;
    if (f2 > 0 && s2) {
      if (!audio_chunk_encode_interleaved_offset(chunk, fmt, channels, f2, s2,
                                                 f1)) {
        if (err)
          backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                             "Failed to encode audio samples");
        return false;
      }
    }
  } else {
    size_t f1 = l1 / blockalign;
    if (f1 > 0) {
      if (!audio_chunk_encode_interleaved_offset(chunk, fmt, channels, f1, s1,
                                                 0)) {
        if (err)
          backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                             "Failed to encode audio samples");
        return false;
      }
    }
    size_t rem_l1 = l1 % blockalign;
    size_t rem_l2 = blockalign - rem_l1;
    uint8_t split_frame[256];
    if (blockalign <= sizeof(split_frame) && s2 && l2 >= rem_l2) {
      if (!audio_chunk_encode_interleaved_offset(chunk, fmt, channels, 1,
                                                 split_frame, f1)) {
        if (err)
          backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                             "Failed to encode split audio frame");
        return false;
      }
      memcpy(s1 + f1 * blockalign, split_frame, rem_l1);
      memcpy(s2, split_frame + rem_l1, rem_l2);
      size_t f2 = (l2 - rem_l2) / blockalign;
      if (f2 > 0) {
        if (!audio_chunk_encode_interleaved_offset(chunk, fmt, channels, f2,
                                                   s2 + rem_l2, f1 + 1)) {
          if (err)
            backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                               "Failed to encode audio samples");
          return false;
        }
      }
    }
  }

  spsc_byte_ring_buffer_advance_write(ring_buffer, bytes_to_write);
  return true;
}

int audio_backend_validate_devices(const devices_config_t *devices,
                                   config_error_t *err) {
  if (!devices)
    return 0;

  // 1. File Backend validation
  if (devices->playback.type == AUDIO_BACKEND_TYPE_FILE) {
    if (devices->playback.cfg.raw_file.wav_header &&
        devices->playback.cfg.raw_file.format ==
            BINARY_SAMPLE_FORMAT_S24_4_RJ_LE) {
      config_error_set(
          err, CONFIG_ERR_INVALID_DEVICE,
          "Wav files do not support the S24_4_RJ_LE sample format");
      return -1;
    }
  }

  if (devices->capture.type == AUDIO_BACKEND_TYPE_FILE) {
    const char *fname = devices->capture.is_wav
                            ? devices->capture.cfg.wav_file.filename
                            : devices->capture.cfg.raw_file.filename;
    FILE *fp = (fname && fname[0] != '\0') ? fopen(fname, "rb") : NULL;
    if (!fp) {
      config_error_set(err, CONFIG_ERR_INVALID_DEVICE,
                       "Could not open input file '%s'", fname ? fname : "");
      return -1;
    }
    fclose(fp);
  }

#if defined(ENABLE_WASAPI)
  if (devices->capture.type == AUDIO_BACKEND_TYPE_WASAPI) {
    const wasapi_capture_config_t *wcap = &devices->capture.cfg.wasapi;
    if (!wcap->exclusive && wcap->has_format &&
        wcap->format != WASAPI_SAMPLE_FORMAT_F32) {
      config_error_set(
          err, CONFIG_ERR_INVALID_DEVICE,
          "Wasapi capture in shared mode only supports the F32 format");
      return -1;
    }
    if (wcap->loopback && wcap->exclusive) {
      config_error_set(err, CONFIG_ERR_INVALID_DEVICE,
                       "Wasapi loopback capture only supported in shared mode");
      return -1;
    }
  }
  if (devices->playback.type == AUDIO_BACKEND_TYPE_WASAPI) {
    const wasapi_playback_config_t *wplay = &devices->playback.cfg.wasapi;
    if (!wplay->exclusive && wplay->has_format &&
        wplay->format != WASAPI_SAMPLE_FORMAT_F32) {
      config_error_set(
          err, CONFIG_ERR_INVALID_DEVICE,
          "Wasapi playback in shared mode only supports the F32 format");
      return -1;
    }
  }
#endif

#if defined(ENABLE_ASIO)
  if (devices->capture.type == AUDIO_BACKEND_TYPE_ASIO &&
      devices->playback.type == AUDIO_BACKEND_TYPE_ASIO) {
    // Capture and playback on the same device share a single driver instance,
    // and therefore a single clock and sample rate, so there is nothing to
    // resample between. Different devices are independent and resample like any
    // other pair.
    if (strcmp(devices->capture.cfg.asio.device,
               devices->playback.cfg.asio.device) == 0 &&
        devices->has_resampler) {
      config_error_set(err, CONFIG_ERR_INVALID_DEVICE,
                       "Resampling is not supported in full-duplex ASIO mode. "
                       "Both capture and playback share the same driver and "
                       "sample rate");
      return -1;
    }
  }
#endif

  // Target level limit validation with backend-specific buffer calculation
  if (devices->has_target_level) {
    if (devices->target_level < 0) {
      config_error_set(err, CONFIG_ERR_INVALID_DEVICE,
                       "target_level must be a non-negative integer");
      return -1;
    }
    int64_t qlimit_val = devices->has_queuelimit ? devices->queuelimit : 4;
    int64_t target_limit = (2 + qlimit_val) * (int64_t)devices->chunksize;
#if defined(ENABLE_ALSA)
    if (devices->playback.type == AUDIO_BACKEND_TYPE_ALSA) {
      target_limit = (4 + qlimit_val) * (int64_t)devices->chunksize;
    }
#endif
    if ((int64_t)devices->target_level > target_limit) {
      config_error_set(err, CONFIG_ERR_INVALID_DEVICE,
                       "target_level cannot be larger than %lld",
                       (long long)target_limit);
      return -1;
    }
  }

#if defined(ENABLE_COREAUDIO)
  if (devices->capture.type == AUDIO_BACKEND_TYPE_CORE_AUDIO &&
      devices->playback.type == AUDIO_BACKEND_TYPE_CORE_AUDIO &&
      devices->capture.cfg.coreaudio.loopback && devices->has_resampler) {
    const char *cap_dev = devices->capture.cfg.coreaudio.has_device
                              ? devices->capture.cfg.coreaudio.device
                              : "";
    const char *pb_dev = devices->playback.cfg.coreaudio.has_device
                             ? devices->playback.cfg.coreaudio.device
                             : "";
    if (strcasecmp(cap_dev, pb_dev) == 0) {
      config_error_set(
          err, CONFIG_ERR_INVALID_DEVICE,
          "Resampling is not supported when CoreAudio loopback captures from "
          "the playback device. Both capture and playback share the same "
          "hardware clock and sample rate");
      return -1;
    }
  }
#endif

  return 0;
}

int audio_backend_apply_device_overrides(
    devices_config_t *devices, const dsp_config_overrides_t *overrides_in,
    config_error_t *err) {
  (void)err;
  if (!devices)
    return 0;

  dsp_config_overrides_t overrides;
  if (overrides_in) {
    overrides = *overrides_in;
  } else {
    memset(&overrides, 0, sizeof(overrides));
    overrides.samplerate = -1;
    overrides.channels = -1;
    overrides.extra_samples = -1;
  }

  // 1. If capture device is WavFile, read WAV info to populate base overrides
  if (devices->capture.type == AUDIO_BACKEND_TYPE_FILE &&
      devices->capture.is_wav && devices->capture.cfg.wav_file.has_filename) {
    const char *fname = devices->capture.cfg.wav_file.filename;
    wav_info_t wav_info;
    char wav_err[256];
    if (wav_read_info_from_file(fname, &wav_info, wav_err, sizeof(wav_err))) {
      logger_info(
          &g_logger,
          "Updating overrides with values from wav input file, rate %u, "
          "format: %s, channels: %u",
          wav_info.sample_rate, file_sample_format_to_string(wav_info.format),
          (unsigned int)wav_info.channels);
      devices->capture.cfg.wav_file.channels = wav_info.channels;
      overrides.channels = (int)wav_info.channels;
      overrides.sample_format = wav_info.format;
      overrides.has_sample_format = true;
      overrides.samplerate = (int)wav_info.sample_rate;
    } else {
      logger_warn(&g_logger, "Failed to read wav header from %s: %s", fname,
                  wav_err);
    }
  }

  // 2. Apply samplerate override
  if (overrides.samplerate > 0) {
    size_t rate = (size_t)overrides.samplerate;
    size_t cfg_rate = devices->samplerate;
    size_t cfg_chunksize = devices->chunksize;

    if (!devices->has_resampler) {
      logger_debug(&g_logger, "Apply override for samplerate: %zu", rate);
      devices->samplerate = rate;
      if (cfg_rate > 0 && cfg_chunksize > 0) {
        size_t scaled_chunksize = cfg_chunksize;
        if (rate > cfg_rate) {
          scaled_chunksize =
              cfg_chunksize * (size_t)round((double)rate / (double)cfg_rate);
        } else {
          size_t divisor = (size_t)round((double)cfg_rate / (double)rate);
          if (divisor > 0)
            scaled_chunksize = cfg_chunksize / divisor;
        }
        if (scaled_chunksize == 0) {
          logger_warn(&g_logger,
                      "Overriding the samplerate to %zu scales chunksize %zu "
                      "below one frame, using 1",
                      rate, cfg_chunksize);
          scaled_chunksize = 1;
        }
        logger_debug(&g_logger,
                     "Samplerate changed, adjusting chunksize: %zu -> %zu",
                     cfg_chunksize, scaled_chunksize);
        devices->chunksize = scaled_chunksize;

        if (devices->capture.type == AUDIO_BACKEND_TYPE_FILE) {
          if (!devices->capture.is_wav &&
              devices->capture.cfg.raw_file.has_extra_samples) {
            devices->capture.cfg.raw_file.extra_samples =
                devices->capture.cfg.raw_file.extra_samples * rate / cfg_rate;
          }
        } else if (devices->capture.type == AUDIO_BACKEND_TYPE_STDIN_OUT &&
                   devices->capture.cfg.stdin_in.has_extra_samples) {
          devices->capture.cfg.stdin_in.extra_samples =
              devices->capture.cfg.stdin_in.extra_samples * rate / cfg_rate;
        }
      }
    } else {
      logger_debug(&g_logger, "Apply override for capture_samplerate: %zu",
                   rate);
      devices->capture_samplerate = rate;
      devices->has_capture_samplerate = true;
      if (rate == cfg_rate && !devices->enable_rate_adjust) {
        logger_debug(&g_logger, "Disabling unnecessary 1:1 resampling");
        devices->has_resampler = false;
      }
    }
  }

  // 3. Apply extra_samples override
  if (overrides.has_extra_samples && overrides.extra_samples >= 0) {
    logger_debug(&g_logger, "Apply override for extra_samples: %d",
                 overrides.extra_samples);
    if (devices->capture.type == AUDIO_BACKEND_TYPE_FILE) {
      if (!devices->capture.is_wav) {
        devices->capture.cfg.raw_file.extra_samples = overrides.extra_samples;
        devices->capture.cfg.raw_file.has_extra_samples = true;
      }
    } else if (devices->capture.type == AUDIO_BACKEND_TYPE_STDIN_OUT) {
      devices->capture.cfg.stdin_in.extra_samples = overrides.extra_samples;
      devices->capture.cfg.stdin_in.has_extra_samples = true;
    }
  }

  // 4. Apply channels override
  if (overrides.channels > 0) {
    logger_debug(&g_logger, "Apply override for capture channels: %d",
                 overrides.channels);
    switch (devices->capture.type) {
    case AUDIO_BACKEND_TYPE_FILE:
      if (!devices->capture.is_wav) {
        devices->capture.cfg.raw_file.channels = overrides.channels;
      }
      break;
    case AUDIO_BACKEND_TYPE_STDIN_OUT:
      devices->capture.cfg.stdin_in.channels = overrides.channels;
      break;
    case AUDIO_BACKEND_TYPE_GENERATOR:
      devices->capture.cfg.generator.channels = overrides.channels;
      break;
#if defined(ENABLE_ALSA)
    case AUDIO_BACKEND_TYPE_ALSA:
      devices->capture.cfg.alsa.channels = overrides.channels;
      break;
#endif
#if defined(ENABLE_PIPEWIRE)
    case AUDIO_BACKEND_TYPE_PIPEWIRE:
      devices->capture.cfg.pipewire.channels = overrides.channels;
      break;
#endif
#if defined(ENABLE_COREAUDIO)
    case AUDIO_BACKEND_TYPE_CORE_AUDIO:
      devices->capture.cfg.coreaudio.channels = overrides.channels;
      break;
#endif
#if defined(ENABLE_WASAPI)
    case AUDIO_BACKEND_TYPE_WASAPI:
      devices->capture.cfg.wasapi.channels = overrides.channels;
      break;
#endif
#if defined(ENABLE_ASIO)
    case AUDIO_BACKEND_TYPE_ASIO:
      devices->capture.cfg.asio.channels = overrides.channels;
      break;
#endif
    case AUDIO_BACKEND_TYPE_INVALID:
      break;
    }
  }

  // 5. Apply sample_format override
  if (overrides.has_sample_format) {
    switch (devices->capture.type) {
    case AUDIO_BACKEND_TYPE_FILE:
      if (!devices->capture.is_wav) {
        devices->capture.cfg.raw_file.format = overrides.sample_format;
        devices->capture.cfg.raw_file.has_format = true;
        logger_debug(&g_logger, "Apply override for capture sample format: %s",
                     file_sample_format_to_string(overrides.sample_format));
      }
      break;
    case AUDIO_BACKEND_TYPE_STDIN_OUT:
      devices->capture.cfg.stdin_in.format = overrides.sample_format;
      logger_debug(&g_logger, "Apply override for capture sample format: %s",
                   file_sample_format_to_string(overrides.sample_format));
      break;
#if defined(ENABLE_ALSA)
    case AUDIO_BACKEND_TYPE_ALSA: {
      alsa_sample_format_t alsa_fmt =
          alsa_sample_format_from_binary_format(overrides.sample_format);
      if (alsa_fmt != ALSA_SAMPLE_FORMAT_INVALID) {
        devices->capture.cfg.alsa.format = alsa_fmt;
        devices->capture.cfg.alsa.has_format = true;
        logger_debug(&g_logger, "Apply override for capture sample format: %s",
                     alsa_sample_format_to_string(alsa_fmt));
      }
      break;
    }
#endif
#if defined(ENABLE_PIPEWIRE)
    case AUDIO_BACKEND_TYPE_PIPEWIRE:
      logger_error(
          &g_logger,
          "Not possible to override capture format for PipeWire, ignoring");
      break;
#endif
#if defined(ENABLE_COREAUDIO)
    case AUDIO_BACKEND_TYPE_CORE_AUDIO: {
      coreaudio_sample_format_t ca_fmt =
          coreaudio_sample_format_from_binary_format(overrides.sample_format);
      if (ca_fmt != COREAUDIO_SAMPLE_FORMAT_INVALID) {
        devices->capture.cfg.coreaudio.format = ca_fmt;
        devices->capture.cfg.coreaudio.has_format = true;
        logger_debug(&g_logger, "Apply override for capture sample format: %s",
                     coreaudio_sample_format_to_string(ca_fmt));
      } else {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "CoreAudio does not have a sample format corresponding to %s",
                 file_sample_format_to_string(overrides.sample_format));
        config_error_set(err, CONFIG_ERR_PARSE, "%s", msg);
        logger_error(&g_logger, "%s", msg);
        return -1;
      }
      break;
    }
#endif
#if defined(ENABLE_WASAPI)
    case AUDIO_BACKEND_TYPE_WASAPI: {
      wasapi_sample_format_t wasapi_fmt =
          wasapi_sample_format_from_binary_format(overrides.sample_format);
      if (wasapi_fmt != WASAPI_SAMPLE_FORMAT_INVALID) {
        devices->capture.cfg.wasapi.format = wasapi_fmt;
        devices->capture.cfg.wasapi.has_format = true;
        logger_debug(&g_logger, "Apply override for capture sample format: %s",
                     wasapi_sample_format_to_string(wasapi_fmt));
      } else {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "Wasapi does not have a sample format corresponding to %s",
                 file_sample_format_to_string(overrides.sample_format));
        config_error_set(err, CONFIG_ERR_PARSE, "%s", msg);
        logger_error(&g_logger, "%s", msg);
        return -1;
      }
      break;
    }
#endif
#if defined(ENABLE_ASIO)
    case AUDIO_BACKEND_TYPE_ASIO: {
      asio_sample_format_t asio_fmt =
          asio_sample_format_from_binary_format(overrides.sample_format);
      if (asio_fmt != ASIO_SAMPLE_FORMAT_INVALID) {
        devices->capture.cfg.asio.format = asio_fmt;
        devices->capture.cfg.asio.has_format = true;
        logger_debug(&g_logger, "Apply override for capture sample format: %s",
                     asio_sample_format_to_string(asio_fmt));
      } else {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "ASIO does not have a sample format corresponding to %s",
                 file_sample_format_to_string(overrides.sample_format));
        config_error_set(err, CONFIG_ERR_PARSE, "%s", msg);
        logger_error(&g_logger, "%s", msg);
        return -1;
      }
      break;
    }
#endif
    case AUDIO_BACKEND_TYPE_GENERATOR:
    case AUDIO_BACKEND_TYPE_INVALID:
      break;
    }
  }

  return 0;
}
