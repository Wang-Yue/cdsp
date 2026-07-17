#include "resampler_config_types.h"

#include <stdio.h>
#include <string.h>

// Standalone resampler configuration types.

const char* resampler_type_to_string(resampler_type_t type) {
  switch (type) {
    case RESAMPLER_TYPE_SYNCHRONOUS:
      return "Synchronous";
    case RESAMPLER_TYPE_ASYNC_SINC:
      return "AsyncSinc";
    case RESAMPLER_TYPE_ASYNC_POLY:
      return "AsyncPoly";
    default:
      return "Synchronous";
  }
}

resampler_type_t resampler_type_from_string(const char* str) {
  if (!str) return RESAMPLER_TYPE_SYNCHRONOUS;
  if (strcmp(str, "Synchronous") == 0) return RESAMPLER_TYPE_SYNCHRONOUS;
  if (strcmp(str, "AsyncSinc") == 0) return RESAMPLER_TYPE_ASYNC_SINC;
  if (strcmp(str, "AsyncPoly") == 0) return RESAMPLER_TYPE_ASYNC_POLY;
  return RESAMPLER_TYPE_SYNCHRONOUS;
}

const char* resampler_profile_to_string(resampler_profile_t profile) {
  switch (profile) {
    case RESAMPLER_PROFILE_VERY_FAST:
      return "VeryFast";
    case RESAMPLER_PROFILE_FAST:
      return "Fast";
    case RESAMPLER_PROFILE_BALANCED:
      return "Balanced";
    case RESAMPLER_PROFILE_ACCURATE:
      return "Accurate";
    default:
      return "Balanced";
  }
}

#include <strings.h>

resampler_profile_t resampler_profile_from_string(const char* str) {
  if (!str) return RESAMPLER_PROFILE_BALANCED;
  if (strcasecmp(str, "VeryFast") == 0) return RESAMPLER_PROFILE_VERY_FAST;
  if (strcasecmp(str, "Fast") == 0) return RESAMPLER_PROFILE_FAST;
  if (strcasecmp(str, "Balanced") == 0) return RESAMPLER_PROFILE_BALANCED;
  if (strcasecmp(str, "Accurate") == 0) return RESAMPLER_PROFILE_ACCURATE;
  return RESAMPLER_PROFILE_BALANCED;
}

void resampler_config_init(resampler_config_t* config, resampler_type_t type) {
  if (!config) return;
  memset(config, 0, sizeof(resampler_config_t));
  config->type = type;
}

void resampler_config_description(const resampler_config_t* config,
                                  char* out_buf, size_t buf_len) {
  if (!config || !out_buf || buf_len == 0) return;
  const char* prof = config->has_profile ? config->profile : "nil";
  const char* interp =
      config->has_interpolation ? config->interpolation : "nil";
  int sinc = config->has_sinc_len ? config->sinc_len : 0;
  snprintf(
      out_buf, buf_len,
      "ResamplerConfig(type: %s, profile: %s, interpolation: %s, sincLen: %d)",
      resampler_type_to_string(config->type), prof, interp, sinc);
}
