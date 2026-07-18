/**
 * @file mixer.c
 * @brief Implementation of the channel routing matrix and audio mixer.
 *
 * Channel Routing Matrix Implementation Details:
 * - The mixer converts user-configured mapping rules into precomputed
 * `prepared_source_list_t` structures per destination channel.
 * - Linear gain conversion: When gain scale is `GAIN_SCALE_DB`, dB values are
 * converted to linear gain using `double_from_db`.
 * - Phase inversion: If `inverted` is set to true, the linear gain is negated
 * (-lin_gain).
 * - Real-time processing (`audio_mixer_process`):
 *   1. Validates that input frames do not exceed `chunk_size` and destination
 * buffer matches `channels_out`.
 *   2. For each destination channel, clears the output waveform buffer to 0.0.
 *   3. Iterates over contributing prepared sources:
 *      - If gain == 1.0, uses `dsp_ops_add` (vectorized addition).
 *      - If gain != 0.0 and != 1.0, uses `dsp_ops_multiply_add` (vectorized
 * multiply-accumulate).
 *   4. Zero-allocation guarantee is strictly maintained on the audio processing
 * path.
 */
#include "Mixer/mixer.h"

#include <stdlib.h>
#include <string.h>

#include "Logging/app_logger.h"

static const logger_t g_logger = {"dsp.mixer"};

/**
 * @brief Represents a prepared source channel contribution to a destination
 * channel.
 */
typedef struct {
  size_t in_channel;  ///< Input channel index.
  double gain;        ///< Linear gain multiplier (negative if inverted).
} prepared_source_t;

/**
 * @brief List of prepared source contributions for a single destination
 * channel.
 */
typedef struct {
  size_t count;                ///< Number of active contributing sources.
  prepared_source_t* sources;  ///< Array of prepared source contributions.
} prepared_source_list_t;

/// Mixer that changes channel count and routes/sums audio between channels.
/// Mixer that changes channel count and routes/sums audio between channels.
struct mixer_s {
  size_t chunk_size;    ///< Maximum number of frames per processing chunk.
  char* name;           ///< Unique name of the mixer instance.
  size_t channels_in;   ///< Expected number of input channels.
  size_t channels_out;  ///< Number of output channels produced.
  prepared_source_list_t*
      mapping;  ///< Array of length channels_out defining source routing.
};

/**
 * @brief Populates the internal routing matrix mapping from a mixer
 * configuration.
 *
 * Precomputes linear gains and channel routing lists for all destination
 * channels.
 *
 * @param mixer Pointer to mixer instance.
 * @param config Configuration containing mapping rules.
 */
static bool populate_mapping(mixer_t* mixer, const mixer_config_t* config) {
  for (size_t i = 0; i < config->mapping_count; i++) {
    const mixer_mapping_t* map = &config->mapping[i];
    size_t dest = (size_t)map->dest;
    // Ignore mappings to out-of-bounds destination channels or muted
    // destination mappings
    if (dest >= mixer->channels_out || map->mute) continue;

    // Count unmuted contributing sources for this destination channel
    size_t valid_count = 0;
    for (size_t j = 0; j < map->sources_count; j++) {
      if (!map->sources[j].mute) valid_count++;
    }
    if (valid_count == 0) continue;

    // Allocate prepared source array for this destination channel
    if (mixer->mapping[dest].sources) {
      free(mixer->mapping[dest].sources);
    }
    mixer->mapping[dest].sources =
        (prepared_source_t*)calloc(valid_count, sizeof(prepared_source_t));
    if (!mixer->mapping[dest].sources) {
      mixer->mapping[dest].count = 0;
      return false;
    }
    mixer->mapping[dest].count = valid_count;

    size_t idx = 0;
    for (size_t j = 0; j < map->sources_count; j++) {
      const mixer_source_t* src = &map->sources[j];
      if (src->mute) continue;

      // Calculate linear gain from dB or linear configuration
      double default_gain = (src->scale == GAIN_SCALE_LINEAR) ? 1.0 : 0.0;
      double gain = src->has_gain ? src->gain : default_gain;
      double lin_gain =
          (src->scale == GAIN_SCALE_LINEAR) ? gain : double_from_db(gain);
      // Invert phase if requested (represented as a negative linear gain
      // coefficient)
      if (src->inverted) {
        lin_gain *= -1.0;
      }
      mixer->mapping[dest].sources[idx].in_channel = (size_t)src->channel;
      mixer->mapping[dest].sources[idx].gain = lin_gain;
      idx++;
    }
  }
  return true;
}

mixer_t* mixer_create(const char* name, const mixer_config_t* config,
                      size_t chunk_size, config_error_t* err) {
  if (mixer_config_validate(config, err) != 0) return NULL;
  mixer_t* mixer = (mixer_t*)calloc(1, sizeof(mixer_t));
  if (!mixer) {
    logger_error(&g_logger, "Failed to allocate mixer_t for '%s'",
                 name ? name : "unnamed");
    return NULL;
  }

  mixer->chunk_size = chunk_size;
  mixer->name = name ? strdup(name) : strdup("mixer");
  mixer->channels_in = (size_t)config->channels_in;
  mixer->channels_out = (size_t)config->channels_out;
  mixer->mapping = (prepared_source_list_t*)calloc(
      mixer->channels_out, sizeof(prepared_source_list_t));
  if (!mixer->mapping) {
    logger_error(&g_logger,
                 "Failed to allocate prepared source list for mixer '%s'",
                 mixer->name);
    mixer_free(mixer);
    return NULL;
  }

  if (!populate_mapping(mixer, config)) {
    logger_error(&g_logger, "Failed to populate mapping matrix for mixer '%s'",
                 mixer->name);
    mixer_free(mixer);
    return NULL;
  }
  logger_debug(&g_logger,
               "Created mixer '%s' (in_channels=%zu, out_channels=%zu)",
               mixer->name, mixer->channels_in, mixer->channels_out);
  return mixer;
}

/// Zero-allocation API. The caller pre-allocates `output` with
/// `output.channels == channelsOut` and `output.frames >= input.validFrames`.
/// The mixer writes the mixed samples directly and sets `output.validFrames`.
///
/// `input` and `output` must reference distinct buffers — the mixer
/// accumulates into the output and reads input concurrently, so aliasing
/// would corrupt the result.
mixer_error_t mixer_process(mixer_t* mixer, const audio_chunk_t* input,
                            audio_chunk_t* output) {
  if (!mixer || !input || !output) return MIXER_ERR_INPUT_SIZE_MISMATCH;
  size_t frames = audio_chunk_get_valid_frames(input);
  if (frames > mixer->chunk_size) {
    logger_warn(&g_logger,
                "Mixer '%s' input frame count exceeds chunk_size: %zu > %zu",
                mixer->name, frames, mixer->chunk_size);
    return MIXER_ERR_INPUT_SIZE_MISMATCH;
  }
  if (audio_chunk_get_channels(output) != mixer->channels_out) {
    logger_warn(
        &g_logger,
        "Mixer '%s' output channel count mismatch: expected %zu, got %zu",
        mixer->name, mixer->channels_out, audio_chunk_get_channels(output));
    return MIXER_ERR_CHANNEL_COUNT_MISMATCH;
  }
  if (audio_chunk_get_frames(output) < frames) {
    logger_warn(&g_logger,
                "Mixer '%s' output buffer frame count too small: valid=%zu, "
                "allocated=%zu",
                mixer->name, frames, audio_chunk_get_frames(output));
    return MIXER_ERR_OUTPUT_BUFFER_TOO_SMALL;
  }

  // Process each output destination channel in a single pass to maximize L1
  // cache locality
  for (size_t out_ch = 0; out_ch < mixer->channels_out; out_ch++) {
    mutable_waveform_t dst = audio_chunk_get_channel(output, out_ch);
    if (!dst) continue;

    dsp_ops_clear(dst, frames);

    prepared_source_list_t* list = &mixer->mapping[out_ch];
    for (size_t i = 0; i < list->count; i++) {
      prepared_source_t* src = &list->sources[i];
      // Skip if mapped source channel is not present in input chunk
      if (src->in_channel >= audio_chunk_get_channels(input)) continue;
      waveform_t src_ptr = audio_chunk_get_channel(input, src->in_channel);
      if (!src_ptr) continue;

      // Optimize direct unity gain addition vs multiply-accumulate to save
      // instruction cycles
      if (src->gain == 1.0) {
        dsp_ops_add(src_ptr, dst, frames);
      } else if (src->gain != 0.0) {
        dsp_ops_multiply_add(src_ptr, src->gain, dst, frames);
      }
    }
  }

  audio_chunk_set_valid_frames(output, frames);
  return MIXER_OK;
}

audio_chunk_t* mixer_process_chunk(mixer_t* mixer, const audio_chunk_t* input) {
  if (!mixer || !input) return NULL;
  audio_chunk_t* output = audio_chunk_create(
      audio_chunk_get_valid_frames(input), mixer->channels_out);
  if (!output) return NULL;
  if (mixer_process(mixer, input, output) != MIXER_OK) {
    audio_chunk_free(output);
    return NULL;
  }
  return output;
}

void mixer_free(mixer_t* mixer) {
  if (!mixer) return;
  if (mixer->name) free(mixer->name);
  if (mixer->mapping) {
    for (size_t i = 0; i < mixer->channels_out; i++) {
      if (mixer->mapping[i].sources) {
        free(mixer->mapping[i].sources);
      }
    }
    free(mixer->mapping);
  }
  free(mixer);
}

size_t mixer_get_channels_in(const mixer_t* mixer) {
  return mixer ? mixer->channels_in : 0;
}

size_t mixer_get_channels_out(const mixer_t* mixer) {
  return mixer ? mixer->channels_out : 0;
}

const char* mixer_get_name(const mixer_t* mixer) {
  return mixer ? mixer->name : NULL;
}

int mixer_config_validate(const mixer_config_t* mixer, config_error_t* err) {
  if (!mixer) {
    config_error_set(err, CONFIG_ERR_INVALID_MIXER, "Null mixer configuration");
    return -1;
  }
  if (mixer->channels_in <= 0 || mixer->channels_out <= 0) {
    config_error_set(err, CONFIG_ERR_INVALID_MIXER,
                     "channels_in and channels_out must be greater than zero");
    return -1;
  }

  bool* seen_dests = (bool*)calloc(
      mixer->channels_out > 0 ? mixer->channels_out : 1, sizeof(bool));
  if (!seen_dests) return -1;

  for (size_t i = 0; i < mixer->mapping_count; i++) {
    int dest = mixer->mapping[i].dest;
    if ((size_t)dest >= mixer->channels_out) {
      config_error_set(err, CONFIG_ERR_INVALID_MIXER,
                       "mixer dest %d >= channels_out %d", dest,
                       mixer->channels_out);
      free(seen_dests);
      return -1;
    }
    if (seen_dests[dest]) {
      config_error_set(err, CONFIG_ERR_INVALID_MIXER,
                       "mixer dest %d mapped more than once", dest);
      free(seen_dests);
      return -1;
    }
    seen_dests[dest] = true;

    bool* seen_sources = (bool*)calloc(
        mixer->channels_in > 0 ? mixer->channels_in : 1, sizeof(bool));
    if (!seen_sources) {
      free(seen_dests);
      return -1;
    }
    for (size_t j = 0; j < mixer->mapping[i].sources_count; j++) {
      int src_ch = mixer->mapping[i].sources[j].channel;
      if ((size_t)src_ch >= mixer->channels_in) {
        config_error_set(err, CONFIG_ERR_INVALID_MIXER,
                         "mixer source channel %d >= channels_in %d", src_ch,
                         mixer->channels_in);
        free(seen_sources);
        free(seen_dests);
        return -1;
      }
      if (seen_sources[src_ch]) {
        config_error_set(
            err, CONFIG_ERR_INVALID_MIXER,
            "mixer source channel %d listed more than once for dest %d", src_ch,
            dest);
        free(seen_sources);
        free(seen_dests);
        return -1;
      }
      seen_sources[src_ch] = true;
    }
    free(seen_sources);
  }

  free(seen_dests);
  return 0;
}
