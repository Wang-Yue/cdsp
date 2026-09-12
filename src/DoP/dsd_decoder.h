#ifndef CLIB_DOP_DSD_DECODER_H
#define CLIB_DOP_DSD_DECODER_H

/**
 * @file dsd_decoder.h
 * @brief DSD (DoP / Native DSD) detection and decoding.
 *
 * DSD-over-PCM packs 16 1-bit DSD samples into the lower 16 bits of each
 * PCM frame; the upper byte carries a magic marker that alternates
 * `0x05` <-> `0xFA` between consecutive frames. We detect by looking for that
 * strict alternation and decode by streaming the recovered DSD bytes
 * through the same 511-tap Kaiser-windowed sinc the previous
 * `DSDPolyphaseDecimator` used (beta=11, cutoff = 20 kHz / dsd_rate),
 * resampling 16:1 back to the carrier rate.
 *
 * The detection state machine is hysteretic: 32 consecutive valid alternating
 * frames per channel to lock on, 64 consecutive bad frames to release. The
 * asymmetry kills the PCM <-> DSD flicker the previous "reset on a single bad
 * frame" code exhibited at chunk boundaries and around isolated bit errors.
 *
 * In Native DSD mode:
 * Raw DSD bitstreams (8-bit, 16-bit, or 32-bit containers, e.g. from the ASIO
 * hardware backend) are streamed directly into the decimation filter,
 * converting the native DSD stream into high-resolution PCM at the carrier rate
 * without requiring DoP markers.
 *
 * The hot path runs on the audio thread, so the decoder allocates nothing
 * per call. Per-channel state is a 64-byte ring FIFO of DSD bytes; the
 * convolution becomes 64 byte-indexed table lookups
 * (`acc += ctables[i][fifo[i]]`) - each table precomputes the contribution
 * of a byte at a given offset in the filter, replacing the per-bit
 * conditional add. Filter shape, tap count, and cutoff are unchanged from
 * the previous design, so the SINAD numbers the existing tests pin down
 * across DSD64 / 128 / 256 at 44.1 / 48 kHz families are preserved.
 */

#include <stdbool.h>
#include <stddef.h>

#include "Audio/audio_chunk.h"
#include "Config/engine_config_types.h"

/**
 * @brief DSD detection and decoding engine.
 */
typedef struct dsd_decoder dsd_decoder_t;

/**
 * @brief Create a DSD decoder instance.
 *
 * @param channels Number of audio channels.
 * @param sample_rate The PCM sample rate (carrier rate).
 * @param mode DSD processing mode (DSD_MODE_PCM, DSD_MODE_DOP, or
 * DSD_MODE_NATIVE).
 * @param dsd_bit_depth DSD container bit depth (8, 16, or 32). Ignored for DoP
 * (fixed at 16).
 * @param bypass_dop If true, DoP detection is disabled and input is passed
 * through.
 * @param cutoff_hz Passband cutoff of the post-DSD lowpass (typically 20000.0).
 *                  Lower values trade ultrasonic passband for higher SINAD.
 * @param multithreaded True if multi-threaded parallelization is enabled.
 * @return Pointer to the allocated dsd_decoder_t instance, or NULL on failure.
 */
dsd_decoder_t* dsd_decoder_create(int channels, double sample_rate,
                                  dsd_mode_t mode, size_t dsd_bit_depth,
                                  bool bypass_dop, double cutoff_hz,
                                  bool multithreaded);

/**
 * @brief Detect DoP / process Native DSD and (when active) decode the chunk in
 * place.
 *
 * @param decoder Pointer to the DSD decoder.
 * @param chunk Pointer to the audio chunk to process.
 * @return True if the chunk was decoded as DSD, false if processed as PCM.
 */
bool dsd_decoder_process(dsd_decoder_t* decoder, audio_chunk_t* chunk);

/**
 * @brief Detect DoP and (when active) decode the chunk in place (alias for
 * dsd_decoder_process).
 *
 * @param decoder Pointer to the DSD decoder.
 * @param chunk Pointer to the audio chunk to process.
 * @return True if the chunk was decoded as DSD, false if processed as PCM.
 */
bool dsd_decoder_detect_and_process(dsd_decoder_t* decoder,
                                    audio_chunk_t* chunk);

/**
 * @brief Check if DSD decoding is globally active (any channel has lock).
 *
 * @param decoder Pointer to the DSD decoder.
 * @return True if DSD is active, false otherwise.
 */
bool dsd_decoder_is_active(const dsd_decoder_t* decoder);

/**
 * @brief Free the DSD decoder instance and its resources.
 *
 * @param decoder Pointer to the DSD decoder to free.
 */
void dsd_decoder_free(dsd_decoder_t* decoder);

/**
 * @brief Get the configured DSD mode of the decoder.
 *
 * @param decoder Pointer to the DSD decoder.
 * @return Active DSD mode.
 */
dsd_mode_t dsd_decoder_get_mode(const dsd_decoder_t* decoder);

/**
 * @brief Get the configured container bit depth of the decoder.
 *
 * @param decoder Pointer to the DSD decoder.
 * @return Container bit depth (8, 16, or 32).
 */
size_t dsd_decoder_get_bit_depth(const dsd_decoder_t* decoder);

#endif  // CLIB_DOP_DSD_DECODER_H
