/**
 * @file resampler_channel_mask.h
 * @brief Shared helpers for honouring an input chunk's used-channel mask in
 * the resamplers.
 *
 * Mirrors upstream CamillaDSP/rubato `active_channels_mask`
 * (camilladsp `utils/resampling.rs`, rubato `asynchro*.rs`, `synchro.rs`,
 * `slip.rs`): channels whose mask entry is false are skipped entirely
 * (no history shift, FFT or interpolation). Unlike upstream, which leaves an
 * inactive channel's internal history untouched (so a re-activated channel
 * first replays stale history), cdsp zeroes that channel's history/carry while
 * it is inactive, so re-activation starts from silence. Inactive output
 * channels are zero-filled and the mask is propagated to the output chunk.
 *
 * All helpers are allocation-free and safe for the real-time hot path.
 */

#ifndef CLIB_RESAMPLER_RESAMPLER_CHANNEL_MASK_H
#define CLIB_RESAMPLER_RESAMPLER_CHANNEL_MASK_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "audio/audio_chunk.h"

/**
 * @brief Returns whether channel @p ch is active under @p mask (NULL = all).
 */
static inline bool resampler_channel_active(const bool *mask, size_t ch) {
  return !mask || mask[ch];
}

/**
 * @brief Zero-fills inactive output channels and propagates the input mask.
 *
 * The mask is only copied when the output chunk already owns a mask buffer,
 * so this never allocates (audio_chunk_set_used_channels memcpy's in place).
 *
 * @param mask Input used-channel mask, or NULL (no-op).
 * @param output Output chunk.
 * @param channels Number of channels.
 * @param frames Number of output frames to zero per inactive channel.
 */
static inline void resampler_finish_masked_output(const bool *mask,
                                                  audio_chunk_t *output,
                                                  size_t channels,
                                                  size_t frames) {
  if (!mask)
    return;
  for (size_t ch = 0; ch < channels; ch++) {
    if (mask[ch])
      continue;
    double *out = audio_chunk_get_channel(output, ch);
    if (out && frames > 0)
      memset(out, 0, frames * sizeof(double));
  }
  if (audio_chunk_get_used_channels(output) &&
      audio_chunk_get_used_channels(output) != mask) {
    audio_chunk_set_used_channels(output, mask);
  }
}

#endif // CLIB_RESAMPLER_RESAMPLER_CHANNEL_MASK_H
