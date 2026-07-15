#ifndef CLIB_PROCESSORS_COMPRESSOR_PROCESSOR_H
#define CLIB_PROCESSORS_COMPRESSOR_PROCESSOR_H

/**
 * @file compressor_processor.h
 * @brief Dynamic range compressor processor module.
 *
 * This module implements a multi-channel dynamic range compressor with optional
 * soft/hard clipping limiter.
 *
 * Compressor Envelope Detection & Gain Reduction Curves Explanation:
 * 1. Channel Monitoring & Summing:
 *    - The compressor monitors a specified subset of input channels (or all
 * channels if unspecified).
 *    - The monitored channels are summed together into a scratch buffer to
 * evaluate overall signal level.
 * 2. Envelope Detection (Loudness Estimation):
 *    - For each sample, the instantaneous loudness in dB is calculated as:
 *      val_db = 20.0 * log10(abs(sample) + 1e-9).
 *    - A first-order IIR exponential smoothing filter tracks the envelope:
 *      - Attack coefficient: attack = exp(-1.0 / (sample_rate * attack_time)).
 *      - Release coefficient: release = exp(-1.0 / (sample_rate *
 * release_time)).
 *      - If val_db >= prev_loudness (signal level rising):
 *        loudness = attack * prev_loudness + (1.0 - attack) * val_db.
 *      - If val_db < prev_loudness (signal level falling):
 *        loudness = release * prev_loudness + (1.0 - release) * val_db.
 * 3. Gain Reduction Curve:
 *    - If the estimated loudness exceeds the threshold (loudness > threshold):
 *      gain_reduction_db = -(loudness - threshold) * (factor - 1.0) / factor.
 *    - Otherwise (loudness <= threshold):
 *      gain_reduction_db = 0.0 dB (unity gain).
 *    - The final linear gain multiplier applied to processing channels is:
 *      lin_gain = 10^((gain_reduction_db + makeup_gain_db) / 20).
 * 4. Limiting:
 *    - An optional post-compression limiter (`limiter_filter_t`) prevents
 * clipping on processed channels.
 * 5. ZERO-ALLOCATION GUARANTEE: Real-time processing
 * (`compressor_processor_process`) performs no memory allocations. All scratch
 * buffers are pre-allocated during initialization.
 */

struct processor_vtable;
extern const struct processor_vtable g_compressor_vtable;

#endif  // CLIB_PROCESSORS_COMPRESSOR_PROCESSOR_H
