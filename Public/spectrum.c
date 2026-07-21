#include "Public/spectrum.h"

#include <stdlib.h>

#include "Engine/dsp_engine.h"

bool cdsp_get_spectrum(dsp_engine_t* engine, cdsp_spectrum_side_t side,
                       const uint32_t* channel, double min_freq,
                       double max_freq, size_t n_bins,
                       cdsp_spectrum_t* out_spec) {
  if (!engine || !out_spec || !engine->get_spectrum) return false;

  bool is_capture = (side == CDSP_SPECTRUM_SIDE_CAPTURE);
  uint32_t chan_val = channel ? *channel : (uint32_t)-1;

  spectrum_t raw_spec = {0};
  if (engine->get_spectrum(engine->ctx, is_capture, chan_val, min_freq,
                           max_freq, (uint32_t)n_bins, &raw_spec)) {
    out_spec->count = raw_spec.count;
    out_spec->frequencies = raw_spec.frequencies;
    out_spec->magnitudes = raw_spec.magnitudes;
    return true;
  }
  return false;
}

void cdsp_free_spectrum(cdsp_spectrum_t* spec) {
  if (!spec) return;
  if (spec->frequencies) free(spec->frequencies);
  if (spec->magnitudes) free(spec->magnitudes);
  spec->frequencies = NULL;
  spec->magnitudes = NULL;
  spec->count = 0;
}
