#ifndef CLIB_PROCESSORS_NOISE_GATE_PROCESSOR_H
#define CLIB_PROCESSORS_NOISE_GATE_PROCESSOR_H

/**
 * @file noise_gate_processor.h
 * @brief Multi-channel noise gate processor module.
 *
 * This module implements a noise gate that attenuates audio signals below a
 * specified loudness threshold.
 *
 * Noise Gate Thresholds & Envelope Detection Explanation:
 * 1. Channel Monitoring & Summing:
 *    - Monitored channels are summed together into a scratch buffer to evaluate
 * overall signal loudness.
 * 2. Envelope Detection (Loudness Estimation):
 *    - Instantaneous loudness in dB: val_db = 20.0 * log10(abs(sample) + 1e-9).
 *    - Exponential smoothing filter tracks envelope with first-order IIR
 * attack/release constants: attack = exp(-1.0 / (sample_rate * attack_time))
 *      release = exp(-1.0 / (sample_rate * release_time))
 * 3. Gate Threshold Logic:
 *    - If estimated loudness falls below the threshold (loudness < threshold):
 *      gain = factor = 10^(-attenuation_db / 20).
 *    - Otherwise (loudness >= threshold):
 *      gain = 1.0 (unity gain, gate open).
 * 4. Gain Application:
 *    - Linear gain multiplier is applied across all processed channels using
 * Apple Accelerate (vDSP) or scalar fallback.
 * 5. ZERO-ALLOCATION GUARANTEE: Real-time processing
 * (`noise_gate_processor_process`) performs no memory allocations. All scratch
 * buffers are pre-allocated during initialization.
 */

struct processor_vtable;
extern const struct processor_vtable g_noise_gate_vtable;

#endif  // CLIB_PROCESSORS_NOISE_GATE_PROCESSOR_H
