#include "Public/spectrum.h"

#include <stdint.h>

#include "Config/engine_config_types.h"
#include "Engine/dsp_engine.h"
#include "Public/cdsp_pub_types.h"

bool cdsp_get_spectrum(dsp_engine_t* engine, cdsp_spectrum_side_t side,
                       const size_t* channel, float min_freq, float max_freq,
                       size_t n_bins, cdsp_spectrum_t* out_spec) {
  if (!engine || !out_spec || !engine->get_spectrum) return false;

  bool is_capture = (side == CDSP_SPECTRUM_SIDE_CAPTURE);

  spectrum_t raw_spec = {
      .frequencies = out_spec->frequencies,
      .magnitudes = out_spec->magnitudes,
      .count = 0,
  };
  if (engine->get_spectrum(engine->ctx, is_capture, channel, min_freq, max_freq,
                           (uint32_t)n_bins, &raw_spec)) {
    out_spec->count = raw_spec.count;
    return true;
  }
  return false;
}
