#include "Config/configuration.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Backend/file_backend.h"
#include "Config/resampler_config_types.h"
#include "Filters/filter.h"
#include "Logging/app_logger.h"
#include "Mixer/mixer.h"
#include "Pipeline/pipeline.h"
#include "Processors/processor.h"
#include "Resampler/audio_resampler.h"

static const logger_t g_logger = {"dsp.config"};

int dsp_config_apply_overrides(dsp_config_t* config,
                               const dsp_config_overrides_t* overrides_in,
                               config_error_t* err) {
  (void)err;
  if (!config) return 0;

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
  if (config->devices.capture.type == AUDIO_BACKEND_TYPE_FILE &&
      config->devices.capture.is_wav &&
      config->devices.capture.cfg.wav_file.has_filename) {
    const char* fname = config->devices.capture.cfg.wav_file.filename;
    cdsp_wav_info_t wav_info;
    char wav_err[256];
    if (cdsp_wav_file_read_info(fname, &wav_info, wav_err, sizeof(wav_err))) {
      logger_info(
          &g_logger,
          "Updating overrides with values from wav input file, rate %u, "
          "format: %s, channels: %u",
          wav_info.sample_rate, file_sample_format_to_string(wav_info.format),
          (unsigned int)wav_info.channels);
      if (overrides.channels <= 0) {
        overrides.channels = wav_info.channels;
      }
      if (!overrides.has_sample_format) {
        overrides.sample_format = wav_info.format;
        overrides.has_sample_format = true;
      }
      if (overrides.samplerate <= 0) {
        overrides.samplerate = (int)wav_info.sample_rate;
      }
    } else {
      logger_warn(&g_logger, "Failed to read wav header from %s: %s", fname,
                  wav_err);
    }
  }

  // 2. Apply samplerate override
  if (overrides.samplerate > 0) {
    size_t rate = (size_t)overrides.samplerate;
    size_t cfg_rate = config->devices.samplerate;
    size_t cfg_chunksize = config->devices.chunksize;

    if (!config->devices.has_resampler) {
      logger_debug(&g_logger, "Apply override for samplerate: %zu", rate);
      config->devices.samplerate = rate;
      if (cfg_rate > 0 && cfg_chunksize > 0) {
        size_t scaled_chunksize = cfg_chunksize;
        if (rate > cfg_rate) {
          scaled_chunksize =
              cfg_chunksize * (size_t)round((double)rate / (double)cfg_rate);
        } else {
          size_t divisor = (size_t)round((double)cfg_rate / (double)rate);
          if (divisor > 0) scaled_chunksize = cfg_chunksize / divisor;
        }
        logger_debug(&g_logger,
                     "Samplerate changed, adjusting chunksize: %zu -> %zu",
                     cfg_chunksize, scaled_chunksize);
        config->devices.chunksize = scaled_chunksize;

        if (config->devices.capture.type == AUDIO_BACKEND_TYPE_FILE) {
          if (config->devices.capture.is_wav &&
              config->devices.capture.cfg.wav_file.has_extra_samples) {
            config->devices.capture.cfg.wav_file.extra_samples =
                config->devices.capture.cfg.wav_file.extra_samples * rate /
                cfg_rate;
          } else if (!config->devices.capture.is_wav &&
                     config->devices.capture.cfg.raw_file.has_extra_samples) {
            config->devices.capture.cfg.raw_file.extra_samples =
                config->devices.capture.cfg.raw_file.extra_samples * rate /
                cfg_rate;
          }
        } else if (config->devices.capture.type ==
                       AUDIO_BACKEND_TYPE_STDIN_OUT &&
                   config->devices.capture.cfg.stdin_in.has_extra_samples) {
          config->devices.capture.cfg.stdin_in.extra_samples =
              config->devices.capture.cfg.stdin_in.extra_samples * rate /
              cfg_rate;
        }
      }
    } else {
      logger_debug(&g_logger, "Apply override for capture_samplerate: %zu",
                   rate);
      config->devices.capture_samplerate = rate;
      config->devices.has_capture_samplerate = true;
      if (rate == cfg_rate && !config->devices.enable_rate_adjust) {
        logger_debug(&g_logger, "Disabling unnecessary 1:1 resampling");
        config->devices.has_resampler = false;
      }
    }
  }

  // 3. Apply extra_samples override
  if (overrides.has_extra_samples && overrides.extra_samples >= 0) {
    logger_debug(&g_logger, "Apply override for extra_samples: %d",
                 overrides.extra_samples);
    if (config->devices.capture.type == AUDIO_BACKEND_TYPE_FILE) {
      if (config->devices.capture.is_wav) {
        config->devices.capture.cfg.wav_file.extra_samples =
            overrides.extra_samples;
        config->devices.capture.cfg.wav_file.has_extra_samples = true;
      } else {
        config->devices.capture.cfg.raw_file.extra_samples =
            overrides.extra_samples;
        config->devices.capture.cfg.raw_file.has_extra_samples = true;
      }
    } else if (config->devices.capture.type == AUDIO_BACKEND_TYPE_STDIN_OUT) {
      config->devices.capture.cfg.stdin_in.extra_samples =
          overrides.extra_samples;
      config->devices.capture.cfg.stdin_in.has_extra_samples = true;
    }
  }

  // 4. Apply channels override
  if (overrides.channels > 0) {
    logger_debug(&g_logger, "Apply override for capture channels: %d",
                 overrides.channels);
    switch (config->devices.capture.type) {
      case AUDIO_BACKEND_TYPE_FILE:
        if (config->devices.capture.is_wav) {
          config->devices.capture.cfg.wav_file.channels = overrides.channels;
        } else {
          config->devices.capture.cfg.raw_file.channels = overrides.channels;
        }
        break;
      case AUDIO_BACKEND_TYPE_STDIN_OUT:
        config->devices.capture.cfg.stdin_in.channels = overrides.channels;
        break;
      case AUDIO_BACKEND_TYPE_GENERATOR:
        config->devices.capture.cfg.generator.channels = overrides.channels;
        break;
#if defined(ENABLE_ALSA)
      case AUDIO_BACKEND_TYPE_ALSA:
        config->devices.capture.cfg.alsa.channels = overrides.channels;
        break;
#endif
#if defined(ENABLE_PIPEWIRE)
      case AUDIO_BACKEND_TYPE_PIPEWIRE:
        config->devices.capture.cfg.pipewire.channels = overrides.channels;
        break;
#endif
#if defined(ENABLE_COREAUDIO)
      case AUDIO_BACKEND_TYPE_CORE_AUDIO:
        config->devices.capture.cfg.coreaudio.channels = overrides.channels;
        break;
#endif
#if defined(ENABLE_WASAPI)
      case AUDIO_BACKEND_TYPE_WASAPI:
        config->devices.capture.cfg.wasapi.channels = overrides.channels;
        break;
#endif
#if defined(ENABLE_ASIO)
      case AUDIO_BACKEND_TYPE_ASIO:
        config->devices.capture.cfg.asio.channels = overrides.channels;
        break;
#endif
      default:
        break;
    }
  }

  // 5. Apply sample_format override
  if (overrides.has_sample_format) {
    switch (config->devices.capture.type) {
      case AUDIO_BACKEND_TYPE_FILE:
        if (!config->devices.capture.is_wav) {
          config->devices.capture.cfg.raw_file.format = overrides.sample_format;
          config->devices.capture.cfg.raw_file.has_format = true;
          logger_debug(&g_logger,
                       "Apply override for capture sample format: %s",
                       file_sample_format_to_string(overrides.sample_format));
        }
        break;
      case AUDIO_BACKEND_TYPE_STDIN_OUT:
        config->devices.capture.cfg.stdin_in.format = overrides.sample_format;
        logger_debug(&g_logger, "Apply override for capture sample format: %s",
                     file_sample_format_to_string(overrides.sample_format));
        break;
#if defined(ENABLE_ALSA)
      case AUDIO_BACKEND_TYPE_ALSA: {
        alsa_sample_format_t alsa_fmt =
            alsa_sample_format_from_binary_format(overrides.sample_format);
        if (alsa_fmt != ALSA_SAMPLE_FORMAT_INVALID) {
          config->devices.capture.cfg.alsa.format = alsa_fmt;
          config->devices.capture.cfg.alsa.has_format = true;
          logger_debug(&g_logger,
                       "Apply override for capture sample format: %s",
                       alsa_sample_format_to_string(alsa_fmt));
        }
        break;
      }
#endif
#if defined(ENABLE_COREAUDIO)
      case AUDIO_BACKEND_TYPE_CORE_AUDIO: {
        coreaudio_sample_format_t ca_fmt =
            coreaudio_sample_format_from_binary_format(overrides.sample_format);
        if (ca_fmt != COREAUDIO_SAMPLE_FORMAT_INVALID) {
          config->devices.capture.cfg.coreaudio.format = ca_fmt;
          config->devices.capture.cfg.coreaudio.has_format = true;
          logger_debug(&g_logger,
                       "Apply override for capture sample format: %s",
                       coreaudio_sample_format_to_string(ca_fmt));
        }
        break;
      }
#endif
#if defined(ENABLE_WASAPI)
      case AUDIO_BACKEND_TYPE_WASAPI: {
        wasapi_sample_format_t wasapi_fmt =
            wasapi_sample_format_from_binary_format(overrides.sample_format);
        if (wasapi_fmt != WASAPI_SAMPLE_FORMAT_INVALID) {
          config->devices.capture.cfg.wasapi.format = wasapi_fmt;
          config->devices.capture.cfg.wasapi.has_format = true;
          logger_debug(&g_logger,
                       "Apply override for capture sample format: %s",
                       wasapi_sample_format_to_string(wasapi_fmt));
        }
        break;
      }
#endif
#if defined(ENABLE_ASIO)
      case AUDIO_BACKEND_TYPE_ASIO: {
        asio_sample_format_t asio_fmt =
            asio_sample_format_from_binary_format(overrides.sample_format);
        if (asio_fmt != ASIO_SAMPLE_FORMAT_INVALID) {
          config->devices.capture.cfg.asio.format = asio_fmt;
          config->devices.capture.cfg.asio.has_format = true;
          logger_debug(&g_logger,
                       "Apply override for capture sample format: %s",
                       asio_sample_format_to_string(asio_fmt));
        }
        break;
      }
#endif
      default:
        break;
    }
  }

  return 0;
}

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
  if (config->devices.playback.type == AUDIO_BACKEND_TYPE_FILE &&
      config->devices.playback.cfg.raw_file.wav_header &&
      config->devices.playback.cfg.raw_file.format ==
          BINARY_SAMPLE_FORMAT_S24_4_RJ_LE) {
    config_error_set(err, CONFIG_ERR_INVALID_DEVICE,
                     "Wav files do not support the S24_4_RJ_LE sample format");
    return -1;
  }

#if defined(ENABLE_WASAPI)
  if (config->devices.capture.type == AUDIO_BACKEND_TYPE_WASAPI) {
    const wasapi_capture_config_t* wcap = &config->devices.capture.cfg.wasapi;
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
  if (config->devices.playback.type == AUDIO_BACKEND_TYPE_WASAPI) {
    const wasapi_playback_config_t* wplay =
        &config->devices.playback.cfg.wasapi;
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
  if (config->devices.capture.type == AUDIO_BACKEND_TYPE_ASIO &&
      config->devices.playback.type == AUDIO_BACKEND_TYPE_ASIO) {
    // Capture and playback on the same device share a single driver instance,
    // and therefore a single clock and sample rate, so there is nothing to
    // resample between. Different devices are independent and resample like any
    // other pair.
    if (strcmp(config->devices.capture.cfg.asio.device,
               config->devices.playback.cfg.asio.device) == 0 &&
        config->devices.has_resampler) {
      config_error_set(err, CONFIG_ERR_INVALID_DEVICE,
                       "Resampling is not supported in full-duplex ASIO mode. "
                       "Both capture and playback share the same driver and "
                       "sample rate");
      return -1;
    }
  }
#endif

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
  int64_t target_limit = (2 + qlimit_val) * (int64_t)config->devices.chunksize;
#if defined(ENABLE_ALSA)
  if (config->devices.playback.type == AUDIO_BACKEND_TYPE_ALSA) {
    target_limit = (4 + qlimit_val) * (int64_t)config->devices.chunksize;
  }
#endif
  if (config->devices.has_target_level) {
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

  if (config->devices.has_adjust_interval_s &&
      config->devices.adjust_interval_s <= 0.0) {
    config_error_set(err, CONFIG_ERR_INVALID_DEVICE,
                     "adjust_interval_s must be positive and > 0");
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
    if (processor_config_validate(&config->processors[i].processor,
                                  (int)config->devices.samplerate,
                                  &sub_err) != 0) {
      config_error_set(err, CONFIG_ERR_INVALID_PROCESSOR, "Processor '%s': %s",
                       config->processors[i].name, sub_err.message);
      return -1;
    }
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
      cap_rate != config->devices.samplerate) {
    config_error_set(err, CONFIG_ERR_INVALID_DEVICE,
                     "Different capture_samplerate (%zu) and samplerate (%zu) "
                     "requires a resampler to be configured",
                     cap_rate, config->devices.samplerate);
    return -1;
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
      if (config->mixers[i].mixer.has_labels &&
          config->mixers[i].mixer.labels) {
        for (size_t j = 0; j < config->mixers[i].mixer.labels_count; j++) {
          free(config->mixers[i].mixer.labels[j]);
        }
        free(config->mixers[i].mixer.labels);
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
      } else if (config->processors[i].processor.type ==
                 PROCESSOR_TYPE_LOOKAHEAD_LIMITER) {
        free(config->processors[i]
                 .processor.parameters.lookahead_limiter.monitor_channels);
        free(config->processors[i]
                 .processor.parameters.lookahead_limiter.process_channels);
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
