#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "filters/filter.h"
#include "filters/volume.h"
#include "logging/app_logger.h"
#include "pipeline/pipeline.h"
#include "pipeline/pipeline_internal.h"
#include "processors/processor.h"

static const logger_t g_logger = {"dsp.pipeline"};

// ============================================================================
// State Transfer
// ============================================================================

static filter_t *step_get_channel_filter(const pipeline_exec_step_t *step,
                                         size_t channel, size_t filter_idx) {
  if (!step)
    return NULL;
  if (step->type == EXEC_STEP_PARALLEL_FILTERS && step->chains) {
    for (size_t c = 0; c < step->chains_count; c++) {
      if (step->chains[c].channel == channel &&
          filter_idx < step->chains[c].filters_count) {
        return step->chains[c].filters[filter_idx];
      }
    }
  } else if (step->type == EXEC_STEP_BIQUAD && step->biquad_step) {
    const biquad_step_t *bq = step->biquad_step;
    for (size_t c = 0; c < bq->channels_count; c++) {
      if (bq->channel_of[c] == channel && filter_idx < bq->filters_count) {
        return bq->filters[c][filter_idx];
      }
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
      dsp_processor_transfer_state(d_step->processor, s_step->processor);
    }
    return;
  }

  if (d_step->type == EXEC_STEP_PARALLEL_FILTERS && d_step->chains) {
    for (size_t c = 0; c < d_step->chains_count; c++) {
      size_t ch = d_step->chains[c].channel;
      for (size_t f = 0; f < d_step->chains[c].filters_count; f++) {
        filter_t *df = d_step->chains[c].filters[f];
        filter_t *sf = step_get_channel_filter(s_step, ch, f);
        if (df && sf && df->type == sf->type) {
          filter_transfer_state(df, sf);
        }
      }
    }
  } else if (d_step->type == EXEC_STEP_BIQUAD && d_step->biquad_step) {
    const biquad_step_t *bq = d_step->biquad_step;
    for (size_t c = 0; c < bq->channels_count; c++) {
      size_t ch = bq->channel_of[c];
      for (size_t f = 0; f < bq->filters_count; f++) {
        filter_t *df = bq->filters[c][f];
        filter_t *sf = step_get_channel_filter(s_step, ch, f);
        if (df && sf && df->type == sf->type) {
          filter_transfer_state(df, sf);
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

  // 1. Transfer Master Volume state (always synced, matching upstream
  // sync_volumes_to_target)
  if (dest->master_volume && src->master_volume) {
    g_volume_vtable.transfer_state(dest->master_volume, src->master_volume);
    logger_info(&g_logger, "Transferred master volume filter state");
  }

  // 2. Transfer matching filter & processor states when filter transfer is
  // requested
  if (transfer_filters && dest->steps && src->steps) {
    size_t count = (dest->steps_count < src->steps_count) ? dest->steps_count
                                                          : src->steps_count;
    for (size_t s = 0; s < count; s++) {
      transfer_step_state(&dest->steps[s], &src->steps[s]);
    }
  }

  logger_info(&g_logger, "Completed pipeline state transfer");
}
