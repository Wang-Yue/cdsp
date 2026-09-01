/**
 * @file biquad_processor.c
 * @brief Implementation of the internal multi-channel biquad processor.
 */

#include "Processors/biquad_processor.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "Audio/audio_chunk.h"
#include "Config/config_error.h"
#include "Filters/biquad.h"
#include "Filters/biquad_canon.h"
#include "Filters/biquad_internal.h"
#include "Filters/filter.h"
#include "Logging/app_logger.h"
#include "Processors/processor.h"

static const logger_t g_logger = {"dsp.processor.biquad"};

struct biquad_processor {
  char name[64];
  size_t channels_count;
  size_t* channels;
  biquad_filter_t*** filters;
  size_t* filters_counts;
  void** canon_filters;
  bool is_all_single_stage;
};

typedef struct biquad_processor biquad_processor_t;

static bool is_supported_biquad_filter(const filter_t* filter) {
  if (!filter) return false;
  return (filter->type == FILTER_INSTANCE_BIQUAD);
}

static int biquad_processor_validate(const processor_config_t* config,
                                     int sample_rate,
                                     config_error_t* err) {
  (void)sample_rate;
  if (!config) {
    if (err) config_error_set(err, CONFIG_ERR_PARSE, "Null config");
    return -1;
  }
  if (config->type != PROCESSOR_TYPE_BIQUAD) {
    if (err) config_error_set(err, CONFIG_ERR_INVALID_FILTER, "Invalid processor type");
    return -1;
  }
  const biquad_processor_config_t* cfg = &config->parameters.biquad;
  if (!cfg->channels || cfg->channels_count == 0 || !cfg->filters ||
      !cfg->filters_counts) {
    if (err) config_error_set(err, CONFIG_ERR_INVALID_FILTER, "Invalid arguments");
    return -1;
  }

  for (size_t c = 0; c < cfg->channels_count; c++) {
    for (size_t j = 0; j < cfg->filters_counts[c]; j++) {
      if (!is_supported_biquad_filter(cfg->filters[c][j])) {
        if (err) config_error_set(err, CONFIG_ERR_INVALID_FILTER, "Non-biquad filter in chains");
        return -1;
      }
    }
  }
  return 0;
}

static void* biquad_processor_create(const char* name,
                                     const processor_config_t* config,
                                     int sample_rate, size_t chunk_size,
                                     config_error_t* err) {
  (void)sample_rate;
  (void)chunk_size;
  if (biquad_processor_validate(config, sample_rate, err) != 0) {
    return NULL;
  }
  const biquad_processor_config_t* cfg = &config->parameters.biquad;

  biquad_processor_t* bp =
      (biquad_processor_t*)calloc(1, sizeof(biquad_processor_t));
  if (!bp) {
    if (err) config_error_set(err, CONFIG_ERR_PARSE, "Allocation failure");
    return NULL;
  }

  if (name && name[0] != '\0') {
    strncpy(bp->name, name, sizeof(bp->name) - 1);
  } else {
    strncpy(bp->name, "biquad_processor", sizeof(bp->name) - 1);
  }

  size_t channels_count = cfg->channels_count;
  bp->channels_count = channels_count;
  bp->channels = (size_t*)calloc(channels_count, sizeof(size_t));
  bp->filters = (biquad_filter_t***)calloc(channels_count,
                                           sizeof(biquad_filter_t**));
  bp->filters_counts = (size_t*)calloc(channels_count, sizeof(size_t));
  bp->canon_filters = (void**)calloc(channels_count, sizeof(void*));

  if (!bp->channels || !bp->filters || !bp->filters_counts ||
      !bp->canon_filters) {
    if (err) config_error_set(err, CONFIG_ERR_PARSE, "Allocation failure");
    g_biquad_processor_vtable.free(bp);
    return NULL;
  }

  for (size_t c = 0; c < channels_count; c++) {
    bp->channels[c] = cfg->channels[c];
    size_t in_count = cfg->filters_counts[c];
    filter_t* const* in_filters = cfg->filters[c];

    // Count biquad sections
    size_t total_sections = 0;
    for (size_t j = 0; j < in_count; j++) {
      filter_t* f = in_filters[j];
      if (f && f->type == FILTER_INSTANCE_BIQUAD) {
        total_sections++;
      }
    }

    bp->filters_counts[c] = total_sections;
    if (total_sections > 0) {
      bp->filters[c] =
          (biquad_filter_t**)calloc(total_sections, sizeof(biquad_filter_t*));
      if (!bp->filters[c]) {
        if (err) config_error_set(err, CONFIG_ERR_PARSE, "Allocation failure");
        goto fail_cleanup;
      }

      size_t bq_idx = 0;
      for (size_t j = 0; j < in_count; j++) {
        filter_t* f = in_filters[j];
        if (f && f->type == FILTER_INSTANCE_BIQUAD) {
          bp->filters[c][bq_idx++] = (biquad_filter_t*)f->instance;
        }
      }

      // Create fake canon filter for cascade processing on this channel
      filter_config_t canon_cfg = {
          .type = FILTER_TYPE_BIQUAD_CANON,
          .parameters.biquad_canon = {
              .sections = bp->filters[c],
              .num_sections = bp->filters_counts[c],
              .owns_sections = false,
          },
      };
      bp->canon_filters[c] = g_biquad_canon_vtable.create(
          bp->name, &canon_cfg, 0, 0, NULL, err);
      if (!bp->canon_filters[c]) {
        goto fail_cleanup;
      }
    }
  }

  // Check if all channels have exactly 1 biquad stage
  bp->is_all_single_stage = (bp->channels_count > 1);
  for (size_t c = 0; c < bp->channels_count; c++) {
    if (bp->filters_counts[c] != 1) {
      bp->is_all_single_stage = false;
      break;
    }
  }

  logger_debug(&g_logger,
               "Created biquad processor '%s' (channels=%zu, single_stage=%d)",
               bp->name, bp->channels_count, bp->is_all_single_stage);
  return bp;

fail_cleanup:
  g_biquad_processor_vtable.free(bp);
  return NULL;
}

#define DEFINE_INTERLEAVED_KERNEL(N) \
static void biquad_interleaved_kernel_##N(biquad_filter_t** filters, double** waveforms, size_t count) { \
  double b0[N], b1[N], b2[N], neg_a1[N], neg_a2[N], z1[N], z2[N]; \
  for (size_t c = 0; c < N; c++) { \
    biquad_filter_t* f = filters[c]; \
    b0[c] = f->coeffs.b0; \
    b1[c] = f->coeffs.b1; \
    b2[c] = f->coeffs.b2; \
    neg_a1[c] = f->neg_a1; \
    neg_a2[c] = f->neg_a2; \
    z1[c] = f->z1; \
    z2[c] = f->z2; \
  } \
  for (size_t i = 0; i < count; i++) { \
    for (size_t c = 0; c < N; c++) { \
      double in = waveforms[c][i]; \
      double out = b0[c] * in + z1[c]; \
      double tmp = b1[c] * in + z2[c]; \
      z1[c] = neg_a1[c] * out + tmp; \
      z2[c] = b2[c] * in + neg_a2[c] * out; \
      waveforms[c][i] = out; \
    } \
  } \
  for (size_t c = 0; c < N; c++) { \
    if (fpclassify(z1[c]) == FP_SUBNORMAL) z1[c] = 0.0; \
    if (fpclassify(z2[c]) == FP_SUBNORMAL) z2[c] = 0.0; \
    filters[c]->z1 = z1[c]; \
    filters[c]->z2 = z2[c]; \
  } \
}

DEFINE_INTERLEAVED_KERNEL(1)
DEFINE_INTERLEAVED_KERNEL(2)
DEFINE_INTERLEAVED_KERNEL(3)
DEFINE_INTERLEAVED_KERNEL(4)

static void biquad_interleaved_process(biquad_filter_t** filters, double** waveforms,
                                        size_t num_channels, size_t count) {
  if (!filters || !waveforms || num_channels == 0 || count == 0) return;
  size_t start = 0;
  while (start < num_channels) {
    size_t take = num_channels - start;
    if (take > 4) take = 4;
    switch (take) {
      case 4: biquad_interleaved_kernel_4(filters + start, waveforms + start, count); break;
      case 3: biquad_interleaved_kernel_3(filters + start, waveforms + start, count); break;
      case 2: biquad_interleaved_kernel_2(filters + start, waveforms + start, count); break;
      case 1: biquad_interleaved_kernel_1(filters + start, waveforms + start, count); break;
    }
    start += take;
  }
}

static void biquad_processor_process(void* impl, audio_chunk_t* chunk) {
  biquad_processor_t* bp = (biquad_processor_t*)impl;
  if (!bp || !chunk) return;
  size_t valid_frames = audio_chunk_get_valid_frames(chunk);
  if (valid_frames == 0) return;

  size_t chunk_channels = audio_chunk_get_channels(chunk);

  if (bp->is_all_single_stage && bp->channels_count > 1) {
    biquad_filter_t* bqs[4];
    double* wfs[4];
    size_t valid_chans = 0;

    for (size_t c = 0; c < bp->channels_count; c++) {
      size_t ch = bp->channels[c];
      if (ch < chunk_channels) {
        double* w = audio_chunk_get_channel(chunk, ch);
        if (w && bp->filters_counts[c] > 0 && bp->filters[c][0]) {
          bqs[valid_chans] = bp->filters[c][0];
          wfs[valid_chans] = w;
          valid_chans++;
          if (valid_chans == 4) {
            biquad_interleaved_process(bqs, wfs, 4, valid_frames);
            valid_chans = 0;
          }
        }
      }
    }
    if (valid_chans > 0) {
      biquad_interleaved_process(bqs, wfs, valid_chans, valid_frames);
    }
  } else {
    for (size_t c = 0; c < bp->channels_count; c++) {
      size_t ch = bp->channels[c];
      if (ch < chunk_channels && bp->canon_filters[c]) {
        double* w = audio_chunk_get_channel(chunk, ch);
        if (w) {
          g_biquad_canon_vtable.process(bp->canon_filters[c], w, valid_frames);
        }
      }
    }
  }
}

static const char* biquad_processor_get_name(const void* impl) {
  const biquad_processor_t* bp = (const biquad_processor_t*)impl;
  return bp ? bp->name : "";
}

static void biquad_processor_free(void* impl) {
  biquad_processor_t* bp = (biquad_processor_t*)impl;
  if (!bp) return;

  if (bp->canon_filters) {
    for (size_t c = 0; c < bp->channels_count; c++) {
      if (bp->canon_filters[c]) {
        g_biquad_canon_vtable.free(bp->canon_filters[c]);
      }
    }
    free(bp->canon_filters);
  }

  if (bp->filters) {
    for (size_t c = 0; c < bp->channels_count; c++) {
      if (bp->filters[c]) {
        free(bp->filters[c]);
      }
    }
    free(bp->filters);
  }
  if (bp->filters_counts) {
    free(bp->filters_counts);
  }

  if (bp->channels) {
    free(bp->channels);
  }

  free(bp);
}

const processor_vtable_t g_biquad_processor_vtable = {
    .validate = biquad_processor_validate,
    .create = biquad_processor_create,
    .process = biquad_processor_process,
    .get_name = biquad_processor_get_name,
    .transfer_state = NULL,
    .free = biquad_processor_free,
};
