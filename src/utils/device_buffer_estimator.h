#ifndef CDSP_DEVICE_BUFFER_ESTIMATOR_H
#define CDSP_DEVICE_BUFFER_ESTIMATOR_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @file device_buffer_estimator.h
 * @brief Estimates how many frames a playback device still holds.
 *
 * Port of upstream CamillaDSP's `DeviceBufferEstimator`
 * (src/utils/countertimer.rs:23-54). The thread that owns the device handle
 * publishes the buffer level whenever it learns it; any other thread can then
 * ask for an estimate, which decays the last published value by the number of
 * frames the device is expected to have consumed since.
 *
 * This exists because querying the device for its fill level (for example
 * `snd_pcm_avail()`) is generally not safe to do concurrently with a write on
 * the same handle. Upstream solves this by sharing the estimator behind a
 * mutex; here the two fields are atomics so the reader never blocks the
 * real-time writer.
 *
 * Threading contract (single producer, multiple readers):
 * - The producer calls device_buffer_estimator_add() from the thread that owns
 *   the device handle.
 * - Readers call device_buffer_estimator_estimate() from any other thread.
 *
 * The producer stores the frame count first and the timestamp last (with
 * release ordering); the reader loads the timestamp first (with acquire
 * ordering) and the frame count after. A reader that races with a publish
 * therefore pairs a fresh frame count with an older timestamp at worst, which
 * makes the estimate decay slightly too fast. Under-estimating is the safe
 * direction: the playback drain loop in the engine waits for the reported
 * level to reach zero, so over-estimating would stall shutdown instead.
 */
typedef struct {
  /** Timestamp of the last publish, in nanoseconds. 0 means "never". */
  _Atomic uint64_t update_time_ns;
  /** Frames the device held at `update_time_ns`. */
  _Atomic size_t frames;
  /** Playback rate in Hz, used to decay `frames` over time. */
  _Atomic double sample_rate;
} device_buffer_estimator_t;

/**
 * @brief Initialize an estimator for a device running at @p sample_rate Hz.
 *
 * Must be called before any other function, and before the producer thread is
 * started. Mirrors `DeviceBufferEstimator::new`.
 *
 * @param est Estimator to initialize.
 * @param sample_rate Device sample rate in Hz. Non-positive disables decay.
 */
void device_buffer_estimator_init(device_buffer_estimator_t *est,
                                  double sample_rate);

/**
 * @brief Forget any published level, so the estimate reads back as 0.
 *
 * Call this when the device is opened or closed. A timestamp left over from a
 * previous session would otherwise be arbitrarily old, making the very first
 * estimate decay straight to zero.
 *
 * @param est Estimator to reset.
 */
void device_buffer_estimator_reset(device_buffer_estimator_t *est);

/**
 * @brief Update the rate used to decay the stored frame count.
 *
 * Call this after the device changes sample rate. The stored level is cleared,
 * since it was measured against the old rate.
 *
 * @param est Estimator to update.
 * @param sample_rate New device sample rate in Hz.
 */
void device_buffer_estimator_set_rate(device_buffer_estimator_t *est,
                                      double sample_rate);

/**
 * @brief Publish the number of frames the device currently holds.
 *
 * Called by the thread that owns the device handle. Despite the name, which
 * is kept to match upstream's `DeviceBufferEstimator::add`, this replaces the
 * stored value rather than accumulating onto it.
 *
 * @param est Estimator to update.
 * @param frames Frames currently buffered by the device.
 */
void device_buffer_estimator_add(device_buffer_estimator_t *est, size_t frames);

/**
 * @brief Estimate how many frames the device still holds right now.
 *
 * Takes the last published level and subtracts the frames the device should
 * have consumed since, based on the sample rate. Mirrors
 * `DeviceBufferEstimator::estimate`.
 *
 * @param est Estimator to read. May be NULL, in which case 0 is returned.
 * @return Estimated frames remaining, saturating at 0.
 */
size_t device_buffer_estimator_estimate(const device_buffer_estimator_t *est);

#endif // CDSP_DEVICE_BUFFER_ESTIMATOR_H
