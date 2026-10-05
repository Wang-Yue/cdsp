#include "cdsp_wasm.h"

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cdsp/cdsp.h"
#include "audio/audio_chunk.h"
#include "audio/audio_history_buffer.h"
#include "audio/processing_parameters.h"
#include "audio/sample_format.h"
#include "config/config_gen.h"
#include "engine/dsp_engine.h"
#include "engine/dsp_session.h"
#include "engine/dsp_session_internal.h"
#include "pipeline/pipeline.h"

// Internal engine implementation layout (matches dsp_engine.c)
struct dsp_engine_impl {
  struct {
    dsp_session_t *active;
    processing_stop_reason_t last_stop_reason;
    uint64_t clipped_samples_accum;
  } session;

  struct {
    audio_history_buffer_t *capture;
    audio_history_buffer_t *playback;
    spectrum_analyzer_t *spectrum;
  } buffers;
};

// Internal processing loop layout (matches engine_processing_loop.c)
struct engine_processing_loop {
  engine_shared_state_t *shared;
  processing_parameters_t *processing_params;
  size_t pipeline_rate;
  pipeline_t *active_pipeline;
  _Atomic(pipeline_t *) next_pipeline;
  _Atomic bool transfer_filter_state;
};

struct cdsp_wasm {
  dsp_engine_t *engine;
  size_t in_channels;
  size_t out_channels;
  size_t quantum_size;
  int sample_rate;

  audio_chunk_t *in_chunk;
  audio_chunk_t *out_chunk;

  float **in_ptrs;
  float **out_ptrs;
  float *in_buffer_storage;
  float *out_buffer_storage;
  float *spectrum_freqs;
  size_t spectrum_freqs_cap;
};

static void cdsp_wasm_update_channels_and_chunks(cdsp_wasm_t *wasm) {
  if (!wasm || !wasm->engine)
    return;

  struct dsp_engine_impl *impl = (struct dsp_engine_impl *)wasm->engine->ctx;
  size_t in_channels = 2;
  size_t out_channels = 2;

  if (impl && impl->session.active && impl->session.active->current_config) {
    in_channels = capture_device_config_get_channels(
        &impl->session.active->current_config->devices.capture);
    out_channels = playback_device_config_get_channels(
        &impl->session.active->current_config->devices.playback);
  }
  if (in_channels == 0) in_channels = 2;
  if (out_channels == 0) out_channels = 2;

  if (in_channels != wasm->in_channels || out_channels != wasm->out_channels ||
      !wasm->in_chunk || !wasm->out_chunk) {
    if (wasm->in_chunk) audio_chunk_free(wasm->in_chunk);
    if (wasm->out_chunk) audio_chunk_free(wasm->out_chunk);
    free(wasm->in_buffer_storage);
    free(wasm->out_buffer_storage);
    free(wasm->in_ptrs);
    free(wasm->out_ptrs);

    wasm->in_channels = in_channels;
    wasm->out_channels = out_channels;

    wasm->in_chunk = audio_chunk_create(wasm->quantum_size, in_channels);
    wasm->out_chunk = audio_chunk_create(wasm->quantum_size, out_channels);

    wasm->in_buffer_storage = (float *)calloc(in_channels * wasm->quantum_size, sizeof(float));
    wasm->out_buffer_storage = (float *)calloc(out_channels * wasm->quantum_size, sizeof(float));
    wasm->in_ptrs = (float **)calloc(in_channels, sizeof(float *));
    wasm->out_ptrs = (float **)calloc(out_channels, sizeof(float *));

    for (size_t c = 0; c < in_channels; c++) {
      wasm->in_ptrs[c] = wasm->in_buffer_storage + (c * wasm->quantum_size);
    }
    for (size_t c = 0; c < out_channels; c++) {
      wasm->out_ptrs[c] = wasm->out_buffer_storage + (c * wasm->quantum_size);
    }
  }
}

cdsp_wasm_t *cdsp_wasm_create(const char *json_config, int sample_rate,
                              int quantum_size) {
  if (quantum_size <= 0) {
    quantum_size = 128;
  }
  if (sample_rate <= 0) {
    sample_rate = 48000;
  }

  dsp_engine_t *engine = cdsp_engine_create();
  if (!engine) {
    fprintf(stderr, "[cdsp.wasm] Failed to create DSP engine\n");
    return NULL;
  }

  const char *config_to_set = json_config;
  static const char *kDefaultJson =
      "{\n"
      "  \"title\": \"WebAudio Default\",\n"
      "  \"devices\": {\n"
      "    \"samplerate\": 48000,\n"
      "    \"chunksize\": 128,\n"
      "    \"capture\": { \"type\": \"WebAudio\", \"channels\": 2 },\n"
      "    \"playback\": { \"type\": \"WebAudio\", \"channels\": 2 }\n"
      "  }\n"
      "}\n";

  if (!config_to_set || strlen(config_to_set) == 0) {
    config_to_set = kDefaultJson;
  }

  cdsp_backend_error_t err;
  memset(&err, 0, sizeof(err));
  if (!cdsp_set_config_json(engine, config_to_set, &err)) {
    fprintf(stderr, "[cdsp.wasm] Failed to set config: %s\n", err.message);
    cdsp_engine_free(engine);
    return NULL;
  }

  cdsp_engine_poll(engine);

  struct dsp_engine_impl *impl = (struct dsp_engine_impl *)engine->ctx;
  if (impl) {
    if (impl->buffers.capture) {
      audio_history_buffer_set_enabled(impl->buffers.capture, true);
    }
    if (impl->buffers.playback) {
      audio_history_buffer_set_enabled(impl->buffers.playback, true);
    }
  }

  cdsp_wasm_t *wasm = (cdsp_wasm_t *)calloc(1, sizeof(cdsp_wasm_t));
  if (!wasm) {
    cdsp_engine_free(engine);
    return NULL;
  }

  wasm->engine = engine;
  wasm->quantum_size = (size_t)quantum_size;
  wasm->sample_rate = sample_rate;

  cdsp_wasm_update_channels_and_chunks(wasm);

  return wasm;
}

void cdsp_wasm_destroy(cdsp_wasm_t *ctx) {
  if (!ctx)
    return;

  if (ctx->engine) {
    cdsp_engine_free(ctx->engine);
    ctx->engine = NULL;
  }
  if (ctx->in_chunk) {
    audio_chunk_free(ctx->in_chunk);
    ctx->in_chunk = NULL;
  }
  if (ctx->out_chunk) {
    audio_chunk_free(ctx->out_chunk);
    ctx->out_chunk = NULL;
  }
  free(ctx->in_buffer_storage);
  free(ctx->out_buffer_storage);
  free(ctx->spectrum_freqs);
  free(ctx->in_ptrs);
  free(ctx->out_ptrs);
  free(ctx);
}

float **cdsp_wasm_get_input_buffer_ptrs(cdsp_wasm_t *ctx) {
  return ctx ? ctx->in_ptrs : NULL;
}

float **cdsp_wasm_get_output_buffer_ptrs(cdsp_wasm_t *ctx) {
  return ctx ? ctx->out_ptrs : NULL;
}

size_t cdsp_wasm_get_input_channels(const cdsp_wasm_t *ctx) {
  return ctx ? ctx->in_channels : 0;
}

size_t cdsp_wasm_get_output_channels(const cdsp_wasm_t *ctx) {
  return ctx ? ctx->out_channels : 0;
}

size_t cdsp_wasm_get_quantum_size(const cdsp_wasm_t *ctx) {
  return ctx ? ctx->quantum_size : 0;
}

static pipeline_t *cdsp_wasm_get_active_pipeline(dsp_session_t *session) {
  if (!session) return NULL;
  if (session->processing_loop) {
    struct engine_processing_loop *loop =
        (struct engine_processing_loop *)session->processing_loop;
    pipeline_t *next = atomic_exchange(&loop->next_pipeline, NULL);
    if (next) {
      if (loop->active_pipeline) {
        bool transfer_filters = atomic_load(&loop->transfer_filter_state);
        pipeline_transfer_state(next, loop->active_pipeline, transfer_filters);
        pipeline_t *uncollected = engine_shared_state_retire_pipeline(
            loop->shared, loop->active_pipeline);
        if (uncollected) {
          pipeline_free(uncollected);
        }
      }
      loop->active_pipeline = next;
      session->pipeline = next;
    }
    if (loop->active_pipeline) {
      return loop->active_pipeline;
    }
  }
  return session->pipeline;
}

bool cdsp_wasm_process(cdsp_wasm_t *ctx, size_t frames) {
  if (!ctx || !ctx->engine || frames == 0) {
    return false;
  }
  if (frames > ctx->quantum_size) {
    frames = ctx->quantum_size;
  }

  struct dsp_engine_impl *impl = (struct dsp_engine_impl *)ctx->engine->ctx;
  if (!impl || !impl->session.active) {
    return false;
  }

  dsp_session_t *session = impl->session.active;
  pipeline_t *pipeline = cdsp_wasm_get_active_pipeline(session);

  // 1. Decode planar float input pointers directly into double audio chunk
  audio_chunk_decode_planar((const void *const *)ctx->in_ptrs,
                            BINARY_SAMPLE_FORMAT_F32_LE, ctx->in_channels,
                            frames, ctx->in_chunk);
  audio_chunk_set_valid_frames(ctx->in_chunk, frames);

  // 2. Capture-side telemetry
  if (impl->buffers.capture) {
    audio_history_buffer_append(impl->buffers.capture, ctx->in_chunk);
  }
  if (session->processing_params) {
    processing_parameters_update_capture_levels(session->processing_params,
                                                ctx->in_chunk);
  }

  // 3. Synchronously execute the full double-precision DSP pipeline
  if (pipeline) {
    pipeline_error_t perr =
        pipeline_process(pipeline, ctx->in_chunk, ctx->out_chunk);
    if (perr != PIPELINE_OK) {
      audio_chunk_zero(ctx->out_chunk);
    }
  } else {
    for (size_t c = 0; c < ctx->out_channels; c++) {
      double *dst = audio_chunk_get_channel(ctx->out_chunk, c);
      if (c < ctx->in_channels) {
        const double *src = audio_chunk_get_channel(ctx->in_chunk, c);
        memcpy(dst, src, frames * sizeof(double));
      } else {
        memset(dst, 0, frames * sizeof(double));
      }
    }
  }
  audio_chunk_set_valid_frames(ctx->out_chunk, frames);

  // 4. Playback-side telemetry
  if (session->processing_params) {
    processing_parameters_update_playback_levels(session->processing_params,
                                                 ctx->out_chunk);
  }
  if (impl->buffers.playback) {
    audio_history_buffer_append(impl->buffers.playback, ctx->out_chunk);
  }

  // 5. Encode output double audio chunk directly into planar float output pointers
  audio_chunk_encode_planar(ctx->out_chunk, BINARY_SAMPLE_FORMAT_F32_LE,
                            ctx->out_channels, frames,
                            (void *const *)ctx->out_ptrs);

  return true;
}

void cdsp_wasm_set_fader_volume(cdsp_wasm_t *ctx, int fader, double volume_db,
                                bool instant) {
  if (!ctx || !ctx->engine)
    return;
  cdsp_set_fader_volume(ctx->engine, (cdsp_fader_t)fader, (float)volume_db, instant);
}

double cdsp_wasm_get_fader_volume(const cdsp_wasm_t *ctx, int fader) {
  if (!ctx || !ctx->engine)
    return 0.0;
  return (double)cdsp_get_fader_volume(ctx->engine, (cdsp_fader_t)fader);
}

void cdsp_wasm_set_fader_mute(cdsp_wasm_t *ctx, int fader, bool mute) {
  if (!ctx || !ctx->engine)
    return;
  cdsp_set_fader_mute(ctx->engine, (cdsp_fader_t)fader, mute);
}

bool cdsp_wasm_get_fader_mute(const cdsp_wasm_t *ctx, int fader) {
  if (!ctx || !ctx->engine)
    return false;
  return cdsp_get_fader_mute(ctx->engine, (cdsp_fader_t)fader);
}

bool cdsp_wasm_set_config_json(cdsp_wasm_t *ctx, const char *json_config) {
  if (!ctx || !ctx->engine || !json_config)
    return false;

  cdsp_backend_error_t err;
  memset(&err, 0, sizeof(err));
  bool ok = cdsp_set_config_json(ctx->engine, json_config, &err);
  if (!ok) {
    fprintf(stderr, "[cdsp.wasm] Failed to set config: %s\n", err.message);
    return false;
  }
  cdsp_engine_poll(ctx->engine);
  cdsp_wasm_update_channels_and_chunks(ctx);
  return true;
}

void cdsp_wasm_get_vu_levels(const cdsp_wasm_t *ctx, float *in_peak,
                             float *in_rms, float *out_peak, float *out_rms) {
  if (!ctx || !ctx->engine)
    return;

  cdsp_vu_levels_t vu = {
      .playback_rms = out_rms,
      .playback_peak = out_peak,
      .capture_rms = in_rms,
      .capture_peak = in_peak,
  };
  cdsp_get_vu_levels(ctx->engine, &vu);
}

bool cdsp_wasm_get_spectrum(cdsp_wasm_t *ctx, bool is_capture, int channel,
                            double min_freq, double max_freq, size_t n_bins,
                            float *out_bins) {
  if (!ctx || !ctx->engine || !out_bins || n_bins == 0)
    return false;

  if (ctx->spectrum_freqs_cap < n_bins) {
    float *new_buf =
        (float *)realloc(ctx->spectrum_freqs, n_bins * sizeof(float));
    if (!new_buf)
      return false;
    ctx->spectrum_freqs = new_buf;
    ctx->spectrum_freqs_cap = n_bins;
  }

  cdsp_spectrum_t spec = {
      .magnitudes = out_bins,
      .frequencies = ctx->spectrum_freqs,
      .count = 0,
  };
  size_t ch = channel >= 0 ? (size_t)channel : 0;
  cdsp_spectrum_side_t side =
      is_capture ? CDSP_SPECTRUM_SIDE_CAPTURE : CDSP_SPECTRUM_SIDE_PLAYBACK;

  bool ok = cdsp_get_spectrum(ctx->engine, side, &ch, (float)min_freq,
                              (float)max_freq, n_bins, &spec);
  return ok && spec.count > 0;
}

size_t cdsp_wasm_get_samples(cdsp_wasm_t *ctx, bool is_capture, size_t n_frames,
                             float *out_left, float *out_right) {
  if (!ctx || !ctx->engine || n_frames == 0)
    return 0;

  float *chan_ptrs[2] = {out_left, out_right};
  cdsp_audio_samples_t samples = {
      .channels = chan_ptrs,
      .channels_count = 2,
      .frames = 0,
  };
  cdsp_backend_error_t err;
  memset(&err, 0, sizeof(err));

  if (!cdsp_get_samples(ctx->engine, is_capture, n_frames, &samples, &err)) {
    return 0;
  }
  return samples.frames;
}


