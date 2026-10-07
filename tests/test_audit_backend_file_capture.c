/**
 * @file test_audit_backend_file_capture.c
 * @brief Regression tests for the file/stdin capture fixes from the backend
 * audit (audit_reports/backends/06_file_generator.md).
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "audio/audio_chunk.h"
#include "backend/audio_backend.h"
#include "backend/backend_error.h"
#include "backend/file_backend.h"
#include "config/configuration.h"
#include "test_support.h"

static void audit_file_fill_chunk(audio_chunk_t *chunk, double value) {
  for (size_t c = 0; c < audio_chunk_get_channels(chunk); c++) {
    double *d = audio_chunk_get_channel(chunk, c);
    for (size_t f = 0; f < audio_chunk_get_frames(chunk); f++) {
      d[f] = value;
    }
  }
}

static void audit_file_raw_capture_cfg(capture_device_config_t *cfg,
                                       const char *path, size_t channels,
                                       binary_sample_format_t fmt) {
  memset(cfg, 0, sizeof(*cfg));
  cfg->type = AUDIO_BACKEND_TYPE_FILE;
  cfg->is_wav = false;
  cfg->has_is_wav = true;
  cfg->cfg.raw_file.channels = channels;
  cfg->cfg.raw_file.format = fmt;
  snprintf(cfg->cfg.raw_file.filename, sizeof(cfg->cfg.raw_file.filename), "%s",
           path);
}

// F-01: a short (final) read must zero the chunk after valid_frames, because
// the pipeline processes the full chunk capacity.
TEST(AuditFileCapturePartialChunkTailZeroed) {
  char path[256];
  snprintf(path, sizeof(path), "/tmp/test_audit_file_tail_%d.raw", getpid());
  FILE *f = fopen(path, "wb");
  ASSERT_TRUE(f != NULL);
  // 6 stereo S16 frames of 0x4000 (= 0.5).
  for (int i = 0; i < 12; i++) {
    int16_t s = 0x4000;
    fwrite(&s, sizeof(s), 1, f);
  }
  fclose(f);

  capture_device_config_t cfg;
  audit_file_raw_capture_cfg(&cfg, path, 2, BINARY_SAMPLE_FORMAT_S16_LE);
  backend_error_t err;
  backend_error_init(&err, BACKEND_ERROR_NONE, "");
  capture_backend_t *cap =
      create_capture_backend(&cfg, 44100, 4, false, NULL, &err);
  ASSERT_TRUE(cap != NULL);
  ASSERT_TRUE(capture_backend_open(cap, &err));

  audio_chunk_t *chunk = audio_chunk_create(4, 2);
  audit_file_fill_chunk(chunk, 7.0);
  ASSERT_TRUE(capture_backend_read(cap, 4, chunk, &err));
  ASSERT_EQ(4, (int)audio_chunk_get_valid_frames(chunk));

  // Second read only gets 2 frames; the stale 7.0 values must be cleared.
  audit_file_fill_chunk(chunk, 7.0);
  ASSERT_TRUE(capture_backend_read(cap, 4, chunk, &err));
  ASSERT_EQ(2, (int)audio_chunk_get_valid_frames(chunk));
  for (size_t c = 0; c < 2; c++) {
    double *d = audio_chunk_get_channel(chunk, c);
    ASSERT_NEAR(0.5, d[0], 1e-6);
    ASSERT_NEAR(0.5, d[1], 1e-6);
    ASSERT_NEAR(0.0, d[2], 0.0);
    ASSERT_NEAR(0.0, d[3], 0.0);
  }

  capture_backend_close(cap);
  capture_backend_free(cap);
  audio_chunk_free(chunk);
  remove(path);
}

#ifndef _WIN32
/// Redirects stdin to a fresh pipe; returns the write end and saves the old
/// stdin in @p saved.
static int audit_file_stdin_pipe(int *saved) {
  int fds[2];
  if (pipe(fds) != 0)
    return -1;
  *saved = dup(STDIN_FILENO);
  dup2(fds[0], STDIN_FILENO);
  close(fds[0]);
  return fds[1];
}

static void audit_file_stdin_restore(int saved) {
  dup2(saved, STDIN_FILENO);
  clearerr(stdin);
  close(saved);
}

static capture_backend_t *audit_file_open_stdin(uint64_t skip_bytes,
                                                backend_error_t *err) {
  capture_device_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.type = AUDIO_BACKEND_TYPE_STDIN_OUT;
  cfg.cfg.stdin_in.channels = 2;
  cfg.cfg.stdin_in.format = BINARY_SAMPLE_FORMAT_S16_LE;
  if (skip_bytes > 0) {
    cfg.cfg.stdin_in.skip_bytes = (int64_t)skip_bytes;
    cfg.cfg.stdin_in.has_skip_bytes = true;
  }
  capture_backend_t *cap =
      create_capture_backend(&cfg, 44100, 4, false, NULL, err);
  if (cap && !capture_backend_open(cap, err)) {
    capture_backend_free(cap);
    return NULL;
  }
  return cap;
}

// F-03 + F-04: a timeout after a sub-frame partial read is "no data yet"
// (not EOF), and the partial frame is kept so the stream stays aligned.
TEST(AuditFileCaptureSubFrameTimeoutKeepsAlignment) {
  int saved = -1;
  int wfd = audit_file_stdin_pipe(&saved);
  ASSERT_TRUE(wfd >= 0);

  backend_error_t err;
  backend_error_init(&err, BACKEND_ERROR_NONE, "");
  capture_backend_t *cap = audit_file_open_stdin(0, &err);
  ASSERT_TRUE(cap != NULL);
  audio_chunk_t *chunk = audio_chunk_create(4, 2);

  // Frame 0 = (0x1000, 0x2000), frame 1 = (0x3000, 0x4000).
  const uint8_t bytes[8] = {0x00, 0x10, 0x00, 0x20, 0x00, 0x30, 0x00, 0x40};
  ASSERT_EQ(3, (int)write(wfd, bytes, 3)); // 3 of 4 bytes of frame 0

  backend_error_init(&err, BACKEND_ERROR_NONE, "");
  ASSERT_TRUE(!capture_backend_read(cap, 4, chunk, &err));
  ASSERT_EQ(BACKEND_ERROR_NONE, err.type); // not READ_EOF

  ASSERT_EQ(5, (int)write(wfd, bytes + 3, 5)); // rest of frame 0 + frame 1
  backend_error_init(&err, BACKEND_ERROR_NONE, "");
  ASSERT_TRUE(capture_backend_read(cap, 4, chunk, &err));
  ASSERT_EQ(2, (int)audio_chunk_get_valid_frames(chunk));
  ASSERT_NEAR(0x1000 / 32768.0, audio_chunk_get_channel(chunk, 0)[0], 1e-9);
  ASSERT_NEAR(0x2000 / 32768.0, audio_chunk_get_channel(chunk, 1)[0], 1e-9);
  ASSERT_NEAR(0x3000 / 32768.0, audio_chunk_get_channel(chunk, 0)[1], 1e-9);
  ASSERT_NEAR(0x4000 / 32768.0, audio_chunk_get_channel(chunk, 1)[1], 1e-9);

  close(wfd);
  backend_error_init(&err, BACKEND_ERROR_NONE, "");
  ASSERT_TRUE(!capture_backend_read(cap, 4, chunk, &err));
  ASSERT_EQ(BACKEND_ERROR_READ_EOF, err.type);

  capture_backend_close(cap);
  capture_backend_free(cap);
  audio_chunk_free(chunk);
  audit_file_stdin_restore(saved);
}

// F-14: skip_bytes on a silent pipe must not block open(); the skip happens
// inside read() under the poll timeout.
TEST(AuditFileCaptureStdinSkipDoesNotBlockOpen) {
  int saved = -1;
  int wfd = audit_file_stdin_pipe(&saved);
  ASSERT_TRUE(wfd >= 0);

  backend_error_t err;
  backend_error_init(&err, BACKEND_ERROR_NONE, "");
  capture_backend_t *cap = audit_file_open_stdin(6, &err); // returns at once
  ASSERT_TRUE(cap != NULL);
  audio_chunk_t *chunk = audio_chunk_create(4, 2);

  backend_error_init(&err, BACKEND_ERROR_NONE, "");
  ASSERT_TRUE(!capture_backend_read(cap, 4, chunk, &err));
  ASSERT_EQ(BACKEND_ERROR_NONE, err.type);

  const uint8_t bytes[10] = {1, 2, 3, 4, 5, 6, 0x00, 0x10, 0x00, 0x20};
  ASSERT_EQ(10, (int)write(wfd, bytes, 10));
  backend_error_init(&err, BACKEND_ERROR_NONE, "");
  ASSERT_TRUE(capture_backend_read(cap, 4, chunk, &err));
  ASSERT_EQ(1, (int)audio_chunk_get_valid_frames(chunk));
  ASSERT_NEAR(0x1000 / 32768.0, audio_chunk_get_channel(chunk, 0)[0], 1e-9);
  ASSERT_NEAR(0x2000 / 32768.0, audio_chunk_get_channel(chunk, 1)[0], 1e-9);

  close(wfd);
  capture_backend_close(cap);
  capture_backend_free(cap);
  audio_chunk_free(chunk);
  audit_file_stdin_restore(saved);
}
#endif

TEST_MAIN()
