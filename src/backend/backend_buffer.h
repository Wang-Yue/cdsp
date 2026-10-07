#ifndef CDSP_BACKEND_BUFFER_H
#define CDSP_BACKEND_BUFFER_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio/audio_chunk.h"
#include "audio/sample_format.h"
#include "backend/backend_error.h"

/**
 * @file backend_buffer.h
 * @brief Unified, opaque backend buffer abstraction.
 *
 * Manages ring buffers, hides planar vs interleaved memory layouts, and
 * coordinates realtime driver rendering, silence prefill, underrun recovery,
 * chunk transfers, and lock-free buffer level estimation across all audio
 * backends.
 */

typedef struct backend_buffer backend_buffer_t;

typedef struct processing_parameters processing_parameters_t;

/* --- Lifecycle Management --- */

/**
 * @brief Creates a backend buffer (interleaved or planar).
 *
 * @param capacity_frames Buffer capacity in frames.
 * @param format Binary sample format.
 * @param channels Number of audio channels.
 * @param sample_rate Device sample rate in Hz.
 * @param is_planar True for planar multi-channel layout, false for interleaved.
 * @param params Optional processing parameters for clipping telemetry (or
 * NULL).
 * @return Allocated backend_buffer_t pointer on success, NULL on failure.
 */
backend_buffer_t *backend_buffer_create(size_t capacity_frames,
                                        binary_sample_format_t format,
                                        size_t channels, double sample_rate,
                                        bool is_planar,
                                        processing_parameters_t *params);

/**
 * @brief Frees the backend buffer and its underlying ring buffer.
 *
 * @param bb Pointer to backend buffer.
 */
void backend_buffer_free(backend_buffer_t *bb);

/* --- Stream Lifecycle & State Control --- */

/**
 * @brief Stream lifecycle states for audio backends.
 */
typedef enum backend_stream_state {
  BACKEND_STREAM_IDLE = 0, /**< Initialized / ready, not yet started */
  BACKEND_STREAM_RUNNING,  /**< Actively processing / streaming audio */
  BACKEND_STREAM_PAUSED,   /**< Stream active, but paused (silence / idling) */
  BACKEND_STREAM_STOPPED,  /**< Stopped; stream terminated or teardown */
} backend_stream_state_t;

/**
 * @brief Gets the current stream lifecycle state.
 *
 * @param bb Pointer to backend buffer.
 * @return Current backend_stream_state_t.
 */
backend_stream_state_t backend_buffer_get_state(const backend_buffer_t *bb);

/**
 * @brief Atomically sets the stream lifecycle state.
 *
 * @param bb Pointer to backend buffer.
 * @param state Target backend_stream_state_t.
 */
void backend_buffer_set_state(backend_buffer_t *bb,
                              backend_stream_state_t state);

/**
 * @brief Checks if a format or sample rate change is pending.
 *
 * @param bb Pointer to backend buffer.
 * @return True if a rate/format change is pending.
 */
bool backend_buffer_has_pending_rate_change(const backend_buffer_t *bb);

/**
 * @brief Sets the pending rate/format change status.
 *
 * @param bb Pointer to backend buffer.
 * @param pending True if a rate/format change is pending.
 */
void backend_buffer_set_pending_rate_change(backend_buffer_t *bb, bool pending);

/* --- Engine Chunk Transfer (Unified Planar & Interleaved) --- */

/**
 * @brief Writes an audio chunk into the buffer (auto-handles planar vs
 * interleaved).
 *
 * @param bb Pointer to backend buffer.
 * @param chunk Audio chunk to encode and write.
 * @param sleep_ms Backoff interval in ms when full (0 is treated as 1 ms).
 * @param max_retries Number of backoff intervals when full (0 is treated as
 * 1). The writer waits at most max_retries * sleep_ms in total for the device
 * to make room, then drops the whole chunk (drop-on-full) and returns true.
 * @param err Pointer to backend_error_t to record errors.
 * @return True on success, on drop-on-full, or when paused; false on error,
 * stream stop or a pending format change.
 */
bool backend_buffer_write_chunk(backend_buffer_t *bb,
                                const audio_chunk_t *chunk, uint32_t sleep_ms,
                                uint32_t max_retries, backend_error_t *err);

/**
 * @brief Reads an audio chunk from the buffer (auto-handles planar vs
 * interleaved).
 *
 * @param bb Pointer to backend buffer.
 * @param frames_requested Number of frames requested to read.
 * @param chunk Destination audio chunk.
 * @param err Pointer to backend_error_t to record errors.
 * @return True on success, false on error, stream stop, or insufficient data.
 */
bool backend_buffer_read_chunk(backend_buffer_t *bb, size_t frames_requested,
                               audio_chunk_t *chunk, backend_error_t *err);

/* --- Silence Prefill, Estimation & Realtime Driver Rendering --- */

/**
 * @brief Prefills silence into the buffer (auto-handles planar vs interleaved).
 *
 * Writes @p frames of silence into the ring, then declares it as the active
 * underrun cushion. Precedence: this also sets the cushion target level to
 * @p frames, overriding any value from backend_buffer_set_target_level()
 * made at create time. The engine calls it once after open with
 * devices.target_level (or chunksize), which is the single upstream
 * target_level, so the engine-level value wins over per-device values.
 * Producer-side call; safe while the device callback is running.
 *
 * @param bb Pointer to backend buffer.
 * @param frames Number of silence frames to prefill.
 * @param silence_byte Byte pattern for silence (typically 0x00, or 0x69 for
 * DSD).
 */
void backend_buffer_prefill_silence(backend_buffer_t *bb, size_t frames,
                                    uint8_t silence_byte);

/**
 * @brief Returns total frames pending playback (live ring fill + device
 * estimator). Automatically handles planar vs interleaved.
 *
 * @param bb Pointer to backend buffer.
 * @return Total frames pending playback.
 */
size_t backend_buffer_get_level(const backend_buffer_t *bb);

/**
 * @brief Renders audio from the buffer to the device buffer (auto-handles
 * planar vs interleaved). For planar buffers: @p dst is (void *const *) channel
 * pointers array. For interleaved buffers: @p dst is (void *) flat destination
 * buffer.
 *
 * @param bb Pointer to backend buffer.
 * @param dst Destination buffer pointer or channel pointers array.
 * @param frames Number of frames to render.
 * @param silence_byte Byte pattern for silence padding on underrun.
 * @return Number of frames actually consumed from buffer.
 */
size_t backend_buffer_render(backend_buffer_t *bb, void *dst, size_t frames,
                             uint8_t silence_byte);

/**
 * @brief Pushes captured audio from the device into the buffer (auto-handles
 * planar vs interleaved). For planar buffers: @p src is (const void *const *)
 * source channel pointers. For interleaved buffers: @p src is (const void *)
 * flat source buffer.
 *
 * @param bb Pointer to backend buffer.
 * @param src Source buffer pointer or channel pointers array.
 * @param frames Number of frames to push.
 * @return Number of frames actually pushed.
 */
size_t backend_buffer_push(backend_buffer_t *bb, const void *src,
                           size_t frames);

/**
 * @brief Consumes audio from the buffer into the destination buffer
 * (auto-handles planar vs interleaved).
 *
 * @param bb Pointer to backend buffer.
 * @param dst Destination buffer pointer or channel pointers array.
 * @param frames Number of frames to consume.
 * @return Number of frames actually consumed.
 */
size_t backend_buffer_consume(backend_buffer_t *bb, void *dst, size_t frames);

/* --- Zero-copy slice access (interleaved/byte buffers only) --- */

/**
 * @brief Exposes up to @p max_frames queued frames as at most two contiguous
 * ring slices, so a driver can hand them straight to the device (e.g. ALSA
 * snd_pcm_writei) without a staging copy (AGENTS.md §3.4). Consumer side only.
 *
 * Only whole frames are exposed. The ring is a power-of-two number of bytes,
 * so with a block size that is not a power of two (e.g. S24_3) one frame can
 * straddle the wrap point. Slices stop before such a frame, and @p n2 is 0. If
 * the function returns 0 while backend_buffer_get_available_read_frames() > 0,
 * the next frame straddles the wrap: move it with backend_buffer_consume()
 * into a one-frame stack buffer. After that the slices are aligned again.
 *
 * @return Total frames exposed (*n1 + *n2). Returns 0 for planar buffers.
 */
size_t backend_buffer_get_read_slices(backend_buffer_t *bb, size_t max_frames,
                                      const void **p1, size_t *n1,
                                      const void **p2, size_t *n2);

/**
 * @brief Releases @p frames previously exposed by
 * backend_buffer_get_read_slices() and signals waiting writers.
 */
void backend_buffer_commit_read(backend_buffer_t *bb, size_t frames);

/**
 * @brief Producer-side counterpart of backend_buffer_get_read_slices(): exposes
 * up to @p max_frames of free space as at most two contiguous slices. It uses
 * the same whole-frame and wrap-straddle rule; for a straddling frame, use
 * backend_buffer_push() with a one-frame buffer.
 *
 * @return Total frames exposed (*n1 + *n2). Returns 0 for planar buffers.
 */
size_t backend_buffer_get_write_slices(backend_buffer_t *bb, size_t max_frames,
                                       void **p1, size_t *n1, void **p2,
                                       size_t *n2);

/**
 * @brief Publishes @p frames written into slices from
 * backend_buffer_get_write_slices() and signals the reader.
 *
 * @p frames_dropped is the number of captured frames the driver discarded
 * because there was no space. A non-zero value starts an overflow episode,
 * which logs one warning per episode plus trace detail, the same as
 * backend_buffer_push(). Zero ends the episode.
 */
void backend_buffer_commit_write(backend_buffer_t *bb, size_t frames,
                                 size_t frames_dropped);

/**
 * @brief Returns the number of frames currently available to read from the
 * buffer.
 *
 * @param bb Pointer to backend buffer.
 * @return Available frames to read.
 */
size_t backend_buffer_get_available_read_frames(const backend_buffer_t *bb);

/**
 * @brief Returns the number of frames currently available to write to the
 * buffer.
 *
 * @param bb Pointer to backend buffer.
 * @return Available frames to write.
 */
size_t backend_buffer_get_available_write_frames(const backend_buffer_t *bb);

/**
 * @brief Discards all pending audio frames in the buffer.
 *
 * This is a flush, not a play-out: queued audio is thrown away by moving the
 * ring's read index to its write index. It writes consumer-owned state, so it
 * must only be called from the thread that consumes the ring (the device
 * callback for playback, the engine for capture) or while that thread is
 * stopped. It is not a suitable implementation of the playback vtable
 * `drain` (which must let queued audio finish playing).
 *
 * @param bb Pointer to backend buffer.
 */
void backend_buffer_drain(backend_buffer_t *bb);

/**
 * @brief Publishes hardware/driver delay frames to the estimator.
 *
 * @param bb Pointer to backend buffer.
 * @param device_frames Frames pending outside the ring buffer.
 */
void backend_buffer_publish(backend_buffer_t *bb, size_t device_frames);

/**
 * @brief Updates estimator sample rate.
 *
 * @param bb Pointer to backend buffer.
 * @param sample_rate New sample rate in Hz.
 */
void backend_buffer_set_rate(backend_buffer_t *bb, double sample_rate);

/**
 * @brief Resets estimator state.
 *
 * @param bb Pointer to backend buffer.
 */
void backend_buffer_reset(backend_buffer_t *bb);

/**
 * @brief Configures target delay level.
 *
 * @param bb Pointer to backend buffer.
 * @param target_level Desired cushion in frames.
 */
void backend_buffer_set_target_level(backend_buffer_t *bb, size_t target_level);

/* --- Thread Synchronization --- */

/**
 * @brief Signals the backend buffer's semaphore to wake waiting
 * consumers/readers.
 *
 * @param bb Pointer to backend buffer.
 */
void backend_buffer_signal(backend_buffer_t *bb);

/**
 * @brief Waits for new data or stream state changes on the backend buffer.
 *
 * Checks if the stream is stopped (returning false if stopped), then waits
 * on the internal semaphore up to @p timeout_ms.
 *
 * @param bb Pointer to backend buffer.
 * @param timeout_ms Maximum time to wait in milliseconds.
 * @return True if signaled/data available, false on timeout or if stream
 * stopped.
 */
bool backend_buffer_wait(backend_buffer_t *bb, uint32_t timeout_ms);

#endif // CDSP_BACKEND_BUFFER_H
