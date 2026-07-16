#include "Public/spectrum.h"
#include <stdlib.h>
#include "Engine/dsp_engine.h"

bool cdsp_get_spectrum(dsp_engine_t* engine, bool is_capture, uint32_t channel,
                       double min_freq, double max_freq, size_t n_bins,
                       cdsp_spectrum_t* out_spec) {
  if (!engine || !out_spec) return false;
  dsp_engine_interface_t* iface = dsp_engine_get_interface(engine);
  if (!iface || !iface->get_spectrum) return false;

  spectrum_t raw_spec = {0};
  if (iface->get_spectrum(iface->ctx, is_capture, channel, min_freq, max_freq, (uint32_t)n_bins, &raw_spec)) {
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
