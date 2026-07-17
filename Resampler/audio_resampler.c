// Resampler protocol + shared types.
// Four resampler implementations conform to AudioResampler:
//   * SynchronousResampler — FFT-based fixed-ratio.
//   * AsyncSincResampler   — Asynchronous windowed-sinc resampler.
//   * AsyncPolyResampler   — Asynchronous polynomial resampler.
//   * AppleResampler       — Core Audio AudioConverter wrapper.

#include "audio_resampler.h"

#include "async_poly_resampler.h"
#include "async_sinc_resampler.h"
#include "synchronous_resampler.h"
#if defined(ENABLE_COREAUDIO)
#include "apple_resampler.h"
#endif

#include "Logging/app_logger.h"

static const logger_t g_logger = {"dsp.resampler"};

#include <stdlib.h>
#include <string.h>

static const resampler_vtable_t* resampler_vtable_from_type(
    resampler_type_t type) {
  switch (type) {
    case RESAMPLER_TYPE_SYNCHRONOUS:
      return &g_synchronous_resampler_vtable;
    case RESAMPLER_TYPE_ASYNC_SINC:
      return &g_async_sinc_resampler_vtable;
    case RESAMPLER_TYPE_ASYNC_POLY:
      return &g_async_poly_resampler_vtable;
#if defined(ENABLE_COREAUDIO)
    case RESAMPLER_TYPE_APPLE:
      return &g_apple_resampler_vtable;
#endif
    default:
      return NULL;
  }
}

static resampler_impl_type_t resampler_impl_type_from_config(
    resampler_type_t type) {
  switch (type) {
    case RESAMPLER_TYPE_SYNCHRONOUS:
      return RESAMPLER_IMPL_SYNCHRONOUS;
    case RESAMPLER_TYPE_ASYNC_SINC:
      return RESAMPLER_IMPL_ASYNC_SINC;
    case RESAMPLER_TYPE_ASYNC_POLY:
      return RESAMPLER_IMPL_ASYNC_POLY;
#if defined(ENABLE_COREAUDIO)
    case RESAMPLER_TYPE_APPLE:
      return RESAMPLER_IMPL_APPLE;
#endif
  }
  return RESAMPLER_IMPL_SYNCHRONOUS;
}

resampler_t* resampler_create_from_config(const resampler_config_t* config,
                                          size_t input_rate, size_t output_rate,
                                          size_t channels, size_t chunk_size,
                                          config_error_t* err) {
  if (resampler_config_validate(config, err) != 0) return NULL;
  char desc_buf[256];
  resampler_config_description(config, desc_buf, sizeof(desc_buf));
  logger_debug(&g_logger,
               "Creating resampler %s (%zuHz -> %zuHz, %zu channels)", desc_buf,
               input_rate, output_rate, channels);
  const resampler_vtable_t* vtable = resampler_vtable_from_type(config->type);
  if (!vtable) {
    logger_error(&g_logger, "Unknown resampler type %s",
                 resampler_type_to_string(config->type));
    config_error_set(err, CONFIG_ERR_PARSE, "Unknown resampler type %s",
                     resampler_type_to_string(config->type));
    return NULL;
  }

  void* impl = vtable->create(config, input_rate, output_rate, channels,
                              chunk_size, err);
  if (!impl) {
    logger_error(&g_logger, "Failed to instantiate resampler type %s",
                 resampler_type_to_string(config->type));
    return NULL;
  }

  resampler_t* res = (resampler_t*)calloc(1, sizeof(resampler_t));
  if (!res) {
    vtable->free(impl);
    return NULL;
  }
  res->type = resampler_impl_type_from_config(config->type);
  res->impl = impl;
  res->vtable = vtable;
  return res;
}

resampler_error_t resampler_process(resampler_t* resampler,
                                    const audio_chunk_t* input,
                                    audio_chunk_t* output) {
  if (!resampler || !resampler->vtable || !resampler->vtable->process)
    return RESAMPLER_ERR_INVALID_PARAMETER;
  return resampler->vtable->process(resampler->impl, input, output);
}

void resampler_set_relative_ratio(resampler_t* resampler, double multiplier) {
  if (resampler && resampler->vtable && resampler->vtable->set_relative_ratio) {
    resampler->vtable->set_relative_ratio(resampler->impl, multiplier);
  }
}

double resampler_get_ratio(const resampler_t* resampler) {
  return (resampler && resampler->vtable && resampler->vtable->get_ratio)
             ? resampler->vtable->get_ratio(resampler->impl)
             : 1.0;
}

size_t resampler_get_max_output_frames(const resampler_t* resampler) {
  return (resampler && resampler->vtable &&
          resampler->vtable->get_max_output_frames)
             ? resampler->vtable->get_max_output_frames(resampler->impl)
             : 0;
}

size_t resampler_get_chunk_size(const resampler_t* resampler) {
  return (resampler && resampler->vtable && resampler->vtable->get_chunk_size)
             ? resampler->vtable->get_chunk_size(resampler->impl)
             : 0;
}

size_t resampler_get_input_frames_next(const resampler_t* resampler) {
  return (resampler && resampler->vtable &&
          resampler->vtable->get_input_frames_next)
             ? resampler->vtable->get_input_frames_next(resampler->impl)
             : 0;
}

size_t resampler_get_output_frames_next(const resampler_t* resampler) {
  return (resampler && resampler->vtable &&
          resampler->vtable->get_output_frames_next)
             ? resampler->vtable->get_output_frames_next(resampler->impl)
             : 0;
}

size_t resampler_get_channels(const resampler_t* resampler) {
  return (resampler && resampler->vtable && resampler->vtable->get_channels)
             ? resampler->vtable->get_channels(resampler->impl)
             : 0;
}

void resampler_free(resampler_t* resampler) {
  if (resampler) {
    if (resampler->vtable && resampler->vtable->free) {
      resampler->vtable->free(resampler->impl);
    }
    free(resampler);
  }
}

int resampler_config_validate(const resampler_config_t* config,
                              config_error_t* err) {
  if (!config) return 0;

  if (config->has_sinc_len && config->sinc_len <= 0) {
    config_error_set(err, CONFIG_ERR_INVALID_RESAMPLER,
                     "sinc_len must be positive, got %d", config->sinc_len);
    return -1;
  }
  if (config->has_oversampling_factor && config->oversampling_factor <= 0) {
    config_error_set(err, CONFIG_ERR_INVALID_RESAMPLER,
                     "oversampling_factor must be positive, got %d",
                     config->oversampling_factor);
    return -1;
  }
  if (config->has_f_cutoff &&
      (config->f_cutoff <= 0.0 || config->f_cutoff > 1.0)) {
    config_error_set(err, CONFIG_ERR_INVALID_RESAMPLER,
                     "f_cutoff must be in (0.0, 1.0], got %g",
                     config->f_cutoff);
    return -1;
  }

  const resampler_vtable_t* vtable = resampler_vtable_from_type(config->type);
  if (vtable && vtable->validate) {
    return vtable->validate(config, err);
  }
  return 0;
}
