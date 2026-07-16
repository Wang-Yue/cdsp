#include "Public/spectrum.h"
#include <stdlib.h>
#include "Engine/dsp_engine.h"

bool cdsp_get_spectrum(dsp_engine_t* engine, bool is_capture, uint32_t channel,
                       double min_freq, double max_freq, size_t n_bins,
                       cdsp_spectrum_t* out_spec) {
  if (!engine || !out_spec) return false;
  
  spectrum_result_t res = {0};
  spectrum_status_t status = dsp_engine_get_spectrum(engine, is_capture, (int)channel,
                                                     min_freq, max_freq, n_bins, &res);
  if (status != SPECTRUM_OK) {
    return false;
  }

  out_spec->count = res.count;
  if (res.count > 0) {
    out_spec->frequencies = (double*)malloc(res.count * sizeof(double));
    out_spec->magnitudes = (double*)malloc(res.count * sizeof(double));
    if (!out_spec->frequencies || !out_spec->magnitudes) {
      if (out_spec->frequencies) free(out_spec->frequencies);
      if (out_spec->magnitudes) free(out_spec->magnitudes);
      out_spec->frequencies = NULL;
      out_spec->magnitudes = NULL;
      out_spec->count = 0;
      return false;
    }
    for (size_t i = 0; i < res.count; i++) {
      out_spec->frequencies[i] = res.frequencies[i];
      out_spec->magnitudes[i] = res.magnitudes[i];
    }
  } else {
    out_spec->frequencies = NULL;
    out_spec->magnitudes = NULL;
  }

  return true;
}

void cdsp_free_spectrum(cdsp_spectrum_t* spec) {
  if (!spec) return;
  if (spec->frequencies) free(spec->frequencies);
  if (spec->magnitudes) free(spec->magnitudes);
  spec->frequencies = NULL;
  spec->magnitudes = NULL;
  spec->count = 0;
}
