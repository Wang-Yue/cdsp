#include "config/configuration.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backend/audio_backend.h"
#include "config/config_gen.h"
#include "filters/filter.h"
#include "logging/app_logger.h"
#include "mixer/mixer.h"
#include "pipeline/pipeline.h"
#include "processors/processor.h"
#include "resampler/audio_resampler.h"

static const logger_t g_logger = {"dsp.config"};

// Top-level configuration validation and memory management.

filter_config_t *dsp_config_get_filter(const dsp_config_t *config,
                                       const char *name) {
  if (!config || !name)
    return NULL;
  for (size_t i = 0; i < config->filters_count; i++) {
    if (strcmp(config->filters[i].name, name) == 0) {
      return &config->filters[i].filter;
    }
  }
  return NULL;
}

mixer_config_t *dsp_config_get_mixer(const dsp_config_t *config,
                                     const char *name) {
  if (!config || !name)
    return NULL;
  for (size_t i = 0; i < config->mixers_count; i++) {
    if (strcmp(config->mixers[i].name, name) == 0) {
      return &config->mixers[i].mixer;
    }
  }
  return NULL;
}

processor_config_t *dsp_config_get_processor(const dsp_config_t *config,
                                             const char *name) {
  if (!config || !name)
    return NULL;
  for (size_t i = 0; i < config->processors_count; i++) {
    if (strcmp(config->processors[i].name, name) == 0) {
      return &config->processors[i].processor;
    }
  }
  return NULL;
}

int dsp_config_validate(const dsp_config_t *config, config_error_t *err) {
  if (!config)
    return 0;

  // Top level checks
  if (config->devices.samplerate == 0) {
    config_error_set(err, CONFIG_ERR_INVALID_DEVICE,
                     "Sample rate must be positive");
    return -1;
  }
  if (config->devices.has_capture_samplerate &&
      config->devices.capture_samplerate == 0) {
    config_error_set(err, CONFIG_ERR_INVALID_DEVICE,
                     "Capture sample rate must be positive");
    return -1;
  }
  if (config->devices.chunksize == 0) {
    config_error_set(err, CONFIG_ERR_INVALID_DEVICE,
                     "Chunk size must be positive");
    return -1;
  }
  if (!config->devices.capture.is_wav &&
      capture_device_config_get_channels(&config->devices.capture) == 0) {
    config_error_set(err, CONFIG_ERR_INVALID_DEVICE,
                     "Capture channels must be positive");
    return -1;
  }
  if (playback_device_config_get_channels(&config->devices.playback) == 0) {
    config_error_set(err, CONFIG_ERR_INVALID_DEVICE,
                     "Playback channels must be positive");
    return -1;
  }

  if (config->devices.has_silence_timeout_s &&
      config->devices.silence_timeout_s < 0.0) {
    config_error_set(err, CONFIG_ERR_INVALID_DEVICE,
                     "silence_timeout_s cannot be negative");
    return -1;
  }
  if (config->devices.has_silence_threshold &&
      config->devices.silence_threshold > 0.0) {
    config_error_set(err, CONFIG_ERR_INVALID_DEVICE,
                     "silence_threshold must be less than or equal to 0");
    return -1;
  }
  if (config->devices.has_volume_ramp_time_ms &&
      config->devices.volume_ramp_time_ms < 0.0) {
    config_error_set(err, CONFIG_ERR_INVALID_DEVICE,
                     "Volume ramp time cannot be negative");
    return -1;
  }
  if (config->devices.has_volume_limit) {
    if (config->devices.volume_limit > 50.0) {
      config_error_set(err, CONFIG_ERR_INVALID_DEVICE,
                       "Volume limit cannot be above +50 dB");
      return -1;
    }
    if (config->devices.volume_limit < -150.0) {
      config_error_set(err, CONFIG_ERR_INVALID_DEVICE,
                       "Volume limit cannot be less than -150 dB");
      return -1;
    }
  }

  int64_t qlimit_val =
      config->devices.has_queuelimit ? config->devices.queuelimit : 4;
  if (qlimit_val < 0) {
    config_error_set(err, CONFIG_ERR_INVALID_DEVICE,
                     "queuelimit cannot be negative");
    return -1;
  }

  if (config->devices.has_worker_threads &&
      config->devices.worker_threads < 0) {
    config_error_set(err, CONFIG_ERR_INVALID_DEVICE,
                     "worker_threads cannot be negative");
    return -1;
  }

  if (config->devices.has_adjust_interval_s &&
      config->devices.adjust_interval_s <= 0.0) {
    config_error_set(err, CONFIG_ERR_INVALID_DEVICE,
                     "adjust_interval_s must be positive and > 0");
    return -1;
  }

  if (config->devices.has_rate_measure_interval_s &&
      config->devices.rate_measure_interval_s <= 0.0) {
    config_error_set(err, CONFIG_ERR_INVALID_DEVICE,
                     "rate_measure_interval_s must be positive and > 0");
    return -1;
  }

  if (config->devices.has_resampler) {
    if (resampler_config_validate(&config->devices.resampler, err) != 0) {
      return -1;
    }
  }

  size_t cap_rate = config->devices.has_capture_samplerate
                        ? config->devices.capture_samplerate
                        : config->devices.samplerate;
  if (!config->devices.has_resampler &&
      config->devices.has_capture_samplerate &&
      config->devices.capture_samplerate != config->devices.samplerate) {
    logger_warn(
        &g_logger,
        "Resampling is disabled and capture_samplerate is different than "
        "samplerate, ignoring capture_samplerate.");
    cap_rate = config->devices.samplerate;
  }

  if (config->devices.has_resampler &&
      config->devices.resampler.type == RESAMPLER_TYPE_SLIP) {
    if (cap_rate != config->devices.samplerate) {
      config_error_set(err, CONFIG_ERR_INVALID_DEVICE,
                       "The Slip resampler requires matching samplerate and "
                       "capture_samplerate");
      return -1;
    }
  }

  // Validate audio backends
  if (audio_backend_validate_devices(&config->devices, err) != 0) {
    return -1;
  }

  // Validate pipeline structure and channel routing
  return pipeline_config_validate(config, err);
}

void dsp_config_free(dsp_config_t *config) {
  if (!config)
    return;
  free_dsp_config_contents(config);
  free(config);
}
