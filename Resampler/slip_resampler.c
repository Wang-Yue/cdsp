#include "slip_resampler.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CROSSFADE_LEN 128

typedef struct {
  size_t channels;
  size_t chunk_size;
  fixed_async_t fixed;
  size_t crossfade_len;
  size_t max_correction;
  size_t needed_input_size;
  size_t needed_output_size;
  int correction;
  double drift_acc;
  double resample_ratio;

  // Pre-allocated scratch buffers
  size_t scratch_capacity;
  double** input_scratch;
  double** output_scratch;
} slip_resampler_t;

static int slip_resampler_validate(const resampler_config_t* config,
                                   config_error_t* err) {
  (void)config;
  (void)err;
  return 0;
}

static void slip_resampler_free(void* impl_ptr) {
  slip_resampler_t* impl = (slip_resampler_t*)impl_ptr;
  if (impl) {
    if (impl->input_scratch) {
      for (size_t i = 0; i < impl->channels; i++) {
        free(impl->input_scratch[i]);
      }
      free(impl->input_scratch);
    }
    if (impl->output_scratch) {
      for (size_t i = 0; i < impl->channels; i++) {
        free(impl->output_scratch[i]);
      }
      free(impl->output_scratch);
    }
    free(impl);
  }
}

static void slip_resampler_replan(slip_resampler_t* impl) {
  double drift_per_chunk;
  if (impl->fixed == FIXED_ASYNC_INPUT) {
#ifdef CDSP_TEST
    volatile double diff = impl->resample_ratio - 1.0;
    drift_per_chunk = diff * (double)impl->chunk_size;
#else
    drift_per_chunk = (impl->resample_ratio - 1.0) * (double)impl->chunk_size;
#endif
  } else {
#ifdef CDSP_TEST
    volatile double diff = 1.0 - 1.0 / impl->resample_ratio;
    drift_per_chunk = diff * (double)impl->chunk_size;
#else
    drift_per_chunk =
        (1.0 - 1.0 / impl->resample_ratio) * (double)impl->chunk_size;
#endif
  }
  double projected = impl->drift_acc + drift_per_chunk;

  int cap = (int)impl->max_correction;
  int corr = (int)trunc(projected);
  if (corr > cap) corr = cap;
  if (corr < -cap) corr = -cap;
  impl->correction = corr;

  if (impl->fixed == FIXED_ASYNC_INPUT) {
    impl->needed_input_size = impl->chunk_size;
    impl->needed_output_size =
        (size_t)((ssize_t)impl->chunk_size + impl->correction);
  } else {
    impl->needed_output_size = impl->chunk_size;
    impl->needed_input_size =
        (size_t)((ssize_t)impl->chunk_size - impl->correction);
  }
}

static void slip_resampler_get_ratio_range(const slip_resampler_t* impl,
                                           double* min_ratio,
                                           double* min_ratio_val) {
  double f = (double)impl->max_correction / (double)impl->chunk_size;
  if (impl->fixed == FIXED_ASYNC_INPUT) {
    *min_ratio = 1.0 - f;
    *min_ratio_val = 1.0 + f;
  } else {
    *min_ratio = 1.0 / (1.0 + f);
    *min_ratio_val = 1.0 / (1.0 - f);
  }
}

static void* slip_resampler_create(const resampler_config_t* config,
                                   size_t input_rate, size_t output_rate,
                                   size_t channels, size_t chunk_size,
                                   config_error_t* err) {
  (void)config;
  if (input_rate != output_rate) {
    config_error_set(err, CONFIG_ERR_INVALID_DEVICE,
                     "Slip resampler requires matching capture and playback "
                     "sample rates");
    return NULL;
  }
  if (chunk_size < 4) {
    config_error_set(err, CONFIG_ERR_INVALID_RESAMPLER,
                     "Slip resampler chunk_size must be at least 4");
    return NULL;
  }
  slip_resampler_t* impl =
      (slip_resampler_t*)calloc(1, sizeof(slip_resampler_t));
  if (!impl) return NULL;
  impl->channels = channels;
  impl->chunk_size = chunk_size;
  impl->fixed = FIXED_ASYNC_INPUT;

  impl->crossfade_len = (chunk_size - 2) / 2;
  if (impl->crossfade_len > MAX_CROSSFADE_LEN) {
    impl->crossfade_len = MAX_CROSSFADE_LEN;
  }

  impl->max_correction = (chunk_size - 1) / (impl->crossfade_len + 2);

  size_t scratch_len = chunk_size + impl->max_correction;
  impl->scratch_capacity = scratch_len;

  impl->input_scratch = (double**)calloc(channels, sizeof(double*));
  impl->output_scratch = (double**)calloc(channels, sizeof(double*));
  if (!impl->input_scratch || !impl->output_scratch) {
    slip_resampler_free(impl);
    return NULL;
  }

  for (size_t i = 0; i < channels; i++) {
    impl->input_scratch[i] = (double*)calloc(scratch_len, sizeof(double));
    impl->output_scratch[i] = (double*)calloc(scratch_len, sizeof(double));
    if (!impl->input_scratch[i] || !impl->output_scratch[i]) {
      slip_resampler_free(impl);
      return NULL;
    }
  }

  impl->drift_acc = 0.0;
  impl->resample_ratio = 1.0;
  slip_resampler_replan(impl);

  return impl;
}

static void place_correction(const double* input, double* output,
                             int correction, size_t crossfade_len,
                             size_t out_len) {
  size_t l = crossfade_len;
  size_t n = abs(correction);
  if (n == 0) {
    memcpy(output, input, out_len * sizeof(double));
    return;
  }

  int step = (correction > 0) ? -1 : 1;
  size_t gap_total = out_len - n * l;
  size_t base_gap = gap_total / (n + 1);
  size_t extra = gap_total % (n + 1);

  int offset = 0;
  size_t pos = 0;
  for (size_t r = 0; r < n; r++) {
    size_t gap = base_gap + ((r < extra) ? 1 : 0);
    int src = (int)pos + offset;
    memcpy(&output[pos], &input[src], gap * sizeof(double));
    pos += gap;

    for (size_t j = 0; j < l; j++) {
      double w = ((double)j + 0.5) / (double)l;
      int i = (int)pos + offset;
      double a = input[i];
      double b = input[i + step];
      output[pos] = a + w * (b - a);
      pos++;
    }
    offset += step;
  }
  int src = (int)pos + offset;
  memcpy(&output[pos], &input[src], (out_len - pos) * sizeof(double));
}

static resampler_error_t slip_resampler_process(void* impl_ptr,
                                                const audio_chunk_t* input,
                                                audio_chunk_t* output) {
  slip_resampler_t* impl = (slip_resampler_t*)impl_ptr;
  if (!impl || !input || !output) return RESAMPLER_ERR_INVALID_PARAMETER;

  size_t frames_to_read = audio_chunk_get_valid_frames(input);
  if (frames_to_read > impl->needed_input_size) {
    frames_to_read = impl->needed_input_size;
  }

  if (audio_chunk_get_frames(output) < impl->needed_output_size) {
    return RESAMPLER_ERR_OUTPUT_BUFFER_TOO_SMALL;
  }

  size_t input_len = impl->needed_input_size;
  size_t output_len = impl->needed_output_size;

  for (size_t chan = 0; chan < impl->channels; chan++) {
    const double* in_data = audio_chunk_get_channel(input, chan);
    double* out_data = audio_chunk_get_channel(output, chan);
    if (!in_data || !out_data) return RESAMPLER_ERR_INVALID_PARAMETER;

    if (impl->correction == 0) {
      memcpy(out_data, in_data, frames_to_read * sizeof(double));
      if (frames_to_read < output_len) {
        memset(&out_data[frames_to_read], 0,
               (output_len - frames_to_read) * sizeof(double));
      }
    } else {
      memcpy(impl->input_scratch[chan], in_data,
             frames_to_read * sizeof(double));
      if (frames_to_read < input_len) {
        memset(&impl->input_scratch[chan][frames_to_read], 0,
               (input_len - frames_to_read) * sizeof(double));
      }

      place_correction(impl->input_scratch[chan], impl->output_scratch[chan],
                       impl->correction, impl->crossfade_len, output_len);

      memcpy(out_data, impl->output_scratch[chan], output_len * sizeof(double));
    }
  }

  double drift_per_chunk;
  if (impl->fixed == FIXED_ASYNC_INPUT) {
    drift_per_chunk = (impl->resample_ratio - 1.0) * (double)impl->chunk_size;
  } else {
    drift_per_chunk =
        (1.0 - 1.0 / impl->resample_ratio) * (double)impl->chunk_size;
  }
  impl->drift_acc += drift_per_chunk;
  impl->drift_acc -= (double)impl->correction;
  slip_resampler_replan(impl);

  audio_chunk_set_valid_frames(output, output_len);
  return RESAMPLER_OK;
}

static void slip_resampler_set_relative_ratio(void* impl_ptr,
                                              double multiplier) {
  slip_resampler_t* impl = (slip_resampler_t*)impl_ptr;
  if (impl) {
    double min_r, max_r;
    slip_resampler_get_ratio_range(impl, &min_r, &max_r);
    double target = multiplier;
    if (target > max_r) target = max_r;
    if (target < min_r) target = min_r;
    impl->resample_ratio = target;
    slip_resampler_replan(impl);
  }
}

static double slip_resampler_get_ratio(const void* impl_ptr) {
  const slip_resampler_t* impl = (const slip_resampler_t*)impl_ptr;
  return impl ? impl->resample_ratio : 1.0;
}

static size_t slip_resampler_get_max_output_frames(const void* impl_ptr) {
  const slip_resampler_t* impl = (const slip_resampler_t*)impl_ptr;
  return impl ? (impl->chunk_size + impl->max_correction) : 0;
}

static size_t slip_resampler_get_chunk_size(const void* impl_ptr) {
  const slip_resampler_t* impl = (const slip_resampler_t*)impl_ptr;
  return impl ? impl->chunk_size : 0;
}

static size_t slip_resampler_get_input_frames_next(const void* impl_ptr) {
  const slip_resampler_t* impl = (const slip_resampler_t*)impl_ptr;
  return impl ? impl->needed_input_size : 0;
}

static size_t slip_resampler_get_output_frames_next(const void* impl_ptr) {
  const slip_resampler_t* impl = (const slip_resampler_t*)impl_ptr;
  return impl ? impl->needed_output_size : 0;
}

static size_t slip_resampler_get_channels(const void* impl_ptr) {
  const slip_resampler_t* impl = (const slip_resampler_t*)impl_ptr;
  return impl ? impl->channels : 0;
}

const resampler_vtable_t g_slip_resampler_vtable = {
    .validate = slip_resampler_validate,
    .create = slip_resampler_create,
    .process = slip_resampler_process,
    .set_relative_ratio = slip_resampler_set_relative_ratio,
    .get_ratio = slip_resampler_get_ratio,
    .get_max_output_frames = slip_resampler_get_max_output_frames,
    .get_chunk_size = slip_resampler_get_chunk_size,
    .get_input_frames_next = slip_resampler_get_input_frames_next,
    .get_output_frames_next = slip_resampler_get_output_frames_next,
    .get_channels = slip_resampler_get_channels,
    .free = slip_resampler_free,
};
