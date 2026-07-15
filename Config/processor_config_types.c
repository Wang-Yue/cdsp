#include "processor_config_types.h"

#include <stdio.h>
#include <string.h>

#include "Processors/compressor_processor.h"
#include "Processors/noise_gate_processor.h"
#include "Processors/race_processor.h"

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
  if (!str) return PROCESSOR_TYPE_COMPRESSOR;
  if (strcmp(str, "Compressor") == 0) return PROCESSOR_TYPE_COMPRESSOR;
  if (strcmp(str, "NoiseGate") == 0) return PROCESSOR_TYPE_NOISE_GATE;
  if (strcmp(str, "RACE") == 0) return PROCESSOR_TYPE_RACE;
  return PROCESSOR_TYPE_COMPRESSOR;
}

int processor_config_validate(const processor_config_t* proc,
                              config_error_t* err) {
  if (!proc) return 0;
  switch (proc->type) {
    case PROCESSOR_TYPE_COMPRESSOR:
      return compressor_parameters_validate(&proc->parameters.compressor, err);
    case PROCESSOR_TYPE_NOISE_GATE:
      return noise_gate_parameters_validate(&proc->parameters.noise_gate, err);
    case PROCESSOR_TYPE_RACE:
      return race_parameters_validate(&proc->parameters.race, err);
  }
  return 0;
}
