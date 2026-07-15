// Resampler protocol + shared types.
// Four resampler implementations conform to AudioResampler:
//   * SynchronousResampler — FFT-based fixed-ratio.
//   * AsyncSincResampler   — Asynchronous windowed-sinc resampler.
//   * AsyncPolyResampler   — Asynchronous polynomial resampler.
//   * AppleResampler       — Core Audio AudioConverter wrapper.

#include "audio_resampler.h"

#include "Logging/app_logger.h"

static const logger_t g_logger = {"dsp.resampler"};



#include <stdlib.h>
#include <string.h>

sinc_interpolation_type_t sinc_interpolation_type_from_string(const char* str) {
  if (!str) return SINC_INTERPOLATION_QUADRATIC;
  if (strcmp(str, "Nearest") == 0) return SINC_INTERPOLATION_NEAREST;
  if (strcmp(str, "Linear") == 0) return SINC_INTERPOLATION_LINEAR;
  if (strcmp(str, "Quadratic") == 0) return SINC_INTERPOLATION_QUADRATIC;
  if (strcmp(str, "Cubic") == 0) return SINC_INTERPOLATION_CUBIC;
  return SINC_INTERPOLATION_QUADRATIC;
}

poly_interpolation_t poly_interpolation_from_string(const char* str) {
  if (!str) return POLY_INTERPOLATION_CUBIC;
  if (strcmp(str, "Linear") == 0) return POLY_INTERPOLATION_LINEAR;
  if (strcmp(str, "Cubic") == 0) return POLY_INTERPOLATION_CUBIC;
  if (strcmp(str, "Quintic") == 0) return POLY_INTERPOLATION_QUINTIC;
  return POLY_INTERPOLATION_CUBIC;
}

audio_resampler_t* audio_resampler_create_from_config(
    const resampler_config_t* config, size_t input_rate, size_t output_rate,
    size_t channels, size_t chunk_size, config_error_t* err) {
  if (!config) {
    logger_error(&g_logger, "Resampler config is NULL");
    config_error_set(err, CONFIG_ERR_PARSE, "Resampler config is NULL");
    return NULL;
  }
  logger_debug(&g_logger,
               "Creating resampler type %d (%zuHz -> %zuHz, %zu channels)",
               config->type, input_rate, output_rate, channels);
  switch (config->type) {
    case RESAMPLER_TYPE_SYNCHRONOUS: {
      synchronous_resampler_t* res = synchronous_resampler_create(
          channels, input_rate, output_rate, chunk_size, err);
      if (!res) {
        logger_error(
            &g_logger,
            "Failed to create synchronous resampler (%zuHz->%zuHz, %zuch): %s",
            input_rate, output_rate, channels,
            err ? err->message : "unknown error");
        return NULL;
      }
      audio_resampler_t* wrap = audio_resampler_wrap_synchronous(res);
      if (!wrap) {
        logger_error(&g_logger, "Failed to wrap synchronous resampler");
        config_error_set(err, CONFIG_ERR_PARSE,
                         "Failed to wrap synchronous resampler");
      }
      return wrap;
    }
    case RESAMPLER_TYPE_ASYNC_SINC: {
      fixed_async_t fixed_mode =
          config->has_fixed ? config->fixed : FIXED_ASYNC_OUTPUT;
      if (config->has_sinc_len && config->has_oversampling_factor &&
          config->has_window && config->has_interpolation) {
        window_function_t wf = window_function_from_string(
            config->window, WINDOW_FUNCTION_BLACKMAN_HARRIS2);
        sinc_interpolation_type_t interp =
            sinc_interpolation_type_from_string(config->interpolation);
        async_sinc_resampler_t* res = async_sinc_resampler_create(
            channels, input_rate, output_rate, (size_t)config->sinc_len,
            (size_t)config->oversampling_factor, interp, wf, config->f_cutoff,
            config->has_f_cutoff, chunk_size, 1.1, fixed_mode, err);
        if (!res) {
          logger_error(
              &g_logger,
              "Failed to create async sinc resampler (%zuHz->%zuHz, %zuch): %s",
              input_rate, output_rate, channels,
              err ? err->message : "unknown error");
          return NULL;
        }
        audio_resampler_t* wrap = audio_resampler_wrap_async_sinc(res);
        if (!wrap) {
          logger_error(&g_logger, "Failed to wrap async sinc resampler");
          config_error_set(err, CONFIG_ERR_PARSE,
                           "Failed to wrap async sinc resampler");
        }
        return wrap;
      } else {
        resampler_profile_t prof = RESAMPLER_PROFILE_BALANCED;
        if (config->has_profile) {
          prof = resampler_profile_from_string(config->profile);
        }
        async_sinc_resampler_t* res = async_sinc_resampler_create_from_profile(
            channels, input_rate, output_rate, prof, chunk_size, 1.1, err);
        if (!res) {
          logger_error(&g_logger,
                       "Failed to create async sinc resampler from profile "
                       "(%zuHz->%zuHz, %zuch): %s",
                       input_rate, output_rate, channels,
                       err ? err->message : "unknown error");
          return NULL;
        }
        audio_resampler_t* wrap = audio_resampler_wrap_async_sinc(res);
        if (!wrap) {
          logger_error(&g_logger, "Failed to wrap async sinc resampler");
          config_error_set(err, CONFIG_ERR_PARSE,
                           "Failed to wrap async sinc resampler");
        }
        return wrap;
      }
    }
    case RESAMPLER_TYPE_ASYNC_POLY: {
      fixed_async_t fixed_mode =
          config->has_fixed ? config->fixed : FIXED_ASYNC_OUTPUT;
      poly_interpolation_t interp = POLY_INTERPOLATION_CUBIC;
      if (config->has_interpolation) {
        interp = poly_interpolation_from_string(config->interpolation);
      }
      async_poly_resampler_t* res =
          async_poly_resampler_create(channels, input_rate, output_rate, interp,
                                      chunk_size, 1.1, fixed_mode, err);
      if (!res) {
        logger_error(
            &g_logger,
            "Failed to create async poly resampler (%zuHz->%zuHz, %zuch): %s",
            input_rate, output_rate, channels,
            err ? err->message : "unknown error");
        return NULL;
      }
      audio_resampler_t* wrap = audio_resampler_wrap_async_poly(res);
      if (!wrap) {
        logger_error(&g_logger, "Failed to wrap async poly resampler");
        config_error_set(err, CONFIG_ERR_PARSE,
                         "Failed to wrap async poly resampler");
      }
      return wrap;
    }
#if defined(ENABLE_COREAUDIO)
    case RESAMPLER_TYPE_APPLE: {
      apple_resampler_quality_t qual = config->has_apple_quality
                                           ? config->apple_quality
                                           : APPLE_RESAMPLER_QUALITY_MAX;
      apple_resampler_complexity_t comp =
          config->has_apple_complexity ? config->apple_complexity
                                       : APPLE_RESAMPLER_COMPLEXITY_NORMAL;
      apple_resampler_t* res = apple_resampler_create(
          channels, input_rate, output_rate, qual, comp, chunk_size, err);
      if (!res) {
        logger_error(
            &g_logger,
            "Failed to create Apple resampler (%zuHz->%zuHz, %zuch): %s",
            input_rate, output_rate, channels,
            err ? err->message : "unknown error");
        return NULL;
      }
      audio_resampler_t* wrap = audio_resampler_wrap_apple(res);
      if (!wrap) {
        logger_error(&g_logger, "Failed to wrap Apple resampler");
        config_error_set(err, CONFIG_ERR_PARSE,
                         "Failed to wrap Apple resampler");
      }
      return wrap;
    }
#endif
    default:
      logger_error(&g_logger, "Unknown resampler type %d", config->type);
      config_error_set(err, CONFIG_ERR_PARSE, "Unknown resampler type %d",
                       config->type);
      return NULL;
  }
}

resampler_error_t audio_resampler_process(audio_resampler_t* resampler,
                                          const audio_chunk_t* input,
                                          audio_chunk_t* output) {
  if (!resampler || !resampler->process) return RESAMPLER_ERR_INVALID_PARAMETER;
  return resampler->process(resampler->impl, input, output);
}

void audio_resampler_set_relative_ratio(audio_resampler_t* resampler,
                                        double multiplier) {
  if (resampler && resampler->set_relative_ratio) {
    resampler->set_relative_ratio(resampler->impl, multiplier);
  }
}

double audio_resampler_get_ratio(const audio_resampler_t* resampler) {
  return (resampler && resampler->get_ratio)
             ? resampler->get_ratio(resampler->impl)
             : 1.0;
}

size_t audio_resampler_get_max_output_frames(
    const audio_resampler_t* resampler) {
  return (resampler && resampler->get_max_output_frames)
             ? resampler->get_max_output_frames(resampler->impl)
             : 0;
}

size_t audio_resampler_get_chunk_size(const audio_resampler_t* resampler) {
  return (resampler && resampler->get_chunk_size)
             ? resampler->get_chunk_size(resampler->impl)
             : 0;
}

size_t audio_resampler_get_input_frames_next(
    const audio_resampler_t* resampler) {
  return (resampler && resampler->get_input_frames_next)
             ? resampler->get_input_frames_next(resampler->impl)
             : 0;
}

size_t audio_resampler_get_output_frames_next(
    const audio_resampler_t* resampler) {
  return (resampler && resampler->get_output_frames_next)
             ? resampler->get_output_frames_next(resampler->impl)
             : 0;
}

size_t audio_resampler_get_channels(const audio_resampler_t* resampler) {
  return (resampler && resampler->get_channels)
             ? resampler->get_channels(resampler->impl)
             : 0;
}

void audio_resampler_free(audio_resampler_t* resampler) {
  if (resampler) {
    if (resampler->free) {
      resampler->free(resampler->impl);
    }
    free(resampler);
  }
}
