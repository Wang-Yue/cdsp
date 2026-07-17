#include "processor_config_types.h"

#include <stdio.h>
#include <string.h>

const char* processor_type_to_string(processor_type_t type) {
  switch (type) {
    case PROCESSOR_TYPE_COMPRESSOR:
      return "Compressor";
    case PROCESSOR_TYPE_NOISE_GATE:
      return "NoiseGate";
    case PROCESSOR_TYPE_RACE:
      return "RACE";
    default:
      return "Compressor";
  }
}

processor_type_t processor_type_from_string(const char* str) {
  if (!str) return PROCESSOR_TYPE_INVALID;
  if (strcmp(str, "Compressor") == 0) return PROCESSOR_TYPE_COMPRESSOR;
  if (strcmp(str, "NoiseGate") == 0) return PROCESSOR_TYPE_NOISE_GATE;
  if (strcmp(str, "RACE") == 0) return PROCESSOR_TYPE_RACE;
  return PROCESSOR_TYPE_INVALID;
}
