/**
 * @file test_audit_backend_file_playback.c
 * @brief Regression tests for the file/stdout playback fixes from the backend
 * audit (audit_reports/backends/06_file_generator.md).
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

#include "audio/audio_chunk.h"
#include "backend/audio_backend.h"
#include "backend/backend_error.h"
#include "backend/file_backend.h"
#include "config/configuration.h"
#include "test_support.h"

#ifndef _WIN32
// F-05: Stdout playback must not leave audio sitting in the stdio buffer;
// each chunk is visible to a pipe reader right after write().
TEST(AuditFilePlaybackStdoutFlushedPerChunk) {
  fflush(stdout);
  int fds[2];
  ASSERT_TRUE(pipe(fds) == 0);
  int saved = dup(STDOUT_FILENO);
  dup2(fds[1], STDOUT_FILENO);
  close(fds[1]);
  fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);

  playback_device_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.type = AUDIO_BACKEND_TYPE_STDIN_OUT;
  cfg.cfg.stdout_out.channels = 2;
  cfg.cfg.stdout_out.format = BINARY_SAMPLE_FORMAT_S16_LE;
  cfg.cfg.stdout_out.wav_header = false;

  backend_error_t err;
  backend_error_init(&err, BACKEND_ERROR_NONE, "");
  playback_backend_t *pb =
      create_playback_backend(&cfg, 44100, 4, false, NULL, &err);
  bool opened = pb && playback_backend_open(pb, &err);

  audio_chunk_t *chunk = audio_chunk_create(4, 2);
  audio_chunk_set_valid_frames(chunk, 4);
  bool wrote = opened && playback_backend_write(pb, chunk, &err);

  uint8_t buf[64];
  ssize_t n = read(fds[0], buf, sizeof(buf));

  if (pb) {
    playback_backend_close(pb);
    playback_backend_free(pb);
  }
  audio_chunk_free(chunk);
  fflush(stdout);
  dup2(saved, STDOUT_FILENO);
  close(saved);
  close(fds[0]);

  ASSERT_TRUE(opened);
  ASSERT_TRUE(wrote);
  ASSERT_EQ(16, (int)n); // 4 frames * 2 ch * 2 bytes, without close()
}
#endif

// F-12: a WAV header cannot describe DSD; reject at create, keep raw DSD.
TEST(AuditFilePlaybackWavRejectsDsdFormats) {
  playback_device_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.type = AUDIO_BACKEND_TYPE_FILE;
  cfg.cfg.raw_file.channels = 2;
  cfg.cfg.raw_file.format = BINARY_SAMPLE_FORMAT_DSD_U8;
  snprintf(cfg.cfg.raw_file.filename, sizeof(cfg.cfg.raw_file.filename), "%s",
           "test_audit_file_dsd_unused.wav"); // create() does not open

  backend_error_t err;
  backend_error_init(&err, BACKEND_ERROR_NONE, "");
  cfg.cfg.raw_file.wav_header = true;
  playback_backend_t *pb =
      create_playback_backend(&cfg, 352800, 64, false, NULL, &err);
  ASSERT_TRUE(pb == NULL);
  ASSERT_EQ(BACKEND_ERROR_INITIALIZATION_FAILED, err.type);

  backend_error_init(&err, BACKEND_ERROR_NONE, "");
  cfg.cfg.raw_file.wav_header = false;
  pb = create_playback_backend(&cfg, 352800, 64, false, NULL, &err);
  ASSERT_TRUE(pb != NULL);
  playback_backend_free(pb);
}

TEST_MAIN()
