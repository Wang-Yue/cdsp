#include "backend/asio_capabilities.h"

#if defined(ENABLE_ASIO)

#define WIN32_LEAN_AND_MEAN
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "backend/asio_backend.h"
#include "backend/asio_types.h"

static inline bool asio_ok(long r) { return r == 0 || r == (long)ASE_SUCCESS; }

int asio_capabilities_available_device_names(bool is_capture,
                                             char out_names[][256],
                                             int max_names) {
  (void)is_capture;
  return asio_list_device_names(out_names, max_names);
}

bool asio_capabilities_default_device_name(bool is_capture, char *out_name,
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

static audio_device_descriptor_t *
probe_device_capabilities(const char *target_dev_name, bool is_capture,
                          device_error_t *err) {
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
    if (strcmp(available_names[i], target_dev_name) == 0) {
      found_name = true;
      break;
    }
  }
  if (!found_name) {
    if (err) {
      device_error_init(err, DEVICE_ERROR_NOT_FOUND, target_dev_name);
    }
    return NULL;
  }

  IASIO *iasio = NULL;
  backend_error_t berr;
  memset(&berr, 0, sizeof(berr));
  if (!asio_driver_load_by_name(target_dev_name, &iasio, &berr) || !iasio) {
    if (err) {
      device_error_init(err, DEVICE_ERROR_OTHER,
                        berr.message[0] ? berr.message
                                        : "Failed to load ASIO driver");
    }
    return NULL;
  }

  audio_device_descriptor_t *desc = NULL;

  // Supported rates probe (lines 1242-1247)
  bool pcm_rate_supported[STANDARD_RATES_COUNT] = {false};
  for (size_t r = 0; r < STANDARD_RATES_COUNT; r++) {
    if (asio_ok(
            iasio->lpVtbl->canSampleRate(iasio, (double)STANDARD_RATES[r]))) {
      pcm_rate_supported[r] = true;
    }
  }

  // 2. Probe native sample format (lines 1249-1260)
  ASIOChannelInfo chan_info;
  memset(&chan_info, 0, sizeof(chan_info));
  chan_info.channel = 0;
  chan_info.isInput = is_capture ? ASIOTrue : ASIOFalse;
  if (!asio_ok(iasio->lpVtbl->getChannelInfo(iasio, &chan_info))) {
    if (err) {
      const char *direction_name = is_capture ? "capture" : "playback";
      char msg[512];
      snprintf(msg, sizeof(msg),
               "Failed to get %s channel info for ASIO device '%s'",
               direction_name, target_dev_name);
      device_error_init(err, DEVICE_ERROR_OTHER, msg);
    }
    asio_driver_teardown(target_dev_name);
    return NULL;
  }

  asio_sample_format_t sample_fmt = asio_sample_type_to_format(chan_info.type);
  if (sample_fmt == ASIO_SAMPLE_FORMAT_INVALID) {
    if (err) {
      const char *direction_name = is_capture ? "capture" : "playback";
      char msg[512];
      snprintf(msg, sizeof(msg),
               "Failed to detect %s sample format for ASIO device '%s'",
               direction_name, target_dev_name);
      device_error_init(err, DEVICE_ERROR_OTHER, msg);
    }
    asio_driver_teardown(target_dev_name);
    return NULL;
  }
  const char *fmt_str = asio_format_to_str(sample_fmt);

  // Get channel count before touching DSD format or setting rate (AS-F9)
  long num_inputs = 0, num_outputs = 0;
  if (!asio_ok(iasio->lpVtbl->getChannels(iasio, &num_inputs, &num_outputs))) {
    if (err) {
      char msg[512];
      snprintf(msg, sizeof(msg), "ASIOGetChannels failed for '%s'",
               target_dev_name);
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
          asio_ok(iasio->lpVtbl->canSampleRate(iasio, raw_dsd_rate))) {
        dsd_rate_supported[r] = true;
      }
    }
    // Switch back to PCM format and restore sample rate (AS-F9)
    ASIOIoFormat pcm_format;
    memset(&pcm_format, 0, sizeof(pcm_format));
    pcm_format.FormatType = kASIOFormatPCM;
    iasio->lpVtbl->future(iasio, kAsioSetIoFormat, &pcm_format);
    iasio->lpVtbl->setSampleRate(iasio, 44100.0);
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
      (audio_device_descriptor_t *)calloc(1, sizeof(audio_device_descriptor_t));
  if (!desc) {
    if (err)
      device_error_init(err, DEVICE_ERROR_OTHER, "Out of memory");
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
      (device_capability_set_t *)calloc(1, sizeof(device_capability_set_t));
  if (!desc->capability_sets) {
    goto error_cleanup;
  }

  device_capability_set_t *set = &desc->capability_sets[0];
  snprintf(set->mode, sizeof(set->mode), "Unified");
  set->capabilities_count = 1;
  set->capabilities =
      (channel_capability_t *)calloc(1, sizeof(channel_capability_t));
  if (!set->capabilities) {
    goto error_cleanup;
  }

  channel_capability_t *cap = &set->capabilities[0];
  cap->channels = (int)target_channels;
  cap->samplerates_count = total_rates;
  cap->samplerates = (samplerate_capability_t *)calloc(
      total_rates, sizeof(samplerate_capability_t));
  if (!cap->samplerates) {
    goto error_cleanup;
  }

  size_t out_idx = 0;
  for (size_t i = 0; i < STANDARD_RATES_COUNT; i++) {
    bool is_pcm = pcm_rate_supported[i];
    bool is_dsd = dsd_rate_supported[i];
    if (!is_pcm && !is_dsd)
      continue;

    samplerate_capability_t *rate_cap = &cap->samplerates[out_idx++];
    rate_cap->samplerate = (int)STANDARD_RATES[i];

    size_t n_fmts = (is_pcm ? 1 : 0) + (is_dsd ? 1 : 0);
    rate_cap->formats = (char **)calloc(n_fmts, sizeof(char *));
    if (!rate_cap->formats) {
      goto error_cleanup;
    }
    size_t f_idx = 0;
    if (is_pcm) {
      rate_cap->formats[f_idx] = strdup(fmt_str);
      if (!rate_cap->formats[f_idx]) {
        goto error_cleanup;
      }
      f_idx++;
    }
    if (is_dsd) {
      rate_cap->formats[f_idx] = strdup("DSD_INT8");
      if (!rate_cap->formats[f_idx]) {
        goto error_cleanup;
      }
      f_idx++;
    }
    rate_cap->formats_count = f_idx;
  }

  return desc;

error_cleanup:
  if (desc) {
    free_audio_device_descriptor(desc);
    desc = NULL;
  }
  return NULL;
}

typedef struct asio_probe_thread_ctx {
  char target_dev_name[256];
  bool is_capture;
  device_error_t err;
  audio_device_descriptor_t *desc;
} asio_probe_thread_ctx_t;

static DWORD WINAPI asio_probe_thread_proc(LPVOID param) {
  asio_probe_thread_ctx_t *ctx = (asio_probe_thread_ctx_t *)param;
  ctx->desc = probe_device_capabilities(ctx->target_dev_name, ctx->is_capture,
                                        &ctx->err);
  return 0;
}

/**
 * @brief Probe an ASIO device for its capabilities on a dedicated thread.
 * Matches CamillaDSP device.rs:get_device_capabilities.
 *
 * The work runs on a thread of its own so that it always starts from a clean
 * COM apartment. Capability requests arrive on the websocket connection thread,
 * which is shared with the other backends, and Wasapi probing puts that thread
 * in an MTA. An ASIO instance cannot be created from there: COM would have to
 * marshal the interface back to the caller's apartment, ASIO interfaces cannot
 * be marshalled at all, and the creation fails with E_NOINTERFACE. A fresh
 * thread gets the STA the driver expects.
 *
 * The driver is loaded and released within the probe, so no instance outlives
 * the thread.
 */
audio_device_descriptor_t *asio_capabilities_describe(const char *device_name,
                                                      bool is_capture,
                                                      device_error_t *err) {
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

  asio_probe_thread_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  snprintf(ctx.target_dev_name, sizeof(ctx.target_dev_name), "%s",
           target_dev_name);
  ctx.is_capture = is_capture;

  HANDLE h_thread =
      CreateThread(NULL, 0, asio_probe_thread_proc, &ctx, 0, NULL);
  if (!h_thread) {
    if (err) {
      device_error_init(err, DEVICE_ERROR_OTHER,
                        "Failed to start the ASIO probe thread");
    }
    return NULL;
  }

  WaitForSingleObject(h_thread, INFINITE);
  CloseHandle(h_thread);

  if (!ctx.desc && err) {
    *err = ctx.err;
  }
  return ctx.desc;
}

#endif // ENABLE_ASIO
