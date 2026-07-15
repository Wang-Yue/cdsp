/**
 * @file dsd_encoder.h
 * @brief PCM to DSD (DoP / Native DSD) encoder.
 *
 * Converts a chunk of PCM audio at the carrier rate into DSD-over-PCM (DoP) or
 * Native DSD (8, 16, or 32-bit container format), in place. For each input
 * frame it:
 * 1. Interpolates to the DSD rate using a 511-tap beta=11 Kaiser-windowed
 *    polyphase sinc (same shape as the decoder, normalized per phase
 *    for unit DC gain).
 * 2. Modulates the oversampled signal with a per-channel sigma-delta
 *    modulator (using the configured `SDMFilter`, defaulting to `sdm-6`).
 * 3. Packs DSD bits into 8, 16, or 32-bit containers (or 24-bit DoP container
 *    with an alternating `0x05` / `0xFA` marker in the upper byte).
 *
 * The encoded chunk satisfies the strict-alternation detection state
 * machine in `DoPDecoder` when in DoP mode and round-trips through any DAC that
 * natively understands DoP or Native DSD. To preserve the bit pattern through
 * CoreAudio in DoP mode, the playback format must be S24 or S32 (F32 will
 * quantize the marker away); the encoder itself just emits float-normalised
 * 24-bit or DSD values and trusts the playback backend to forward them
 * losslessly.
 *
 * SDM state per channel is carried by an embedded `SigmaDeltaModulator`;
 * the polyphase coefficient table is shared across channels and built
 * once at init.
 */

#ifndef CLIB_DOP_DSD_ENCODER_H
#define CLIB_DOP_DSD_ENCODER_H

#include <stdbool.h>

#include "Audio/audio_chunk.h"
#include "Config/engine_config_types.h"

/**
 * @brief DSD encoder context.
 */
typedef struct dsd_encoder dsd_encoder_t;

/**
 * @brief Construct a DSD/DoP encoder.
 *
 * Encodes when `mode` is `DSD_MODE_DOP` or `DSD_MODE_NATIVE` and `sample_rate`
 * is a supported carrier rate. When `mode` is `DSD_MODE_PCM`, the encoder is
 * disabled and `dsd_encoder_encode` becomes a no-op.
 *
 * @param channels Number of audio channels.
 * @param sample_rate The input PCM audio sample rate.
 * @param mode DSD processing mode (DSD_MODE_PCM, DSD_MODE_DOP, or
 * DSD_MODE_NATIVE).
 * @param dsd_bit_depth DSD container bit depth per output frame (8, 16, or 32).
 * @param filter_name Noise-shaper filter name.
 * @param cutoff_hz Passband cutoff of the interpolation filter.
 *                  Ignored when `enabled` is false.
 * @return Pointer to the created dsd_encoder_t instance.
 */
dsd_encoder_t* dsd_encoder_create(int channels, size_t sample_rate,
                                  dsd_mode_t mode, size_t dsd_bit_depth,
                                  sdm_filter_t filter_name, double cutoff_hz);

/**
 * @brief Encode the chunk's PCM samples into DoP/DSD, in place.
 *
 * No-op when `enabled` is false, the chunk is empty, or the channel
 * count doesn't match what the encoder was constructed with.
 *
 * @param encoder Pointer to the encoder instance.
 * @param chunk Pointer to the audio chunk to encode.
 */
void dsd_encoder_encode(dsd_encoder_t* encoder, audio_chunk_t* chunk);

/**
 * @brief Free the DSD encoder and its resources.
 *
 * @param encoder Pointer to the encoder instance to free.
 */
void dsd_encoder_free(dsd_encoder_t* encoder);

/**
 * @brief Check if a carrier rate is supported for DoP or native DSD encoding.
 *
 * For DoP encoding (DSD_MODE_DOP), container format is fixed at 16 DSD bits per
 * 24-bit container frame, supporting carrier rates: 176.4k, 192k, 352.8k, 384k,
 * 705.6k, 768k Hz (DSD64/128/256).
 * For Native DSD encoding (DSD_MODE_NATIVE), container formats (8, 16, or 32
 * bits) support carrier rates: 88.2k, 96k, 176.4k, 192k, 352.8k, 384k, 705.6k,
 * 768k, 1411.2k, 1536k Hz (DSD64/128/256).
 *
 * @param rate The carrier rate to check.
 * @param mode DSD processing mode (DSD_MODE_DOP or DSD_MODE_NATIVE).
 * @return True if the rate is supported for the given mode, false otherwise.
 */
bool dsd_encoder_is_supported_carrier_rate(int rate, dsd_mode_t mode);

/**
 * @brief Check if the DSD encoder is enabled.
 *
 * @param encoder Pointer to the encoder instance.
 * @return True if enabled, false otherwise.
 */
bool dsd_encoder_is_enabled(const dsd_encoder_t* encoder);

/**
 * @brief Fill an audio chunk with DSD silence.
 *
 * For Native DSD mode, fills with DSD silence pattern 0x69 decoded to samples.
 * For DoP mode, fills with 16-bit DSD silence 0x6969 and alternating 0x05 /
 * 0xFA markers.
 *
 * @param encoder Pointer to the encoder instance.
 * @param chunk Pointer to the audio chunk to fill.
 */
void dsd_encoder_fill_silence(dsd_encoder_t* encoder, audio_chunk_t* chunk);

#endif  // CLIB_DOP_DSD_ENCODER_H
