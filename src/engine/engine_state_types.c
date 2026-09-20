#include "engine/engine_state_types.h"

#include <string.h>

uint8_t processing_state_to_raw_byte(processing_state_t state) {
  switch (state) {
  case PROCESSING_STATE_INACTIVE:
    return 0;
  case PROCESSING_STATE_STARTING:
    return 1;
  case PROCESSING_STATE_RUNNING:
    return 2;
  case PROCESSING_STATE_PAUSED:
    return 3;
  case PROCESSING_STATE_STALLED:
    return 4;
  }
  CDSP_UNREACHABLE();
  return 0;
}

processing_state_t processing_state_from_raw_byte(uint8_t raw_byte) {
  switch (raw_byte) {
  case 0:
    return PROCESSING_STATE_INACTIVE;
  case 1:
    return PROCESSING_STATE_STARTING;
  case 2:
    return PROCESSING_STATE_RUNNING;
  case 3:
    return PROCESSING_STATE_PAUSED;
  case 4:
    return PROCESSING_STATE_STALLED;
  }
  CDSP_UNREACHABLE();
  return PROCESSING_STATE_INACTIVE;
}

const char *processing_state_to_string(processing_state_t state) {
  switch (state) {
  case PROCESSING_STATE_INACTIVE:
    return "Inactive";
  case PROCESSING_STATE_STARTING:
    return "Starting";
  case PROCESSING_STATE_RUNNING:
    return "Running";
  case PROCESSING_STATE_PAUSED:
    return "Paused";
  case PROCESSING_STATE_STALLED:
    return "Stalled";
  }
  CDSP_UNREACHABLE();
  return "Inactive";
}

processing_state_t processing_state_from_string(const char *str) {
  if (!str)
    return PROCESSING_STATE_INACTIVE;
  if (strcmp(str, "Starting") == 0)
    return PROCESSING_STATE_STARTING;
  if (strcmp(str, "Running") == 0)
    return PROCESSING_STATE_RUNNING;
  if (strcmp(str, "Paused") == 0)
    return PROCESSING_STATE_PAUSED;
  if (strcmp(str, "Stalled") == 0)
    return PROCESSING_STATE_STALLED;
  return PROCESSING_STATE_INACTIVE;
}
