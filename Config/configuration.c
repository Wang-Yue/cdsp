#include "configuration.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Filters/filter.h"
#include "Mixer/mixer.h"
#include "Pipeline/pipeline.h"
#include "Processors/processor.h"
#include "Resampler/audio_resampler.h"

// Top-level configuration validation and memory management.

filter_config_t* dsp_config_get_filter(const dsp_config_t* config,
                                       const char* name) {
  if (!config || !name) return NULL;
  for (size_t i = 0; i < config->filters_count; i++) {
    if (strcmp(config->filters[i].name, name) == 0) {
      return &config->filters[i].filter;
    }
  }
  return NULL;
}

mixer_config_t* dsp_config_get_mixer(const dsp_config_t* config,
                                     const char* name) {
  if (!config || !name) return NULL;
  for (size_t i = 0; i < config->mixers_count; i++) {
    if (strcmp(config->mixers[i].name, name) == 0) {
      return &config->mixers[i].mixer;
    }
  }
  return NULL;
}

processor_config_t* dsp_config_get_processor(const dsp_config_t* config,
                                             const char* name) {
  if (!config || !name) return NULL;
  for (size_t i = 0; i < config->processors_count; i++) {
    if (strcmp(config->processors[i].name, name) == 0) {
      return &config->processors[i].processor;
    }
  }
  return NULL;
}

int dsp_config_validate(const dsp_config_t* config, config_error_t* err) {
  if (!config) return 0;

  // Top level checks
  if (config->devices.samplerate == 0) {
    config_error_set(err, CONFIG_ERR_INVALID_DEVICE,
                     "Sample rate must be positive");
    return -1;
  }
  if (config->devices.chunksize == 0) {
    config_error_set(err, CONFIG_ERR_INVALID_DEVICE,
                     "Chunk size must be positive");
    return -1;
  }
  if (!config->devices.capture.is_wav &&
      capture_device_config_get_channels(&config->devices.capture) <= 0) {
    config_error_set(err, CONFIG_ERR_INVALID_DEVICE,
                     "Capture channels must be positive");
    return -1;
  }
  if (playback_device_config_get_channels(&config->devices.playback) <= 0) {
    config_error_set(err, CONFIG_ERR_INVALID_DEVICE,
                     "Playback channels must be positive");
    return -1;
  }

  if (config->devices.has_silence_timeout &&
      config->devices.silence_timeout < 0.0) {
    config_error_set(err, CONFIG_ERR_INVALID_DEVICE,
                     "silence_timeout cannot be negative");
    return -1;
  }
  if (config->devices.has_silence_threshold &&
      config->devices.silence_threshold > 0.0) {
    config_error_set(err, CONFIG_ERR_INVALID_DEVICE,
                     "silence_threshold must be less than or equal to 0");
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
  if (config->devices.has_target_level) {
    /* Target level is verified against a maximum theoretical limit.
     * The limit is buffer-dependent and differs for ALSA due to the larger
     * buffer requirements of the backend. */
    int64_t qlimit_val =
        config->devices.has_queuelimit ? config->devices.queuelimit : 4;
    if (qlimit_val < 0 || qlimit_val > 1000) {
      config_error_set(err, CONFIG_ERR_INVALID_DEVICE,
                       "queuelimit must be between 0 and 1000");
      return -1;
    }
    if (config->devices.chunksize <= 0 || config->devices.chunksize > 1000000) {
      config_error_set(err, CONFIG_ERR_INVALID_DEVICE,
                       "chunksize must be between 1 and 1000000");
      return -1;
    }
    int64_t target_limit =
        (2 + qlimit_val) * (int64_t)config->devices.chunksize;
#if defined(ENABLE_ALSA)
    if (config->devices.playback.type == AUDIO_BACKEND_TYPE_ALSA) {
      target_limit = (4 + qlimit_val) * (int64_t)config->devices.chunksize;
    }
#endif
    if ((int64_t)config->devices.target_level > target_limit ||
        config->devices.target_level <= 0) {
      char msg[128];
      snprintf(msg, sizeof(msg), "target_level must be between 1 and %lld",
               (long long)target_limit);
      config_error_set(err, CONFIG_ERR_INVALID_DEVICE, msg);
      return -1;
    }
  }

  if (config->devices.has_worker_threads &&
      config->devices.worker_threads <= 0) {
    config_error_set(err, CONFIG_ERR_INVALID_DEVICE,
                     "worker_threads must be positive");
    return -1;
  }

  if (config->devices.has_adjust_period &&
      config->devices.adjust_period < 0.1) {
    config_error_set(err, CONFIG_ERR_INVALID_DEVICE,
                     "adjust_period must be at least 0.1 seconds");
    return -1;
  }

  // Validate filters
  for (size_t i = 0; i < config->filters_count; i++) {
    config_error_t sub_err;
    config_error_init(&sub_err);
    if (filter_config_validate(&config->filters[i].filter,
                               config->devices.samplerate, &sub_err) != 0) {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER, "Filter '%s': %s",
                       config->filters[i].name, sub_err.message);
      return -1;
    }
  }

  // Validate mixers
  for (size_t i = 0; i < config->mixers_count; i++) {
    config_error_t sub_err;
    config_error_init(&sub_err);
    if (mixer_config_validate(&config->mixers[i].mixer, &sub_err) != 0) {
      config_error_set(err, CONFIG_ERR_INVALID_MIXER, "Mixer '%s': %s",
                       config->mixers[i].name, sub_err.message);
      return -1;
    }
  }

  // Validate processors
  for (size_t i = 0; i < config->processors_count; i++) {
    config_error_t sub_err;
    config_error_init(&sub_err);
    if (processor_config_validate(&config->processors[i].processor, &sub_err) !=
        0) {
      config_error_set(err, CONFIG_ERR_INVALID_PROCESSOR, "Processor '%s': %s",
                       config->processors[i].name, sub_err.message);
      return -1;
    }
  }

  // Validate resampler
  if (resampler_config_validate(&config->devices.resampler, err) != 0) {
    return -1;
  }

  // Validate pipeline structure and channel routing
  return pipeline_config_validate(config, err);
}

void dsp_config_free(dsp_config_t* config) {
  if (!config) return;
  if (config->filters) {
    for (size_t i = 0; i < config->filters_count; i++) {
      if (config->filters[i].filter.type == FILTER_TYPE_CONV) {
        free(config->filters[i].filter.parameters.conv.values);
      } else if (config->filters[i].filter.type == FILTER_TYPE_BIQUAD_COMBO) {
        free(config->filters[i].filter.parameters.biquad_combo.gains);
      } else if (config->filters[i].filter.type == FILTER_TYPE_DIFF_EQ) {
        free(config->filters[i].filter.parameters.diff_eq.a);
        free(config->filters[i].filter.parameters.diff_eq.b);
      }
    }
    free(config->filters);
  }
  if (config->mixers) {
    for (size_t i = 0; i < config->mixers_count; i++) {
      if (config->mixers[i].mixer.mapping) {
        for (size_t j = 0; j < config->mixers[i].mixer.mapping_count; j++) {
          free(config->mixers[i].mixer.mapping[j].sources);
        }
        free(config->mixers[i].mixer.mapping);
      }
    }
    free(config->mixers);
  }
  if (config->processors) {
    for (size_t i = 0; i < config->processors_count; i++) {
      if (config->processors[i].processor.type == PROCESSOR_TYPE_COMPRESSOR) {
        free(config->processors[i]
                 .processor.parameters.compressor.monitor_channels);
        free(config->processors[i]
                 .processor.parameters.compressor.process_channels);
      } else if (config->processors[i].processor.type ==
                 PROCESSOR_TYPE_NOISE_GATE) {
        free(config->processors[i]
                 .processor.parameters.noise_gate.monitor_channels);
        free(config->processors[i]
                 .processor.parameters.noise_gate.process_channels);
      }
    }
    free(config->processors);
  }
  if (config->pipeline) {
    for (size_t i = 0; i < config->pipeline_count; i++) {
      free(config->pipeline[i].channels);
      if (config->pipeline[i].names) {
        for (size_t j = 0; j < config->pipeline[i].names_count; j++) {
          free(config->pipeline[i].names[j]);
        }
        free(config->pipeline[i].names);
      }
    }
    free(config->pipeline);
  }
  if (config->devices.capture.has_labels && config->devices.capture.labels) {
    for (size_t i = 0; i < config->devices.capture.labels_count; i++) {
      free(config->devices.capture.labels[i]);
    }
    free(config->devices.capture.labels);
  }
  if (config->devices.playback.has_labels && config->devices.playback.labels) {
    for (size_t i = 0; i < config->devices.playback.labels_count; i++) {
      free(config->devices.playback.labels[i]);
    }
    free(config->devices.playback.labels);
  }
  free(config);
}
