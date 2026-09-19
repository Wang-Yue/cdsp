#include "config/engine_config_types.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// Standalone Engine Configuration and API Types

const uint32_t STANDARD_RATES[STANDARD_RATES_COUNT] = {
    5512,  8000,  11025,  16000,  22050,  32000,  44100,  48000,  64000,
    88200, 96000, 176400, 192000, 352800, 384000, 705600, 768000,
};

/// Engine processing state.
uint8_t processing_state_to_raw_byte(processing_state_t state) {
  switch (state) {
  case PROCESSING_STATE_INACTIVE:
    return 0;
  case PROCESSING_STATE_STARTING:
    return 1;
  case PROCESSING_STATE_RUNNING:
    return 2;
  case PROCESSING_STATE_PAUSED:
    return 3;
  case PROCESSING_STATE_STALLED:
    return 4;
  }
  CDSP_UNREACHABLE();
  return 0;
}

processing_state_t processing_state_from_raw_byte(uint8_t raw_byte) {
  switch (raw_byte) {
  case 0:
    return PROCESSING_STATE_INACTIVE;
  case 1:
    return PROCESSING_STATE_STARTING;
  case 2:
    return PROCESSING_STATE_RUNNING;
  case 3:
    return PROCESSING_STATE_PAUSED;
  case 4:
    return PROCESSING_STATE_STALLED;
  }
  CDSP_UNREACHABLE();
  return PROCESSING_STATE_INACTIVE;
}

const char *processing_state_to_string(processing_state_t state) {
  switch (state) {
  case PROCESSING_STATE_INACTIVE:
    return "Inactive";
  case PROCESSING_STATE_STARTING:
    return "Starting";
  case PROCESSING_STATE_RUNNING:
    return "Running";
  case PROCESSING_STATE_PAUSED:
    return "Paused";
  case PROCESSING_STATE_STALLED:
    return "Stalled";
  }
  CDSP_UNREACHABLE();
  return "Inactive";
}

processing_state_t processing_state_from_string(const char *str) {
  if (!str)
    return PROCESSING_STATE_INACTIVE;
  if (strcmp(str, "Starting") == 0)
    return PROCESSING_STATE_STARTING;
  if (strcmp(str, "Running") == 0)
    return PROCESSING_STATE_RUNNING;
  if (strcmp(str, "Paused") == 0)
    return PROCESSING_STATE_PAUSED;
  if (strcmp(str, "Stalled") == 0)
    return PROCESSING_STATE_STALLED;
  return PROCESSING_STATE_INACTIVE;
}

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

// MARK: - Capability data model

const char *dsd_mode_to_string(dsd_mode_t mode) {
  switch (mode) {
  case DSD_MODE_DOP:
    return "dop";
  case DSD_MODE_NATIVE:
    return "dsd";
  case DSD_MODE_PCM:
    return "pcm";
  }
  CDSP_UNREACHABLE();
  return "pcm";
}

dsd_mode_t dsd_mode_from_string(const char *str) {
  if (!str)
    return DSD_MODE_PCM;
  if (strcasecmp(str, "dop") == 0)
    return DSD_MODE_DOP;
  if (strcasecmp(str, "dsd") == 0 || strcasecmp(str, "native") == 0)
    return DSD_MODE_NATIVE;
  return DSD_MODE_PCM;
}

const char *file_sample_format_to_string(binary_sample_format_t fmt) {
  return binary_sample_format_to_string(fmt);
}

binary_sample_format_t file_sample_format_from_string(const char *str) {
  return binary_sample_format_from_string(str);
}

size_t sample_format_bytes_per_sample(binary_sample_format_t fmt) {
  switch (fmt) {
  case BINARY_SAMPLE_FORMAT_DSD_U8:
    return 1;
  case BINARY_SAMPLE_FORMAT_S16_LE:
  case BINARY_SAMPLE_FORMAT_DSD_U16_LE:
  case BINARY_SAMPLE_FORMAT_DSD_U16_BE:
    return 2;
  case BINARY_SAMPLE_FORMAT_S24_3_LE:
    return 3;
  case BINARY_SAMPLE_FORMAT_S24_4_RJ_LE:
  case BINARY_SAMPLE_FORMAT_S24_4_LJ_LE:
  case BINARY_SAMPLE_FORMAT_S32_LE:
  case BINARY_SAMPLE_FORMAT_F32_LE:
  case BINARY_SAMPLE_FORMAT_DSD_U32_LE:
  case BINARY_SAMPLE_FORMAT_DSD_U32_BE:
  case BINARY_SAMPLE_FORMAT_DSD_U32_REVERSED:
    return 4;
  case BINARY_SAMPLE_FORMAT_F64_LE:
    return 8;
  case BINARY_SAMPLE_FORMAT_INVALID:
    return 0;
  }
  CDSP_UNREACHABLE();
  return 0;
}

bool sample_format_is_dsd(binary_sample_format_t fmt) {
  switch (fmt) {
  case BINARY_SAMPLE_FORMAT_DSD_U8:
  case BINARY_SAMPLE_FORMAT_DSD_U16_LE:
  case BINARY_SAMPLE_FORMAT_DSD_U16_BE:
  case BINARY_SAMPLE_FORMAT_DSD_U32_LE:
  case BINARY_SAMPLE_FORMAT_DSD_U32_BE:
  case BINARY_SAMPLE_FORMAT_DSD_U32_REVERSED:
    return true;
  case BINARY_SAMPLE_FORMAT_INVALID:
  case BINARY_SAMPLE_FORMAT_S16_LE:
  case BINARY_SAMPLE_FORMAT_S24_3_LE:
  case BINARY_SAMPLE_FORMAT_S24_4_RJ_LE:
  case BINARY_SAMPLE_FORMAT_S24_4_LJ_LE:
  case BINARY_SAMPLE_FORMAT_S32_LE:
  case BINARY_SAMPLE_FORMAT_F32_LE:
  case BINARY_SAMPLE_FORMAT_F64_LE:
    return false;
  }
  CDSP_UNREACHABLE();
  return false;
}

bool sample_format_is_float(binary_sample_format_t fmt) {
  switch (fmt) {
  case BINARY_SAMPLE_FORMAT_F32_LE:
  case BINARY_SAMPLE_FORMAT_F64_LE:
    return true;
  case BINARY_SAMPLE_FORMAT_INVALID:
  case BINARY_SAMPLE_FORMAT_S16_LE:
  case BINARY_SAMPLE_FORMAT_S24_3_LE:
  case BINARY_SAMPLE_FORMAT_S24_4_RJ_LE:
  case BINARY_SAMPLE_FORMAT_S24_4_LJ_LE:
  case BINARY_SAMPLE_FORMAT_S32_LE:
  case BINARY_SAMPLE_FORMAT_DSD_U8:
  case BINARY_SAMPLE_FORMAT_DSD_U16_LE:
  case BINARY_SAMPLE_FORMAT_DSD_U16_BE:
  case BINARY_SAMPLE_FORMAT_DSD_U32_LE:
  case BINARY_SAMPLE_FORMAT_DSD_U32_BE:
  case BINARY_SAMPLE_FORMAT_DSD_U32_REVERSED:
    return false;
  }
  CDSP_UNREACHABLE();
  return false;
}

#if defined(ENABLE_COREAUDIO)
binary_sample_format_t
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

coreaudio_sample_format_t
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
binary_sample_format_t
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

alsa_sample_format_t
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
binary_sample_format_t
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

wasapi_sample_format_t
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

asio_sample_format_t
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

sdm_filter_t playback_device_config_get_dsd_encoder_filter(
    const playback_device_config_t *config) {
  if (!config)
    return SDM_FILTER_INVALID;
  return config->dsd_encoder_filter;
}

/// Capture sample rate when different from playback (requires resampler)
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
