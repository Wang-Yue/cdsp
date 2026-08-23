#include "Backend/asio_capabilities.h"

#if defined(ENABLE_ASIO)

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Backend/asio_backend.h"
#include "Backend/asio_types.h"

int asio_capabilities_available_device_names(bool is_capture,
                                             char out_names[][256],
                                             int max_names) {
  (void)is_capture;
  return asio_list_device_names(out_names, max_names);
}

bool asio_capabilities_default_device_name(bool is_capture, char* out_name,
                                           size_t max_len) {
  (void)is_capture;
  char names[1][256];
  int count = asio_list_device_names(names, 1);
  if (count > 0) {
    snprintf(out_name, max_len, "%s", names[0]);
    return true;
  }
  out_name[0] = '\0';
  return false;
}

audio_device_descriptor_t* asio_capabilities_describe(const char* device_name,
                                                      bool is_capture,
                                                      device_error_t* err) {
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

  // Refuse to probe drivers that tolerate only one instance per process.
  // Probing loads an instance and releases it again, which leaves such a driver
  // holding the device. Every later instance in this process then deadlocks or
  // takes the process down, so a probe would break the device for the rest of
  // the session. Probing ASIO4ALL twice was enough to kill the process
  // outright.
  if (asio_is_single_instance_driver(target_dev_name)) {
    if (err) {
      char msg[512];
      snprintf(
          msg, sizeof(msg),
          "ASIO driver '%s' cannot be probed, it tolerates only one instance "
          "per process.",
          target_dev_name);
      device_error_init(err, DEVICE_ERROR_OTHER, msg);
    }
    return NULL;
  }

  // Refuse to probe if an in-process ASIO driver is already loaded for this
  // device (live stream). Matches CamillaDSP device.rs lines 990-997.
  if (asio_driver_is_loaded(target_dev_name)) {
    if (err) {
      char msg[512];
      snprintf(msg, sizeof(msg),
               "ASIO driver is already in use; cannot probe '%s' while a "
               "stream is active",
               target_dev_name);
      device_error_init(err, DEVICE_ERROR_BUSY, msg);
    }
    return NULL;
  }

  // Check if device name exists in list_device_names (lines 1232-1235)
  char available_names[64][256];
  int available_count = asio_list_device_names(available_names, 64);
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

  IASIO* iasio = NULL;
  backend_error_t berr;
  memset(&berr, 0, sizeof(berr));
  if (!asio_driver_load_by_name(target_dev_name, &iasio, &berr) || !iasio) {
    if (err) {
      device_error_init(
          err, DEVICE_ERROR_OTHER,
          berr.message[0] ? berr.message : "Failed to load ASIO driver");
    }
    return NULL;
  }

  audio_device_descriptor_t* desc = NULL;

  // Supported rates probe (lines 1242-1247)
  bool pcm_rate_supported[STANDARD_RATES_COUNT] = {false};
  for (size_t r = 0; r < STANDARD_RATES_COUNT; r++) {
    if (iasio->lpVtbl->canSampleRate(iasio, (double)STANDARD_RATES[r]) == 0) {
      pcm_rate_supported[r] = true;
    }
  }

  // 2. Probe native sample format (lines 1249-1260)
  ASIOChannelInfo chan_info;
  memset(&chan_info, 0, sizeof(chan_info));
  chan_info.channel = 0;
  chan_info.isInput = is_capture ? ASIOTrue : ASIOFalse;
  if (iasio->lpVtbl->getChannelInfo(iasio, &chan_info) != 0) {
    chan_info.isInput = is_capture ? ASIOFalse : ASIOTrue;
    chan_info.channel = 0;
    iasio->lpVtbl->getChannelInfo(iasio, &chan_info);
  }

  asio_sample_format_t sample_fmt = asio_sample_type_to_format(chan_info.type);
  const char* fmt_str = asio_format_to_str(sample_fmt);
  if (!fmt_str) {
    if (err) {
      const char* direction_name = is_capture ? "capture" : "playback";
      char msg[512];
      snprintf(msg, sizeof(msg),
               "Failed to detect %s sample format for ASIO device '%s'",
               direction_name, target_dev_name);
      device_error_init(err, DEVICE_ERROR_OTHER, msg);
    }
    asio_driver_teardown(target_dev_name);
    return NULL;
  }

  // 3. Check whether Native DSD is supported by the ASIO driver and probe DSD
  // rates in DSD mode
  ASIOIoFormat dsd_format;
  memset(&dsd_format, 0, sizeof(dsd_format));
  dsd_format.FormatType = kASIOFormatDSD;
  bool supports_dsd = false;
  if (sample_fmt == ASIO_SAMPLE_FORMAT_DSD_INT8) {
    supports_dsd = true;
  } else if (iasio->lpVtbl->future) {
    ASIOError fut_res = (ASIOError)(uintptr_t)iasio->lpVtbl->future(
        iasio, kAsioCanDoIoFormat, &dsd_format);
    if (fut_res == (ASIOError)ASE_SUCCESS || fut_res == 0 || fut_res == 1) {
      supports_dsd = true;
    }
  }

  bool dsd_rate_supported[STANDARD_RATES_COUNT] = {false};
  if (supports_dsd) {
    iasio->lpVtbl->future(iasio, kAsioSetIoFormat, &dsd_format);
    for (size_t r = 0; r < STANDARD_RATES_COUNT; r++) {
      double raw_dsd_rate = (double)STANDARD_RATES[r] * 32.0;
      if (raw_dsd_rate >= 2822400.0 &&
          iasio->lpVtbl->canSampleRate(iasio, raw_dsd_rate) == 0) {
        dsd_rate_supported[r] = true;
      }
    }
    // Switch back
    iasio->lpVtbl->setSampleRate(iasio, 44100.0);
  }

  // Get channel count (lines 1262-1274)
  long num_inputs = 0, num_outputs = 0;
  if (iasio->lpVtbl->getChannels(iasio, &num_inputs, &num_outputs) != 0) {
    if (err) {
      char msg[512];
      snprintf(msg, sizeof(msg), "ASIOGetChannels failed for '%s'",
               target_dev_name);
      device_error_init(err, DEVICE_ERROR_OTHER, msg);
    }
    asio_driver_teardown(target_dev_name);
    return NULL;
  }

  // Teardown driver now that probing is finished
  asio_driver_teardown(target_dev_name);

  long target_channels = is_capture ? num_inputs : num_outputs;

  // Count total supported unique rates
  size_t total_rates = 0;
  for (size_t i = 0; i < STANDARD_RATES_COUNT; i++) {
    if (pcm_rate_supported[i] || dsd_rate_supported[i]) {
      total_rates++;
    }
  }

  desc =
      (audio_device_descriptor_t*)calloc(1, sizeof(audio_device_descriptor_t));
  if (!desc) {
    if (err) device_error_init(err, DEVICE_ERROR_OTHER, "Out of memory");
    return NULL;
  }
  snprintf(desc->name, sizeof(desc->name), "%s", target_dev_name);

  // Filter 0 channels or empty supported rates (lines 1283-1292)
  if (target_channels <= 0 || total_rates == 0) {
    desc->capability_sets_count = 0;
    desc->capability_sets = NULL;
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
  cap->samplerates_count = total_rates;
  cap->samplerates = (samplerate_capability_t*)calloc(
      total_rates, sizeof(samplerate_capability_t));
  if (!cap->samplerates) {
    goto error_cleanup;
  }

  size_t out_idx = 0;
  for (size_t i = 0; i < STANDARD_RATES_COUNT; i++) {
    bool is_pcm = pcm_rate_supported[i];
    bool is_dsd = dsd_rate_supported[i];
    if (!is_pcm && !is_dsd) continue;

    samplerate_capability_t* rate_cap = &cap->samplerates[out_idx++];
    rate_cap->samplerate = (int)STANDARD_RATES[i];

    size_t n_fmts = (is_pcm ? 1 : 0) + (is_dsd ? 1 : 0);
    rate_cap->formats_count = n_fmts;
    rate_cap->formats = (char**)calloc(n_fmts, sizeof(char*));
    if (rate_cap->formats) {
      size_t f_idx = 0;
      if (is_pcm) {
        rate_cap->formats[f_idx++] = strdup(fmt_str);
      }
      if (is_dsd) {
        rate_cap->formats[f_idx++] = strdup("DSD_INT8");
      }
    }
  }

  return desc;

error_cleanup:
  if (desc) {
    free_audio_device_descriptor(desc);
    desc = NULL;
  }
  return NULL;
}

#endif  // ENABLE_ASIO
