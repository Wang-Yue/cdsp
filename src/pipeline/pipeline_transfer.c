#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "filters/filter.h"
#include "filters/loudness.h"
#include "filters/volume.h"
#include "logging/app_logger.h"
#include "pipeline/pipeline.h"
#include "pipeline/pipeline_internal.h"
#include "processors/processor.h"

static const logger_t g_logger = {"dsp.pipeline"};

// ============================================================================
// State Transfer
// ============================================================================

static inline size_t step_channel_count(const pipeline_exec_step_t *step) {
  if (step->type == EXEC_STEP_PARALLEL_FILTERS && step->chains)
    return step->chains_count;
  if (step->type == EXEC_STEP_BIQUAD && step->biquad_step)
    return step->biquad_step->channels_count;
  return 0;
}

static inline size_t step_channel_filter_count(const pipeline_exec_step_t *step,
                                               size_t ch_idx,
                                               size_t *out_channel) {
  if (step->type == EXEC_STEP_PARALLEL_FILTERS && step->chains) {
    if (out_channel)
      *out_channel = step->chains[ch_idx].channel;
    return step->chains[ch_idx].filters_count;
  }
  if (step->type == EXEC_STEP_BIQUAD && step->biquad_step) {
    if (out_channel)
      *out_channel = step->biquad_step->channel_of[ch_idx];
    return step->biquad_step->filters_count;
  }
  return 0;
}

static inline filter_t *step_get_filter(const pipeline_exec_step_t *step,
                                        size_t ch_idx, size_t filter_idx) {
  if (step->type == EXEC_STEP_PARALLEL_FILTERS && step->chains)
    return step->chains[ch_idx].filters[filter_idx];
  if (step->type == EXEC_STEP_BIQUAD && step->biquad_step)
    return step->biquad_step->filters[ch_idx][filter_idx];
  return NULL;
}

/// Find the filter in @p step that corresponds to a destination filter on
/// @p channel. Named filters are matched by name, type and occurrence: the
/// n-th filter called "x" on a channel pairs with the n-th "x" on the same
/// channel of the source, so cascaded copies of one filter (e.g.
/// `names: [hp, hp]`) each keep their own state instead of all receiving the
/// first copy's. Unnamed filters fall back to their position.
static filter_t *step_find_filter(const pipeline_exec_step_t *step,
                                  size_t channel, const char *name,
                                  filter_instance_type_t type,
                                  size_t occurrence, size_t fallback_idx) {
  if (!step)
    return NULL;
  size_t ch_count = step_channel_count(step);
  for (size_t c = 0; c < ch_count; c++) {
    size_t ch = 0;
    size_t f_count = step_channel_filter_count(step, c, &ch);
    if (ch != channel)
      continue;
    if (name && name[0] != '\0') {
      size_t seen = 0;
      for (size_t f = 0; f < f_count; f++) {
        filter_t *filt = step_get_filter(step, c, f);
        if (filt && filt->type == type && strcmp(filt->name, name) == 0) {
          if (seen == occurrence)
            return filt;
          seen++;
        }
      }
    } else if (fallback_idx < f_count) {
      filter_t *filt = step_get_filter(step, c, fallback_idx);
      if (filt && filt->type == type)
        return filt;
    }
  }
  return NULL;
}

static void transfer_step_state(pipeline_exec_step_t *d_step,
                                const pipeline_exec_step_t *s_step) {
  if (!d_step || !s_step)
    return;

  if (d_step->type == EXEC_STEP_PROCESSOR &&
      s_step->type == EXEC_STEP_PROCESSOR) {
    if (d_step->processor && s_step->processor &&
        d_step->processor->type == s_step->processor->type) {
      const char *d_name = dsp_processor_get_name(d_step->processor);
      const char *s_name = dsp_processor_get_name(s_step->processor);
      if (!d_name || !s_name || strcmp(d_name, s_name) == 0) {
        dsp_processor_transfer_state(d_step->processor, s_step->processor);
      }
    }
    return;
  }

  size_t ch_count = step_channel_count(d_step);
  for (size_t c = 0; c < ch_count; c++) {
    size_t ch = 0;
    size_t f_count = step_channel_filter_count(d_step, c, &ch);
    for (size_t f = 0; f < f_count; f++) {
      filter_t *df = step_get_filter(d_step, c, f);
      if (!df)
        continue;
      size_t occurrence = 0;
      for (size_t k = 0; k < f; k++) {
        filter_t *prev = step_get_filter(d_step, c, k);
        if (prev && prev->type == df->type && strcmp(prev->name, df->name) == 0)
          occurrence++;
      }
      filter_t *sf =
          step_find_filter(s_step, ch, df->name, df->type, occurrence, f);
      if (sf) {
        filter_transfer_state(df, sf);
      }
    }
  }
}

/// Re-read the fader levels into every Loudness filter of @p pipeline (after
/// the fader bank was re-seeded).
static void refresh_loudness_filters(pipeline_t *pipeline) {
  for (size_t s = 0; s < pipeline->steps_count; s++) {
    const pipeline_exec_step_t *step = &pipeline->steps[s];
    if (step->type != EXEC_STEP_PARALLEL_FILTERS || !step->chains)
      continue;
    for (size_t c = 0; c < step->chains_count; c++) {
      const parallel_filter_chain_t *chain = &step->chains[c];
      for (size_t f = 0; f < chain->filters_count; f++) {
        filter_t *filt = chain->filters[f];
        if (filt && filt->instance && filt->type == FILTER_INSTANCE_LOUDNESS) {
          loudness_filter_bind_fader_levels((loudness_filter_t *)filt->instance,
                                            &pipeline->faders.levels);
        }
      }
    }
  }
}

void pipeline_transfer_state(pipeline_t *dest, const pipeline_t *src,
                             bool transfer_filters) {
  if (!dest || !src)
    return;

  logger_info(&g_logger, "Starting pipeline state transfer");

  // 1. Carry the fader ramps over (when transfer_filters is requested, like
  // upstream Faders::update_parameters) or sync processing_parameters to the
  // target levels and start the faders settled there (when rebuilding the
  // pipeline). This must precede step 2: Loudness filters read the faders.
  if (transfer_filters) {
    pipeline_faders_transfer(&dest->faders, &src->faders);
    refresh_loudness_filters(dest);
    logger_info(&g_logger, "Transferred fader ramp state");
  } else {
    // Structural Pipeline or Mixer rebuild: match upstream
    // processing_params.sync_volumes_to_target(): snap all current volumes
    // to their targets immediately so newly built faders start settled.
    if (dest->proc_params) {
      for (int i = 0; i < FADER_COUNT; i++) {
        double target = processing_parameters_get_target_volume_for_fader(
            dest->proc_params, (fader_t)i);
        processing_parameters_set_current_volume_for_fader(dest->proc_params,
                                                           target, (fader_t)i);
      }
    }
    // dest's faders were seeded when it was built, on the control thread;
    // seed them again from the synced levels, as upstream's Faders::new runs
    // after sync_volumes_to_target(), and let its Loudness filters follow.
    pipeline_faders_reseed(&dest->faders);
    refresh_loudness_filters(dest);
    logger_info(&g_logger,
                "Synced all faders to target volume on pipeline rebuild");
  }

  // 2. Transfer matching filter & processor states when filter transfer is
  // requested. Filters and processors are matched by channel, name, and type
  // to prevent cross-filter state contamination across structural changes.
  if (transfer_filters && dest->steps && src->steps) {
    size_t count = (dest->steps_count < src->steps_count) ? dest->steps_count
                                                          : src->steps_count;
    for (size_t s = 0; s < count; s++) {
      transfer_step_state(&dest->steps[s], &src->steps[s]);
    }
  }

  logger_info(&g_logger, "Completed pipeline state transfer");
}
