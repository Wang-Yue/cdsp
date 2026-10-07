// Regression tests for audit report 08 §1.6 (decode honors used-channel mask).
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "audio/audio_chunk.h"
#include "audio/sample_format.h"
#include "test_support.h"

static void fill_garbage(audio_chunk_t *chunk) {
  for (size_t c = 0; c < audio_chunk_get_channels(chunk); c++) {
    double *ch = audio_chunk_get_channel(chunk, c);
    for (size_t i = 0; i < audio_chunk_get_frames(chunk); i++)
      ch[i] = 123.0;
  }
}

TEST(AuditAudio_DecodeInterleavedSkipsUnusedChannels) {
  enum { FRAMES = 8, CH = 3 };
  int16_t src[FRAMES * CH];
  for (size_t f = 0; f < FRAMES; f++)
    for (size_t c = 0; c < CH; c++)
      src[f * CH + c] = (int16_t)(1024 * (c + 1));

  audio_chunk_t *chunk = audio_chunk_create(FRAMES, CH);
  ASSERT_TRUE(chunk != NULL);
  const bool mask[CH] = {true, false, true};
  audio_chunk_set_used_channels(chunk, mask);
  fill_garbage(chunk);

  ASSERT_TRUE(audio_chunk_decode_interleaved(src, BINARY_SAMPLE_FORMAT_S16_LE,
                                             CH, FRAMES, chunk));
  for (size_t f = 0; f < FRAMES; f++) {
    ASSERT_DOUBLE_EQ(1024.0 / 32768.0, audio_chunk_get_channel(chunk, 0)[f]);
    ASSERT_DOUBLE_EQ(0.0, audio_chunk_get_channel(chunk, 1)[f]);
    ASSERT_DOUBLE_EQ(3072.0 / 32768.0, audio_chunk_get_channel(chunk, 2)[f]);
  }
  audio_chunk_free(chunk);
}

TEST(AuditAudio_DecodeStereoOneChannelUnused) {
  enum { FRAMES = 8 };
  int16_t src[FRAMES * 2];
  for (size_t f = 0; f < FRAMES; f++) {
    src[2 * f] = 4096;
    src[2 * f + 1] = -4096;
  }
  audio_chunk_t *chunk = audio_chunk_create(FRAMES, 2);
  const bool mask[2] = {false, true};
  audio_chunk_set_used_channels(chunk, mask);
  fill_garbage(chunk);
  ASSERT_TRUE(audio_chunk_decode_interleaved(src, BINARY_SAMPLE_FORMAT_S16_LE,
                                             2, FRAMES, chunk));
  for (size_t f = 0; f < FRAMES; f++) {
    ASSERT_DOUBLE_EQ(0.0, audio_chunk_get_channel(chunk, 0)[f]);
    ASSERT_DOUBLE_EQ(-0.125, audio_chunk_get_channel(chunk, 1)[f]);
  }

  // Without a mask every channel is decoded (unchanged behavior).
  audio_chunk_set_used_channels(chunk, NULL);
  ASSERT_TRUE(audio_chunk_decode_interleaved(src, BINARY_SAMPLE_FORMAT_S16_LE,
                                             2, FRAMES, chunk));
  ASSERT_DOUBLE_EQ(0.125, audio_chunk_get_channel(chunk, 0)[0]);
  audio_chunk_free(chunk);
}

TEST(AuditAudio_DecodePlanarSkipsUnusedChannels) {
  enum { FRAMES = 4 };
  int16_t a[FRAMES] = {8192, 8192, 8192, 8192};
  int16_t b[FRAMES] = {16384, 16384, 16384, 16384};
  const void *srcs[2] = {a, b};
  audio_chunk_t *chunk = audio_chunk_create(FRAMES, 2);
  const bool mask[2] = {true, false};
  audio_chunk_set_used_channels(chunk, mask);
  fill_garbage(chunk);
  ASSERT_TRUE(audio_chunk_decode_planar(srcs, BINARY_SAMPLE_FORMAT_S16_LE, 2,
                                        FRAMES, chunk));
  for (size_t f = 0; f < FRAMES; f++) {
    ASSERT_DOUBLE_EQ(0.25, audio_chunk_get_channel(chunk, 0)[f]);
    ASSERT_DOUBLE_EQ(0.0, audio_chunk_get_channel(chunk, 1)[f]);
  }
  audio_chunk_free(chunk);
}

TEST_MAIN()
