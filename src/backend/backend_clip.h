#ifndef CDSP_BACKEND_CLIP_H
#define CDSP_BACKEND_CLIP_H

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio/audio_chunk.h"
#include "audio/processing_parameters.h"
#include "logging/app_logger.h"
#include "utils/cdsp_time.h"

/// Minimum interval between two "Clipping detected" warnings from one device.
#define BACKEND_CLIP_WARN_INTERVAL_NS 1000000000ULL

/**
 * @brief Count samples that clip when converted to an integer output format,
 * add them to the clipped-sample statistics and warn like upstream
 * (`conversions.rs` `chunk_to_buffer_with_adapter`):
 * "Clipping detected, N samples clipped, peak +X dB (Y%)".
 *
 * Unused (empty) channels are skipped, as upstream skips empty waveforms.
 * Upstream warns on every clipping chunk; here the warning is limited to one
 * per BACKEND_CLIP_WARN_INTERVAL_NS per device so a constantly clipping stream
 * cannot flood the (lock-free) log ring from the audio thread. The counter is
 * still updated on every chunk. No allocation, no locks.
 *
 * @param params Shared processing parameters (clip statistics).
 * @param chunk Output chunk about to be encoded.
 * @param max_channels Only the first max_channels channels are output and
 * counted (a chunk may carry more channels than the device).
 * @param last_warn_ns Per-device timestamp of the last warning (0 = never).
 * @param log Logger of the calling backend.
 */
static inline void
backend_count_clipped_channels(processing_parameters_t *params,
                               const audio_chunk_t *chunk, size_t max_channels,
                               uint64_t *last_warn_ns, const logger_t *log) {
  size_t channels = audio_chunk_get_channels(chunk);
  if (channels > max_channels)
    channels = max_channels;
  size_t frames = audio_chunk_get_valid_frames(chunk);
  uint64_t clipped = 0;
  double peak = 0.0;
  const bool *used = audio_chunk_get_used_channels(chunk);
  for (size_t c = 0; c < channels; c++) {
    if (used && !used[c])
      continue;
    mutable_waveform_t data = audio_chunk_get_channel(chunk, c);
    uint64_t ch_clipped = 0;
    double ch_peak = 0.0;
    for (size_t f = 0; f < frames; f++) {
      double v = data[f];
      // NaN counts as clipped, like upstream's float-to-int conversion
      // (audioadapter-sample to_clamped_int).
      ch_clipped += (v < 1.0 && v >= -1.0) ? 0u : 1u;
      ch_peak = fmax(ch_peak, fabs(v));
    }
    clipped += ch_clipped;
    if (ch_clipped > 0 && ch_peak > peak)
      peak = ch_peak;
  }
  if (clipped == 0)
    return;
  processing_parameters_add_clipped_samples(params, clipped);
  uint64_t now = cdsp_time_now_ns();
  if (last_warn_ns && (*last_warn_ns == 0 ||
                       now - *last_warn_ns >= BACKEND_CLIP_WARN_INTERVAL_NS)) {
    *last_warn_ns = now;
    logger_warn(
        log, "Clipping detected, %llu samples clipped, peak +%.2f dB (%.1f%%)",
        (unsigned long long)clipped, 20.0 * log10(peak), peak * 100.0);
  }
}

/**
 * @brief backend_count_clipped_channels() over every channel of @p chunk.
 */
static inline void backend_count_clipped(processing_parameters_t *params,
                                         const audio_chunk_t *chunk,
                                         uint64_t *last_warn_ns,
                                         const logger_t *log) {
  backend_count_clipped_channels(params, chunk, SIZE_MAX, last_warn_ns, log);
}

#endif // CDSP_BACKEND_CLIP_H
