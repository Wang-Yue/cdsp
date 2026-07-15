/**
 * @file processor.c
 * @brief Implementation of polymorphic processor wrapper and factory dispatch.
 *
 * This implementation provides concrete dispatch tables for each supported
 * audio processor type, routing real-time processing and parameter updates to
 * the appropriate underlying processor (compressor, noise gate, or RACE
 * cross-talk cancellation).
 */

#include "processor.h"

#include <stdlib.h>
#include <string.h>

#include "Logging/app_logger.h"

static const logger_t g_logger = {"dsp.processor"};

typedef struct processor_vtable {
  void (*process)(void* impl, audio_chunk_t* chunk);
  const char* (*get_name)(const void* impl);
  void (*transfer_state)(void* dest, const void* src);
  void (*free)(void* impl);
} processor_vtable_t;

struct dsp_processor {
  processor_impl_type_t type;  ///< Concrete implementation type identifier.
  void* impl;                  ///< Pointer to underlying processor structure.
  const processor_vtable_t* vtable;  ///< Virtual table for dispatch.
};

/* --- Static VTable Definitions --- */

static const processor_vtable_t g_comp_vtable = {
    .process = (void (*)(void*, audio_chunk_t*))compressor_processor_process,
    .get_name = (const char* (*)(const void*))compressor_processor_get_name,
    .transfer_state =
        (void (*)(void*, const void*))compressor_processor_transfer_state,
    .free = (void (*)(void*))compressor_processor_free};

static const processor_vtable_t g_gate_vtable = {
    .process = (void (*)(void*, audio_chunk_t*))noise_gate_processor_process,
    .get_name = (const char* (*)(const void*))noise_gate_processor_get_name,
    .transfer_state =
        (void (*)(void*, const void*))noise_gate_processor_transfer_state,
    .free = (void (*)(void*))noise_gate_processor_free};

static const processor_vtable_t g_race_vtable = {
    .process = (void (*)(void*, audio_chunk_t*))race_processor_process,
    .get_name = (const char* (*)(const void*))race_processor_get_name,
    .transfer_state =
        (void (*)(void*, const void*))race_processor_transfer_state,
    .free = (void (*)(void*))race_processor_free};

void dsp_processor_process(dsp_processor_t* proc, audio_chunk_t* chunk) {
  if (proc && proc->impl && proc->vtable && proc->vtable->process) {
    proc->vtable->process(proc->impl, chunk);
  }
}

void dsp_processor_transfer_state(dsp_processor_t* dest,
                                  const dsp_processor_t* src) {
  if (!dest || !src || dest->type != src->type || !dest->impl || !src->impl ||
      !dest->vtable || !dest->vtable->transfer_state)
    return;
  dest->vtable->transfer_state(dest->impl, src->impl);
  logger_info(&g_logger, "Transferred processor state for '%s'",
              dsp_processor_get_name(dest));
}

const char* dsp_processor_get_name(const dsp_processor_t* proc) {
  return (proc && proc->impl && proc->vtable && proc->vtable->get_name)
             ? proc->vtable->get_name(proc->impl)
             : "";
}

void dsp_processor_free(dsp_processor_t* proc) {
  if (!proc) return;
  if (proc->impl && proc->vtable && proc->vtable->free) {
    proc->vtable->free(proc->impl);
  }
  free(proc);
}

dsp_processor_t* dsp_processor_wrap_compressor(compressor_processor_t* p) {
  if (!p) return NULL;
  dsp_processor_t* wrap = (dsp_processor_t*)calloc(1, sizeof(dsp_processor_t));
  if (!wrap) {
    compressor_processor_free(p);
    return NULL;
  }
  wrap->type = PROCESSOR_IMPL_COMPRESSOR;
  wrap->impl = p;
  wrap->vtable = &g_comp_vtable;
  return wrap;
}

dsp_processor_t* dsp_processor_wrap_noise_gate(noise_gate_processor_t* p) {
  if (!p) return NULL;
  dsp_processor_t* wrap = (dsp_processor_t*)calloc(1, sizeof(dsp_processor_t));
  if (!wrap) {
    noise_gate_processor_free(p);
    return NULL;
  }
  wrap->type = PROCESSOR_IMPL_NOISE_GATE;
  wrap->impl = p;
  wrap->vtable = &g_gate_vtable;
  return wrap;
}

dsp_processor_t* dsp_processor_wrap_race(race_processor_t* p) {
  if (!p) return NULL;
  dsp_processor_t* wrap = (dsp_processor_t*)calloc(1, sizeof(dsp_processor_t));
  if (!wrap) {
    race_processor_free(p);
    return NULL;
  }
  wrap->type = PROCESSOR_IMPL_RACE;
  wrap->impl = p;
  wrap->vtable = &g_race_vtable;
  return wrap;
}

dsp_processor_t* dsp_processor_create(const char* name,
                                      const processor_config_t* config,
                                      int sample_rate, size_t chunk_size,
                                      config_error_t* err) {
  if (processor_config_validate(config, err) != 0) return NULL;
  switch (config->type) {
    case PROCESSOR_TYPE_COMPRESSOR: {
      compressor_processor_t* p = compressor_processor_create(
          name, &config->parameters.compressor, sample_rate, chunk_size, err);
      if (!p) {
        logger_error(
            &g_logger,
            "Failed to create compressor processor '%s' (rate=%d, chunk=%zu)",
            name ? name : "", sample_rate, chunk_size);
        return NULL;
      }
      dsp_processor_t* wrap = dsp_processor_wrap_compressor(p);
      if (!wrap) {
        logger_error(&g_logger, "Failed to wrap compressor processor '%s'",
                     name ? name : "");
        config_error_set(err, CONFIG_ERR_PARSE,
                         "Failed to wrap compressor processor '%s'",
                         name ? name : "");
        return NULL;
      }
      logger_debug(&g_logger, "Created compressor processor '%s'",
                   name ? name : "");
      return wrap;
    }
    case PROCESSOR_TYPE_NOISE_GATE: {
      noise_gate_processor_t* p = noise_gate_processor_create(
          name, &config->parameters.noise_gate, sample_rate, chunk_size, err);
      if (!p) {
        logger_error(
            &g_logger,
            "Failed to create noise gate processor '%s' (rate=%d, chunk=%zu)",
            name ? name : "", sample_rate, chunk_size);
        return NULL;
      }
      dsp_processor_t* wrap = dsp_processor_wrap_noise_gate(p);
      if (!wrap) {
        logger_error(&g_logger, "Failed to wrap noise gate processor '%s'",
                     name ? name : "");
        config_error_set(err, CONFIG_ERR_PARSE,
                         "Failed to wrap noise gate processor '%s'",
                         name ? name : "");
        return NULL;
      }
      logger_debug(&g_logger, "Created noise gate processor '%s'",
                   name ? name : "");
      return wrap;
    }
    case PROCESSOR_TYPE_RACE: {
      race_processor_t* p = race_processor_create(
          name, &config->parameters.race, sample_rate, err);
      if (!p) {
        logger_error(&g_logger,
                     "Failed to create RACE processor '%s' (rate=%d): %s",
                     name ? name : "", sample_rate,
                     err ? err->message : "unknown error");
        return NULL;
      }
      dsp_processor_t* wrap = dsp_processor_wrap_race(p);
      if (!wrap) {
        logger_error(&g_logger, "Failed to wrap RACE processor '%s'",
                     name ? name : "");
        config_error_set(err, CONFIG_ERR_PARSE,
                         "Failed to wrap RACE processor '%s'",
                         name ? name : "");
        return NULL;
      }
      logger_debug(&g_logger, "Created RACE processor '%s'", name ? name : "");
      return wrap;
    }
    default:
      logger_error(&g_logger, "Unknown processor type %d for '%s'",
                   config->type, name ? name : "");
      config_error_set(err, CONFIG_ERR_PARSE,
                       "Unknown processor type %d for '%s'", config->type,
                       name ? name : "");
      return NULL;
  }
}

int processor_config_validate(const processor_config_t* proc,
                              config_error_t* err) {
  if (!proc) return 0;
  switch (proc->type) {
    case PROCESSOR_TYPE_COMPRESSOR:
      return compressor_config_validate(&proc->parameters.compressor, err);
    case PROCESSOR_TYPE_NOISE_GATE:
      return noise_gate_config_validate(&proc->parameters.noise_gate, err);
    case PROCESSOR_TYPE_RACE:
      return race_config_validate(&proc->parameters.race, err);
  }
  return 0;
}
