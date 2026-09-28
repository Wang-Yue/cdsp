#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "audio/sample_format.h"
#include "backend/audio_backend.h"
#include "config/config_error.h"
#include "config/config_gen.h"
#include "config/configuration.h"
#include "filters/filter.h"
#include "mixer/mixer.h"
#include "pipeline/pipeline.h"
#include "processors/processor.h"

// ============================================================================
// Configuration Validation
// ============================================================================

static bool validate_filter_step(const pipeline_step_config_t *step,
                                 size_t step_idx, size_t num_channels,
                                 const dsp_config_t *config,
                                 config_error_t *err) {
  if (!step->names && !step->has_names) {
    config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                     "Filter step %zu must have 'names'", step_idx);
    return false;
  }
  if (step->has_channel) {
    if (step->channel >= num_channels) {
      config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                       "Filter step %zu references channel %zu but "
                       "pipeline only has %zu channel(s) at this point",
                       step_idx, step->channel, num_channels);
      return false;
    }
  }
  for (size_t j = 0; j < step->channels_count; j++) {
    if (step->channels[j] >= num_channels) {
      config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                       "Filter step %zu references channel %zu but "
                       "pipeline only has %zu channel(s) at this point",
                       step_idx, step->channels[j], num_channels);
      return false;
    }
    for (size_t k = 0; k < j; k++) {
      if (step->channels[j] == step->channels[k]) {
        config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                         "Filter step %zu references duplicated channel %zu",
                         step_idx, step->channels[j]);
        return false;
      }
    }
  }
  for (size_t j = 0; j < step->names_count; j++) {
    if (!step->names[j] || step->names[j][0] == '\0') {
      config_error_set(
          err, CONFIG_ERR_INVALID_PIPELINE,
          "Filter step %zu has invalid/empty filter name at index %zu",
          step_idx, j);
      return false;
    }
    const filter_config_t *filt = dsp_config_get_filter(config, step->names[j]);
    if (!filt) {
      config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                       "Filter '%s' referenced in pipeline but not defined",
                       step->names[j]);
      return false;
    }
    config_error_t sub_err;
    config_error_init(&sub_err);
    if (filter_config_validate(filt, config->devices.samplerate, &sub_err) !=
        0) {
      config_error_set(err, CONFIG_ERR_INVALID_FILTER, "Filter '%s': %s",
                       step->names[j], sub_err.message);
      return false;
    }
  }
  return true;
}

static bool validate_mixer_step(const pipeline_step_config_t *step,
                                size_t step_idx, size_t *inout_channels,
                                const dsp_config_t *config,
                                config_error_t *err) {
  if (!step->has_name || step->name[0] == '\0') {
    config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                     "Mixer step %zu must have 'name'", step_idx);
    return false;
  }
  const mixer_config_t *mixer = dsp_config_get_mixer(config, step->name);
  if (!mixer) {
    config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                     "Mixer '%s' referenced in pipeline but not defined",
                     step->name);
    return false;
  }
  if (mixer->channels_in != *inout_channels) {
    config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                     "Mixer '%s' expects %zu input channel(s) but "
                     "pipeline has %zu at this point",
                     step->name, mixer->channels_in, *inout_channels);
    return false;
  }
  *inout_channels = mixer->channels_out;
  config_error_t sub_err;
  config_error_init(&sub_err);
  if (mixer_config_validate(mixer, &sub_err) != 0) {
    config_error_set(err, CONFIG_ERR_INVALID_MIXER, "Mixer '%s': %s",
                     step->name, sub_err.message);
    return false;
  }
  return true;
}

static bool validate_processor_step(const pipeline_step_config_t *step,
                                    size_t step_idx, size_t num_channels,
                                    const dsp_config_t *config,
                                    config_error_t *err) {
  if (!step->has_name || step->name[0] == '\0') {
    config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                     "Processor step %zu must have 'name'", step_idx);
    return false;
  }
  const processor_config_t *proc = dsp_config_get_processor(config, step->name);
  if (!proc) {
    config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                     "Processor '%s' referenced in pipeline but not defined",
                     step->name);
    return false;
  }
  size_t expected_channels = 0;
  switch (proc->type) {
  case PROCESSOR_TYPE_COMPRESSOR:
    expected_channels = proc->parameters.compressor.channels;
    break;
  case PROCESSOR_TYPE_NOISE_GATE:
    expected_channels = proc->parameters.noise_gate.channels;
    break;
  case PROCESSOR_TYPE_RACE:
    expected_channels = proc->parameters.race.channels;
    break;
  case PROCESSOR_TYPE_LOOKAHEAD_LIMITER:
    expected_channels = proc->parameters.lookahead_limiter.channels;
    break;
  case PROCESSOR_TYPE_INVALID:
    break;
  }
  if (expected_channels != num_channels) {
    config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                     "Processor '%s' expects %zu channel(s) but pipeline "
                     "has %zu at this point",
                     step->name, expected_channels, num_channels);
    return false;
  }
  config_error_t sub_err;
  config_error_init(&sub_err);
  if (processor_config_validate(proc, (int)config->devices.samplerate,
                                &sub_err) != 0) {
    config_error_set(err, CONFIG_ERR_INVALID_PROCESSOR, "Processor '%s': %s",
                     step->name, sub_err.message);
    return false;
  }
  return true;
}

typedef struct {
  double ramp_time_ms;
  double limit;
} fader_setting_t;

static bool validate_fader_settings(const dsp_config_t *config,
                                    config_error_t *err) {
  fader_setting_t settings[5];
  const char *set_by[5] = {NULL, NULL, NULL, NULL, NULL};

  settings[0].ramp_time_ms =
      config->devices.has_volume_ramp_time_ms
          ? config->devices.volume_ramp_time_ms
          : 400.0;
  settings[0].limit =
      config->devices.has_volume_limit ? config->devices.volume_limit : 50.0;
  set_by[0] = "devices";

  for (size_t f = 1; f < 5; f++) {
    settings[f].ramp_time_ms = 0.0;
    settings[f].limit = 50.0;
  }

  if (!config->pipeline || config->pipeline_count == 0 ||
      !config->filters || config->filters_count == 0) {
    return true;
  }

  for (size_t i = 0; i < config->pipeline_count; i++) {
    const pipeline_step_config_t *step = &config->pipeline[i];
    if (step->bypassed)
      continue;
    if (step->type != PIPELINE_STEP_TYPE_FILTER)
      continue;
    if (step->has_channels && step->channels_count == 0)
      continue;

    for (size_t j = 0; j < step->names_count; j++) {
      const char *name = step->names[j];
      if (!name || name[0] == '\0')
        continue;
      const filter_config_t *filt = dsp_config_get_filter(config, name);
      if (!filt || filt->type != FILTER_TYPE_VOLUME)
        continue;

      const volume_config_t *vol = &filt->parameters.volume;
      int fader_idx = (int)vol->fader;
      if (fader_idx < 1 || fader_idx > 4)
        continue;

      fader_setting_t these;
      these.ramp_time_ms =
          vol->has_ramp_time_ms ? vol->ramp_time_ms : 400.0;
      these.limit = vol->has_limit ? vol->limit : 50.0;

      if (!set_by[fader_idx]) {
        settings[fader_idx] = these;
        set_by[fader_idx] = name;
      } else {
        if (settings[fader_idx].ramp_time_ms != these.ramp_time_ms ||
            settings[fader_idx].limit != these.limit) {
          config_error_set(
              err, CONFIG_ERR_INVALID_FILTER,
              "Volume filters '%s' and '%s' use the same fader %s, but have "
              "different ramp_time_ms or limit",
              set_by[fader_idx], name, volume_fader_to_string(vol->fader));
          return false;
        }
      }
    }
  }

  return true;
}

int pipeline_config_validate(const dsp_config_t *config, config_error_t *err) {
  if (!config) {
    config_error_set(err, CONFIG_ERR_PARSE, "Configuration is null");
    return -1;
  }

  size_t num_channels =
      capture_device_config_get_channels(&config->devices.capture);
  if (num_channels == 0) {
    if (config->devices.capture.type == AUDIO_BACKEND_TYPE_FILE &&
        config->devices.capture.is_wav) {
      config_error_set(
          err, CONFIG_ERR_INVALID_PIPELINE,
          "Failed to open WAV capture file '%s' or parse channels from header",
          config->devices.capture.cfg.wav_file.filename);
    } else {
      config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                       "Invalid capture channel count: %zu", num_channels);
    }
    return -1;
  }

  for (size_t i = 0; i < config->pipeline_count; i++) {
    const pipeline_step_config_t *step = &config->pipeline[i];
    if (step->bypassed)
      continue;

    bool ok = false;
    switch (step->type) {
    case PIPELINE_STEP_TYPE_FILTER:
      ok = validate_filter_step(step, i, num_channels, config, err);
      break;
    case PIPELINE_STEP_TYPE_MIXER:
      ok = validate_mixer_step(step, i, &num_channels, config, err);
      break;
    case PIPELINE_STEP_TYPE_PROCESSOR:
      ok = validate_processor_step(step, i, num_channels, config, err);
      break;
    }
    if (!ok) {
      return -1;
    }
  }

  size_t playback_channels =
      playback_device_config_get_channels(&config->devices.playback);
  if (num_channels != playback_channels) {
    config_error_set(
        err, CONFIG_ERR_INVALID_PIPELINE,
        "Pipeline outputs %zu channel(s) but playback device expects %zu",
        num_channels, playback_channels);
    return -1;
  }

  if (!validate_fader_settings(config, err)) {
    return -1;
  }

  return 0;
}
