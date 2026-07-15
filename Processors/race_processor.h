#ifndef CLIB_PROCESSORS_RACE_PROCESSOR_H
#define CLIB_PROCESSORS_RACE_PROCESSOR_H

/**
 * @file race_processor.h
 * @brief RACE (Recursive Attenuator and Cross-talk Cancellation) processor
 * module.
 *
 * This module implements the RACE algorithm for binaural/stereo acoustic
 * cross-talk cancellation.
 *
 * RACE Cross-Talk Cancellation Math Explanation:
 * 1. Acoustic Cross-Talk Problem:
 *    - When listening to stereo loudspeakers, sound from the left speaker
 * reaches the right ear (contralateral path) after a short time delay and
 * acoustic attenuation, and vice versa.
 *    - This cross-talk degrades spatial imaging and binaural cues.
 * 2. Recursive Cross-Talk Cancellation:
 *    - To cancel the contralateral signal at the listener's ears, a delayed and
 * attenuated inverted version of the contralateral channel is added to the
 * ipsilateral channel.
 *    - Because adding a cancellation signal creates a secondary cross-talk path
 * (which in turn must be cancelled), a recursive feedback loop is employed.
 * 3. Processing Algorithm per Sample `i`:
 *    - Let `val_A` and `val_B` be the input samples for channels A and B at
 * time index `i`.
 *    - Let `feedback_A` and `feedback_B` be the recursive cancellation signals
 * from the previous step.
 *    - Step 1: Add contralateral feedback cancellation signals:
 *      added_A = val_A + feedback_B
 *      added_B = val_B + feedback_A
 *    - Step 2: Pass the combined signals through delay filters (`delay_A`,
 * `delay_B`) representing the interaural time difference (ITD): delayed_A =
 * delay_filter_process_single(delay_A, added_A) delayed_B =
 * delay_filter_process_single(delay_B, added_B)
 *    - Step 3: Pass the delayed signals through gain filters (`gain`)
 * representing acoustic attenuation and phase inversion (negative gain):
 *      feedback_A = gain_filter_process_single(gain, delayed_A)
 *      feedback_B = gain_filter_process_single(gain, delayed_B)
 *    - Step 4: Output the cancelled samples:
 *      out_A[i] = added_A
 *      out_B[i] = added_B
 * 4. Delay Unit Conversion & Subsample Accuracy:
 *    - Supports delay units in microseconds (us), milliseconds (ms),
 * millimeters (mm @ 343 m/s), and samples.
 *    - Compensates for 1 sample period processing latency: compensated_delay =
 * max(delay - sample_period, 0.0).
 * 5. ZERO-ALLOCATION GUARANTEE: Real-time processing (`race_processor_process`)
 * performs no memory allocations. All delay lines and state variables are
 * pre-allocated during initialization.
 */

struct processor_vtable;
extern const struct processor_vtable g_race_vtable;

#endif  // CLIB_PROCESSORS_RACE_PROCESSOR_H
