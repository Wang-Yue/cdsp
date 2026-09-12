#include <stdbool.h>
#include <string.h>

#include "Filters/filter.h"
#include "Filters/volume.h"
#include "Logging/app_logger.h"
#include "Pipeline/pipeline.h"
#include "Pipeline/pipeline_internal.h"
#include "Processors/processor.h"

static const logger_t g_logger = {"dsp.pipeline"};

// ============================================================================
// State Transfer (Real-Time Audio Thread Safe - Zero Allocations)
// ============================================================================

typedef struct {
  size_t channel;
  filter_t* filter;
} pipeline_channel_filter_t;

static size_t pipeline_collect_filters(const pipeline_t* p,
                                       pipeline_channel_filter_t* out_filters,
                                       size_t max_filters) {
  if (!p || !p->steps || !out_filters || max_filters == 0) return 0;
  size_t count = 0;
  for (size_t s = 0; s < p->steps_count; s++) {
    const pipeline_exec_step_t* step = &p->steps[s];
    if (step->type == EXEC_STEP_PARALLEL_FILTERS && step->chains) {
      for (size_t c = 0; c < step->chains_count; c++) {
        const parallel_filter_chain_t* chain = &step->chains[c];
        for (size_t f = 0; f < chain->filters_count; f++) {
          if (chain->filters[f] && count < max_filters) {
            out_filters[count++] = (pipeline_channel_filter_t){
                .channel = chain->channel,
                .filter = chain->filters[f],
            };
          }
        }
      }
    } else if (step->type == EXEC_STEP_BIQUAD && step->biquad_step) {
      const biquad_step_t* bq = step->biquad_step;
      for (size_t c = 0; c < bq->channels_count; c++) {
        size_t ch = bq->channel_of[c];
        for (size_t f = 0; f < bq->filters_count; f++) {
          if (bq->filters[c][f] && count < max_filters) {
            out_filters[count++] = (pipeline_channel_filter_t){
                .channel = ch,
                .filter = bq->filters[c][f],
            };
          }
        }
      }
    }
  }
  return count;
}

static void transfer_all_filters_state(pipeline_t* dest,
                                       const pipeline_t* src) {
  pipeline_channel_filter_t dest_filters[512];
  pipeline_channel_filter_t src_filters[512];
  bool src_used[512] = {false};

  size_t dest_count = pipeline_collect_filters(dest, dest_filters, 512);
  size_t src_count = pipeline_collect_filters(src, src_filters, 512);

  // 1. Match each destination filter with a source filter on the same channel
  // by name
  for (size_t di = 0; di < dest_count; di++) {
    filter_t* dest_f = dest_filters[di].filter;
    const char* dname = filter_get_name(dest_f);
    if (!dname || dname[0] == '\0') continue;
    size_t d_ch = dest_filters[di].channel;

    bool matched = false;
    for (size_t si = 0; si < src_count; si++) {
      if (src_used[si]) continue;
      if (src_filters[si].channel != d_ch) continue;

      filter_t* src_f = src_filters[si].filter;
      const char* sname = filter_get_name(src_f);
      if (sname && strcmp(dname, sname) == 0) {
        filter_transfer_state(dest_f, src_f);
        src_used[si] = true;
        matched = true;
        break;
      }
    }
    if (!matched) {
      logger_debug(&g_logger,
                   "Filter '%s' (ch=%zu) is new, state initialized clean",
                   dname, d_ch);
    }
  }

  // 2. Log retired filters
  for (size_t si = 0; si < src_count; si++) {
    if (!src_used[si]) {
      logger_debug(&g_logger, "Filter '%s' (ch=%zu) retired from pipeline",
                   filter_get_name(src_filters[si].filter),
                   src_filters[si].channel);
    }
  }
}

/// Transfer state for named audio processors.
static void transfer_named_processors_state(pipeline_t* dest,
                                            const pipeline_t* src) {
  if (!dest || !src || !dest->steps || !src->steps) return;
  bool src_proc_used[128] = {false};
  size_t max_src = src->steps_count < 128 ? src->steps_count : 128;

  for (size_t di = 0; di < dest->steps_count; di++) {
    pipeline_exec_step_t* d_step = &dest->steps[di];
    if (d_step->type != EXEC_STEP_PROCESSOR || !d_step->processor) {
      continue;
    }
    const char* dname = dsp_processor_get_name(d_step->processor);
    if (!dname || dname[0] == '\0') continue;

    bool matched = false;
    for (size_t si = 0; si < max_src; si++) {
      if (src_proc_used[si]) continue;
      pipeline_exec_step_t* s_step = &src->steps[si];
      if (s_step->type != EXEC_STEP_PROCESSOR || !s_step->processor ||
          s_step->processor->type != d_step->processor->type) {
        continue;
      }
      const char* sname = dsp_processor_get_name(s_step->processor);
      if (sname && strcmp(dname, sname) == 0) {
        dsp_processor_transfer_state(d_step->processor, s_step->processor);
        src_proc_used[si] = true;
        matched = true;
        break;
      }
    }
    if (!matched) {
      logger_debug(&g_logger, "Processor '%s' is new, state initialized clean",
                   dname);
    }
  }
}

void pipeline_transfer_state(pipeline_t* dest, const pipeline_t* src) {
  if (!dest || !src) return;

  logger_info(&g_logger, "Starting pipeline state transfer");

  // 1. Transfer Master Volume state
  if (dest->master_volume && src->master_volume) {
    g_volume_vtable.transfer_state(dest->master_volume, src->master_volume);
    logger_info(&g_logger, "Transferred master volume filter state");
  }

  // 2. Transfer all channel filters (step-type agnostic, matched by channel and
  // name)
  transfer_all_filters_state(dest, src);

  // 3. Transfer named multi-channel processors
  transfer_named_processors_state(dest, src);

  logger_info(&g_logger, "Completed pipeline state transfer");
}
