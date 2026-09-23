#ifndef CDSP_PLAYBACK_BUFFER_H
#define CDSP_PLAYBACK_BUFFER_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "utils/device_buffer_estimator.h"
#include "utils/lock_free_ring_buffer.h"

/**
 * @file playback_buffer.h
 * @brief Reports how many frames a playback backend still has pending and
 * coordinates unified realtime silence injection and underrun recovery.
 *
 * Every playback backend has the same two-part answer to "how much audio is
 * still queued?", and this splits the two parts by how they can be measured:
 *
 * - **The ring buffer.** Read live on every query. It is a lock-free SPSC
 *   structure, so sampling its fill level from the engine thread is cheap and
 *   exact; there is nothing to estimate.
 * - **Everything past the ring.** Frames already handed to the device, or
 *   staged in a callback-local queue, or queued as silence. These generally
 *   cannot be queried safely from the engine thread, so the thread that owns
 *   the device publishes the value and this extrapolates between updates.
 *
 * Backends therefore call playback_buffer_publish() with *only* the
 * non-ring frames, and playback_buffer_level() adds the live ring fill back
 * on. Publishing the ring contents as well would double-count them, and would
 * also make a stalled device appear to drain when it is not.
 */
typedef struct {
  device_buffer_estimator_t device;
  _Atomic size_t target_level;
  _Atomic size_t silence_to_insert;
  _Atomic bool is_running;
} playback_buffer_t;

/**
 * @brief Initialize for a device running at @p sample_rate Hz.
 *
 * Call once, before the device callback or worker thread can run.
 *
 * @param pb Buffer tracker to initialize.
 * @param sample_rate Device sample rate in Hz.
 */
void playback_buffer_init(playback_buffer_t *pb, double sample_rate);

/**
 * @brief Point the tracker at a new sample rate and drop the stored level.
 *
 * Call from open(), and after the device changes rate.
 *
 * @param pb Buffer tracker to update.
 * @param sample_rate New device sample rate in Hz.
 */
void playback_buffer_set_rate(playback_buffer_t *pb, double sample_rate);

/**
 * @brief Forget the published device level without changing the rate.
 *
 * @param pb Buffer tracker to reset.
 */
void playback_buffer_reset(playback_buffer_t *pb);

/**
 * @brief Publish the frames held beyond the ring buffer.
 *
 * Called by the thread that owns the device: the ALSA/WASAPI worker, or the
 * CoreAudio/ASIO/PipeWire callback. Pass only frames that are *not* in the
 * ring buffer, such as the ALSA hardware delay or silence still queued.
 *
 * @param pb Buffer tracker to update.
 * @param device_frames Frames pending outside the ring buffer.
 */
void playback_buffer_publish(playback_buffer_t *pb, size_t device_frames);

/**
 * @brief Total frames still pending playback.
 *
 * @param pb Buffer tracker to read. May be NULL.
 * @param ring Ring buffer feeding the device. May be NULL.
 * @param blockalign Bytes per frame in @p ring. 0 excludes the ring term.
 * @return Live ring fill plus the extrapolated device-side level, in frames.
 */
size_t playback_buffer_level(const playback_buffer_t *pb,
                             const spsc_byte_ring_buffer_t *ring,
                             size_t blockalign);

/**
 * @brief Total frames still pending playback for planar ring buffers.
 *
 * @param pb Buffer tracker to read. May be NULL.
 * @param ring Planar ring buffer feeding the device. May be NULL.
 * @return Live planar ring fill plus the extrapolated device-side level, in
 * frames.
 */
size_t playback_buffer_planar_level(const playback_buffer_t *pb,
                                    const spsc_planar_ring_buffer_t *ring);

/**
 * @brief Configure target delay cushion level (in frames).
 *
 * @param pb Buffer tracker.
 * @param target_level Desired buffer level cushion in frames.
 */
void playback_buffer_set_target_level(playback_buffer_t *pb,
                                      size_t target_level);

/**
 * @brief Prefill silence into planar ring buffer to establish target delay.
 *
 * @param pb Buffer tracker.
 * @param ring Planar ring buffer (optional).
 * @param frames Frames of silence to prefill.
 */
void playback_buffer_prefill_planar(playback_buffer_t *pb,
                                    spsc_planar_ring_buffer_t *ring,
                                    size_t frames);

/**
 * @brief Prefill silence into byte ring buffer to establish target delay.
 *
 * @param pb Buffer tracker.
 * @param ring Byte ring buffer (optional).
 * @param frames Frames of silence to prefill.
 * @param blockalign Frame size in bytes (channels * bytes_per_sample).
 * @param silence_byte Byte pattern for silence (typically 0x00, or 0x69 for
 * DSD).
 */
void playback_buffer_prefill_byte(playback_buffer_t *pb,
                                  spsc_byte_ring_buffer_t *ring, size_t frames,
                                  size_t blockalign, uint8_t silence_byte);

/**
 * @brief Render planar audio from ring buffer to device destination channels,
 * handling startup/underrun silence rebuilding, tail zeroing, and buffer level
 * tracking.
 *
 * @param pb Buffer tracker.
 * @param ring Planar ring buffer.
 * @param dst_channels Array of channel output pointers.
 * @param frames Number of frames to render.
 * @param silence_byte Byte pattern for silence.
 * @return Number of audio frames read from ring.
 */
size_t playback_buffer_render_planar(playback_buffer_t *pb,
                                     spsc_planar_ring_buffer_t *ring,
                                     void *const *dst_channels, size_t frames,
                                     uint8_t silence_byte);

/**
 * @brief Render interleaved audio from byte ring buffer to device destination,
 * handling startup/underrun silence rebuilding, tail zeroing, and buffer level
 * tracking.
 *
 * @param pb Buffer tracker.
 * @param ring Byte ring buffer.
 * @param dst Destination output buffer.
 * @param frames Number of frames to render.
 * @param blockalign Frame size in bytes (channels * bytes_per_sample).
 * @param silence_byte Byte pattern for silence.
 * @return Number of audio frames read from ring.
 */
size_t playback_buffer_render_byte(playback_buffer_t *pb,
                                   spsc_byte_ring_buffer_t *ring, void *dst,
                                   size_t frames, size_t blockalign,
                                   uint8_t silence_byte);

#endif // CDSP_PLAYBACK_BUFFER_H
