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
#define STANDARD_RATES_COUNT \
  (sizeof(STANDARD_RATES) / sizeof(STANDARD_RATES[0]))

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

static void alsa_sanitize_device_name(const char* in_name, char* out_name,
                                      size_t out_len) {
  if (!in_name || in_name[0] == '\0') {
    snprintf(out_name, out_len, "default");
    return;
  }
  snprintf(out_name, out_len, "%s", in_name);
  char* paren = strstr(out_name, " (");
  if (paren) {
    *paren = '\0';
  } else if (out_name[0] != '\0' && out_name[0] != '(') {
    char* single_paren = strchr(out_name, '(');
    if (single_paren) {
      *single_paren = '\0';
    }
  }
  size_t len = strlen(out_name);
  while (len > 0 && (out_name[len - 1] == ' ' || out_name[len - 1] == '\t')) {
    out_name[--len] = '\0';
  }
  if (out_name[0] == '\0') {
    snprintf(out_name, out_len, "default");
  }
}

// Supported channel values matching supported_channel_values in upstream
// (src/alsa_backend/utils.rs:342-359)
static int supported_channel_values(snd_pcm_t* pcm, snd_pcm_hw_params_t* hwp,
                                    unsigned int limit,
                                    unsigned int* out_channels,
                                    size_t max_out) {
  unsigned int min_channels = 0, max_channels = 0;
  if (snd_pcm_hw_params_get_channels_min(hwp, &min_channels) < 0 ||
      snd_pcm_hw_params_get_channels_max(hwp, &max_channels) < 0) {
    return -1;
  }
  if (min_channels == max_channels) {
    if (max_out > 0) {
      out_channels[0] = min_channels;
      return 1;
    }
    return 0;
  }

  unsigned int check_max = max_channels < limit ? max_channels : limit;
  size_t count = 0;
  for (unsigned int chan = min_channels; chan <= check_max && count < max_out;
       chan++) {
    if (snd_pcm_hw_params_test_channels(pcm, hwp, chan) == 0) {
      out_channels[count++] = chan;
    }
  }
  return (int)count;
}

// Supported rate values matching SupportedValues & list_samplerates in
// upstream (src/alsa_backend/utils.rs:41-44 & 312-331)
typedef enum {
  SUPPORTED_RATES_RANGE,
  SUPPORTED_RATES_DISCRETE
} supported_rates_type_t;

typedef struct {
  supported_rates_type_t type;
  unsigned int min;
  unsigned int max;
  unsigned int discrete[STANDARD_RATES_COUNT];
  size_t discrete_count;
} supported_rates_t;

static bool list_samplerates(snd_pcm_t* pcm, snd_pcm_hw_params_t* hwp,
                             supported_rates_t* out_rates) {
  unsigned int min_rate = 0, max_rate = 0;
  if (snd_pcm_hw_params_get_rate_min(hwp, &min_rate, NULL) < 0 ||
      snd_pcm_hw_params_get_rate_max(hwp, &max_rate, NULL) < 0) {
    return false;
  }
  if (min_rate == max_rate) {
    out_rates->type = SUPPORTED_RATES_DISCRETE;
    out_rates->discrete[0] = min_rate;
    out_rates->discrete_count = 1;
    return true;
  } else if (snd_pcm_hw_params_test_rate(pcm, hwp, min_rate + 1, 0) == 0) {
    out_rates->type = SUPPORTED_RATES_RANGE;
    out_rates->min = min_rate;
    out_rates->max = max_rate;
    return true;
  }
  out_rates->type = SUPPORTED_RATES_DISCRETE;
  out_rates->discrete_count = 0;
  for (size_t i = 0; i < STANDARD_RATES_COUNT; i++) {
    if (snd_pcm_hw_params_test_rate(pcm, hwp, STANDARD_RATES[i], 0) == 0) {
      out_rates->discrete[out_rates->discrete_count++] = STANDARD_RATES[i];
    }
  }
  return true;
}

// Sample format map matching list_formats and alsa_format_to_str in upstream
// (src/alsa_backend/utils.rs:397-431)
static const struct {
  snd_pcm_format_t pcm_fmt;
  const char* str;
} FORMAT_MAP[] = {
    {SND_PCM_FORMAT_S16_LE, "S16_LE"},
    {SND_PCM_FORMAT_S24_LE, "S24_4_LE"},
    {SND_PCM_FORMAT_S24_3LE, "S24_3_LE"},
    {SND_PCM_FORMAT_S32_LE, "S32_LE"},
    {SND_PCM_FORMAT_FLOAT_LE, "F32_LE"},
    {SND_PCM_FORMAT_FLOAT64_LE, "F64_LE"},
    {SND_PCM_FORMAT_DSD_U8, "DSD_U8"},
    {SND_PCM_FORMAT_DSD_U16_LE, "DSD_U16_LE"},
    {SND_PCM_FORMAT_DSD_U16_BE, "DSD_U16_BE"},
    {SND_PCM_FORMAT_DSD_U32_LE, "DSD_U32_LE"},
    {SND_PCM_FORMAT_DSD_U32_BE, "DSD_U32_BE"},
};
#define FORMAT_MAP_COUNT (sizeof(FORMAT_MAP) / sizeof(FORMAT_MAP[0]))

static size_t list_formats(snd_pcm_t* pcm, snd_pcm_hw_params_t* hwp,
                           const char* out_formats[], size_t max_formats) {
  size_t count = 0;
  for (size_t i = 0; i < FORMAT_MAP_COUNT && count < max_formats; i++) {
    if (snd_pcm_hw_params_test_format(pcm, hwp, FORMAT_MAP[i].pcm_fmt) == 0) {
      out_formats[count++] = FORMAT_MAP[i].str;
    }
  }
  return count;
}

// Queries ALSA device capabilities matching get_device_capabilities in upstream
// (src/alsa_backend/utils.rs:165-253)
audio_device_descriptor_t* alsa_capabilities_describe(const char* device_name,
                                                      bool is_capture,
                                                      device_error_t* err) {
  char clean_dev[256];
  alsa_sanitize_device_name(device_name, clean_dev, sizeof(clean_dev));

  pthread_mutex_lock(&g_alsa_mutex);
  snd_pcm_stream_t stream =
      is_capture ? SND_PCM_STREAM_CAPTURE : SND_PCM_STREAM_PLAYBACK;
  snd_pcm_t* pcm = NULL;
  int open_res = snd_pcm_open(&pcm, clean_dev, stream, SND_PCM_NONBLOCK);
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
    pthread_mutex_unlock(&g_alsa_mutex);
    return NULL;
  }

  // 1. let hwp = HwParams::any(&pcm) (src/alsa_backend/utils.rs:189-193)
  snd_pcm_hw_params_t* hwp = NULL;
  snd_pcm_hw_params_alloca(&hwp);
  if (snd_pcm_hw_params_any(pcm, hwp) < 0) {
    if (err) {
      device_error_init(err, DEVICE_ERROR_OTHER,
                        "Failed to query ALSA hardware parameters");
    }
    snd_pcm_close(pcm);
    pthread_mutex_unlock(&g_alsa_mutex);
    return NULL;
  }

  // 2. let channel_values = supported_channel_values(&hwp,
  // CAPABILITY_PROBE_CHANNEL_LIMIT) (src/alsa_backend/utils.rs:194-199)
  unsigned int channel_values[CAPABILITY_PROBE_CHANNEL_LIMIT];
  int n_channels =
      supported_channel_values(pcm, hwp, CAPABILITY_PROBE_CHANNEL_LIMIT,
                               channel_values, CAPABILITY_PROBE_CHANNEL_LIMIT);
  if (n_channels <= 0) {
    if (err) {
      device_error_init(err, DEVICE_ERROR_OTHER,
                        "Failed to query ALSA channel limits");
    }
    snd_pcm_close(pcm);
    pthread_mutex_unlock(&g_alsa_mutex);
    return NULL;
  }

  audio_device_descriptor_t* desc =
      (audio_device_descriptor_t*)calloc(1, sizeof(audio_device_descriptor_t));
  if (!desc) {
    if (err) {
      device_error_init(err, DEVICE_ERROR_OTHER, "Out of memory");
    }
    snd_pcm_close(pcm);
    pthread_mutex_unlock(&g_alsa_mutex);
    return NULL;
  }
  snprintf(desc->name, sizeof(desc->name), "%s",
           (device_name && device_name[0]) ? device_name : clean_dev);

  desc->capability_sets_count = 1;
  desc->capability_sets =
      (device_capability_set_t*)calloc(1, sizeof(device_capability_set_t));
  if (!desc->capability_sets) goto error_cleanup;

  device_capability_set_t* set = &desc->capability_sets[0];
  snprintf(set->mode, sizeof(set->mode), "Unified");

  set->capabilities = (channel_capability_t*)calloc(
      (size_t)n_channels, sizeof(channel_capability_t));
  if (!set->capabilities) goto error_cleanup;

  size_t cap_idx = 0;

  // 3. for channels in channel_values (src/alsa_backend/utils.rs:202)
  for (int c = 0; c < n_channels; c++) {
    unsigned int channels = channel_values[c];

    // if let Ok(hwp_ch) = HwParams::any(&pcm) &&
    // hwp_ch.set_channels(channels).is_ok()
    //    && let Ok(rates_values) = list_samplerates(&hwp_ch)
    // (src/alsa_backend/utils.rs:204-206)
    if (snd_pcm_hw_params_any(pcm, hwp) < 0 ||
        snd_pcm_hw_params_set_channels(pcm, hwp, channels) < 0) {
      continue;
    }

    supported_rates_t rates_values;
    if (!list_samplerates(pcm, hwp, &rates_values)) {
      continue;
    }

    unsigned int rates[STANDARD_RATES_COUNT];
    size_t rates_count = 0;
    if (rates_values.type == SUPPORTED_RATES_DISCRETE) {
      for (size_t r = 0; r < rates_values.discrete_count; r++) {
        rates[rates_count++] = rates_values.discrete[r];
      }
    } else {
      for (size_t r = 0; r < STANDARD_RATES_COUNT; r++) {
        if (STANDARD_RATES[r] >= rates_values.min &&
            STANDARD_RATES[r] <= rates_values.max) {
          rates[rates_count++] = STANDARD_RATES[r];
        }
      }
    }

    channel_capability_t* cap = &set->capabilities[cap_idx];
    cap->channels = (int)channels;
    cap->samplerates = (samplerate_capability_t*)calloc(
        rates_count + 1, sizeof(samplerate_capability_t));
    if (!cap->samplerates) goto error_cleanup;

    size_t rate_idx = 0;

    // 4. for rate in rates (src/alsa_backend/utils.rs:217)
    for (size_t r = 0; r < rates_count; r++) {
      unsigned int rate = rates[r];

      // if let Ok(hwp_rate) = HwParams::any(&pcm)
      //    && hwp_rate.set_channels(channels).is_ok()
      //    && hwp_rate.set_rate(rate, alsa::ValueOr::Nearest).is_ok()
      //    && hwp_rate.get_rate().ok() == Some(rate)
      //    && let Ok(supported_formats) = list_formats(&hwp_rate)
      // (src/alsa_backend/utils.rs:219-223)
      if (snd_pcm_hw_params_any(pcm, hwp) < 0 ||
          snd_pcm_hw_params_set_channels(pcm, hwp, channels) < 0) {
        continue;
      }

      unsigned int applied_rate = rate;
      int dir = 0;
      if (snd_pcm_hw_params_set_rate_near(pcm, hwp, &applied_rate, &dir) < 0 ||
          applied_rate != rate) {
        continue;
      }

      const char* supported_formats[FORMAT_MAP_COUNT];
      size_t n_formats =
          list_formats(pcm, hwp, supported_formats, FORMAT_MAP_COUNT);
      if (n_formats == 0) {
        continue;
      }

      samplerate_capability_t* rate_cap = &cap->samplerates[rate_idx];
      rate_cap->samplerate = (int)rate;
      rate_cap->formats = (char**)calloc(n_formats, sizeof(char*));
      if (!rate_cap->formats) goto error_cleanup;

      for (size_t f = 0; f < n_formats; f++) {
        rate_cap->formats[f] = strdup(supported_formats[f]);
      }
      rate_cap->formats_count = n_formats;
      rate_idx++;
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
