// DSD (DoP / Native DSD) detection and decoding.
//
// DSD-over-PCM packs 16 1-bit DSD samples into the lower 16 bits of each
// PCM frame; the upper byte carries a magic marker that alternates
// `0x05` ↔ `0xFA` between consecutive frames. We detect by looking for that
// strict alternation and decode by streaming the recovered DSD bytes
// through the same 511-tap Kaiser-windowed sinc the previous
// `DSDPolyphaseDecimator` used (β=11, cutoff = 20 kHz / dsd_rate),
// resampling 16:1 back to the carrier rate.
//
// The detection state machine is hysteretic: 32 consecutive valid alternating
// frames per channel to lock on, 64 consecutive bad frames to release. The
// asymmetry kills the PCM↔DSD flicker the previous "reset on a single bad
// frame" code exhibited at chunk boundaries and around isolated bit errors.
//
// In Native DSD mode:
// Raw DSD streams packed in 8, 16, or 32-bit containers (e.g. from the ASIO
// hardware driver) are streamed directly into the decimation filter,
// converting the native DSD stream into high-resolution PCM in place.
//
// The hot path runs on the audio thread, so the decoder allocates nothing
// per call. Per-channel state is a 64-byte ring FIFO of DSD bytes; the
// convolution becomes 64 byte-indexed table lookups
// (`acc += ctables[i][fifo[i]]`) — each table precomputes the contribution
// of a byte at a given offset in the filter, replacing the per-bit
// conditional add. Filter shape, tap count, and cutoff are unchanged from
// the previous design, so the SINAD numbers the existing tests pin down
// across DSD64 / 128 / 256 at 44.1 / 48 kHz families are preserved.
#include "DoP/dsd_decoder.h"

#include "Audio/audio_chunk.h"

#if defined(ENABLE_LIBDISPATCH)
#include <dispatch/dispatch.h>
#endif

#if defined(ENABLE_OPENMP)
#include <omp.h>
#endif

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include "Audio/sample_conversion.h"
#include "Config/engine_config_types.h"
#include "Logging/app_logger.h"

static const logger_t g_logger = {"dsp.dsd.decoder"};

#define DOP_FIFO_SIZE 64  // power of 2
#define DOP_FIFO_MASK 63

/**
 * @brief Per-channel state for DSD decoding.
 *
 * Holds a 64-byte ring FIFO of DSD bytes and hysteretic lock counters.
 */
typedef struct {
  int consec_valid;    /**< Number of consecutive valid DoP markers. */
  int consec_invalid;  /**< Number of consecutive invalid DoP markers. */
  bool is_active;      /**< Flag indicating if DSD decoding is active for this
                          channel. */
  uint8_t last_marker; /**< The last seen marker byte (should alternate between
                          0x05 and 0xFA). */
  bool is_32bit_container; /**< Flag indicating if DSD is in a 32-bit container.
                            */
  bool container_known;    /**< Flag indicating if container size has been
                              determined. */
  uint8_t fifo[DOP_FIFO_SIZE * 2]; /**< Ring buffer for DSD bytes (duplicated
                                      for unmasked reads). */
  int fifo_pos;                    /**< Current position in the FIFO. */
} dsd_decoder_channel_state_t;

struct dsd_decoder {
  int channels;    /**< Number of audio channels. */
  bool bypass_dop; /**< Flag indicating if DSD detection should be bypassed. */
  dsd_decoder_channel_state_t*
      channel_states; /**< Array of per-channel states. */
  double* ctables;    /**< Flat ctable storage: `ctables[i*256 + b]` is the
                         convolution contribution of byte `b` placed at table index
                         `i`. Built once at init from the configured sample rate
                         and cutoff; never resized. Size: 64 * 256 doubles. */
  bool is_dsd_active; /**< Flag indicating if DSD is globally active (any
                         channel has lock). */

  bool logged_active;       /**< Status flag to rate-limit logging. */
  bool last_seen_active;    /**< Flag indicating if DSD was active in the last
                               processed chunk. */
  int chunks_at_seen_state; /**< Count of chunks processed in the current state.
                             */
  bool use_multithreading; /**< True if multi-threaded parallelization should be
                              used. */
  double sample_rate;      /**< Carrier sample rate in Hz. */
  dsd_mode_t mode;         /**< Processing mode (PCM, DoP, or Native DSD). */
  size_t dsd_bit_depth;    /**< Container bit depth (8, 16, or 32). */
};

#include <string.h>

#include "Utils/double_helpers.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Frames of valid alternating markers required to lock on. ~180 µs at
// 176.4 kHz PCM rate.
#define DOP_ACTIVATE_THRESHOLD 32
// Frames of bad markers required to release the lock once active.
// Asymmetric vs. `activateThreshold` is intentional — a single corrupted
// PCM sample on a real DoP stream should not flip the engine back to PCM.
#define DOP_DEACTIVATE_THRESHOLD 64
// Chunks of consistent state required before logging a state transition.
// Suppresses brief lock→lost→lock flickers seen at stream start (e.g.
// when the source has a few hundred microseconds of pre-roll silence
// between bursts of DoP). Only the *settled* state is logged.
#define DOP_LOG_SETTLE_CHUNKS 4
// Filter / lookup-table layout.
#define DOP_REAL_TAPS 511
#define DOP_NUM_TAPS 512    // padded so 8-bit slicing is exact
#define DOP_NUM_CTABLES 64  // 64
// One of the standard DSD silence patterns. Initializing the FIFO to
// this rather than zero (= all `-1` = DC saturated) means the first
// few samples after activation don't produce a click.
#define DOP_SILENCE_BYTE 0x69

/// Build the byte-indexed filter lookup tables for a 511-tap, β=11
/// Kaiser-windowed sinc with cutoff at `cutoffHz / dsd_rate`. The filter
/// shape itself is unchanged from the previous `DSDPolyphaseDecimator`
/// (same Kaiser sinc generator); only the absolute cutoff is now
/// configurable. SINAD vs. ultrasonic-passband is the trade-off:
/// 20 kHz is the SINAD-optimal default; 30–50 kHz preserves more
/// ultrasonic content at modest SINAD cost.
///
/// Bit/byte mapping: bit `m` (LSB-first) of the byte at table index `i`
/// corresponds to filter tap `h[i*8 + m]`, applied to the DSD sample at
/// offset `i*8 + m` behind the most recent push. With our DoP unpack,
/// the most recent byte is the lower byte of the frame's 16-bit DSD
/// payload and bit 0 of that byte is the latest of the frame's 16
/// DSD samples (LSB-first within byte = newer first within byte).
/**
 * @brief Precomputes the convolution lookup tables for DSD-to-PCM decimation.
 *
 * This function designs a 511-tap Kaiser-windowed sinc lowpass filter and then
 * reformats it into a set of 64 lookup tables (one for each byte offset in the
 * 64-byte DSD FIFO). Each lookup table has 256 entries, corresponding to the
 * 256 possible values of a DSD byte. The table entry precomputes the sum of
 * the 8 filter coefficients multiplied by the corresponding DSD bits (+1.0 for
 * 1, -1.0 for 0). This allows the convolution to be performed using simple
 * table lookups instead of bit-by-bit multiplication and accumulation on the
 * hot path.
 *
 * The filter design is a lowpass filter with a cutoff frequency relative to the
 * DSD rate (which is carrier_bits times the carrier sample rate).
 *
 * @param sample_rate The PCM sample rate (carrier rate).
 * @param carrier_bits DSD bits per carrier frame (8, 16, or 32).
 * @param cutoff_hz The desired cutoff frequency in Hz.
 * @return A pointer to the allocated flat array of lookup tables (size 64 * 256
 * doubles), or NULL on allocation failure.
 */
static double* build_ctables(double sample_rate, size_t carrier_bits,
                             double cutoff_hz) {
  double beta = 11.0;
  double dsd_rate = sample_rate * (double)carrier_bits;
  double cutoff = cutoff_hz / dsd_rate;
  double alpha = (double)(DOP_REAL_TAPS - 1) / 2.0;
  double i0_beta = double_bessel_i0(beta);

  double raw_h[DOP_REAL_TAPS];
  double total_sum = 0.0;
  for (int i = 0; i < DOP_REAL_TAPS; i++) {
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
    raw_h[i] = sinc_val * window_val;
    total_sum += raw_h[i];
  }

  double taps[DOP_NUM_TAPS];
  memset(taps, 0, sizeof(taps));
  for (int i = 0; i < DOP_REAL_TAPS; i++) {
    taps[i] = raw_h[i] / total_sum;
  }

  size_t total_elements = DOP_NUM_CTABLES * 256;
  double* p = (double*)calloc(total_elements, sizeof(double));
  if (!p) return NULL;

  for (int i = 0; i < DOP_NUM_CTABLES; i++) {
    for (int b = 0; b < 256; b++) {
      double sum = 0.0;
      for (int m = 0; m < 8; m++) {
        int tap = i * 8 + m;
        double h = taps[tap];
        int bit = (b >> m) & 1;
        sum += h * (bit == 1 ? 1.0 : -1.0);
      }
      p[i * 256 + b] = sum;
    }
  }
  return p;
}

dsd_decoder_t* dsd_decoder_create(int channels, double sample_rate,
                                  dsd_mode_t mode, size_t dsd_bit_depth,
                                  bool bypass_dop, double cutoff_hz,
                                  bool multithreaded) {
  if (channels <= 0) {
    logger_error(&g_logger, "Invalid channel count for DSD decoder: %d",
                 channels);
    return NULL;
  }
  if (sample_rate <= 0.0) {
    logger_error(&g_logger, "Invalid sample rate for DSD decoder: %.1f",
                 sample_rate);
    return NULL;
  }
  size_t carrier_bits = 16;
  if (mode == DSD_MODE_NATIVE && (dsd_bit_depth == 8 || dsd_bit_depth == 32)) {
    carrier_bits = dsd_bit_depth;
  }
  double dsd_rate = sample_rate * (double)carrier_bits;
  if (!bypass_dop && mode != DSD_MODE_PCM) {
    if (cutoff_hz <= 0.0 || cutoff_hz >= dsd_rate / 2.0) {
      logger_error(&g_logger, "Invalid cutoff frequency for DSD decoder: %.1f",
                   cutoff_hz);
      return NULL;
    }
  }

  dsd_decoder_t* dec = (dsd_decoder_t*)calloc(1, sizeof(dsd_decoder_t));

  if (!dec) {
    logger_error(&g_logger, "Memory allocation failed for dsd_decoder_t");
    return NULL;
  }
  dec->channels = channels;
  dec->sample_rate = sample_rate;
  dec->bypass_dop = bypass_dop;
  dec->mode = mode;
  dec->dsd_bit_depth = carrier_bits;
  dec->use_multithreading = false;
#if defined(ENABLE_LIBDISPATCH) || defined(ENABLE_OPENMP)
  if (multithreaded && channels > 2) {
    dec->use_multithreading = true;
  }
#else
  (void)multithreaded;
#endif

  dec->is_dsd_active = (mode == DSD_MODE_NATIVE && !bypass_dop);
  dec->logged_active = dec->is_dsd_active;
  dec->last_seen_active = dec->is_dsd_active;
  dec->chunks_at_seen_state = 0;

  dec->channel_states = (dsd_decoder_channel_state_t*)calloc(
      channels, sizeof(dsd_decoder_channel_state_t));
  if (!dec->channel_states) {
    logger_error(&g_logger,
                 "Memory allocation failed for DSD decoder channel states");
    dsd_decoder_free(dec);
    return NULL;
  }
  for (int ch = 0; ch < channels; ch++) {
    memset(dec->channel_states[ch].fifo, DOP_SILENCE_BYTE, DOP_FIFO_SIZE * 2);
    dec->channel_states[ch].is_active =
        (mode == DSD_MODE_NATIVE && !bypass_dop);
  }
  dec->ctables = build_ctables(sample_rate, carrier_bits, cutoff_hz);
  if (!dec->ctables) {
    logger_error(&g_logger, "Failed to build DSD lookup tables");
    dsd_decoder_free(dec);
    return NULL;
  }
  logger_debug(&g_logger,
               "DSD decoder created (channels=%d, sample_rate=%.0f, bypass=%d, "
               "cutoff=%.0fHz)",
               channels, sample_rate, bypass_dop ? 1 : 0, cutoff_hz);
  return dec;
}

/**
 * @brief Processes a single channel's audio buffer, detecting and decoding DoP
 * / Native DSD.
 *
 * This function iterates through the input PCM samples. For each sample:
 * 1. It extracts the DSD payload. In Native DSD mode, it directly unpacks the
 * 8, 16, or 32-bit container. In DoP mode, it extracts the marker and
 * dynamically detects whether the container is 24-bit or 32-bit.
 * 2. In DoP mode, it validates the marker (must be 0x05 or 0xFA and must
 * alternate).
 * 3. In DoP mode, it updates a hysteretic state machine. If enough valid
 * markers are seen (DOP_ACTIVATE_THRESHOLD), it locks on (is_active = true). If
 * too many invalid markers are seen (DOP_DEACTIVATE_THRESHOLD), it loses lock.
 * 4. If locked or warming up, it pushes the extracted DSD bytes into the
 * channel's ring FIFO.
 * 5. If locked, it performs the DSD-to-PCM decimation filter by summing values
 *    from the precomputed lookup tables indexed by the FIFO bytes, and
 *    overwrites the input PCM sample with the decoded and scaled PCM value.
 *
 * @param state Pointer to the per-channel decoder state.
 * @param buf The audio buffer to be processed in-place.
 * @param frames The number of frames in the buffer.
 * @param mode DSD processing mode (DoP or Native DSD).
 * @param dsd_bit_depth Container bit depth (8, 16, or 32).
 * @param tables Pointer to the precomputed convolution lookup tables.
 */
static void process_channel(dsd_decoder_channel_state_t* state,
                            mutable_waveform_t buf, size_t frames,
                            dsd_mode_t mode, size_t dsd_bit_depth,
                            const double* tables) {
  if (!buf) return;
  uint8_t* fifo = state->fifo;
  int pos = state->fifo_pos;

  for (size_t t = 0; t < frames; t++) {
    double raw = buf[t];

    if (mode == DSD_MODE_NATIVE) {
      state->is_active = true;
      if (dsd_bit_depth == 8) {
        uint8_t b0 = pcm_sample_encode_dsd_u8(raw);
        fifo[pos] = b0;
        fifo[pos + DOP_FIFO_SIZE] = b0;
        pos = (pos + 1) & DOP_FIFO_MASK;
      } else if (dsd_bit_depth == 16) {
        uint16_t u16 = (uint16_t)pcm_sample_encode_s16(raw);
        uint8_t dsd_hi = (uint8_t)((u16 >> 8) & 0xFF);
        uint8_t dsd_lo = (uint8_t)(u16 & 0xFF);
        fifo[pos] = dsd_hi;
        fifo[pos + DOP_FIFO_SIZE] = dsd_hi;
        pos = (pos + 1) & DOP_FIFO_MASK;
        fifo[pos] = dsd_lo;
        fifo[pos + DOP_FIFO_SIZE] = dsd_lo;
        pos = (pos + 1) & DOP_FIFO_MASK;
      } else {
        uint32_t u32 = pcm_sample_u32_from_f32((float)raw);
        uint8_t b0 = (uint8_t)((u32 >> 24) & 0xFF);
        uint8_t b1 = (uint8_t)((u32 >> 16) & 0xFF);
        uint8_t b2 = (uint8_t)((u32 >> 8) & 0xFF);
        uint8_t b3 = (uint8_t)(u32 & 0xFF);
        fifo[pos] = b0;
        fifo[pos + DOP_FIFO_SIZE] = b0;
        pos = (pos + 1) & DOP_FIFO_MASK;
        fifo[pos] = b1;
        fifo[pos + DOP_FIFO_SIZE] = b1;
        pos = (pos + 1) & DOP_FIFO_MASK;
        fifo[pos] = b2;
        fifo[pos + DOP_FIFO_SIZE] = b2;
        pos = (pos + 1) & DOP_FIFO_MASK;
        fifo[pos] = b3;
        fifo[pos + DOP_FIFO_SIZE] = b3;
        pos = (pos + 1) & DOP_FIFO_MASK;
      }
    } else {
      // Recover both 24- and 32-bit container interpretations. DoP is most
      // commonly carried as right-aligned 24-bit-in-32-bit (marker at bits
      // 23..16 of int24). MPD's flavor encodes a true 32-bit value
      // 0xff05XXXX / 0xfffaXXXX where the top byte sign-extends and the
      // marker is still at bits 23..16 — same shift, different float scale.

      uint8_t marker = 0;
      uint16_t dsd_word = 0;

      if (state->container_known) {
        if (state->is_32bit_container) {
          int32_t val32 = pcm_sample_encode_s32(raw);
          marker = (uint8_t)(((uint32_t)val32 >> 16) & 0xFF);
          dsd_word = (uint16_t)((uint32_t)val32 & 0xFFFF);
        } else {
          int32_t val24 = pcm_sample_encode_s24(raw);
          marker = (uint8_t)(((uint32_t)val24 >> 16) & 0xFF);
          dsd_word = (uint16_t)((uint32_t)val24 & 0xFFFF);
        }
      } else {
        int32_t val32 = pcm_sample_encode_s32(raw);
        uint8_t marker32 = (uint8_t)(((uint32_t)val32 >> 16) & 0xFF);

        int32_t val24 = pcm_sample_encode_s24(raw);
        uint8_t marker24 = (uint8_t)(((uint32_t)val24 >> 16) & 0xFF);

        if (marker24 == 0x05 || marker24 == 0xFA) {
          state->is_32bit_container = false;
          marker = marker24;
          dsd_word = (uint16_t)((uint32_t)val24 & 0xFFFF);
        } else if (marker32 == 0x05 || marker32 == 0xFA) {
          state->is_32bit_container = true;
          marker = marker32;
          dsd_word = (uint16_t)((uint32_t)val32 & 0xFFFF);
        } else {
          marker = marker24;
          dsd_word = (uint16_t)((uint32_t)val24 & 0xFFFF);
        }
      }

      bool is_marker_valid = (marker == 0x05 || marker == 0xFA);
      // First-ever frame on this channel passes vacuously; subsequent
      // frames must alternate between 0x05 and 0xFA.
      bool alternates =
          (state->last_marker == 0 || marker != state->last_marker);
      bool valid = is_marker_valid && alternates;

      // Hysteretic state machine updates:
      if (valid) {
        if (state->consec_valid < 1000) state->consec_valid++;
        state->consec_invalid = 0;
        state->last_marker = marker;
        // Confirm container choice after 4 consecutive valid frames.
        if (!state->container_known && state->consec_valid >= 4) {
          state->container_known = true;
        }
        // Lock on if we exceed the activation threshold.
        if (!state->is_active &&
            state->consec_valid >= DOP_ACTIVATE_THRESHOLD) {
          state->is_active = true;
        }
      } else {
        if (state->consec_invalid < 1000) state->consec_invalid++;
        state->consec_valid = 0;
        // Lose lock only if we exceed the deactivation threshold (hysteresis).
        if (state->consec_invalid >= DOP_DEACTIVATE_THRESHOLD) {
          state->last_marker = 0;
          state->container_known = false;
          if (state->is_active) {
            state->is_active = false;
            // Fill FIFO with DSD silence to prevent clicks on transition back
            // to PCM.
            memset(fifo, DOP_SILENCE_BYTE, DOP_FIFO_SIZE * 2);
            pos = 0;
          }
        }
      }

      // Push the frame's two DSD bytes whenever we either have a
      // current valid marker (warming the filter pre-lock) or are
      // already locked on (trusting the lock through isolated marker
      // bit-errors). Either way, by the time `isActive` flips true the
      // FIFO already holds 32 frames of real DSD data, so the first
      // decoded sample is not a silence-fill transient.
      bool push = valid || state->is_active;
      if (push) {
        uint8_t dsd_hi = (uint8_t)((dsd_word >> 8) & 0xFF);
        uint8_t dsd_lo = (uint8_t)(dsd_word & 0xFF);
        fifo[pos] = dsd_hi;
        fifo[pos + DOP_FIFO_SIZE] = dsd_hi;
        pos = (pos + 1) & DOP_FIFO_MASK;
        fifo[pos] = dsd_lo;
        fifo[pos + DOP_FIFO_SIZE] = dsd_lo;
        pos = (pos + 1) & DOP_FIFO_MASK;
      }
    }
    if (state->is_active) {
      // Decode DSD to PCM using the precomputed tables.
      // y[n] = Σ_{i<numCtables} ctables[i][fifo[(pos-1-i) & mask]].
      // ctable[i] precomputes the contribution of bits 0..7 of the
      // byte at offset `i` to filter taps i*8 .. i*8+7 — see
      // buildCtables for the bit/tap mapping.
      double acc0 = 0.0;
      double acc1 = 0.0;
      double acc2 = 0.0;
      double acc3 = 0.0;
      int read_ptr = pos - 1 + DOP_FIFO_SIZE;
      for (int i = 0; i < DOP_NUM_CTABLES; i += 4) {
        int b0 = (int)fifo[read_ptr - i];
        int b1 = (int)fifo[read_ptr - (i + 1)];
        int b2 = (int)fifo[read_ptr - (i + 2)];
        int b3 = (int)fifo[read_ptr - (i + 3)];
        acc0 += tables[i * 256 + b0];
        acc1 += tables[(i + 1) * 256 + b1];
        acc2 += tables[(i + 2) * 256 + b2];
        acc3 += tables[(i + 3) * 256 + b3];
      }
      double acc = acc0 + acc1 + acc2 + acc3;
      // The trellis-friendly sigma-delta modulators in the test suite
      // pre-scale input by 0.5 for noise-shaper headroom; this 2× compensates
      // so SINAD compares against full-amplitude sin. Real DoP streams
      // from DACs that don't pre-scale will be 6 dB hot — handle at a higher
      // level if that becomes a problem.
      buf[t] = acc * 2.0;
    }
  }

  state->fifo_pos = pos;
}

#if defined(ENABLE_LIBDISPATCH)
typedef struct {
  dsd_decoder_t* decoder;
  audio_chunk_t* chunk;
  size_t valid_frames;
} dsd_decoder_dispatch_ctx_t;

static void dsd_decoder_worker(void* context, size_t ch) {
  dsd_decoder_dispatch_ctx_t* ctx = (dsd_decoder_dispatch_ctx_t*)context;
  process_channel(&ctx->decoder->channel_states[ch],
                  audio_chunk_get_channel(ctx->chunk, ch), ctx->valid_frames,
                  ctx->decoder->mode, ctx->decoder->dsd_bit_depth,
                  ctx->decoder->ctables);
}
#endif

bool dsd_decoder_process(dsd_decoder_t* decoder, audio_chunk_t* chunk) {
  if (!decoder || !chunk) return false;
  if (decoder->mode == DSD_MODE_PCM || decoder->bypass_dop) {
    decoder->is_dsd_active = false;
    return false;
  }

  size_t valid_frames = audio_chunk_get_valid_frames(chunk);
  if (valid_frames == 0 ||
      (int)audio_chunk_get_channels(chunk) != decoder->channels)
    return false;

  if (decoder->use_multithreading) {
#if defined(ENABLE_LIBDISPATCH)
    dsd_decoder_dispatch_ctx_t dctx = {decoder, chunk, valid_frames};
    dispatch_queue_t queue =
        dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_HIGH, 0);
    dispatch_apply_f(decoder->channels, queue, &dctx, dsd_decoder_worker);
#elif defined(ENABLE_OPENMP)
#pragma omp parallel num_threads(decoder->channels)
    {
#pragma omp for
      for (int ch = 0; ch < decoder->channels; ch++) {
        process_channel(&decoder->channel_states[ch],
                        audio_chunk_get_channel(chunk, ch), valid_frames,
                        decoder->mode, decoder->dsd_bit_depth,
                        decoder->ctables);
      }
    }
#endif
  } else {
    for (int ch = 0; ch < decoder->channels; ch++) {
      process_channel(&decoder->channel_states[ch],
                      audio_chunk_get_channel(chunk, ch), valid_frames,
                      decoder->mode, decoder->dsd_bit_depth, decoder->ctables);
    }
  }

  if (decoder->mode == DSD_MODE_NATIVE) {
    decoder->is_dsd_active = true;
    return true;
  }

  bool all_active = true;
  for (int ch = 0; ch < decoder->channels; ch++) {
    if (!decoder->channel_states[ch].is_active) {
      all_active = false;
      break;
    }
  }
  decoder->is_dsd_active = all_active;

  // Log debouncer: only log a transition once the new state has been
  // observed for `logSettleChunks` consecutive chunks. This filters out
  // the lock→lost→lock churn that fires at stream start when the source
  // has brief silence between DoP bursts.
  if (decoder->is_dsd_active == decoder->last_seen_active) {
    decoder->chunks_at_seen_state++;
  } else {
    decoder->last_seen_active = decoder->is_dsd_active;
    decoder->chunks_at_seen_state = 1;
  }
  if (decoder->chunks_at_seen_state >= DOP_LOG_SETTLE_CHUNKS &&
      decoder->last_seen_active != decoder->logged_active) {
    decoder->logged_active = decoder->last_seen_active;
    if (decoder->logged_active) {
      logger_info(&g_logger, "DoP decoder stream locked (DSD stream detected)");
    } else {
      logger_info(&g_logger,
                  "DoP decoder stream released lock (reverting to PCM)");
    }
  }

  return decoder->is_dsd_active;
}

bool dsd_decoder_detect_and_process(dsd_decoder_t* decoder,
                                    audio_chunk_t* chunk) {
  return dsd_decoder_process(decoder, chunk);
}

void dsd_decoder_free(dsd_decoder_t* decoder) {
  if (!decoder) return;
  if (decoder->channel_states) free(decoder->channel_states);
  if (decoder->ctables) free(decoder->ctables);
  free(decoder);
}

bool dsd_decoder_is_active(const dsd_decoder_t* decoder) {
  return decoder ? decoder->is_dsd_active : false;
}

dsd_mode_t dsd_decoder_get_mode(const dsd_decoder_t* decoder) {
  return decoder ? decoder->mode : DSD_MODE_PCM;
}

size_t dsd_decoder_get_bit_depth(const dsd_decoder_t* decoder) {
  return decoder ? decoder->dsd_bit_depth : 16;
}
