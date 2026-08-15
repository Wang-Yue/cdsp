#include "Backend/alsa_capabilities.h"

#if defined(ENABLE_ALSA)

#include <alsa/asoundlib.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "Backend/alsa_device.h"

// Standard sample rates matching STANDARD_RATES in upstream CamillaDSP
// (src/lib.rs:721-724)
static const unsigned int STANDARD_RATES[] = {
    5512,  8000,  11025,  16000,  22050,  32000,  44100,  48000, 64000,
    88200, 96000, 176400, 192000, 352800, 384000, 705600, 768000};
static const size_t STANDARD_RATES_COUNT =
    sizeof(STANDARD_RATES) / sizeof(STANDARD_RATES[0]);

static const unsigned int CAPABILITY_PROBE_CHANNEL_LIMIT = 128;

// Lists hardware devices matching get_card_names and list_hw_devices in
// upstream (src/alsa_backend/utils.rs:85-133)
static int list_hw_devices(bool input, char out_names[][256], int max_names,
                           int start_idx) {
  int count = start_idx;
  int card = -1;
  snd_pcm_stream_t stream =
      input ? SND_PCM_STREAM_CAPTURE : SND_PCM_STREAM_PLAYBACK;

  while (snd_card_next(&card) == 0 && card >= 0 && count < max_names) {
    char ctl_id[32];
    snprintf(ctl_id, sizeof(ctl_id), "hw:%d", card);
    snd_ctl_t* ctl = NULL;
    if (snd_ctl_open(&ctl, ctl_id, 0) < 0 || !ctl) {
      continue;
    }

    snd_ctl_card_info_t* cardinfo;
    snd_ctl_card_info_alloca(&cardinfo);
    if (snd_ctl_card_info(ctl, cardinfo) < 0) {
      snd_ctl_close(ctl);
      continue;
    }

    const char* card_id = snd_ctl_card_info_get_id(cardinfo);
    const char* card_name = snd_ctl_card_info_get_name(cardinfo);

    int dev = -1;
    while (snd_ctl_pcm_next_device(ctl, &dev) == 0 && dev >= 0 &&
           count < max_names) {
      snd_pcm_info_t* pcm_info;
      snd_pcm_info_alloca(&pcm_info);
      snd_pcm_info_set_device(pcm_info, (unsigned int)dev);
      snd_pcm_info_set_subdevice(pcm_info, 0);
      snd_pcm_info_set_stream(pcm_info, stream);

      if (snd_ctl_pcm_info(ctl, pcm_info) < 0) {
        continue;
      }

      const char* pcm_name = snd_pcm_info_get_name(pcm_info);
      unsigned int subdevs = snd_pcm_info_get_subdevices_count(pcm_info);

      for (unsigned int subdev = 0; subdev < subdevs && count < max_names;
           subdev++) {
        snd_pcm_info_set_subdevice(pcm_info, subdev);
        if (snd_ctl_pcm_info(ctl, pcm_info) < 0) {
          continue;
        }
        const char* subdev_name = snd_pcm_info_get_subdevice_name(pcm_info);
        if (subdev_name && subdev_name[0]) {
          snprintf(out_names[count++], 256, "hw:%s,%d,%d (%s, %s, %s)", card_id,
                   dev, subdev, card_name ? card_name : "",
                   pcm_name ? pcm_name : "", subdev_name);
        } else {
          snprintf(out_names[count++], 256, "hw:%s,%d,%d (%s, %s)", card_id,
                   dev, subdev, card_name ? card_name : "",
                   pcm_name ? pcm_name : "");
        }
      }
    }
    snd_ctl_close(ctl);
  }
  return count;
}

// Lists PCM hint devices matching list_pcm_devices in upstream
// (src/alsa_backend/utils.rs:135-156)
static int list_pcm_devices(bool input, char out_names[][256], int max_names,
                            int start_idx) {
  int count = start_idx;
  void** hints = NULL;
  if (snd_device_name_hint(-1, "pcm", &hints) < 0 || !hints) {
    return count;
  }

  for (void** h = hints; *h && count < max_names; ++h) {
    char* name = snd_device_name_get_hint(*h, "NAME");
    if (!name) continue;

    char* ioid = snd_device_name_get_hint(*h, "IOID");
    if (ioid) {
      if (input && strcasecmp(ioid, "Output") == 0) {
        free(ioid);
        free(name);
        continue;
      }
      if (!input && strcasecmp(ioid, "Input") == 0) {
        free(ioid);
        free(name);
        continue;
      }
      free(ioid);
    }

    char* desc = snd_device_name_get_hint(*h, "DESC");
    if (desc && desc[0]) {
      // Replace newlines with spaces for clean display
      char desc_clean[256];
      snprintf(desc_clean, sizeof(desc_clean), "%s", desc);
      for (char* p = desc_clean; *p; p++) {
        if (*p == '\n' || *p == '\r') *p = ' ';
      }
      snprintf(out_names[count++], 256, "%.120s (%.120s)", name, desc_clean);
      free(desc);
    } else {
      snprintf(out_names[count++], 256, "%s", name);
    }
    free(name);
  }

  snd_device_name_free_hint(hints);
  return count;
}

// Combines HW and PCM devices matching list_device_names in upstream
// (src/alsa_backend/utils.rs:158-163)
int alsa_capabilities_available_device_names(bool is_capture,
                                             char out_names[][256],
                                             int max_names) {
  pthread_mutex_lock(&g_alsa_mutex);
  int count = list_hw_devices(is_capture, out_names, max_names, 0);
  count = list_pcm_devices(is_capture, out_names, max_names, count);
  pthread_mutex_unlock(&g_alsa_mutex);
  return count;
}

// Queries ALSA device capabilities matching get_device_capabilities in upstream
// (src/alsa_backend/utils.rs:165-253)
audio_device_descriptor_t* alsa_capabilities_describe(const char* device_name,
                                                      bool is_capture,
                                                      device_error_t* err) {
  if (!device_name || device_name[0] == '\0') {
    device_name = "default";
  }
  pthread_mutex_lock(&g_alsa_mutex);
  audio_device_descriptor_t* desc =
      (audio_device_descriptor_t*)calloc(1, sizeof(audio_device_descriptor_t));
  if (!desc) {
    if (err) {
      device_error_init(err, DEVICE_ERROR_OTHER, "Out of memory");
    }
    pthread_mutex_unlock(&g_alsa_mutex);
    return NULL;
  }
  snprintf(desc->name, sizeof(desc->name), "%s", device_name);

  snd_pcm_stream_t stream =
      is_capture ? SND_PCM_STREAM_CAPTURE : SND_PCM_STREAM_PLAYBACK;
  snd_pcm_t* pcm = NULL;
  int open_res = snd_pcm_open(&pcm, device_name, stream, SND_PCM_NONBLOCK);
  if (open_res < 0) {
    if (err) {
      if (open_res == -EBUSY) {
        device_error_init(err, DEVICE_ERROR_BUSY, "Device or resource busy");
      } else if (open_res == -ENOENT || open_res == -ENODEV) {
        device_error_init(err, DEVICE_ERROR_NOT_FOUND, "Device not found");
      } else {
        char msg[256];
        snprintf(msg, sizeof(msg), "ALSA open failed: %s",
                 snd_strerror(open_res));
        device_error_init(err, DEVICE_ERROR_OTHER, msg);
      }
    }
    goto error_cleanup;
  }

  snd_pcm_hw_params_t* hwp = NULL;
  snd_pcm_hw_params_alloca(&hwp);
  if (snd_pcm_hw_params_any(pcm, hwp) < 0) {
    if (err) {
      device_error_init(err, DEVICE_ERROR_OTHER,
                        "Failed to query ALSA hardware parameters");
    }
    goto error_cleanup;
  }

  unsigned int min_ch = 1, max_ch = 2;
  snd_pcm_hw_params_get_channels_min(hwp, &min_ch);
  snd_pcm_hw_params_get_channels_max(hwp, &max_ch);
  unsigned int check_max = max_ch < CAPABILITY_PROBE_CHANNEL_LIMIT
                               ? max_ch
                               : CAPABILITY_PROBE_CHANNEL_LIMIT;

  desc->capability_sets_count = 1;
  desc->capability_sets =
      (device_capability_set_t*)calloc(1, sizeof(device_capability_set_t));
  if (!desc->capability_sets) goto error_cleanup;

  device_capability_set_t* set = &desc->capability_sets[0];
  snprintf(set->mode, sizeof(set->mode), "Unified");

  size_t cap_alloc = (check_max >= min_ch) ? (check_max - min_ch + 1) : 1;
  set->capabilities =
      (channel_capability_t*)calloc(cap_alloc, sizeof(channel_capability_t));
  if (!set->capabilities) goto error_cleanup;

  size_t cap_idx = 0;

  for (unsigned int ch = min_ch; ch <= check_max; ch++) {
    if (snd_pcm_hw_params_test_channels(pcm, hwp, ch) != 0) {
      continue;
    }

    if (snd_pcm_hw_params_any(pcm, hwp) < 0 ||
        snd_pcm_hw_params_set_channels(pcm, hwp, ch) < 0) {
      continue;
    }

    unsigned int min_rate = 0, max_rate = 0;
    snd_pcm_hw_params_get_rate_min(hwp, &min_rate, NULL);
    snd_pcm_hw_params_get_rate_max(hwp, &max_rate, NULL);

    bool is_range =
        (min_rate != max_rate &&
         snd_pcm_hw_params_test_rate(pcm, hwp, min_rate + 1, 0) == 0);

    channel_capability_t* cap = &set->capabilities[cap_idx];
    cap->channels = (int)ch;
    cap->samplerates = (samplerate_capability_t*)calloc(
        STANDARD_RATES_COUNT + 1, sizeof(samplerate_capability_t));
    if (!cap->samplerates) goto error_cleanup;

    size_t rate_idx = 0;

    for (size_t r = 0; r < STANDARD_RATES_COUNT; r++) {
      unsigned int test_rate = STANDARD_RATES[r];
      if (is_range) {
        if (test_rate < min_rate || test_rate > max_rate) continue;
      } else if (min_rate == max_rate) {
        if (test_rate != min_rate) continue;
      } else {
        if (snd_pcm_hw_params_test_rate(pcm, hwp, test_rate, 0) != 0) continue;
      }

      if (snd_pcm_hw_params_any(pcm, hwp) < 0 ||
          snd_pcm_hw_params_set_channels(pcm, hwp, ch) < 0) {
        continue;
      }

      unsigned int applied_rate = test_rate;
      int dir = 0;
      if (snd_pcm_hw_params_set_rate_near(pcm, hwp, &applied_rate, &dir) < 0 ||
          applied_rate != test_rate) {
        continue;
      }

      // Check formats matching list_formats in upstream
      // (src/alsa_backend/utils.rs:408-431)
      const snd_pcm_format_t test_fmts[] = {
          SND_PCM_FORMAT_S16_LE,     SND_PCM_FORMAT_S24_LE,
          SND_PCM_FORMAT_S24_3LE,    SND_PCM_FORMAT_S32_LE,
          SND_PCM_FORMAT_FLOAT_LE,   SND_PCM_FORMAT_FLOAT64_LE,
          SND_PCM_FORMAT_DSD_U8,     SND_PCM_FORMAT_DSD_U16_LE,
          SND_PCM_FORMAT_DSD_U16_BE, SND_PCM_FORMAT_DSD_U32_LE,
          SND_PCM_FORMAT_DSD_U32_BE};
      const char* fmt_names[] = {"S16_LE",     "S24_4_LE",   "S24_3_LE",
                                 "S32_LE",     "F32_LE",     "F64_LE",
                                 "DSD_U8",     "DSD_U16_LE", "DSD_U16_BE",
                                 "DSD_U32_LE", "DSD_U32_BE"};
      const size_t num_test_fmts = sizeof(test_fmts) / sizeof(test_fmts[0]);

      samplerate_capability_t* rate_cap = &cap->samplerates[rate_idx];
      rate_cap->samplerate = (int)test_rate;
      rate_cap->formats = (char**)calloc(num_test_fmts, sizeof(char*));
      if (!rate_cap->formats) goto error_cleanup;

      size_t fmt_idx = 0;
      for (size_t f = 0; f < num_test_fmts; f++) {
        if (snd_pcm_hw_params_test_format(pcm, hwp, test_fmts[f]) == 0) {
          rate_cap->formats[fmt_idx++] = strdup(fmt_names[f]);
        }
      }

      if (fmt_idx > 0) {
        rate_cap->formats_count = fmt_idx;
        rate_idx++;
      } else {
        free(rate_cap->formats);
        rate_cap->formats = NULL;
        rate_cap->formats_count = 0;
      }
    }

    if (rate_idx > 0) {
      cap->samplerates_count = rate_idx;
      cap_idx++;
    } else {
      free(cap->samplerates);
      cap->samplerates = NULL;
      cap->samplerates_count = 0;
    }
  }

  set->capabilities_count = cap_idx;
  snd_pcm_close(pcm);
  pthread_mutex_unlock(&g_alsa_mutex);
  return desc;

error_cleanup:
  if (pcm) {
    snd_pcm_close(pcm);
  }
  if (desc) {
    free_audio_device_descriptor(desc);
  }
  pthread_mutex_unlock(&g_alsa_mutex);
  return NULL;
}

#endif  // defined(ENABLE_ALSA)
