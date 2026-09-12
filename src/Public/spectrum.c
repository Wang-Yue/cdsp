#include "cdsp/spectrum.h"

#include <stdint.h>
#include <stdio.h>

#include "Config/engine_config_types.h"
#include "Engine/dsp_engine.h"
#include "cdsp/cdsp_pub_types.h"

bool cdsp_get_spectrum(dsp_engine_t* engine, cdsp_spectrum_side_t side,
                       const size_t* channel, float min_freq, float max_freq,
                       size_t n_bins, cdsp_spectrum_t* out_spec) {
  if (!engine || !out_spec || !engine->get_spectrum) return false;

  bool is_capture = (side == CDSP_SPECTRUM_SIDE_CAPTURE);

  spectrum_t raw_spec = {
      .frequencies = out_spec->frequencies,
      .magnitudes = out_spec->magnitudes,
      .count = 0,
      .error_message = {0},
  };
  out_spec->error_message[0] = '\0';
  if (engine->get_spectrum(engine->ctx, is_capture, channel, min_freq, max_freq,
                           (uint32_t)n_bins, &raw_spec)) {
    out_spec->count = raw_spec.count;
    return true;
  }
  snprintf(out_spec->error_message, sizeof(out_spec->error_message), "%s",
           raw_spec.error_message);
  return false;
}
