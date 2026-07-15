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

static const processor_vtable_t* processor_vtable_from_type(
    processor_type_t type) {
  switch (type) {
    case PROCESSOR_TYPE_COMPRESSOR:
      return &g_compressor_vtable;
    case PROCESSOR_TYPE_NOISE_GATE:
      return &g_noise_gate_vtable;
    case PROCESSOR_TYPE_RACE:
      return &g_race_vtable;
    default:
      return NULL;
  }
}

static processor_impl_type_t processor_impl_type_from_config(
    processor_type_t type) {
  switch (type) {
    case PROCESSOR_TYPE_COMPRESSOR:
      return PROCESSOR_IMPL_COMPRESSOR;
    case PROCESSOR_TYPE_NOISE_GATE:
      return PROCESSOR_IMPL_NOISE_GATE;
    case PROCESSOR_TYPE_RACE:
      return PROCESSOR_IMPL_RACE;
  }
  return PROCESSOR_IMPL_COMPRESSOR;
}

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

dsp_processor_t* dsp_processor_create(const char* name,
                                      const processor_config_t* config,
                                      int sample_rate, size_t chunk_size,
                                      config_error_t* err) {
  if (processor_config_validate(config, err) != 0) return NULL;
  const processor_vtable_t* vtable = processor_vtable_from_type(config->type);
  if (!vtable) {
    logger_error(&g_logger, "Unknown processor type %d for '%s'", config->type,
                 name ? name : "");
    config_error_set(err, CONFIG_ERR_PARSE, "Unknown processor type");
    return NULL;
  }

  void* impl = vtable->create(name, config, sample_rate, chunk_size, err);
  if (!impl) {
    logger_error(&g_logger, "Failed to instantiate processor '%s'",
                 name ? name : "");
    return NULL;
  }

  dsp_processor_t* proc = (dsp_processor_t*)calloc(1, sizeof(dsp_processor_t));
  if (!proc) {
    vtable->free(impl);
    return NULL;
  }

  proc->type = processor_impl_type_from_config(config->type);
  proc->impl = impl;
  proc->vtable = vtable;
  return proc;
}

int processor_config_validate(const processor_config_t* proc,
                              config_error_t* err) {
  if (!proc) return 0;
  const processor_vtable_t* vtable = processor_vtable_from_type(proc->type);
  if (vtable && vtable->validate) {
    return vtable->validate(proc, err);
  }
  return 0;
}
