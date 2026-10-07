// Regression tests for the ALSA backend audit
// (audit_reports/backends/01_alsa.md). They need the snd-aloop "Loopback" card;
// every test skips cleanly when it is not present.
#if defined(__linux__) && defined(ENABLE_ALSA)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <alsa/asoundlib.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "audio/audio_chunk.h"
#include "backend/alsa_capabilities.h"
#include "backend/audio_backend.h"
#include "backend/backend_error.h"
#include "test_support.h"

// cdsp_sleep_ms is time-scaled in test builds; real ALSA hardware is not.
static void hw_sleep_ms(long ms) {
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000L;
  nanosleep(&ts, NULL);
}

static bool loopback_present(void) {
  snd_ctl_t *ctl = NULL;
  if (snd_ctl_open(&ctl, "hw:Loopback", 0) < 0 || !ctl)
    return false;
  snd_ctl_close(ctl);
  return true;
}

static playback_backend_t *open_loopback_playback(const char *dev,
                                                  backend_error_t *err) {
  playback_device_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.type = AUDIO_BACKEND_TYPE_ALSA;
  cfg.cfg.alsa.channels = 2;
  snprintf(cfg.cfg.alsa.device, sizeof(cfg.cfg.alsa.device), "%s", dev);
  cfg.cfg.alsa.format = ALSA_SAMPLE_FORMAT_S16_LE;
  cfg.cfg.alsa.has_format = true;
  playback_backend_t *pb =
      create_playback_backend(&cfg, 48000, 1024, false, NULL, err);
  if (!pb)
    return NULL;
  if (!playback_backend_open(pb, err)) {
    playback_backend_free(pb);
    return NULL;
  }
  return pb;
}

// H1: the engine passes the playback clock multiplier (1/speed). The gadget
// control must receive multiplier * 1e6, i.e. upstream's 1e6 / speed.
TEST(ALSAAudit_PlaybackPitch_NotDoubleInverted) {
  if (!loopback_present()) {
    printf("snd-aloop not present (skipping test)\n");
    return;
  }
  snd_ctl_t *ctl = NULL;
  ASSERT_TRUE(snd_ctl_open(&ctl, "hw:Loopback", 0) >= 0);
  snd_ctl_elem_id_t *id;
  snd_ctl_elem_id_alloca(&id);
  snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_PCM);
  snd_ctl_elem_id_set_device(id, 0);
  snd_ctl_elem_id_set_subdevice(id, 0);
  snd_ctl_elem_id_set_name(id, "Playback Pitch 1000000");
  snd_ctl_elem_remove(ctl, id);
  if (snd_ctl_elem_add_integer(ctl, id, 1, 500000, 2000000, 1) < 0) {
    printf("cannot add user control (skipping test)\n");
    snd_ctl_close(ctl);
    return;
  }
  // The kernel makes the adding handle the owner of a user control; only the
  // owner may write it until it is unlocked.
  snd_ctl_elem_unlock(ctl, id);

  backend_error_t err;
  playback_backend_t *pb = open_loopback_playback("hw:Loopback,0,0", &err);
  if (!pb) {
    printf("ALSA playback open failed: %s (skipping test)\n", err.message);
    snd_ctl_elem_remove(ctl, id);
    snd_ctl_close(ctl);
    return;
  }
  ASSERT_TRUE(playback_backend_pitch_control_supported(pb));

  const double speed = 1.001;
  playback_backend_set_pitch(pb, 1.0 / speed);

  snd_ctl_elem_value_t *val;
  snd_ctl_elem_value_alloca(&val);
  snd_ctl_elem_value_set_id(val, id);
  ASSERT_TRUE(snd_ctl_elem_read(ctl, val) >= 0);
  long written = snd_ctl_elem_value_get_integer(val, 0);
  long expected = (long)(1000000.0 / speed);
  printf("pitch control written=%ld expected=%ld\n", written, expected);
  ASSERT_TRUE(labs(written - expected) <= 1);

  playback_backend_close(pb);
  playback_backend_free(pb);
  snd_ctl_elem_remove(ctl, id);
  snd_ctl_close(ctl);
}

static capture_backend_t *open_loopback_capture(const char *dev,
                                                backend_error_t *err) {
  capture_device_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.type = AUDIO_BACKEND_TYPE_ALSA;
  cfg.cfg.alsa.channels = 2;
  snprintf(cfg.cfg.alsa.device, sizeof(cfg.cfg.alsa.device), "%s", dev);
  cfg.cfg.alsa.format = ALSA_SAMPLE_FORMAT_S16_LE;
  cfg.cfg.alsa.has_format = true;
  capture_backend_t *cap =
      create_capture_backend(&cfg, 48000, 1024, false, NULL, err);
  if (!cap)
    return NULL;
  if (!capture_backend_open(cap, err)) {
    capture_backend_free(cap);
    return NULL;
  }
  return cap;
}

// H3 + L7: once the capture inner thread has stopped, read() must first hand
// out the frames still queued in the ring and then report a real error. It
// used to return BACKEND_ERROR_NONE ("no data yet") at once, which dropped the
// queued frames and made the engine capture loop spin forever.
TEST(ALSAAudit_CaptureStopped_DrainsRingThenReportsError) {
  if (!loopback_present()) {
    printf("snd-aloop not present (skipping test)\n");
    return;
  }
  backend_error_t err;
  capture_backend_t *cap = open_loopback_capture("hw:Loopback,1,0", &err);
  if (!cap) {
    printf("ALSA capture open failed: %s (skipping test)\n", err.message);
    return;
  }
  audio_chunk_t *chunk = audio_chunk_create(1024, 2);
  ASSERT_TRUE(chunk != NULL);

  // snd-aloop delivers silence on the capture side even without a playback
  // stream; let a few periods accumulate in the ring.
  hw_sleep_ms(150);
  capture_backend_stop(cap);

  int ok_reads = 0;
  bool got_error = false;
  for (int i = 0; i < 1000; i++) {
    memset(&err, 0, sizeof(err));
    if (capture_backend_read(cap, 128, chunk, &err)) {
      ok_reads++;
      continue;
    }
    ASSERT_TRUE(err.type != BACKEND_ERROR_NONE);
    ASSERT_EQ(BACKEND_ERROR_READ_ERROR, err.type);
    got_error = true;
    break;
  }
  printf("drained %d reads of 128 frames before error\n", ok_reads);
  ASSERT_TRUE(ok_reads > 0);
  ASSERT_TRUE(got_error);

  audio_chunk_free(chunk);
  capture_backend_close(cap);
  capture_backend_free(cap);
}

typedef struct {
  _Atomic bool done;
  audio_device_descriptor_t *desc;
  device_error_t err;
} describe_ctx_t;

static void *describe_thread(void *arg) {
  describe_ctx_t *ctx = (describe_ctx_t *)arg;
  ctx->desc = alsa_capabilities_describe("hw:Loopback,0,0", false, &ctx->err);
  atomic_store(&ctx->done, true);
  return NULL;
}

// H4: describing a hw: device that cdsp itself holds open must return
// DEVICE_ERROR_BUSY promptly instead of sleeping in snd_pcm_open() while
// holding g_alsa_mutex (which would deadlock our own close()). With the stock
// alsa.conf (defaults.pcm.nonblock 1) alsa-lib already opens hw: devices
// non-blocking, so this also passes on the old code; describe now passes
// SND_PCM_NONBLOCK explicitly so it no longer depends on that default.
TEST(ALSAAudit_DescribeBusyDevice_ReturnsBusy) {
  if (!loopback_present()) {
    printf("snd-aloop not present (skipping test)\n");
    return;
  }
  backend_error_t err;
  playback_backend_t *pb = open_loopback_playback("hw:Loopback,0,0", &err);
  if (!pb) {
    printf("ALSA playback open failed: %s (skipping test)\n", err.message);
    return;
  }
  // The kernel only sleeps in a blocking open when *every* substream is busy
  // (the normal case for a single-substream USB DAC). Occupy the remaining
  // Loopback substreams to reproduce that.
  snd_pcm_t *others[8] = {0};
  int n_others = 0;
  for (int sub = 1; sub < 8; sub++) {
    char name[32];
    snprintf(name, sizeof(name), "hw:Loopback,0,%d", sub);
    if (snd_pcm_open(&others[n_others], name, SND_PCM_STREAM_PLAYBACK,
                     SND_PCM_NONBLOCK) < 0)
      break;
    n_others++;
  }
  printf("occupied %d extra substreams\n", n_others);
  static describe_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  pthread_t th;
  ASSERT_TRUE(pthread_create(&th, NULL, describe_thread, &ctx) == 0);
  bool finished = false;
  for (int i = 0; i < 300; i++) {
    if (atomic_load(&ctx.done)) {
      finished = true;
      break;
    }
    hw_sleep_ms(10);
  }
  if (!finished) {
    // The describe thread is stuck in the kernel holding g_alsa_mutex; any
    // further ALSA call from this process would hang too.
    printf("describe still blocked after 3 s\n");
    fflush(stdout);
    _exit(1);
  }
  pthread_join(th, NULL);
  printf("describe err=%d msg=%s\n", (int)ctx.err.type, ctx.err.message);
  for (int i = 0; i < n_others; i++)
    snd_pcm_close(others[i]);
  ASSERT_TRUE(ctx.desc == NULL);
  ASSERT_EQ(DEVICE_ERROR_BUSY, ctx.err.type);

  playback_backend_close(pb);
  playback_backend_free(pb);
}

TEST_MAIN()

#else

#include "test_support.h"
TEST(ALSAAuditSkippedOnNonLinux) {}
TEST_MAIN()

#endif
