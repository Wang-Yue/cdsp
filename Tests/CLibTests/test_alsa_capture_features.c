#if defined(__linux__) && defined(ENABLE_ALSA)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <alsa/asoundlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "Audio/audio_chunk.h"
#include "Backend/audio_backend.h"
#include "Backend/backend_error.h"
#include "Config/engine_config_types.h"
#include "Utils/cdsp_time.h"
#include "test_support.h"

static void remove_capture_rate_ctl_if_present(void) {
  snd_ctl_t* ctl = NULL;
  if (snd_ctl_open(&ctl, "hw:0", 0) >= 0 && ctl) {
    snd_ctl_elem_id_t* id;
    snd_ctl_elem_id_alloca(&id);
    snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_PCM);
    snd_ctl_elem_id_set_device(id, 1);
    snd_ctl_elem_id_set_subdevice(id, 1);
    snd_ctl_elem_id_set_name(id, "Capture Rate");
    snd_ctl_elem_remove(ctl, id);
    snd_ctl_close(ctl);
  }
}

TEST(ALSACapture_StopOnInactive_SourceStatus) {
  remove_capture_rate_ctl_if_present();

  // 1. Configure capture on ALSA Loopback device 1, subdevice 1 with
  // stop_on_inactive = true
  capture_device_config_t cap_cfg;
  memset(&cap_cfg, 0, sizeof(cap_cfg));
  cap_cfg.type = AUDIO_BACKEND_TYPE_ALSA;
  cap_cfg.cfg.alsa.channels = 2;
  snprintf(cap_cfg.cfg.alsa.device, sizeof(cap_cfg.cfg.alsa.device),
           "hw:Loopback,1,1");
  cap_cfg.cfg.alsa.format = ALSA_SAMPLE_FORMAT_S16_LE;
  cap_cfg.cfg.alsa.has_format = true;
  cap_cfg.cfg.alsa.stop_on_inactive = true;
  cap_cfg.cfg.alsa.has_stop_on_inactive = true;

  backend_error_t err;
  capture_backend_t* capture =
      create_capture_backend(&cap_cfg, 48000, 1024, false, NULL, &err);
  if (!capture) {
    printf("ALSA capture creation failed: %s (skipping test)\n", err.message);
    return;
  }

  if (!capture_backend_open(capture, &err)) {
    printf("ALSA capture open failed: %s (skipping test)\n", err.message);
    capture_backend_free(capture);
    return;
  }

  audio_chunk_t* chunk = audio_chunk_create(1024, 2);

  // 2. Case A: When slave (Loopback 0,1) is inactive, read should detect
  // inactive and return false
  bool read_inactive = capture_backend_read(capture, 128, chunk, &err);
  ASSERT_FALSE(read_inactive);
  ASSERT_STR_EQ("Capture source inactive", err.message);

  // 3. Case B: Start playback on Loopback 0,1 to activate PCM Slave Active
  snd_pcm_t* play_pcm = NULL;
  int rc =
      snd_pcm_open(&play_pcm, "hw:Loopback,0,1", SND_PCM_STREAM_PLAYBACK, 0);
  if (rc >= 0 && play_pcm) {
    snd_pcm_set_params(play_pcm, SND_PCM_FORMAT_S16_LE,
                       SND_PCM_ACCESS_RW_INTERLEAVED, 2, 48000, 1, 500000);
    int16_t silence[1024 * 2] = {0};
    for (int i = 0; i < 8; i++) {
      snd_pcm_writei(play_pcm, silence, 1024);
    }
    snd_pcm_start(play_pcm);
    cdsp_sleep_ms(20);

    bool read_active = false;
    for (int retry = 0; retry < 5; retry++) {
      if (capture_backend_read(capture, 128, chunk, &err)) {
        read_active = true;
        break;
      }
      cdsp_sleep_ms(10);
    }
    ASSERT_TRUE(read_active);

    snd_pcm_close(play_pcm);
  }

  audio_chunk_free(chunk);
  capture_backend_close(capture);
  capture_backend_free(capture);
}

TEST(ALSACapture_DynamicRateChange_HCtlMonitoring) {
  remove_capture_rate_ctl_if_present();

  // 1. Add temporary "Capture Rate" user control on hw:0
  snd_ctl_t* ctl = NULL;
  snd_ctl_elem_id_t* id;
  snd_ctl_elem_id_alloca(&id);
  snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_PCM);
  snd_ctl_elem_id_set_device(id, 1);
  snd_ctl_elem_id_set_subdevice(id, 1);
  snd_ctl_elem_id_set_name(id, "Capture Rate");

  int ctl_err = snd_ctl_open(&ctl, "hw:0", 0);
  bool ctl_added = false;
  if (ctl_err >= 0 && ctl) {
    if (snd_ctl_elem_add_integer(ctl, id, 1, 0, 192000, 1) >= 0) {
      ctl_added = true;
      snd_ctl_elem_value_t* val;
      snd_ctl_elem_value_alloca(&val);
      snd_ctl_elem_value_set_id(val, id);
      snd_ctl_elem_value_set_integer(val, 0, 96000);
      snd_ctl_elem_write(ctl, val);
    }
  }

  capture_device_config_t cap_cfg;
  memset(&cap_cfg, 0, sizeof(cap_cfg));
  cap_cfg.type = AUDIO_BACKEND_TYPE_ALSA;
  cap_cfg.cfg.alsa.channels = 2;
  snprintf(cap_cfg.cfg.alsa.device, sizeof(cap_cfg.cfg.alsa.device),
           "hw:Loopback,1,1");
  cap_cfg.cfg.alsa.format = ALSA_SAMPLE_FORMAT_S16_LE;
  cap_cfg.cfg.alsa.has_format = true;

  backend_error_t err;
  capture_backend_t* capture =
      create_capture_backend(&cap_cfg, 48000, 1024, false, NULL, &err);
  if (!capture) {
    printf("ALSA capture creation failed: %s (skipping test)\n", err.message);
    if (ctl && ctl_added) {
      snd_ctl_elem_remove(ctl, id);
      snd_ctl_close(ctl);
    }
    return;
  }

  if (!capture_backend_open(capture, &err)) {
    printf("ALSA capture open failed: %s (skipping test)\n", err.message);
    capture_backend_free(capture);
    if (ctl && ctl_added) {
      snd_ctl_elem_remove(ctl, id);
      snd_ctl_close(ctl);
    }
    return;
  }

  double pending_rate = 0.0;
  bool has_change =
      capture_backend_get_pending_rate_change(capture, &pending_rate);

  // Assert that dynamic rate change to 96000 was detected from the HCtl element
  ASSERT_TRUE(has_change);
  ASSERT_EQ((int)pending_rate, 96000);

  capture_backend_close(capture);
  capture_backend_free(capture);

  if (ctl) {
    if (ctl_added) {
      snd_ctl_elem_remove(ctl, id);
    }
    snd_ctl_close(ctl);
  }
}

TEST_MAIN()

#else

#include "test_support.h"
TEST(ALSASkippedOnNonLinux) {}
TEST_MAIN()

#endif
