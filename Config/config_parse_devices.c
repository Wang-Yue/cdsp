#include "config_parse_devices.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "Logging/app_logger.h"
#include "cJSON.h"
#include "config_parser_internal.h"
#include "configuration.h"

/**
 * @brief Parses the "resampler" JSON object and populates the devices
 * configuration.
 *
 * @param res_obj The cJSON object containing resampler settings.
 * @param devices Pointer to the devices configuration structure to populate.
 */
static void parse_resampler(const cJSON* res_obj, devices_config_t* devices) {
  if (!cJSON_IsObject(res_obj)) return;
  resampler_config_t* res = &devices->resampler;
  devices->has_resampler = true;

  cJSON* item;

  item = cJSON_GetObjectItemCaseSensitive(res_obj, "type");
  if (cJSON_IsString(item) && item->valuestring) {
    res->type = resampler_type_from_string(item->valuestring);
  }

  item = cJSON_GetObjectItemCaseSensitive(res_obj, "profile");
  if (cJSON_IsString(item) && item->valuestring) {
    strncpy(res->profile, item->valuestring, sizeof(res->profile) - 1);
    res->has_profile = true;
  }

  item = cJSON_GetObjectItemCaseSensitive(res_obj, "interpolation");
  if (cJSON_IsString(item) && item->valuestring) {
    strncpy(res->interpolation, item->valuestring,
            sizeof(res->interpolation) - 1);
    res->has_interpolation = true;
  }

#if defined(ENABLE_COREAUDIO)
  item = cJSON_GetObjectItemCaseSensitive(res_obj, "apple_quality");
  if (cJSON_IsString(item) && item->valuestring) {
    res->apple_quality = apple_resampler_quality_from_string(item->valuestring);
    res->has_apple_quality = true;
  }
  item = cJSON_GetObjectItemCaseSensitive(res_obj, "apple_complexity");
  if (cJSON_IsString(item) && item->valuestring) {
    res->apple_complexity =
        apple_resampler_complexity_from_string(item->valuestring);
    res->has_apple_complexity = true;
  }
#endif

  item = cJSON_GetObjectItemCaseSensitive(res_obj, "sinc_len");
  if (cJSON_IsNumber(item)) {
    res->sinc_len = item->valueint;
    res->has_sinc_len = (res->sinc_len > 0);
  }

  item = cJSON_GetObjectItemCaseSensitive(res_obj, "oversampling_factor");
  if (cJSON_IsNumber(item)) {
    res->oversampling_factor = item->valueint;
    res->has_oversampling_factor = (res->oversampling_factor > 0);
  }

  item = cJSON_GetObjectItemCaseSensitive(res_obj, "window");
  if (cJSON_IsString(item) && item->valuestring) {
    strncpy(res->window, item->valuestring, sizeof(res->window) - 1);
    res->has_window = true;
  }

  item = cJSON_GetObjectItemCaseSensitive(res_obj, "f_cutoff");
  if (cJSON_IsNumber(item)) {
    res->f_cutoff = item->valuedouble;
    res->has_f_cutoff = (res->f_cutoff > 0.0);
  }

  item = cJSON_GetObjectItemCaseSensitive(res_obj, "fixed");
  if (cJSON_IsString(item) && item->valuestring) {
    if (strcasecmp(item->valuestring, "output") == 0) {
      res->fixed = FIXED_ASYNC_OUTPUT;
    } else {
      res->fixed = FIXED_ASYNC_INPUT;
    }
    res->has_fixed = true;
  }
}

typedef struct {
  audio_backend_type_t type;
  int channels;
  char device[256];
  bool has_device;
#if defined(ENABLE_COREAUDIO)
  coreaudio_sample_format_t format;
  bool has_format;
#endif
#if defined(ENABLE_ALSA)
  alsa_sample_format_t alsa_format;
  bool has_alsa_format;
#endif
  bool bypass_dop;
  bool has_bypass_dop;
  double dop_cutoff_hz;
  bool has_dop_cutoff_hz;
  generator_signal_t generator;

  char filename[512];
  bool has_filename;
  binary_sample_format_t file_format;
  bool has_file_format;
  bool is_wav;
  bool has_is_wav;
  int skip_bytes;
  bool has_skip_bytes;
  int read_bytes;
  bool has_read_bytes;
  int extra_samples;
  bool has_extra_samples;
#ifdef CDSP_TEST
  bool realtime;
  bool has_realtime;
#endif
  char** labels;
  size_t labels_count;
  bool has_labels;

  // Win32 / WASAPI / ASIO Capture fields
  bool loopback;
  bool has_loopback;
  bool exclusive;
  bool has_exclusive;
  bool polling;
  bool has_polling;
#if defined(ENABLE_WASAPI)
  wasapi_sample_format_t wasapi_format;
  bool has_wasapi_format;
#endif
#if defined(ENABLE_ASIO)
  asio_sample_format_t asio_format;
  bool has_asio_format;
#endif

  // ALSA / Pulse / PipeWire Capture fields
  bool stop_on_inactive;
  bool has_stop_on_inactive;
  char link_volume_control[256];
  bool has_link_volume_control;
  char link_mute_control[256];
  bool has_link_mute_control;

  // PipeWire Capture fields
  char node_name[256];
  bool has_node_name;
  char node_description[256];
  bool has_node_description;
  char node_group_name[256];
  bool has_node_group_name;
  char autoconnect_to[256];
  bool has_autoconnect_to;

  // Bluez Capture fields
  char service[256];
  bool has_service;
  char dbus_path[256];
  bool has_dbus_path;
#if defined(ENABLE_BLUEZ)
  binary_sample_format_t bluez_format;
#endif
} flat_capture_device_config_t;

/**
 * @brief Parses the "capture" JSON object and populates the capture
 * configuration.
 *
 * @param cap_obj The cJSON object containing capture settings.
 * @param devices Pointer to the devices configuration structure to populate.
 */
static void parse_capture(const cJSON* cap_obj, devices_config_t* devices) {
  if (!cJSON_IsObject(cap_obj)) return;

  flat_capture_device_config_t temp = {0};
  flat_capture_device_config_t* cap = &temp;

  cJSON* item;

  item = cJSON_GetObjectItemCaseSensitive(cap_obj, "channels");
  if (cJSON_IsNumber(item)) {
    cap->channels = item->valueint;
  }

  item = cJSON_GetObjectItemCaseSensitive(cap_obj, "type");
  char type_str[64] = "";
  if (cJSON_IsString(item) && item->valuestring) {
    strncpy(type_str, item->valuestring, sizeof(type_str) - 1);
    cap->type = audio_backend_type_from_string(type_str);
    if (strcasecmp(type_str, "WavFile") == 0) {
      cap->is_wav = true;
      cap->has_is_wav = true;
    }
  } else {
#if defined(ENABLE_COREAUDIO)
    cap->type = AUDIO_BACKEND_TYPE_CORE_AUDIO;
#elif defined(ENABLE_ALSA)
    cap->type = AUDIO_BACKEND_TYPE_ALSA;
#elif defined(ENABLE_WASAPI)
    cap->type = AUDIO_BACKEND_TYPE_WASAPI;
#else
    cap->type = AUDIO_BACKEND_TYPE_FILE;
#endif
  }

  item = cJSON_GetObjectItemCaseSensitive(cap_obj, "device");
  if (cJSON_IsString(item) && item->valuestring) {
    strncpy(cap->device, item->valuestring, sizeof(cap->device) - 1);
    cap->has_device = true;
  }

  item = cJSON_GetObjectItemCaseSensitive(cap_obj, "filename");
  if (cJSON_IsString(item) && item->valuestring) {
    strncpy(cap->filename, item->valuestring, sizeof(cap->filename) - 1);
    cap->has_filename = true;
  }

  item = cJSON_GetObjectItemCaseSensitive(cap_obj, "format");
  if (cJSON_IsString(item) && item->valuestring) {
    if (cap->type == AUDIO_BACKEND_TYPE_FILE ||
        cap->type == AUDIO_BACKEND_TYPE_STDIN_OUT) {
      cap->file_format = binary_sample_format_from_string(item->valuestring);
      cap->has_file_format = true;
#if defined(ENABLE_WASAPI)
    } else if (cap->type == AUDIO_BACKEND_TYPE_WASAPI) {
      cap->wasapi_format = wasapi_sample_format_from_string(item->valuestring);
      cap->has_wasapi_format = true;
#endif
#if defined(ENABLE_ASIO)
    } else if (cap->type == AUDIO_BACKEND_TYPE_ASIO) {
      cap->asio_format = asio_sample_format_from_string(item->valuestring);
      cap->has_asio_format = true;
#endif
#if defined(ENABLE_ALSA)
    } else if (cap->type == AUDIO_BACKEND_TYPE_ALSA) {
      cap->alsa_format = alsa_sample_format_from_string(item->valuestring);
      cap->has_alsa_format = true;
#endif
#if defined(ENABLE_COREAUDIO)
    } else if (cap->type == AUDIO_BACKEND_TYPE_CORE_AUDIO) {
      cap->format = coreaudio_sample_format_from_string(item->valuestring);
      cap->has_format = true;
#endif
#if defined(ENABLE_BLUEZ)
    } else if (cap->type == AUDIO_BACKEND_TYPE_BLUEZ) {
      cap->bluez_format = binary_sample_format_from_string(item->valuestring);
#endif
    }
  }

  item = cJSON_GetObjectItemCaseSensitive(cap_obj, "skip_bytes");
  if (cJSON_IsNumber(item)) {
    cap->skip_bytes = item->valueint;
    cap->has_skip_bytes = true;
  }

  item = cJSON_GetObjectItemCaseSensitive(cap_obj, "read_bytes");
  if (cJSON_IsNumber(item)) {
    cap->read_bytes = item->valueint;
    cap->has_read_bytes = true;
  }

  item = cJSON_GetObjectItemCaseSensitive(cap_obj, "extra_samples");
  if (cJSON_IsNumber(item)) {
    cap->extra_samples = item->valueint;
    cap->has_extra_samples = true;
  }

  item = cJSON_GetObjectItemCaseSensitive(cap_obj, "exclusive");
  if (cJSON_IsBool(item)) {
    cap->exclusive = cJSON_IsTrue(item);
    cap->has_exclusive = true;
  }

  item = cJSON_GetObjectItemCaseSensitive(cap_obj, "loopback");
  if (cJSON_IsBool(item)) {
    cap->loopback = cJSON_IsTrue(item);
    cap->has_loopback = true;
  }

#if defined(_WIN32)
  item = cJSON_GetObjectItemCaseSensitive(cap_obj, "polling");
  if (cJSON_IsBool(item)) {
    cap->polling = cJSON_IsTrue(item);
    cap->has_polling = true;
  }
#endif

#if defined(ENABLE_ALSA)
  item = cJSON_GetObjectItemCaseSensitive(cap_obj, "stop_on_inactive");
  if (cJSON_IsBool(item)) {
    cap->stop_on_inactive = cJSON_IsTrue(item);
    cap->has_stop_on_inactive = true;
  }
  item = cJSON_GetObjectItemCaseSensitive(cap_obj, "link_volume_control");
  if (cJSON_IsString(item) && item->valuestring) {
    strncpy(cap->link_volume_control, item->valuestring,
            sizeof(cap->link_volume_control) - 1);
    cap->has_link_volume_control = true;
  }
  item = cJSON_GetObjectItemCaseSensitive(cap_obj, "link_mute_control");
  if (cJSON_IsString(item) && item->valuestring) {
    strncpy(cap->link_mute_control, item->valuestring,
            sizeof(cap->link_mute_control) - 1);
    cap->has_link_mute_control = true;
  }
#endif

#if defined(ENABLE_PIPEWIRE)
  item = cJSON_GetObjectItemCaseSensitive(cap_obj, "node_name");
  if (cJSON_IsString(item) && item->valuestring) {
    strncpy(cap->node_name, item->valuestring, sizeof(cap->node_name) - 1);
    cap->has_node_name = true;
  }
  item = cJSON_GetObjectItemCaseSensitive(cap_obj, "node_description");
  if (cJSON_IsString(item) && item->valuestring) {
    strncpy(cap->node_description, item->valuestring,
            sizeof(cap->node_description) - 1);
    cap->has_node_description = true;
  }
  item = cJSON_GetObjectItemCaseSensitive(cap_obj, "node_group_name");
  if (cJSON_IsString(item) && item->valuestring) {
    strncpy(cap->node_group_name, item->valuestring,
            sizeof(cap->node_group_name) - 1);
    cap->has_node_group_name = true;
  }
  item = cJSON_GetObjectItemCaseSensitive(cap_obj, "autoconnect_to");
  if (cJSON_IsString(item) && item->valuestring) {
    strncpy(cap->autoconnect_to, item->valuestring,
            sizeof(cap->autoconnect_to) - 1);
    cap->has_autoconnect_to = true;
  }
#endif

  parse_labels_array(cJSON_GetObjectItemCaseSensitive(cap_obj, "labels"),
                     &cap->labels, &cap->labels_count, &cap->has_labels);

#if defined(ENABLE_BLUEZ)
  item = cJSON_GetObjectItemCaseSensitive(cap_obj, "service");
  if (cJSON_IsString(item) && item->valuestring) {
    strncpy(cap->service, item->valuestring, sizeof(cap->service) - 1);
    cap->has_service = true;
  }
  item = cJSON_GetObjectItemCaseSensitive(cap_obj, "dbus_path");
  if (cJSON_IsString(item) && item->valuestring) {
    strncpy(cap->dbus_path, item->valuestring, sizeof(cap->dbus_path) - 1);
    cap->has_dbus_path = true;
  }
#endif

  item = cJSON_GetObjectItemCaseSensitive(cap_obj, "bypass_dop");
  if (cJSON_IsBool(item)) {
    cap->bypass_dop = cJSON_IsTrue(item);
    cap->has_bypass_dop = true;
  }

  item = cJSON_GetObjectItemCaseSensitive(cap_obj, "dop_cutoff_hz");
  if (cJSON_IsNumber(item)) {
    cap->dop_cutoff_hz = item->valuedouble;
    cap->has_dop_cutoff_hz = true;
  }

#ifdef CDSP_TEST
  item = cJSON_GetObjectItemCaseSensitive(cap_obj, "realtime");
  if (cJSON_IsBool(item)) {
    cap->realtime = cJSON_IsTrue(item);
    cap->has_realtime = true;
  }
#endif

  cJSON* sig_obj = cJSON_GetObjectItemCaseSensitive(cap_obj, "signal");
  if (cJSON_IsObject(sig_obj)) {
    cJSON* sig_type = cJSON_GetObjectItemCaseSensitive(sig_obj, "type");
    if (cJSON_IsString(sig_type) && sig_type->valuestring) {
      cap->generator.type = signal_type_from_string(sig_type->valuestring);
    } else {
      cap->generator.type = SIGNAL_TYPE_SINE;
    }
    cJSON* freq = cJSON_GetObjectItemCaseSensitive(sig_obj, "freq");
    if (cJSON_IsNumber(freq)) {
      cap->generator.frequency = freq->valuedouble;
    } else {
      cap->generator.frequency = 1000.0;
    }
    cJSON* level = cJSON_GetObjectItemCaseSensitive(sig_obj, "level");
    if (cJSON_IsNumber(level)) {
      cap->generator.level = level->valuedouble;
    } else {
      cap->generator.level = 0.0;
    }
  }

  // Copy flat temp to union configuration
  capture_device_config_t* final_cap = &devices->capture;
  final_cap->type = temp.type;
  final_cap->labels = temp.labels;
  final_cap->labels_count = temp.labels_count;
  final_cap->has_labels = temp.has_labels;
  final_cap->is_wav = temp.is_wav;
  final_cap->has_is_wav = temp.has_is_wav;
  final_cap->bypass_dop = temp.has_bypass_dop ? temp.bypass_dop : true;
  final_cap->has_bypass_dop = temp.has_bypass_dop;
  final_cap->dop_cutoff_hz =
      temp.has_dop_cutoff_hz ? temp.dop_cutoff_hz : 20000.0;
  final_cap->has_dop_cutoff_hz = temp.has_dop_cutoff_hz;

  switch (temp.type) {
#if defined(ENABLE_COREAUDIO)
    case AUDIO_BACKEND_TYPE_CORE_AUDIO:
      final_cap->cfg.coreaudio.channels = temp.channels;
      snprintf(final_cap->cfg.coreaudio.device,
               sizeof(final_cap->cfg.coreaudio.device), "%s", temp.device);
      final_cap->cfg.coreaudio.has_device = temp.has_device;
      final_cap->cfg.coreaudio.format = temp.format;
      final_cap->cfg.coreaudio.has_format = temp.has_format;
      break;
#endif
#if defined(ENABLE_ALSA)
    case AUDIO_BACKEND_TYPE_ALSA:
      final_cap->cfg.alsa.channels = temp.channels;
      snprintf(final_cap->cfg.alsa.device, sizeof(final_cap->cfg.alsa.device),
               "%s", temp.device);
      final_cap->cfg.alsa.format = temp.alsa_format;
      final_cap->cfg.alsa.has_format = temp.has_alsa_format;
      final_cap->cfg.alsa.stop_on_inactive = temp.stop_on_inactive;
      final_cap->cfg.alsa.has_stop_on_inactive = temp.has_stop_on_inactive;
      snprintf(final_cap->cfg.alsa.link_volume_control,
               sizeof(final_cap->cfg.alsa.link_volume_control), "%s",
               temp.link_volume_control);
      final_cap->cfg.alsa.has_link_volume_control =
          temp.has_link_volume_control;
      snprintf(final_cap->cfg.alsa.link_mute_control,
               sizeof(final_cap->cfg.alsa.link_mute_control), "%s",
               temp.link_mute_control);
      final_cap->cfg.alsa.has_link_mute_control = temp.has_link_mute_control;
      break;
#endif
#if defined(ENABLE_PULSE)
    case AUDIO_BACKEND_TYPE_PULSE_AUDIO:
      final_cap->cfg.pulse.channels = temp.channels;
      snprintf(final_cap->cfg.pulse.device, sizeof(final_cap->cfg.pulse.device),
               "%s", temp.device);
      break;
#endif
#if defined(ENABLE_PIPEWIRE)
    case AUDIO_BACKEND_TYPE_PIPEWIRE:
      final_cap->cfg.pipewire.channels = temp.channels;
      snprintf(final_cap->cfg.pipewire.device,
               sizeof(final_cap->cfg.pipewire.device), "%s", temp.device);
      final_cap->cfg.pipewire.has_device = temp.has_device;
      snprintf(final_cap->cfg.pipewire.node_name,
               sizeof(final_cap->cfg.pipewire.node_name), "%s", temp.node_name);
      final_cap->cfg.pipewire.has_node_name = temp.has_node_name;
      snprintf(final_cap->cfg.pipewire.node_description,
               sizeof(final_cap->cfg.pipewire.node_description), "%s",
               temp.node_description);
      final_cap->cfg.pipewire.has_node_description = temp.has_node_description;
      snprintf(final_cap->cfg.pipewire.node_group_name,
               sizeof(final_cap->cfg.pipewire.node_group_name), "%s",
               temp.node_group_name);
      final_cap->cfg.pipewire.has_node_group_name = temp.has_node_group_name;
      snprintf(final_cap->cfg.pipewire.autoconnect_to,
               sizeof(final_cap->cfg.pipewire.autoconnect_to), "%s",
               temp.autoconnect_to);
      final_cap->cfg.pipewire.has_autoconnect_to = temp.has_autoconnect_to;
      break;
#endif
#if defined(ENABLE_JACK)
    case AUDIO_BACKEND_TYPE_JACK:
      final_cap->cfg.jack.channels = temp.channels;
      snprintf(final_cap->cfg.jack.device, sizeof(final_cap->cfg.jack.device),
               "%s", temp.device);
      break;
#endif
    case AUDIO_BACKEND_TYPE_FILE:
      if (temp.is_wav) {
        snprintf(final_cap->cfg.wav_file.filename,
                 sizeof(final_cap->cfg.wav_file.filename), "%s", temp.filename);
        final_cap->cfg.wav_file.has_filename = temp.has_filename;
        final_cap->cfg.wav_file.extra_samples = temp.extra_samples;
        final_cap->cfg.wav_file.has_extra_samples = temp.has_extra_samples;
        final_cap->cfg.raw_file.channels = temp.channels;
#ifdef CDSP_TEST
        final_cap->cfg.wav_file.realtime = temp.realtime;
        final_cap->cfg.wav_file.has_realtime = temp.has_realtime;
#endif
      } else {
        snprintf(final_cap->cfg.raw_file.filename,
                 sizeof(final_cap->cfg.raw_file.filename), "%s", temp.filename);
        final_cap->cfg.raw_file.has_filename = temp.has_filename;
        final_cap->cfg.raw_file.format = temp.file_format;
        final_cap->cfg.raw_file.has_format = temp.has_file_format;
        final_cap->cfg.raw_file.channels = temp.channels;
        final_cap->cfg.raw_file.skip_bytes = temp.skip_bytes;
        final_cap->cfg.raw_file.has_skip_bytes = temp.has_skip_bytes;
        final_cap->cfg.raw_file.read_bytes = temp.read_bytes;
        final_cap->cfg.raw_file.has_read_bytes = temp.has_read_bytes;
        final_cap->cfg.raw_file.extra_samples = temp.extra_samples;
        final_cap->cfg.raw_file.has_extra_samples = temp.has_extra_samples;
#ifdef CDSP_TEST
        final_cap->cfg.raw_file.realtime = temp.realtime;
        final_cap->cfg.raw_file.has_realtime = temp.has_realtime;
#endif
      }
      break;
    case AUDIO_BACKEND_TYPE_STDIN_OUT:
      final_cap->cfg.stdin_in.channels = temp.channels;
      final_cap->cfg.stdin_in.format = temp.file_format;
      final_cap->cfg.stdin_in.extra_samples = temp.extra_samples;
      final_cap->cfg.stdin_in.has_extra_samples = temp.has_extra_samples;
      final_cap->cfg.stdin_in.skip_bytes = temp.skip_bytes;
      final_cap->cfg.stdin_in.has_skip_bytes = temp.has_skip_bytes;
      final_cap->cfg.stdin_in.read_bytes = temp.read_bytes;
      final_cap->cfg.stdin_in.has_read_bytes = temp.has_read_bytes;
      break;
    case AUDIO_BACKEND_TYPE_GENERATOR:
      final_cap->cfg.generator.channels = temp.channels;
      final_cap->cfg.generator.signal = temp.generator;
      break;
#if defined(ENABLE_WASAPI)
    case AUDIO_BACKEND_TYPE_WASAPI:
      final_cap->cfg.wasapi.channels = temp.channels;
      snprintf(final_cap->cfg.wasapi.device,
               sizeof(final_cap->cfg.wasapi.device), "%s", temp.device);
      final_cap->cfg.wasapi.has_device = temp.has_device;
      final_cap->cfg.wasapi.format = temp.wasapi_format;
      final_cap->cfg.wasapi.has_format = temp.has_wasapi_format;
      final_cap->cfg.wasapi.exclusive = temp.exclusive;
      final_cap->cfg.wasapi.has_exclusive = temp.has_exclusive;
      final_cap->cfg.wasapi.loopback = temp.loopback;
      final_cap->cfg.wasapi.has_loopback = temp.has_loopback;
      final_cap->cfg.wasapi.polling = temp.polling;
      final_cap->cfg.wasapi.has_polling = temp.has_polling;
      break;
#endif
#if defined(ENABLE_ASIO)
    case AUDIO_BACKEND_TYPE_ASIO:
      final_cap->cfg.asio.channels = temp.channels;
      snprintf(final_cap->cfg.asio.device, sizeof(final_cap->cfg.asio.device),
               "%s", temp.device);
      final_cap->cfg.asio.format = temp.asio_format;
      final_cap->cfg.asio.has_format = temp.has_asio_format;
      break;
#endif
#if defined(ENABLE_BLUEZ)
    case AUDIO_BACKEND_TYPE_BLUEZ:
      snprintf(final_cap->cfg.bluez.service,
               sizeof(final_cap->cfg.bluez.service), "%s", temp.service);
      final_cap->cfg.bluez.has_service = temp.has_service;
      snprintf(final_cap->cfg.bluez.dbus_path,
               sizeof(final_cap->cfg.bluez.dbus_path), "%s", temp.dbus_path);
      final_cap->cfg.bluez.has_dbus_path = temp.has_dbus_path;
      final_cap->cfg.bluez.format = temp.bluez_format;
      final_cap->cfg.bluez.channels = temp.channels;
      break;
#endif
    default:
      break;
  }
}

typedef struct {
  audio_backend_type_t type;
  int channels;
  char device[256];
  bool has_device;
#if defined(ENABLE_COREAUDIO)
  coreaudio_sample_format_t format;
  bool has_format;
#endif
#if defined(ENABLE_ALSA)
  alsa_sample_format_t alsa_format;
  bool has_alsa_format;
#endif
  bool exclusive;
  bool has_exclusive;
  bool output_dop;
  bool has_output_dop;
  bool output_dsd;
  bool has_output_dsd;
  sdm_filter_t dop_encoder_filter;
  bool has_dop_encoder_filter;
  char filename[512];
  bool has_filename;
  binary_sample_format_t file_format;
  bool has_file_format;
  bool is_wav;
  bool has_is_wav;
#ifdef CDSP_TEST
  bool realtime;
  bool has_realtime;
#endif
  char** labels;
  size_t labels_count;
  bool has_labels;

  // Win32 / WASAPI / ASIO fields
  bool polling;
  bool has_polling;
#if defined(ENABLE_WASAPI)
  wasapi_sample_format_t wasapi_format;
  bool has_wasapi_format;
#endif
#if defined(ENABLE_ASIO)
  asio_sample_format_t asio_format;
  bool has_asio_format;
#endif

  // PipeWire fields
  char node_name[256];
  bool has_node_name;
  char node_description[256];
  bool has_node_description;
  char node_group_name[256];
  bool has_node_group_name;
  char autoconnect_to[256];
  bool has_autoconnect_to;
} flat_playback_device_config_t;

/**
 * @brief Parses the "playback" JSON object and populates the playback
 * configuration.
 *
 * @param play_obj The cJSON object containing playback settings.
 * @param devices Pointer to the devices configuration structure to populate.
 */
static void parse_playback(const cJSON* play_obj, devices_config_t* devices) {
  if (!cJSON_IsObject(play_obj)) return;

  flat_playback_device_config_t temp = {0};
  flat_playback_device_config_t* play = &temp;

  cJSON* item;

  item = cJSON_GetObjectItemCaseSensitive(play_obj, "channels");
  if (cJSON_IsNumber(item)) {
    play->channels = item->valueint;
  }

  item = cJSON_GetObjectItemCaseSensitive(play_obj, "type");
  if (cJSON_IsString(item) && item->valuestring) {
    play->type = audio_backend_type_from_string(item->valuestring);
  } else {
#if defined(ENABLE_COREAUDIO)
    play->type = AUDIO_BACKEND_TYPE_CORE_AUDIO;
#elif defined(ENABLE_ALSA)
    play->type = AUDIO_BACKEND_TYPE_ALSA;
#elif defined(ENABLE_WASAPI)
    play->type = AUDIO_BACKEND_TYPE_WASAPI;
#else
    play->type = AUDIO_BACKEND_TYPE_FILE;
#endif
  }

  item = cJSON_GetObjectItemCaseSensitive(play_obj, "device");
  if (cJSON_IsString(item) && item->valuestring) {
    strncpy(play->device, item->valuestring, sizeof(play->device) - 1);
    play->has_device = true;
  }

  item = cJSON_GetObjectItemCaseSensitive(play_obj, "filename");
  if (cJSON_IsString(item) && item->valuestring) {
    strncpy(play->filename, item->valuestring, sizeof(play->filename) - 1);
    play->has_filename = true;
  }

  item = cJSON_GetObjectItemCaseSensitive(play_obj, "format");
  if (cJSON_IsString(item) && item->valuestring) {
    if (play->type == AUDIO_BACKEND_TYPE_FILE ||
        play->type == AUDIO_BACKEND_TYPE_STDIN_OUT) {
      play->file_format = binary_sample_format_from_string(item->valuestring);
      play->has_file_format = true;
#if defined(ENABLE_WASAPI)
    } else if (play->type == AUDIO_BACKEND_TYPE_WASAPI) {
      play->wasapi_format = wasapi_sample_format_from_string(item->valuestring);
      play->has_wasapi_format = true;
#endif
#if defined(ENABLE_ASIO)
    } else if (play->type == AUDIO_BACKEND_TYPE_ASIO) {
      play->asio_format = asio_sample_format_from_string(item->valuestring);
      play->has_asio_format = true;
#endif
#if defined(ENABLE_ALSA)
    } else if (play->type == AUDIO_BACKEND_TYPE_ALSA) {
      play->alsa_format = alsa_sample_format_from_string(item->valuestring);
      play->has_alsa_format = true;
#endif
#if defined(ENABLE_COREAUDIO)
    } else if (play->type == AUDIO_BACKEND_TYPE_CORE_AUDIO) {
      play->format = coreaudio_sample_format_from_string(item->valuestring);
      play->has_format = true;
#endif
    }
  }

  item = cJSON_GetObjectItemCaseSensitive(play_obj, "wav_header");
  if (cJSON_IsBool(item)) {
    play->is_wav = cJSON_IsTrue(item);
    play->has_is_wav = true;
  }

  item = cJSON_GetObjectItemCaseSensitive(play_obj, "exclusive");
  if (cJSON_IsBool(item)) {
    play->exclusive = cJSON_IsTrue(item);
    play->has_exclusive = true;
  }

#if defined(_WIN32)
  item = cJSON_GetObjectItemCaseSensitive(play_obj, "polling");
  if (cJSON_IsBool(item)) {
    play->polling = cJSON_IsTrue(item);
    play->has_polling = true;
  }
#endif

  item = cJSON_GetObjectItemCaseSensitive(play_obj, "output_dop");
  if (cJSON_IsBool(item)) {
    play->output_dop = cJSON_IsTrue(item);
    play->has_output_dop = true;
  }

  item = cJSON_GetObjectItemCaseSensitive(play_obj, "output_dsd");
  if (cJSON_IsBool(item)) {
    play->output_dsd = cJSON_IsTrue(item);
    play->has_output_dsd = true;
  }

  item = cJSON_GetObjectItemCaseSensitive(play_obj, "dop_encoder_filter");
  if (cJSON_IsString(item) && item->valuestring) {
    play->dop_encoder_filter = sdm_filter_from_string(item->valuestring);
    play->has_dop_encoder_filter =
        (play->dop_encoder_filter != SDM_FILTER_INVALID);
  }

#ifdef CDSP_TEST
  item = cJSON_GetObjectItemCaseSensitive(play_obj, "realtime");
  if (cJSON_IsBool(item)) {
    play->realtime = cJSON_IsTrue(item);
    play->has_realtime = true;
  }
#endif

#if defined(ENABLE_PIPEWIRE)
  item = cJSON_GetObjectItemCaseSensitive(play_obj, "node_name");
  if (cJSON_IsString(item) && item->valuestring) {
    strncpy(play->node_name, item->valuestring, sizeof(play->node_name) - 1);
    play->has_node_name = true;
  }
  item = cJSON_GetObjectItemCaseSensitive(play_obj, "node_description");
  if (cJSON_IsString(item) && item->valuestring) {
    strncpy(play->node_description, item->valuestring,
            sizeof(play->node_description) - 1);
    play->has_node_description = true;
  }
  item = cJSON_GetObjectItemCaseSensitive(play_obj, "node_group_name");
  if (cJSON_IsString(item) && item->valuestring) {
    strncpy(play->node_group_name, item->valuestring,
            sizeof(play->node_group_name) - 1);
    play->has_node_group_name = true;
  }
  item = cJSON_GetObjectItemCaseSensitive(play_obj, "autoconnect_to");
  if (cJSON_IsString(item) && item->valuestring) {
    strncpy(play->autoconnect_to, item->valuestring,
            sizeof(play->autoconnect_to) - 1);
    play->has_autoconnect_to = true;
  }
#endif

  parse_labels_array(cJSON_GetObjectItemCaseSensitive(play_obj, "labels"),
                     &play->labels, &play->labels_count, &play->has_labels);

  // Copy flat temp to union configuration
  playback_device_config_t* final_play = &devices->playback;
  final_play->type = temp.type;
  final_play->labels = temp.labels;
  final_play->labels_count = temp.labels_count;
  final_play->has_labels = temp.has_labels;
  final_play->is_wav = temp.is_wav;
  final_play->has_is_wav = temp.has_is_wav;
  final_play->output_dop = temp.has_output_dop ? temp.output_dop : false;
  final_play->has_output_dop = temp.has_output_dop;
  final_play->output_dsd = temp.has_output_dsd ? temp.output_dsd : false;
  final_play->has_output_dsd = temp.has_output_dsd;
  final_play->dop_encoder_filter = temp.has_dop_encoder_filter
                                       ? temp.dop_encoder_filter
                                       : SDM_FILTER_INVALID;
  final_play->has_dop_encoder_filter = temp.has_dop_encoder_filter;

  switch (temp.type) {
#if defined(ENABLE_COREAUDIO)
    case AUDIO_BACKEND_TYPE_CORE_AUDIO:
      final_play->cfg.coreaudio.channels = temp.channels;
      snprintf(final_play->cfg.coreaudio.device,
               sizeof(final_play->cfg.coreaudio.device), "%s", temp.device);
      final_play->cfg.coreaudio.has_device = temp.has_device;
      final_play->cfg.coreaudio.format = temp.format;
      final_play->cfg.coreaudio.has_format = temp.has_format;
      final_play->cfg.coreaudio.exclusive = temp.exclusive;
      final_play->cfg.coreaudio.has_exclusive = temp.has_exclusive;
      break;
#endif
#if defined(ENABLE_ALSA)
    case AUDIO_BACKEND_TYPE_ALSA:
      final_play->cfg.alsa.channels = temp.channels;
      snprintf(final_play->cfg.alsa.device, sizeof(final_play->cfg.alsa.device),
               "%s", temp.device);
      final_play->cfg.alsa.format = temp.alsa_format;
      final_play->cfg.alsa.has_format = temp.has_alsa_format;
      break;
#endif
#if defined(ENABLE_PULSE)
    case AUDIO_BACKEND_TYPE_PULSE_AUDIO:
      final_play->cfg.pulse.channels = temp.channels;
      snprintf(final_play->cfg.pulse.device,
               sizeof(final_play->cfg.pulse.device), "%s", temp.device);
      break;
#endif
#if defined(ENABLE_PIPEWIRE)
    case AUDIO_BACKEND_TYPE_PIPEWIRE:
      final_play->cfg.pipewire.channels = temp.channels;
      snprintf(final_play->cfg.pipewire.device,
               sizeof(final_play->cfg.pipewire.device), "%s", temp.device);
      final_play->cfg.pipewire.has_device = temp.has_device;
      snprintf(final_play->cfg.pipewire.node_name,
               sizeof(final_play->cfg.pipewire.node_name), "%s",
               temp.node_name);
      final_play->cfg.pipewire.has_node_name = temp.has_node_name;
      snprintf(final_play->cfg.pipewire.node_description,
               sizeof(final_play->cfg.pipewire.node_description), "%s",
               temp.node_description);
      final_play->cfg.pipewire.has_node_description = temp.has_node_description;
      snprintf(final_play->cfg.pipewire.node_group_name,
               sizeof(final_play->cfg.pipewire.node_group_name), "%s",
               temp.node_group_name);
      final_play->cfg.pipewire.has_node_group_name = temp.has_node_group_name;
      snprintf(final_play->cfg.pipewire.autoconnect_to,
               sizeof(final_play->cfg.pipewire.autoconnect_to), "%s",
               temp.autoconnect_to);
      final_play->cfg.pipewire.has_autoconnect_to = temp.has_autoconnect_to;
      break;
#endif
#if defined(ENABLE_JACK)
    case AUDIO_BACKEND_TYPE_JACK:
      final_play->cfg.jack.channels = temp.channels;
      snprintf(final_play->cfg.jack.device, sizeof(final_play->cfg.jack.device),
               "%s", temp.device);
      break;
#endif
    case AUDIO_BACKEND_TYPE_FILE:
      snprintf(final_play->cfg.raw_file.filename,
               sizeof(final_play->cfg.raw_file.filename), "%s", temp.filename);
      final_play->cfg.raw_file.has_filename = temp.has_filename;
      final_play->cfg.raw_file.format = temp.file_format;
      final_play->cfg.raw_file.has_format = temp.has_file_format;
      final_play->cfg.raw_file.channels = temp.channels;
      final_play->cfg.raw_file.wav_header = temp.is_wav;
      final_play->cfg.raw_file.has_wav_header = temp.has_is_wav;
#ifdef CDSP_TEST
      final_play->cfg.raw_file.realtime = temp.realtime;
      final_play->cfg.raw_file.has_realtime = temp.has_realtime;
#endif
      break;
    case AUDIO_BACKEND_TYPE_STDIN_OUT:
      final_play->cfg.stdout_out.channels = temp.channels;
      final_play->cfg.stdout_out.format = temp.file_format;
      final_play->cfg.stdout_out.wav_header = temp.is_wav;
      final_play->cfg.stdout_out.has_wav_header = temp.has_is_wav;
      break;
#if defined(ENABLE_WASAPI)
    case AUDIO_BACKEND_TYPE_WASAPI:
      final_play->cfg.wasapi.channels = temp.channels;
      snprintf(final_play->cfg.wasapi.device,
               sizeof(final_play->cfg.wasapi.device), "%s", temp.device);
      final_play->cfg.wasapi.has_device = temp.has_device;
      final_play->cfg.wasapi.format = temp.wasapi_format;
      final_play->cfg.wasapi.has_format = temp.has_wasapi_format;
      final_play->cfg.wasapi.exclusive = temp.exclusive;
      final_play->cfg.wasapi.has_exclusive = temp.has_exclusive;
      final_play->cfg.wasapi.polling = temp.polling;
      final_play->cfg.wasapi.has_polling = temp.has_polling;
      break;
#endif
#if defined(ENABLE_ASIO)
    case AUDIO_BACKEND_TYPE_ASIO:
      final_play->cfg.asio.channels = temp.channels;
      snprintf(final_play->cfg.asio.device, sizeof(final_play->cfg.asio.device),
               "%s", temp.device);
      final_play->cfg.asio.format = temp.asio_format;
      final_play->cfg.asio.has_format = temp.has_asio_format;
      break;
#endif
    default:
      break;
  }
}

int config_parse_devices(const cJSON* dev_obj, dsp_config_t* config,
                         config_error_t* err) {
  if (!cJSON_IsObject(dev_obj)) {
    config_error_set(err, CONFIG_ERR_PARSE, "devices must be an object");
    return -1;
  }
  devices_config_t* dev = &config->devices;

  cJSON* item;

  item = cJSON_GetObjectItemCaseSensitive(dev_obj, "samplerate");
  if (cJSON_IsNumber(item)) {
    dev->samplerate = item->valueint > 0 ? (size_t)item->valueint : 0;
  }

  item = cJSON_GetObjectItemCaseSensitive(dev_obj, "chunksize");
  if (cJSON_IsNumber(item)) {
    dev->chunksize = item->valueint > 0 ? (size_t)item->valueint : 0;
  }

  item = cJSON_GetObjectItemCaseSensitive(dev_obj, "queuelimit");
  if (cJSON_IsNumber(item)) {
    dev->queuelimit = item->valueint;
    dev->has_queuelimit = (dev->queuelimit > 0);
  }

  item = cJSON_GetObjectItemCaseSensitive(dev_obj, "enable_rate_adjust");
  if (cJSON_IsBool(item)) {
    dev->enable_rate_adjust = cJSON_IsTrue(item);
    dev->has_enable_rate_adjust = true;
  }

  item = cJSON_GetObjectItemCaseSensitive(dev_obj, "target_level");
  if (cJSON_IsNumber(item)) {
    dev->target_level = item->valueint;
    dev->has_target_level = (dev->target_level > 0);
  }

  item = cJSON_GetObjectItemCaseSensitive(dev_obj, "adjust_period");
  if (cJSON_IsNumber(item)) {
    dev->adjust_period = item->valuedouble;
    dev->has_adjust_period = (dev->adjust_period > 0.0);
  }

  item = cJSON_GetObjectItemCaseSensitive(dev_obj, "silence_threshold");
  if (cJSON_IsNumber(item)) {
    dev->silence_threshold = item->valuedouble;
    dev->has_silence_threshold = (dev->silence_threshold != 0.0);
  }

  item = cJSON_GetObjectItemCaseSensitive(dev_obj, "silence_timeout");
  if (cJSON_IsNumber(item)) {
    dev->silence_timeout = item->valuedouble;
    dev->has_silence_timeout = (dev->silence_timeout > 0.0);
  }

  item = cJSON_GetObjectItemCaseSensitive(dev_obj, "capture_samplerate");
  if (cJSON_IsNumber(item)) {
    dev->capture_samplerate = item->valueint > 0 ? (size_t)item->valueint : 0;
    dev->has_capture_samplerate = (dev->capture_samplerate > 0);
  }

  item = cJSON_GetObjectItemCaseSensitive(dev_obj, "volume_ramp_time");
  if (cJSON_IsNumber(item)) {
    dev->volume_ramp_time = item->valuedouble;
    dev->has_volume_ramp_time = (dev->volume_ramp_time > 0.0);
  }

  item = cJSON_GetObjectItemCaseSensitive(dev_obj, "volume_limit");
  if (cJSON_IsNumber(item)) {
    dev->volume_limit = item->valuedouble;
    dev->has_volume_limit = (dev->volume_limit > 0.0);
  }

  item = cJSON_GetObjectItemCaseSensitive(dev_obj, "stop_on_rate_change");
  if (cJSON_IsBool(item)) {
    dev->stop_on_rate_change = cJSON_IsTrue(item);
    dev->has_stop_on_rate_change = true;
  }

  item = cJSON_GetObjectItemCaseSensitive(dev_obj, "rate_measure_interval");
  if (cJSON_IsNumber(item)) {
    dev->rate_measure_interval = item->valuedouble;
    dev->has_rate_measure_interval = (dev->rate_measure_interval > 0.0);
  }

  item = cJSON_GetObjectItemCaseSensitive(dev_obj, "multithreaded");
  if (cJSON_IsBool(item)) {
    dev->multithreaded = cJSON_IsTrue(item);
    dev->has_multithreaded = true;
  }

  item = cJSON_GetObjectItemCaseSensitive(dev_obj, "worker_threads");
  if (cJSON_IsNumber(item)) {
    dev->worker_threads = item->valueint;
    dev->has_worker_threads = (dev->worker_threads > 0);
  }

  parse_resampler(cJSON_GetObjectItemCaseSensitive(dev_obj, "resampler"), dev);
  parse_capture(cJSON_GetObjectItemCaseSensitive(dev_obj, "capture"), dev);
  parse_playback(cJSON_GetObjectItemCaseSensitive(dev_obj, "playback"), dev);

  return 0;
}
