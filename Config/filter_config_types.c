#include "filter_config_types.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "Filters/biquad.h"
#include "Filters/biquad_combo.h"
#include "Filters/convolution.h"
#include "Filters/delay.h"
#include "Filters/diffeq.h"
#include "Filters/dither.h"
#include "Filters/gain.h"
#include "Filters/limiter.h"
#include "Filters/lookahead_limiter.h"
#include "Filters/loudness.h"
#include "Filters/volume.h"

// Standalone filter configuration types.

const char* fader_to_string(fader_t fader) {
  switch (fader) {
    case FADER_MAIN:
      return "Main";
    case FADER_AUX1:
      return "Aux1";
    case FADER_AUX2:
      return "Aux2";
    case FADER_AUX3:
      return "Aux3";
    case FADER_AUX4:
      return "Aux4";
    default:
      return "Main";
  }
}

fader_t fader_from_string(const char* str) {
  if (!str) return FADER_NONE;
  if (strcasecmp(str, "main") == 0) return FADER_MAIN;
  if (strcasecmp(str, "aux1") == 0) return FADER_AUX1;
  if (strcasecmp(str, "aux2") == 0) return FADER_AUX2;
  if (strcasecmp(str, "aux3") == 0) return FADER_AUX3;
  if (strcasecmp(str, "aux4") == 0) return FADER_AUX4;
  return FADER_NONE;
}

const char* filter_type_to_string(filter_type_t type) {
  switch (type) {
    case FILTER_TYPE_GAIN:
      return "Gain";
    case FILTER_TYPE_VOLUME:
      return "Volume";
    case FILTER_TYPE_LOUDNESS:
      return "Loudness";
    case FILTER_TYPE_BIQUAD:
      return "Biquad";
    case FILTER_TYPE_CONV:
      return "Conv";
    case FILTER_TYPE_DELAY:
      return "Delay";
    case FILTER_TYPE_BIQUAD_COMBO:
      return "BiquadCombo";
    case FILTER_TYPE_DIFF_EQ:
      return "DiffEq";
    case FILTER_TYPE_DITHER:
      return "Dither";
    case FILTER_TYPE_LIMITER:
      return "Limiter";
    case FILTER_TYPE_LOOKAHEAD_LIMITER:
      return "LookaheadLimiter";
    default:
      return "Gain";
  }
}

filter_type_t filter_type_from_string(const char* str) {
  if (!str) return FILTER_TYPE_GAIN;
  if (strcmp(str, "Gain") == 0) return FILTER_TYPE_GAIN;
  if (strcmp(str, "Volume") == 0) return FILTER_TYPE_VOLUME;
  if (strcmp(str, "Loudness") == 0) return FILTER_TYPE_LOUDNESS;
  if (strcmp(str, "Biquad") == 0) return FILTER_TYPE_BIQUAD;
  if (strcmp(str, "Conv") == 0) return FILTER_TYPE_CONV;
  if (strcmp(str, "Delay") == 0) return FILTER_TYPE_DELAY;
  if (strcmp(str, "BiquadCombo") == 0) return FILTER_TYPE_BIQUAD_COMBO;
  if (strcmp(str, "DiffEq") == 0) return FILTER_TYPE_DIFF_EQ;
  if (strcmp(str, "Dither") == 0) return FILTER_TYPE_DITHER;
  if (strcmp(str, "Limiter") == 0) return FILTER_TYPE_LIMITER;
  if (strcmp(str, "LookaheadLimiter") == 0)
    return FILTER_TYPE_LOOKAHEAD_LIMITER;
  return FILTER_TYPE_GAIN;
}
