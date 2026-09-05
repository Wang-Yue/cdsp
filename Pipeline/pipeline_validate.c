#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "Config/config_error.h"
#include "Config/configuration.h"
#include "Config/engine_config_types.h"
#include "Config/filter_config_types.h"
#include "Config/processor_config_types.h"
#include "Pipeline/pipeline.h"

// ============================================================================
// Configuration Validation
// ============================================================================

static bool validate_filter_step(const pipeline_step_config_t* step,
                                 size_t step_idx, size_t num_channels,
                                 const dsp_config_t* config,
                                 config_error_t* err) {
  if (!step->names || step->names_count == 0) {
    config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                     "Filter step %zu must have 'names'", step_idx);
    return false;
  }
  for (size_t j = 0; j < step->names_count; j++) {
    if (!step->names[j] || step->names[j][0] == '\0') {
      config_error_set(
          err, CONFIG_ERR_INVALID_PIPELINE,
          "Filter step %zu has invalid/empty filter name at index %zu",
          step_idx, j);
      return false;
    }
    if (!dsp_config_get_filter(config, step->names[j])) {
      config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                       "Filter '%s' referenced in pipeline but not defined",
                       step->names[j]);
      return false;
    }
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
  return true;
}

static bool validate_mixer_step(const pipeline_step_config_t* step,
                                size_t step_idx, size_t* inout_channels,
                                const dsp_config_t* config,
                                config_error_t* err) {
  if (!step->has_name || step->name[0] == '\0') {
    config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                     "Mixer step %zu must have 'name'", step_idx);
    return false;
  }
  const mixer_config_t* mixer = dsp_config_get_mixer(config, step->name);
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
  return true;
}

static bool validate_processor_step(const pipeline_step_config_t* step,
                                    size_t step_idx, size_t num_channels,
                                    const dsp_config_t* config,
                                    config_error_t* err) {
  if (!step->has_name || step->name[0] == '\0') {
    config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                     "Processor step %zu must have 'name'", step_idx);
    return false;
  }
  const processor_config_t* proc = dsp_config_get_processor(config, step->name);
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
  return true;
}

int pipeline_config_validate(const dsp_config_t* config, config_error_t* err) {
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
    const pipeline_step_config_t* step = &config->pipeline[i];
    if (step->bypassed) continue;

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

  return 0;
}
