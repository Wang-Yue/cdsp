#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "Filters/filter.h"
#include "Filters/volume.h"
#include "Logging/app_logger.h"
#include "Pipeline/pipeline.h"
#include "Pipeline/pipeline_internal.h"
#include "Processors/processor.h"

static const logger_t g_logger = {"dsp.pipeline"};

// ============================================================================
// State Transfer
// ============================================================================

static filter_t* step_get_kth_filter(const pipeline_exec_step_t* step,
                                     size_t channel, const char* name,
                                     size_t target_k) {
  if (!step || !name || name[0] == '\0') return NULL;
  size_t current_k = 0;

  if (step->type == EXEC_STEP_PARALLEL_FILTERS && step->chains) {
    for (size_t c = 0; c < step->chains_count; c++) {
      const parallel_filter_chain_t* chain = &step->chains[c];
      if (chain->channel != channel) continue;
      for (size_t f = 0; f < chain->filters_count; f++) {
        filter_t* flt = chain->filters[f];
        if (!flt) continue;
        const char* fname = filter_get_name(flt);
        if (fname && strcmp(fname, name) == 0) {
          if (current_k == target_k) {
            return flt;
          }
          current_k++;
        }
      }
    }
  } else if (step->type == EXEC_STEP_BIQUAD && step->biquad_step) {
    const biquad_step_t* bq = step->biquad_step;
    for (size_t c = 0; c < bq->channels_count; c++) {
      if (bq->channel_of[c] != channel) continue;
      for (size_t f = 0; f < bq->filters_count; f++) {
        filter_t* flt = bq->filters[c][f];
        if (!flt) continue;
        const char* fname = filter_get_name(flt);
        if (fname && strcmp(fname, name) == 0) {
          if (current_k == target_k) {
            return flt;
          }
          current_k++;
        }
      }
    }
  }

  return NULL;
}

static size_t step_filter_occurrence_index(const pipeline_exec_step_t* step,
                                           size_t channel,
                                           const filter_t* target_filter) {
  const char* name = filter_get_name(target_filter);
  if (!name || name[0] == '\0') return 0;
  size_t k = 0;

  if (step->type == EXEC_STEP_PARALLEL_FILTERS && step->chains) {
    for (size_t c = 0; c < step->chains_count; c++) {
      const parallel_filter_chain_t* chain = &step->chains[c];
      if (chain->channel != channel) continue;
      for (size_t f = 0; f < chain->filters_count; f++) {
        filter_t* flt = chain->filters[f];
        if (flt == target_filter) return k;
        if (!flt) continue;
        const char* fname = filter_get_name(flt);
        if (fname && strcmp(fname, name) == 0) {
          k++;
        }
      }
    }
  } else if (step->type == EXEC_STEP_BIQUAD && step->biquad_step) {
    const biquad_step_t* bq = step->biquad_step;
    for (size_t c = 0; c < bq->channels_count; c++) {
      if (bq->channel_of[c] != channel) continue;
      for (size_t f = 0; f < bq->filters_count; f++) {
        filter_t* flt = bq->filters[c][f];
        if (flt == target_filter) return k;
        if (!flt) continue;
        const char* fname = filter_get_name(flt);
        if (fname && strcmp(fname, name) == 0) {
          k++;
        }
      }
    }
  }

  return k;
}

static void transfer_all_filters_state(pipeline_t* dest,
                                       const pipeline_t* src) {
  if (!dest || !src || !dest->steps || !src->steps) return;

  // 1. Transfer matching filters and log new filters
  for (size_t s = 0; s < dest->steps_count; s++) {
    pipeline_exec_step_t* d_step = &dest->steps[s];
    const pipeline_exec_step_t* s_step =
        (s < src->steps_count) ? &src->steps[s] : NULL;

    if (d_step->type == EXEC_STEP_PARALLEL_FILTERS && d_step->chains) {
      for (size_t c = 0; c < d_step->chains_count; c++) {
        parallel_filter_chain_t* chain = &d_step->chains[c];
        for (size_t f = 0; f < chain->filters_count; f++) {
          filter_t* dest_f = chain->filters[f];
          if (!dest_f) continue;
          const char* name = filter_get_name(dest_f);
          if (!name || name[0] == '\0') continue;

          size_t k =
              step_filter_occurrence_index(d_step, chain->channel, dest_f);
          filter_t* src_f =
              s_step ? step_get_kth_filter(s_step, chain->channel, name, k)
                     : NULL;
          if (src_f) {
            filter_transfer_state(dest_f, src_f);
          } else {
            logger_debug(&g_logger,
                         "Filter '%s' (ch=%zu) is new, state initialized clean",
                         name, chain->channel);
          }
        }
      }
    } else if (d_step->type == EXEC_STEP_BIQUAD && d_step->biquad_step) {
      biquad_step_t* bq = d_step->biquad_step;
      for (size_t c = 0; c < bq->channels_count; c++) {
        size_t ch = bq->channel_of[c];
        for (size_t f = 0; f < bq->filters_count; f++) {
          filter_t* dest_f = bq->filters[c][f];
          if (!dest_f) continue;
          const char* name = filter_get_name(dest_f);
          if (!name || name[0] == '\0') continue;

          size_t k = step_filter_occurrence_index(d_step, ch, dest_f);
          filter_t* src_f =
              s_step ? step_get_kth_filter(s_step, ch, name, k) : NULL;
          if (src_f) {
            filter_transfer_state(dest_f, src_f);
          } else {
            logger_debug(&g_logger,
                         "Filter '%s' (ch=%zu) is new, state initialized clean",
                         name, ch);
          }
        }
      }
    }
  }

  // 2. Log retired filters
  for (size_t s = 0; s < src->steps_count; s++) {
    const pipeline_exec_step_t* s_step = &src->steps[s];
    const pipeline_exec_step_t* d_step =
        (s < dest->steps_count) ? &dest->steps[s] : NULL;

    if (s_step->type == EXEC_STEP_PARALLEL_FILTERS && s_step->chains) {
      for (size_t c = 0; c < s_step->chains_count; c++) {
        const parallel_filter_chain_t* chain = &s_step->chains[c];
        for (size_t f = 0; f < chain->filters_count; f++) {
          filter_t* src_f = chain->filters[f];
          if (!src_f) continue;
          const char* name = filter_get_name(src_f);
          if (!name || name[0] == '\0') continue;

          size_t k =
              step_filter_occurrence_index(s_step, chain->channel, src_f);
          filter_t* dest_f =
              d_step ? step_get_kth_filter(d_step, chain->channel, name, k)
                     : NULL;
          if (!dest_f) {
            logger_debug(&g_logger,
                         "Filter '%s' (ch=%zu) retired from pipeline", name,
                         chain->channel);
          }
        }
      }
    } else if (s_step->type == EXEC_STEP_BIQUAD && s_step->biquad_step) {
      const biquad_step_t* bq = s_step->biquad_step;
      for (size_t c = 0; c < bq->channels_count; c++) {
        size_t ch = bq->channel_of[c];
        for (size_t f = 0; f < bq->filters_count; f++) {
          filter_t* src_f = bq->filters[c][f];
          if (!src_f) continue;
          const char* name = filter_get_name(src_f);
          if (!name || name[0] == '\0') continue;

          size_t k = step_filter_occurrence_index(s_step, ch, src_f);
          filter_t* dest_f =
              d_step ? step_get_kth_filter(d_step, ch, name, k) : NULL;
          if (!dest_f) {
            logger_debug(&g_logger,
                         "Filter '%s' (ch=%zu) retired from pipeline", name,
                         ch);
          }
        }
      }
    }
  }
}

/// Transfer state for named audio processors.
static void transfer_named_processors_state(pipeline_t* dest,
                                            const pipeline_t* src) {
  if (!dest || !src || !dest->steps || !src->steps) return;

  for (size_t di = 0; di < dest->steps_count; di++) {
    pipeline_exec_step_t* d_step = &dest->steps[di];
    if (d_step->type != EXEC_STEP_PROCESSOR || !d_step->processor) {
      continue;
    }
    const char* dname = dsp_processor_get_name(d_step->processor);
    if (!dname || dname[0] == '\0') continue;

    bool matched = false;
    if (di < src->steps_count) {
      pipeline_exec_step_t* s_step = &src->steps[di];
      if (s_step->type == EXEC_STEP_PROCESSOR && s_step->processor &&
          s_step->processor->type == d_step->processor->type) {
        const char* sname = dsp_processor_get_name(s_step->processor);
        if (sname && strcmp(dname, sname) == 0) {
          dsp_processor_transfer_state(d_step->processor, s_step->processor);
          matched = true;
        }
      }
    }

    if (!matched) {
      logger_debug(&g_logger, "Processor '%s' is new, state initialized clean",
                   dname);
    }
  }

  // Log retired processors
  for (size_t si = 0; si < src->steps_count; si++) {
    pipeline_exec_step_t* s_step = &src->steps[si];
    if (s_step->type != EXEC_STEP_PROCESSOR || !s_step->processor) {
      continue;
    }
    const char* sname = dsp_processor_get_name(s_step->processor);
    if (!sname || sname[0] == '\0') continue;

    bool matched = false;
    if (si < dest->steps_count) {
      pipeline_exec_step_t* d_step = &dest->steps[si];
      if (d_step->type == EXEC_STEP_PROCESSOR && d_step->processor &&
          d_step->processor->type == s_step->processor->type) {
        const char* dname = dsp_processor_get_name(d_step->processor);
        if (dname && strcmp(dname, sname) == 0) {
          matched = true;
        }
      }
    }

    if (!matched) {
      logger_debug(&g_logger, "Processor '%s' retired from pipeline", sname);
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

  // 2. Transfer all channel filters (step-type agnostic, matched by step index,
  // channel and name, zero heap allocations)
  transfer_all_filters_state(dest, src);

  // 3. Transfer named multi-channel processors (matched by step index and name,
  // zero heap allocations)
  transfer_named_processors_state(dest, src);

  logger_info(&g_logger, "Completed pipeline state transfer");
}
