/**
 * @file test_audit_backend_pipewire_bounds.c
 * @brief Regression tests for PipeWire callback helpers (audit 05 F-02,
 * F-04, F-06).
 *
 * The helpers are platform independent (pipewire_backend.h exposes them
 * outside the ENABLE_PIPEWIRE guard) so these run on every platform.
 */
#include <stddef.h>
#include <stdint.h>

#include "backend/pipewire_internal.h"
#include "test_support.h"

// F-02: a well-formed chunk is passed through unchanged.
TEST(Audit_PipeWireCaptureChunkInBounds) {
  size_t off = 99;
  // 2 ch F32 => 8 bytes/frame; 256 frames at offset 64 in a 4096-byte map.
  size_t frames = pipewire_capture_chunk_frames(64, 2048, 4096, 8, &off);
  ASSERT_EQ(64, (int)off);
  ASSERT_EQ(256, (int)frames);
}

// F-02: offset + size beyond maxsize is clamped to the mapped region.
TEST(Audit_PipeWireCaptureChunkSizeClamped) {
  size_t off = 0;
  size_t frames = pipewire_capture_chunk_frames(4000, 2048, 4096, 8, &off);
  ASSERT_EQ(4000, (int)off);
  ASSERT_EQ(12, (int)frames); // (4096 - 4000) / 8
  ASSERT_TRUE(off + frames * 8 <= 4096);
}

// F-02: offset at/after maxsize yields nothing to read.
TEST(Audit_PipeWireCaptureChunkOffsetPastEnd) {
  size_t off = 1;
  ASSERT_EQ(0, (int)pipewire_capture_chunk_frames(4096, 512, 4096, 8, &off));
  ASSERT_TRUE(off <= 4096);
  ASSERT_EQ(0, (int)pipewire_capture_chunk_frames(UINT32_MAX, UINT32_MAX, 4096,
                                                  8, &off));
  ASSERT_TRUE(off <= 4096);
}

// F-02: degenerate parameters never divide by zero or read.
TEST(Audit_PipeWireCaptureChunkDegenerate) {
  size_t off = 5;
  ASSERT_EQ(0, (int)pipewire_capture_chunk_frames(0, 512, 0, 8, &off));
  ASSERT_EQ(0, (int)off);
  ASSERT_EQ(0, (int)pipewire_capture_chunk_frames(0, 512, 4096, 0, &off));
  ASSERT_EQ(0, (int)pipewire_capture_chunk_frames(0, 7, 4096, 8, NULL));
}

// Playback byte count matches upstream pipewire_playback_callback_bytes
// (device.rs:235-248).
TEST(Audit_PipeWirePlaybackCallbackBytes) {
  // Valid request: used as-is (rounded to whole frames).
  ASSERT_EQ(1024, (int)pipewire_playback_callback_bytes(1024, 8192, 8, 256));
  ASSERT_EQ(1016, (int)pipewire_playback_callback_bytes(1020, 8192, 8, 256));
  // No request: chunk size fallback.
  ASSERT_EQ(2048, (int)pipewire_playback_callback_bytes(0, 8192, 8, 256));
  // Request larger than capacity: fallback bounded by capacity.
  ASSERT_EQ(2048, (int)pipewire_playback_callback_bytes(99999, 8192, 8, 256));
  ASSERT_EQ(1000, (int)pipewire_playback_callback_bytes(0, 1004, 8, 256));
  // Zero stride never divides by zero.
  ASSERT_EQ(0, (int)pipewire_playback_callback_bytes(1024, 8192, 0, 256));
}

// F-04: graph rate tracking. The first observation is the baseline (no
// report, so a graph already running at another rate keeps working through
// PipeWire's adapter); every later change is reported once.
TEST(Audit_PipeWireGraphRateUpdate) {
  uint32_t seen = 0;
  ASSERT_EQ(0, (int)pipewire_graph_rate_update(0, &seen)); // unknown
  ASSERT_EQ(0, (int)seen);
  ASSERT_EQ(0, (int)pipewire_graph_rate_update(44100, &seen)); // baseline
  ASSERT_EQ(44100, (int)seen);
  ASSERT_EQ(0, (int)pipewire_graph_rate_update(44100, &seen)); // unchanged
  ASSERT_EQ(96000, (int)pipewire_graph_rate_update(96000, &seen));
  ASSERT_EQ(0, (int)pipewire_graph_rate_update(96000, &seen)); // reported once
  ASSERT_EQ(48000, (int)pipewire_graph_rate_update(48000, &seen));
  ASSERT_EQ(0, (int)pipewire_graph_rate_update(0, &seen)); // transient 0
  ASSERT_EQ(48000, (int)seen);
  ASSERT_EQ(0, (int)pipewire_graph_rate_update(48000, NULL));
}

TEST_MAIN()
