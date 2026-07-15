#include "mixer_config_types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Standalone mixer configuration types.

// Support both nested format `channels: { in: N, out: N }` and
// flat format `channels_in: N, channels_out: N`
// Try nested format first: channels: { in: N, out: N }
// Fall back to flat format: channels_in / channels_out
// Encode in the nested format

/// Convenience accessor: 0.0 when gain is nil
double mixer_source_gain_value(const mixer_source_t* src) {
  if (!src || !src->has_gain) return 0.0;
  return src->gain;
}
