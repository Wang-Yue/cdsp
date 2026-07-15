// PCM → DSD (DoP / Native DSD) encoder. Inverse of `DoPDecoder`: converts a
// chunk of PCM audio at the carrier rate into DSD-over-PCM (DoP) or Native DSD,
// in place. For each input frame we
//   1. interpolate to the DSD rate using a 511-tap β=11 Kaiser-windowed
//      polyphase sinc (same shape as the decoder, normalized per phase
//      for unit DC gain),
//   2. modulate the oversampled signal with a per-channel sigma-delta
//      modulator (using the configured `SDMFilter`, defaulting to `sdm-6`), and
//   3. pack DSD bits into 8, 16, or 32-bit containers (or 24-bit DoP container
//      with an alternating `0x05` / `0xFA` marker in the upper byte).
//
// The encoded chunk satisfies the strict-alternation detection state
// machine in `DoPDecoder` when in DoP mode and round-trips through any DAC that
// natively understands DoP or Native DSD. To preserve the bit pattern through
// CoreAudio in DoP mode, the playback format must be S24 or S32 (F32 will
// quantize the marker away); the encoder itself just emits float-normalised
// 24-bit or DSD values and trusts the playback backend to forward them
// losslessly.
//
// SDM state per channel is carried by an embedded `SigmaDeltaModulator`;
// the polyphase coefficient table is shared across channels and built
// once at init.

#include "dsd_encoder.h"

#include "Audio/sample_conversion.h"
#include "Logging/app_logger.h"

static const logger_t g_logger = {"dsp.dsd.encoder"};
#include "sigma_delta_modulator.h"
#if defined(ENABLE_BLAS)
#include <cblas.h>
#elif defined(ENABLE_ACCELERATE)
#include <Accelerate/Accelerate.h>
#endif
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief State for a single DSD encoder channel.
 */
typedef struct {
  /** FIFO buffer for interpolation. Holds 32 * 2 floats. */
  float fifo[64];
  /** Current position in the FIFO buffer. */
  int fifo_pos;
  /** Alternating DoP marker byte (0x05 or 0xFA). */
  uint8_t marker;
  /** Sigma-delta modulator instance for this channel. */
  sigma_delta_modulator_t* modulator;
} dsd_encoder_channel_state_t;

struct dsd_encoder {
  /** Number of audio channels. */
  int channels;
  /**
   * True if the encoder is enabled (i.e. constructor was asked to encode
   * AND the carrier rate is supported).
   */
  bool enabled;
  /** Active DSD processing mode (DSD_MODE_PCM, DSD_MODE_DOP, or
   * DSD_MODE_NATIVE). */
  dsd_mode_t mode;
  /** DSD container bit depth per output frame (8, 16, or 32). */
  size_t dsd_bit_depth;
  /** Array of channel states. */
  dsd_encoder_channel_state_t* channel_states;
  /**
   * Polyphase coefficient table laid out as `coeffs[phase * subFilterTaps +
   * tap]`. Each phase is normalized to unit DC gain. Single-precision float
   * representation for maximum SIMD (NEON) vectorization throughput and minimal cache footprint.
   */
  float* coeffs;
};

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../Audio/double_helpers.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifdef ENABLE_ACCELERATE
#include <Accelerate/Accelerate.h>
#endif

#define DSD_ENC_REAL_TAPS 511
#define DSD_ENC_NUM_TAPS 512
#define DSD_ENC_SUB_FILTER_TAPS 32
#define DSD_ENC_FIFO_MASK 31

// Carrier sample rates that produce a valid DoP stream (16-bit DSD payload per
// frame).
static const int supported_dop_carrier_rates[] = {176400, 352800, 705600,
                                                  192000, 384000, 768000};

// Carrier sample rates that produce a valid native DSD stream across 8-bit,
// 16-bit, and 32-bit containers.
static const int supported_native_carrier_rates[] = {
    88200,  96000,  176400, 192000,  352800,
    384000, 705600, 768000, 1411200, 1536000};

bool dsd_encoder_is_supported_carrier_rate(int rate, dsd_mode_t mode) {
  if (mode == DSD_MODE_DOP) {
    size_t count = sizeof(supported_dop_carrier_rates) /
                   sizeof(supported_dop_carrier_rates[0]);
    for (size_t i = 0; i < count; i++) {
      if (supported_dop_carrier_rates[i] == rate) return true;
    }
  } else if (mode == DSD_MODE_NATIVE) {
    size_t count = sizeof(supported_native_carrier_rates) /
                   sizeof(supported_native_carrier_rates[0]);
    for (size_t i = 0; i < count; i++) {
      if (supported_native_carrier_rates[i] == rate) return true;
    }
  }
  return false;
}

/// Build a polyphase decomposition of a 511-tap β=11 Kaiser-windowed
/// sinc with cutoff `cutoffHz / dsdRate`. Phase `p` gets taps
/// `h[m·phases + p]` for `m = 0..<subFilterTaps`; each phase is
/// normalized to unit DC gain so a constant input passes through
/// unchanged.
/**
 * @brief Builds the polyphase coefficient table for the interpolation
 * filter (float precision).
 *
 * Designs a 511-tap Kaiser-windowed sinc filter and decomposes it into
 * `dsd_bit_depth` phases (polyphase representation) with 32 taps per phase.
 * Each phase is normalized to ensure unit DC gain, so a constant input passes
 * through unchanged.
 *
 * @param sample_rate The PCM sample rate (carrier rate).
 * @param dsd_bit_depth DSD container bit depth per output frame (8, 16, 32).
 * @param cutoff_hz The desired cutoff frequency in Hz.
 * @return A pointer to the allocated flat array of single-precision polyphase coefficients, or
 * NULL on allocation failure.
 */
static float* build_coeffs(size_t sample_rate, size_t dsd_bit_depth,
                           double cutoff_hz) {
  double beta = 11.0;
  double dsd_rate = (double)sample_rate * (double)dsd_bit_depth;
  double cutoff = cutoff_hz / dsd_rate;
  size_t phases = dsd_bit_depth;
  int num_taps = (int)(phases * DSD_ENC_SUB_FILTER_TAPS);
  int real_taps = num_taps - 1;
  double alpha = (double)(real_taps - 1) / 2.0;
  double i0_beta = double_bessel_i0(beta);

  double* taps = (double*)calloc(num_taps, sizeof(double));
  if (!taps) return NULL;

  for (int i = 0; i < real_taps; i++) {
    double t = (double)i - alpha;
    double sinc_val = 0.0;
    if (t == 0.0) {
      sinc_val = 2.0 * cutoff;
    } else {
      double angle = 2.0 * M_PI * cutoff * t;
      sinc_val = sin(angle) / (M_PI * t);
    }
    double term = 1.0 - pow(t / alpha, 2.0);
    double widx = sqrt(term > 0.0 ? term : 0.0);
    double window_val = double_bessel_i0(beta * widx) / i0_beta;
    taps[i] = sinc_val * window_val;
  }

  size_t total_elements = phases * DSD_ENC_SUB_FILTER_TAPS;
  float* p = (float*)calloc(total_elements, sizeof(float));
  if (!p) {
    free(taps);
    return NULL;
  }

  for (size_t ph = 0; ph < phases; ph++) {
    double sub_sum = 0.0;
    for (int m = 0; m < DSD_ENC_SUB_FILTER_TAPS; m++) {
      sub_sum += taps[m * phases + ph];
    }
    double scale = (sub_sum != 0.0) ? (0.5 / sub_sum) : 0.0;
    for (int m = 0; m < DSD_ENC_SUB_FILTER_TAPS; m++) {
      double v = taps[m * phases + ph] * scale;
      int store_idx = (int)(ph * DSD_ENC_SUB_FILTER_TAPS +
                            (DSD_ENC_SUB_FILTER_TAPS - 1 - m));
      p[store_idx] = (float)v;
    }
  }
  free(taps);
  return p;
}

dsd_encoder_t* dsd_encoder_create(int channels, size_t sample_rate,
                                  dsd_mode_t mode, size_t dsd_bit_depth,
                                  sdm_filter_t filter_name, double cutoff_hz) {
  if (channels <= 0) {
    logger_error(&g_logger, "Invalid channel count for DSD encoder: %d",
                 channels);
    return NULL;
  }
  if (dsd_bit_depth != 8 && dsd_bit_depth != 16 && dsd_bit_depth != 32) {
    dsd_bit_depth = 16;
  }
  dsd_encoder_t* enc = (dsd_encoder_t*)calloc(1, sizeof(dsd_encoder_t));
  if (!enc) {
    logger_error(&g_logger, "Memory allocation failed for dsd_encoder_t");
    return NULL;
  }
  enc->channels = channels;
  enc->dsd_bit_depth = dsd_bit_depth;
  enc->coeffs = build_coeffs(sample_rate, dsd_bit_depth, cutoff_hz);
  if (!enc->coeffs) {
    logger_error(&g_logger,
                 "Failed to build polyphase coefficients for DSD encoder");
    dsd_encoder_free(enc);
    return NULL;
  }

  bool supported =
      dsd_encoder_is_supported_carrier_rate((int)sample_rate, mode);
  enc->mode = (supported && mode != DSD_MODE_PCM) ? mode : DSD_MODE_PCM;
  enc->enabled = (enc->mode != DSD_MODE_PCM);

  if (!enc->enabled) {
    logger_debug(&g_logger,
                 "DSD encoder created (disabled: mode=%s, rate_supported=%d)",
                 dsd_mode_to_string(mode), supported ? 1 : 0);
    return enc;
  }

  enc->channel_states = (dsd_encoder_channel_state_t*)calloc(
      channels, sizeof(dsd_encoder_channel_state_t));
  if (!enc->channel_states) {
    logger_error(&g_logger,
                 "Memory allocation failed for DSD encoder channel states");
    dsd_encoder_free(enc);
    return NULL;
  }

  double dsd_rate = (double)sample_rate * (double)dsd_bit_depth;
  uint32_t freq = (uint32_t)round(dsd_rate);
  for (int ch = 0; ch < channels; ch++) {
    enc->channel_states[ch].modulator =
        sigma_delta_modulator_create(filter_name, freq);
    enc->channel_states[ch].marker = 0x05;
    if (!enc->channel_states[ch].modulator) {
      logger_error(&g_logger,
                   "Failed to create Sigma-Delta modulator for channel %d", ch);
      dsd_encoder_free(enc);
      return NULL;
    }
  }
  logger_debug(&g_logger,
               "DSD encoder created and enabled (channels=%d, sample_rate=%zu, "
               "bit_depth=%zu, mode=%s)",
               channels, sample_rate, dsd_bit_depth,
               dsd_mode_to_string(enc->mode));
  return enc;
}

/**
 * @brief Encodes a single channel's PCM buffer to DSD/DoP in-place.
 *
 * For each input PCM frame, this function:
 * 1. Pushes the sample into a duplicate-history FIFO.
 * 2. Runs a polyphase interpolation filter for `dsd_bit_depth` phases via
 *    cblas_dgemv (or fallback dot product).
 * 3. Feeds each interpolated sample to the Sigma-Delta Modulator.
 * 4. Packs DSD bits into 8, 16, or 32-bit container normalized float
 * [-1.0, 1.0].
 */


#if defined(__GNUC__) || defined(__clang__)
typedef float v4sf __attribute__((vector_size(16)));
typedef float v4sf_u __attribute__((vector_size(16), aligned(4)));

static inline float dot_product_32(const float* a, const float* b) {
  v4sf acc0 = {0.0f, 0.0f, 0.0f, 0.0f};
  v4sf acc1 = {0.0f, 0.0f, 0.0f, 0.0f};

  const v4sf* va = (const v4sf*)a;
  const v4sf_u* vb = (const v4sf_u*)b;

  for (int i = 0; i < 8; i += 2) {
    acc0 += va[i + 0] * vb[i + 0];
    acc1 += va[i + 1] * vb[i + 1];
  }

  v4sf acc = acc0 + acc1;
  return acc[0] + acc[1] + acc[2] + acc[3];
}
#else
static inline float dot_product_32(const float* a, const float* b) {
  float acc0 = 0.0f, acc1 = 0.0f, acc2 = 0.0f, acc3 = 0.0f;
  for (int i = 0; i < 32; i += 4) {
    acc0 += a[i + 0] * b[i + 0];
    acc1 += a[i + 1] * b[i + 1];
    acc2 += a[i + 2] * b[i + 2];
    acc3 += a[i + 3] * b[i + 3];
  }
  return (acc0 + acc1) + (acc2 + acc3);
}
#endif

static inline double pack_dsd_sample(uint32_t word, size_t bit_depth,
                                    dsd_mode_t mode, uint8_t* marker) {
  if (bit_depth == 16) {
    if (mode == DSD_MODE_NATIVE) {
      return pcm_sample_decode_s16((int16_t)word);
    } else {
      uint32_t val24 = ((uint32_t)(*marker) << 16) | (uint32_t)word;
      int32_t int_val = (int32_t)(val24 << 8) >> 8;
      *marker = (*marker == 0x05) ? 0xFA : 0x05;
      return pcm_sample_decode_s24(int_val);
    }
  } else if (bit_depth == 32) {
    return pcm_sample_decode_f32_u32(word);
  } else {
    return pcm_sample_decode_dsd_u8((uint8_t)word);
  }
}

static void encode_channel(dsd_encoder_channel_state_t* state,
                           mutable_waveform_t buf, size_t frames,
                           const float* coeffs, dsd_mode_t mode,
                           size_t dsd_bit_depth) {
  if (!buf) return;
  float* fifo = state->fifo;
  int pos = state->fifo_pos;
  uint8_t marker = state->marker;
  sigma_delta_modulator_t* mod = state->modulator;

  for (size_t t = 0; t < frames; t++) {
    float sample_val = (float)buf[t];
    fifo[pos] = sample_val;
    fifo[pos + DSD_ENC_SUB_FILTER_TAPS] = sample_val;

    const float* fifo_p = fifo + pos + 1;
    const float* cp = coeffs;

    uint32_t word = 0;
    for (size_t p = 0; p < dsd_bit_depth; p++) {
      float Y_p = dot_product_32(cp, fifo_p);
      cp += 32;
      if (sigma_delta_modulator_sample(mod, Y_p)) {
        word |= ((uint32_t)1 << (dsd_bit_depth - 1 - p));
      }
    }
    buf[t] = pack_dsd_sample(word, dsd_bit_depth, mode, &marker);
    pos = (pos + 1) & DSD_ENC_FIFO_MASK;
  }

  state->fifo_pos = pos;
  state->marker = marker;
}



static void encode_dual_channels(dsd_encoder_channel_state_t* state0,
                                 dsd_encoder_channel_state_t* state1,
                                 mutable_waveform_t buf0,
                                 mutable_waveform_t buf1,
                                 size_t frames,
                                 const float* coeffs,
                                 dsd_mode_t mode,
                                 size_t dsd_bit_depth) {
  if (!buf0 || !buf1) return;
  float* fifo0 = state0->fifo;
  float* fifo1 = state1->fifo;
  int pos0 = state0->fifo_pos;
  int pos1 = state1->fifo_pos;
  uint8_t marker0 = state0->marker;
  uint8_t marker1 = state1->marker;
  sigma_delta_modulator_t* mod0 = state0->modulator;
  sigma_delta_modulator_t* mod1 = state1->modulator;

  for (size_t t = 0; t < frames; t++) {
    float val0 = (float)buf0[t];
    float val1 = (float)buf1[t];
    fifo0[pos0] = val0; fifo0[pos0 + DSD_ENC_SUB_FILTER_TAPS] = val0;
    fifo1[pos1] = val1; fifo1[pos1 + DSD_ENC_SUB_FILTER_TAPS] = val1;

    const float* fifo_p0 = fifo0 + pos0 + 1;
    const float* fifo_p1 = fifo1 + pos1 + 1;
    const float* cp = coeffs;

    uint32_t word0 = 0, word1 = 0;
    for (size_t p = 0; p < dsd_bit_depth; p++) {
      float Y_p0 = dot_product_32(cp, fifo_p0);
      float Y_p1 = dot_product_32(cp, fifo_p1);
      cp += 32;
      if (sigma_delta_modulator_sample(mod0, Y_p0)) {
        word0 |= ((uint32_t)1 << (dsd_bit_depth - 1 - p));
      }
      if (sigma_delta_modulator_sample(mod1, Y_p1)) {
        word1 |= ((uint32_t)1 << (dsd_bit_depth - 1 - p));
      }
    }
    buf0[t] = pack_dsd_sample(word0, dsd_bit_depth, mode, &marker0);
    buf1[t] = pack_dsd_sample(word1, dsd_bit_depth, mode, &marker1);

    pos0 = (pos0 + 1) & DSD_ENC_FIFO_MASK;
    pos1 = (pos1 + 1) & DSD_ENC_FIFO_MASK;
  }

  state0->fifo_pos = pos0;
  state0->marker = marker0;
  state1->fifo_pos = pos1;
  state1->marker = marker1;
}

void dsd_encoder_encode(dsd_encoder_t* encoder, audio_chunk_t* chunk) {
  if (!encoder || !encoder->enabled || !chunk) return;
  size_t n = audio_chunk_get_valid_frames(chunk);
  int chs = encoder->channels;
  if (n == 0 || (int)audio_chunk_get_channels(chunk) != chs)
    return;

  int ch = 0;
  for (; ch + 1 < chs; ch += 2) {
    encode_dual_channels(&encoder->channel_states[ch],
                         &encoder->channel_states[ch + 1],
                         audio_chunk_get_channel(chunk, ch),
                         audio_chunk_get_channel(chunk, ch + 1),
                         n, encoder->coeffs, encoder->mode,
                         encoder->dsd_bit_depth);
  }
  for (; ch < chs; ch++) {
    encode_channel(&encoder->channel_states[ch],
                   audio_chunk_get_channel(chunk, ch), n, encoder->coeffs,
                   encoder->mode, encoder->dsd_bit_depth);
  }
}

void dsd_encoder_free(dsd_encoder_t* encoder) {
  if (!encoder) return;
  if (encoder->channel_states) {
    for (int ch = 0; ch < encoder->channels; ch++) {
      sigma_delta_modulator_free(encoder->channel_states[ch].modulator);
    }
    free(encoder->channel_states);
  }
  if (encoder->coeffs) free(encoder->coeffs);
  free(encoder);
}

bool dsd_encoder_is_enabled(const dsd_encoder_t* encoder) {
  return encoder ? encoder->enabled : false;
}

void dsd_encoder_fill_silence(dsd_encoder_t* encoder, audio_chunk_t* chunk) {
  if (!encoder || !encoder->enabled || !chunk) return;
  size_t n = audio_chunk_get_valid_frames(chunk);
  if (n == 0 || (int)audio_chunk_get_channels(chunk) != encoder->channels)
    return;

  if (encoder->mode == DSD_MODE_NATIVE) {
    double sample_val = 0.0;
    if (encoder->dsd_bit_depth == 8) {
      sample_val = pcm_sample_decode_dsd_u8(0x69);
    } else if (encoder->dsd_bit_depth == 16) {
      sample_val = pcm_sample_decode_s16((int16_t)0x6969);
    } else if (encoder->dsd_bit_depth == 32) {
      uint32_t silence_word = 0x69696969;
      sample_val = pcm_sample_decode_f32_u32(silence_word);
    }

    for (int ch = 0; ch < encoder->channels; ch++) {
      mutable_waveform_t dst = audio_chunk_get_channel(chunk, ch);
      if (!dst) continue;
      for (size_t t = 0; t < n; t++) {
        dst[t] = sample_val;
      }
    }
  } else if (encoder->mode == DSD_MODE_DOP) {
    for (int ch = 0; ch < encoder->channels; ch++) {
      mutable_waveform_t dst = audio_chunk_get_channel(chunk, ch);
      if (!dst) continue;
      uint8_t marker = encoder->channel_states[ch].marker;
      for (size_t t = 0; t < n; t++) {
        uint32_t val24 = ((uint32_t)marker << 16) | 0x6969;
        int32_t int_val =
            (val24 & 0x800000) ? (int32_t)(val24 | 0xFF000000) : (int32_t)val24;
        dst[t] = pcm_sample_decode_s24(int_val);
        marker = (marker == 0x05) ? 0xFA : 0x05;
      }
      encoder->channel_states[ch].marker = marker;
    }
  }
}
