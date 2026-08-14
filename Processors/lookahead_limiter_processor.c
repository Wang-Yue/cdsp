#include "Processors/lookahead_limiter_processor.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Audio/audio_chunk.h"
#include "Config/config_error.h"
#include "Config/filter_config_types.h"
#include "Config/processor_config_types.h"
#include "Filters/delay.h"
#include "Filters/filter.h"
#include "Filters/lookahead_limiter.h"
#include "Logging/app_logger.h"
#include "Processors/processor.h"

static const logger_t g_logger = {"lookahead_limiter_processor"};

struct lookahead_limiter_processor {
  char name[64];
  size_t channels;
  int* monitor_channels;
  size_t monitor_channels_count;
  int* process_channels;
  size_t process_channels_count;
  bool delay_processed_only;
  int sample_rate;
  void* gain;
  void** delays;  // Array of delay filter pointers (one per channel)
  size_t delays_count;
  double* scratch;  // Peak detection buffer (matches chunk_size)
  size_t scratch_capacity;
  bool channel_warning_logged;
};

typedef struct lookahead_limiter_processor lookahead_limiter_processor_t;

static const char* lookahead_limiter_processor_get_name(const void* impl) {
  const lookahead_limiter_processor_t* processor =
      (const lookahead_limiter_processor_t*)impl;
  return processor ? processor->name : "";
}

static double compute_time_samples(double value, time_unit_t unit,
                                   int sample_rate) {
  switch (unit) {
    case TIME_UNIT_US:
      return value / 1000000.0 * (double)sample_rate;
    case TIME_UNIT_MS:
      return value / 1000.0 * (double)sample_rate;
    case TIME_UNIT_S:
      return value * (double)sample_rate;
    case TIME_UNIT_SAMPLES:
      return value;
  }
  return 0.0;
}

static delay_unit_t map_time_unit_to_delay_unit(time_unit_t unit) {
  switch (unit) {
    case TIME_UNIT_US:
      return DELAY_UNIT_US;
    case TIME_UNIT_MS:
      return DELAY_UNIT_MS;
    case TIME_UNIT_S:
      return DELAY_UNIT_S;
    case TIME_UNIT_SAMPLES:
      return DELAY_UNIT_SAMPLES;
  }
  return DELAY_UNIT_SAMPLES;
}

static int lookahead_limiter_config_validate(const processor_config_t* config,
                                             int sample_rate,
                                             config_error_t* err) {
  if (!config || config->type != PROCESSOR_TYPE_LOOKAHEAD_LIMITER) return -1;
  const lookahead_limiter_processor_config_t* p =
      &config->parameters.lookahead_limiter;

  if (p->channels <= 0) {
    config_error_set(err, CONFIG_ERR_INVALID_PROCESSOR,
                     "LookaheadLimiter: channels must be > 0, got %d",
                     p->channels);
    return -1;
  }
  if (p->attack < 0.0) {
    config_error_set(err, CONFIG_ERR_INVALID_PROCESSOR,
                     "LookaheadLimiter: attack must be >= 0, got %g",
                     p->attack);
    return -1;
  }

  if (sample_rate > 0) {
    double attack_samples =
        round(compute_time_samples(p->attack, p->attack_unit, sample_rate));
    if (attack_samples > (double)sample_rate) {
      config_error_set(err, CONFIG_ERR_INVALID_PROCESSOR,
                       "LookaheadLimiter: attack time must be less than or "
                       "equal to 1 second");
      return -1;
    }
  }
  if (p->release < 0.0) {
    config_error_set(err, CONFIG_ERR_INVALID_PROCESSOR,
                     "LookaheadLimiter: release must be >= 0, got %g",
                     p->release);
    return -1;
  }

  for (size_t i = 0; i < p->monitor_channels_count; i++) {
    if (p->monitor_channels[i] < 0 || p->monitor_channels[i] >= p->channels) {
      config_error_set(
          err, CONFIG_ERR_INVALID_PROCESSOR,
          "LookaheadLimiter: monitor channel %d is invalid (max: %d)",
          p->monitor_channels[i], p->channels - 1);
      return -1;
    }
  }
  for (size_t i = 0; i < p->process_channels_count; i++) {
    if (p->process_channels[i] < 0 || p->process_channels[i] >= p->channels) {
      config_error_set(
          err, CONFIG_ERR_INVALID_PROCESSOR,
          "LookaheadLimiter: process channel %d is invalid (max: %d)",
          p->process_channels[i], p->channels - 1);
      return -1;
    }
  }
  return 0;
}

static void lookahead_limiter_processor_free(void* impl) {
  lookahead_limiter_processor_t* processor =
      (lookahead_limiter_processor_t*)impl;
  if (!processor) return;

  if (processor->monitor_channels) free(processor->monitor_channels);
  if (processor->process_channels) free(processor->process_channels);
  if (processor->scratch) free(processor->scratch);

  if (processor->gain) g_lookahead_gain_vtable.free(processor->gain);

  if (processor->delays) {
    for (size_t i = 0; i < processor->delays_count; i++) {
      if (processor->delays[i]) {
        g_delay_vtable.free(processor->delays[i]);
      }
    }
    free(processor->delays);
  }

  free(processor);
}

static void* lookahead_limiter_processor_create(
    const char* name, const processor_config_t* config, int sample_rate,
    size_t chunk_size, config_error_t* err) {
  if (!config || config->type != PROCESSOR_TYPE_LOOKAHEAD_LIMITER) return NULL;
  const lookahead_limiter_processor_config_t* params =
      &config->parameters.lookahead_limiter;
  if (lookahead_limiter_config_validate(config, sample_rate, err) != 0)
    return NULL;
  if (sample_rate <= 0 || chunk_size == 0) return NULL;

  lookahead_limiter_processor_t* processor =
      (lookahead_limiter_processor_t*)calloc(
          1, sizeof(lookahead_limiter_processor_t));
  if (!processor) return NULL;

  if (name) {
    strncpy(processor->name, name, sizeof(processor->name) - 1);
    processor->name[sizeof(processor->name) - 1] = '\0';
  } else {
    strcpy(processor->name, "lookahead_limiter_proc");
  }

  processor->channels = (size_t)params->channels;
  processor->sample_rate = sample_rate;
  processor->delay_processed_only = params->delay_processed_only;

  processor->scratch_capacity = chunk_size;
  processor->scratch = (double*)calloc(chunk_size, sizeof(double));
  if (!processor->scratch) {
    lookahead_limiter_processor_free(processor);
    return NULL;
  }

  // Set up monitored channels
  if (params->monitor_channels_count > 0 && params->monitor_channels) {
    processor->monitor_channels_count = params->monitor_channels_count;
    processor->monitor_channels =
        (int*)calloc(processor->monitor_channels_count, sizeof(int));
    if (processor->monitor_channels) {
      memcpy(processor->monitor_channels, params->monitor_channels,
             processor->monitor_channels_count * sizeof(int));
    }
  } else {
    processor->monitor_channels_count = processor->channels;
    processor->monitor_channels =
        (int*)calloc(processor->monitor_channels_count, sizeof(int));
    if (processor->monitor_channels) {
      for (size_t i = 0; i < processor->monitor_channels_count; i++) {
        processor->monitor_channels[i] = (int)i;
      }
    }
  }

  // Set up process channels
  if (params->process_channels_count > 0 && params->process_channels) {
    processor->process_channels_count = params->process_channels_count;
    processor->process_channels =
        (int*)calloc(processor->process_channels_count, sizeof(int));
    if (processor->process_channels) {
      memcpy(processor->process_channels, params->process_channels,
             processor->process_channels_count * sizeof(int));
    }
  } else {
    processor->process_channels_count = processor->channels;
    processor->process_channels =
        (int*)calloc(processor->process_channels_count, sizeof(int));
    if (processor->process_channels) {
      for (size_t i = 0; i < processor->process_channels_count; i++) {
        processor->process_channels[i] = (int)i;
      }
    }
  }

  if (!processor->monitor_channels || !processor->process_channels) {
    lookahead_limiter_processor_free(processor);
    return NULL;
  }

  filter_config_t gain_cfg = {
      .type = FILTER_TYPE_LOOKAHEAD_LIMITER,
      .parameters.lookahead_limiter = {.limit = params->limit,
                                       .attack = params->attack,
                                       .attack_unit = params->attack_unit,
                                       .release = params->release,
                                       .release_unit = params->release_unit}};

  processor->gain = g_lookahead_gain_vtable.create(NULL, &gain_cfg, sample_rate,
                                                   chunk_size, NULL, NULL);
  if (!processor->gain) {
    lookahead_limiter_processor_free(processor);
    return NULL;
  }

  // Configure delays (one per channel)
  processor->delays_count = processor->channels;
  processor->delays = (void**)calloc(processor->delays_count, sizeof(void*));
  if (!processor->delays) {
    lookahead_limiter_processor_free(processor);
    return NULL;
  }

  delay_config_t dparams = {0};
  dparams.delay = params->attack;
  dparams.delay_unit = map_time_unit_to_delay_unit(params->attack_unit);
  dparams.subsample = false;

  filter_config_t fcfg = {.type = FILTER_TYPE_DELAY,
                          .parameters.delay = dparams};

  for (size_t i = 0; i < processor->delays_count; i++) {
    char dname[128];
    snprintf(dname, sizeof(dname), "%s_delay_%zu", processor->name, i);
    processor->delays[i] =
        g_delay_vtable.create(dname, &fcfg, sample_rate, chunk_size, NULL, err);
    if (!processor->delays[i]) {
      lookahead_limiter_processor_free(processor);
      return NULL;
    }
  }

  return processor;
}

static void lookahead_limiter_processor_process(void* impl,
                                                audio_chunk_t* chunk) {
  lookahead_limiter_processor_t* processor =
      (lookahead_limiter_processor_t*)impl;
  if (!processor || !chunk || !processor->scratch) return;
  size_t count = audio_chunk_get_valid_frames(chunk);
  if (count > processor->scratch_capacity) count = processor->scratch_capacity;
  if (count == 0 || processor->monitor_channels_count == 0) return;

  size_t ch_count = audio_chunk_get_channels(chunk);

  // Check channel indices sanity
  bool mismatch = false;
  for (size_t i = 0; i < processor->monitor_channels_count; i++) {
    if (processor->monitor_channels[i] < 0 ||
        (size_t)processor->monitor_channels[i] >= ch_count) {
      mismatch = true;
      break;
    }
  }
  if (!mismatch) {
    for (size_t i = 0; i < processor->process_channels_count; i++) {
      if (processor->process_channels[i] < 0 ||
          (size_t)processor->process_channels[i] >= ch_count) {
        mismatch = true;
        break;
      }
    }
  }
  if (mismatch) {
    if (!processor->channel_warning_logged) {
      logger_error(&g_logger,
                   "LookaheadLimiter processor '%s': Channel mismatch! Chunk "
                   "has %zu channels, but processor is configured for %zu.",
                   processor->name, ch_count, processor->channels);
      processor->channel_warning_logged = true;
    }
    return;
  }

  // 1. Detect peaks: Find maximum absolute amplitude of all monitored channels
  // at each sample index
  memset(processor->scratch, 0, count * sizeof(double));
  for (size_t i = 0; i < processor->monitor_channels_count; i++) {
    int ch = processor->monitor_channels[i];
    const double* ch_buf = audio_chunk_get_channel(chunk, (size_t)ch);
    for (size_t f = 0; f < count; f++) {
      double val = fabs(ch_buf[f]);
      if (val > processor->scratch[f]) {
        processor->scratch[f] = val;
      }
    }
  }

  // 2. Compute gain envelope (scratch buffer is overwritten in-place with
  // envelope)
  g_lookahead_gain_vtable.process(processor->gain, processor->scratch, count);

  // 3. Apply delay to keep channels time-aligned
  for (size_t ch = 0; ch < ch_count; ch++) {
    bool should_delay = true;
    if (processor->delay_processed_only) {
      should_delay = false;
      for (size_t p = 0; p < processor->process_channels_count; p++) {
        if (processor->process_channels[p] == (int)ch) {
          should_delay = true;
          break;
        }
      }
    }

    if (should_delay && ch < processor->delays_count && processor->delays[ch]) {
      double* ch_buf = audio_chunk_get_channel(chunk, ch);
      g_delay_vtable.process(processor->delays[ch], ch_buf, count);
    }
  }

  // 4. Apply gain envelope to process channels
  audio_chunk_apply_gain(chunk, processor->process_channels,
                         processor->process_channels_count, processor->scratch,
                         count);
}

static void lookahead_limiter_processor_transfer_state(void* dest_ptr,
                                                       const void* src_ptr) {
  lookahead_limiter_processor_t* dest =
      (lookahead_limiter_processor_t*)dest_ptr;
  const lookahead_limiter_processor_t* src =
      (const lookahead_limiter_processor_t*)src_ptr;
  if (!dest || !src) return;

  if (dest->gain && src->gain) {
    g_lookahead_gain_vtable.transfer_state(dest->gain, src->gain);
  }

  // Transfer delay states
  size_t d_count = dest->delays_count < src->delays_count ? dest->delays_count
                                                          : src->delays_count;
  for (size_t i = 0; i < d_count; i++) {
    if (dest->delays[i] && src->delays[i]) {
      g_delay_vtable.transfer_state(dest->delays[i], src->delays[i]);
    }
  }
}

const processor_vtable_t g_lookahead_limiter_processor_vtable = {
    .validate = lookahead_limiter_config_validate,
    .create = lookahead_limiter_processor_create,
    .process = lookahead_limiter_processor_process,
    .get_name = lookahead_limiter_processor_get_name,
    .transfer_state = lookahead_limiter_processor_transfer_state,
    .free = lookahead_limiter_processor_free};
