#if defined(ENABLE_ALSA)
#include "alsa_playback.h"

#include <stdatomic.h>

#include "Audio/sample_conversion.h"
#include "Logging/app_logger.h"
#include "alsa_device.h"

static const logger_t g_logger = {"dsp.backend.alsa"};

struct alsa_playback {
  char device_name[256];
  int sample_rate;
  int channels;
  size_t chunk_size;

  bool has_format;
  alsa_sample_format_t requested_format;
  processing_parameters_t* params;

  snd_pcm_t* pcm;
  snd_pcm_format_t format;
  _Atomic bool paused;
  bool currently_paused;

  void* interleaved_buf;
  size_t interleaved_buf_size;

  snd_mixer_t* mixer;
  snd_mixer_elem_t* pitch_elem;
  pthread_mutex_t mixer_mutex;
  bool stopped;
};
/**
 * @brief Open the ALSA playback device.
 *
 * @param ctx Pointer to the ALSA playback instance.
 * @param err Pointer to store error details if opening fails.
 * @return true if the device was successfully opened, false otherwise.
 */
static bool alsa_playback_open(void* ctx, backend_error_t* err) {
  alsa_playback_t* playback = (alsa_playback_t*)ctx;
  if (!playback) return false;
  pthread_mutex_lock(&g_alsa_mutex);
  if (playback->pcm != NULL) {
    pthread_mutex_unlock(&g_alsa_mutex);
    return true;
  }
  int rc;
  rc = snd_pcm_open(&playback->pcm, playback->device_name,
                    SND_PCM_STREAM_PLAYBACK, 0);
  if (rc < 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         snd_strerror(rc));
    pthread_mutex_unlock(&g_alsa_mutex);
    return false;
  }

  snd_pcm_hw_params_t* params;
  snd_pcm_hw_params_alloca(&params);
  rc = snd_pcm_hw_params_any(playback->pcm, params);
  if (rc < 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         snd_strerror(rc));
    goto error_cleanup;
  }

  rc = snd_pcm_hw_params_set_access(playback->pcm, params,
                                    SND_PCM_ACCESS_RW_INTERLEAVED);
  if (rc < 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         snd_strerror(rc));
    goto error_cleanup;
  }

  // Select format: If a specific format was requested, attempt to use only
  // that. Otherwise, probe format support in order of preference: float32 ->
  // int32 -> int24 (3 bytes) -> int16.
  snd_pcm_format_t formats[5];
  size_t num_formats = 0;
  if (playback->has_format) {
    if (playback->requested_format == ALSA_SAMPLE_FORMAT_S16_LE) {
      formats[0] = SND_PCM_FORMAT_S16_LE;
      num_formats = 1;
    } else if (playback->requested_format == ALSA_SAMPLE_FORMAT_S24_3_LE) {
      formats[0] = SND_PCM_FORMAT_S24_3LE;
      num_formats = 1;
    } else if (playback->requested_format == ALSA_SAMPLE_FORMAT_S24_4_LE) {
      formats[0] = SND_PCM_FORMAT_S24_LE;
      num_formats = 1;
    } else if (playback->requested_format == ALSA_SAMPLE_FORMAT_S32_LE) {
      formats[0] = SND_PCM_FORMAT_S32_LE;
      num_formats = 1;
    } else if (playback->requested_format == ALSA_SAMPLE_FORMAT_F32_LE) {
      formats[0] = SND_PCM_FORMAT_FLOAT_LE;
      num_formats = 1;
    } else if (playback->requested_format == ALSA_SAMPLE_FORMAT_F64_LE) {
      formats[0] = SND_PCM_FORMAT_FLOAT64_LE;
      num_formats = 1;
    } else if (playback->requested_format == ALSA_SAMPLE_FORMAT_DSD_U8) {
      formats[0] = SND_PCM_FORMAT_DSD_U8;
      num_formats = 1;
    } else if (playback->requested_format == ALSA_SAMPLE_FORMAT_DSD_U16_LE) {
      formats[0] = SND_PCM_FORMAT_DSD_U16_LE;
      num_formats = 1;
    } else if (playback->requested_format == ALSA_SAMPLE_FORMAT_DSD_U16_BE) {
      formats[0] = SND_PCM_FORMAT_DSD_U16_BE;
      num_formats = 1;
    } else if (playback->requested_format == ALSA_SAMPLE_FORMAT_DSD_U32_LE) {
      formats[0] = SND_PCM_FORMAT_DSD_U32_LE;
      num_formats = 1;
    } else if (playback->requested_format == ALSA_SAMPLE_FORMAT_DSD_U32_BE) {
      formats[0] = SND_PCM_FORMAT_DSD_U32_BE;
      num_formats = 1;
    }
  } else {
    formats[0] = SND_PCM_FORMAT_FLOAT_LE;
    formats[1] = SND_PCM_FORMAT_S32_LE;
    formats[2] = SND_PCM_FORMAT_S24_3LE;
    formats[3] = SND_PCM_FORMAT_S16_LE;
    num_formats = 4;
  }
  bool format_ok = false;
  for (size_t i = 0; i < num_formats; i++) {
    rc = snd_pcm_hw_params_set_format(playback->pcm, params, formats[i]);
    if (rc >= 0) {
      playback->format = formats[i];
      format_ok = true;
      break;
    }
  }
  if (!format_ok) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Requested or supported ALSA format not available");
    goto error_cleanup;
  }

  rc =
      snd_pcm_hw_params_set_channels(playback->pcm, params, playback->channels);
  if (rc < 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         snd_strerror(rc));
    goto error_cleanup;
  }

  unsigned int val = playback->sample_rate;
  int dir = 0;
  rc = snd_pcm_hw_params_set_rate_near(playback->pcm, params, &val, &dir);
  if (rc < 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         snd_strerror(rc));
    goto error_cleanup;
  }

  snd_pcm_uframes_t period_size = playback->chunk_size;
  rc = snd_pcm_hw_params_set_period_size_near(playback->pcm, params,
                                              &period_size, &dir);
  if (rc < 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         snd_strerror(rc));
    goto error_cleanup;
  }

  snd_pcm_uframes_t buffer_size = period_size * 4;
  rc = snd_pcm_hw_params_set_buffer_size_near(playback->pcm, params,
                                              &buffer_size);
  if (rc < 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         snd_strerror(rc));
    goto error_cleanup;
  }

  rc = snd_pcm_hw_params(playback->pcm, params);
  if (rc < 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         snd_strerror(rc));
    goto error_cleanup;
  }

  snd_pcm_sw_params_t* sw_params;
  snd_pcm_sw_params_alloca(&sw_params);
  rc = snd_pcm_sw_params_current(playback->pcm, sw_params);
  if (rc >= 0) {
    snd_pcm_sw_params_set_start_threshold(playback->pcm, sw_params, 1);
    snd_pcm_sw_params_set_avail_min(playback->pcm, sw_params,
                                    playback->chunk_size);
    rc = snd_pcm_sw_params(playback->pcm, sw_params);
    if (rc < 0) {
      logger_warn(&g_logger, "Failed to set ALSA software parameters: %s",
                  snd_strerror(rc));
    }
  }

  size_t sample_size = 4;
  if (playback->format == SND_PCM_FORMAT_S16_LE) {
    sample_size = 2;
  } else if (playback->format == SND_PCM_FORMAT_S24_3LE) {
    sample_size = 3;
  } else if (playback->format == SND_PCM_FORMAT_S24_LE) {
    sample_size = 4;
  } else if (playback->format == SND_PCM_FORMAT_FLOAT64_LE) {
    sample_size = 8;
  } else if (playback->format == SND_PCM_FORMAT_DSD_U8) {
    sample_size = 1;
  } else if (playback->format == SND_PCM_FORMAT_DSD_U16_LE) {
    sample_size = 2;
  } else if (playback->format == SND_PCM_FORMAT_DSD_U16_BE) {
    sample_size = 2;
  } else if (playback->format == SND_PCM_FORMAT_DSD_U32_LE) {
    sample_size = 4;
  } else if (playback->format == SND_PCM_FORMAT_DSD_U32_BE) {
    sample_size = 4;
  }
  playback->interleaved_buf_size =
      playback->chunk_size * playback->channels * sample_size;
  playback->interleaved_buf = calloc(playback->interleaved_buf_size, 1);
  if (!playback->interleaved_buf) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to allocate ALSA playback interleaved buffer");
    goto error_cleanup;
  }

  playback->paused = false;

  // Initialize mixer for pitch control
  // Initialize mixer for pitch control.
  // Queries the ALSA hardware card related to the PCM device and attempts to
  // find a simple mixer control element named "Playback Pitch 1000000" which is
  // used to scale the playback speed.
  snd_pcm_info_t* pcm_info;
  snd_pcm_info_alloca(&pcm_info);
  if (snd_pcm_info(playback->pcm, pcm_info) >= 0) {
    char ctl_name[32];
    int card = snd_pcm_info_get_card(pcm_info);
    if (card >= 0) {
      snprintf(ctl_name, sizeof(ctl_name), "hw:%d", card);
      snd_mixer_t* mixer = NULL;
      if (snd_mixer_open(&mixer, 0) >= 0) {
        if (snd_mixer_attach(mixer, ctl_name) >= 0 &&
            snd_mixer_selem_register(mixer, NULL, NULL) >= 0 &&
            snd_mixer_load(mixer) >= 0) {
          pthread_mutex_lock(&playback->mixer_mutex);
          playback->mixer = mixer;

          snd_mixer_selem_id_t* sid;
          snd_mixer_selem_id_alloca(&sid);
          snd_mixer_selem_id_set_name(sid, "Playback Pitch 1000000");
          playback->pitch_elem = snd_mixer_find_selem(mixer, sid);
          pthread_mutex_unlock(&playback->mixer_mutex);
        } else {
          snd_mixer_close(mixer);
        }
      }
    }
  }

  pthread_mutex_unlock(&g_alsa_mutex);
  return true;

error_cleanup:
  if (playback->pcm) {
    snd_pcm_close(playback->pcm);
    playback->pcm = NULL;
  }
  if (playback->interleaved_buf) {
    free(playback->interleaved_buf);
    playback->interleaved_buf = NULL;
  }
  pthread_mutex_unlock(&g_alsa_mutex);
  return false;
}

/**
 * @brief Write a chunk of audio to the ALSA device.
 *
 * @param ctx Pointer to the ALSA playback instance.
 * @param chunk Pointer to the audio chunk to write.
 * @param[out] err Pointer to store error details if the write fails.
 * @return true on success, false on failure (e.g. xrun or write error).
 */
static bool alsa_playback_write(void* ctx, const audio_chunk_t* chunk,
                                backend_error_t* err) {
  alsa_playback_t* playback = (alsa_playback_t*)ctx;
  if (!playback || !playback->pcm) return false;

  if (audio_chunk_get_channels(chunk) < (size_t)playback->channels) {
    if (err) {
      backend_error_init(
          err, BACKEND_ERROR_WRITE_ERROR,
          "Chunk channels count is smaller than playback device channels");
    }
    return false;
  }

  size_t frames = audio_chunk_get_valid_frames(chunk);
  if (frames == 0) return true;
  size_t frames_to_write =
      (frames > playback->chunk_size) ? playback->chunk_size : frames;

  // Convert the input audio chunk (planar double format) to the target
  // interleaved format buffer. Clip samples to valid range when converting to
  // fixed-point formats.
  if (playback->format == SND_PCM_FORMAT_FLOAT_LE) {
    // Convert double to float.
    float* dst = (float*)playback->interleaved_buf;
    for (size_t f = 0; f < frames_to_write; f++) {
      for (size_t c = 0; c < (size_t)playback->channels; c++) {
        double val = audio_chunk_get_channel(chunk, c)[f];
        dst[f * playback->channels + c] = pcm_sample_encode_f32(val);
      }
    }
  } else if (playback->format == SND_PCM_FORMAT_S32_LE) {
    // Convert double to 32-bit signed integer.
    int32_t* dst = (int32_t*)playback->interleaved_buf;
    for (size_t f = 0; f < frames_to_write; f++) {
      for (size_t c = 0; c < (size_t)playback->channels; c++) {
        double val = audio_chunk_get_channel(chunk, c)[f];
        dst[f * playback->channels + c] = pcm_sample_encode_s32(val);
      }
    }
  } else if (playback->format == SND_PCM_FORMAT_S24_3LE) {
    // Convert double to 24-bit signed integer, packed in 3 bytes.
    uint8_t* dst = (uint8_t*)playback->interleaved_buf;
    for (size_t f = 0; f < frames_to_write; f++) {
      for (size_t c = 0; c < (size_t)playback->channels; c++) {
        double val = audio_chunk_get_channel(chunk, c)[f];
        size_t offset = (f * playback->channels + c) * 3;
        pcm_sample_encode_s24_3bytes(val, &dst[offset]);
      }
    }
  } else if (playback->format == SND_PCM_FORMAT_S24_LE) {
    // Convert double to 24-bit signed integer inside 32-bit containers.
    int32_t* dst = (int32_t*)playback->interleaved_buf;
    for (size_t f = 0; f < frames_to_write; f++) {
      for (size_t c = 0; c < (size_t)playback->channels; c++) {
        double val = audio_chunk_get_channel(chunk, c)[f];
        dst[f * playback->channels + c] = pcm_sample_encode_s24(val);
      }
    }
  } else if (playback->format == SND_PCM_FORMAT_FLOAT64_LE) {
    // Direct copy for double format.
    double* dst = (double*)playback->interleaved_buf;
    for (size_t f = 0; f < frames_to_write; f++) {
      for (size_t c = 0; c < (size_t)playback->channels; c++) {
        dst[f * playback->channels + c] = audio_chunk_get_channel(chunk, c)[f];
      }
    }
  } else if (playback->format == SND_PCM_FORMAT_S16_LE) {
    // Convert double to 16-bit signed integer.
    int16_t* dst = (int16_t*)playback->interleaved_buf;
    for (size_t f = 0; f < frames_to_write; f++) {
      for (size_t c = 0; c < (size_t)playback->channels; c++) {
        double val = audio_chunk_get_channel(chunk, c)[f];
        dst[f * playback->channels + c] = pcm_sample_encode_s16(val);
      }
    }
  } else if (playback->format == SND_PCM_FORMAT_DSD_U8) {
    uint8_t* dst = (uint8_t*)playback->interleaved_buf;
    for (size_t f = 0; f < frames_to_write; f++) {
      for (size_t c = 0; c < (size_t)playback->channels; c++) {
        double val = audio_chunk_get_channel(chunk, c)[f];
        dst[f * playback->channels + c] = pcm_sample_encode_dsd_u8(val);
      }
    }
  } else if (playback->format == SND_PCM_FORMAT_DSD_U16_LE) {
    uint16_t* dst = (uint16_t*)playback->interleaved_buf;
    for (size_t f = 0; f < frames_to_write; f++) {
      for (size_t c = 0; c < (size_t)playback->channels; c++) {
        double val = audio_chunk_get_channel(chunk, c)[f];
        uint16_t encoded = (uint16_t)pcm_sample_encode_s16(val);
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && \
    __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        dst[f * playback->channels + c] = __builtin_bswap16(encoded);
#else
        dst[f * playback->channels + c] = encoded;
#endif
      }
    }
  } else if (playback->format == SND_PCM_FORMAT_DSD_U16_BE) {
    uint16_t* dst = (uint16_t*)playback->interleaved_buf;
    for (size_t f = 0; f < frames_to_write; f++) {
      for (size_t c = 0; c < (size_t)playback->channels; c++) {
        double val = audio_chunk_get_channel(chunk, c)[f];
        uint16_t encoded = (uint16_t)pcm_sample_encode_s16(val);
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && \
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        dst[f * playback->channels + c] = __builtin_bswap16(encoded);
#else
        dst[f * playback->channels + c] = encoded;
#endif
      }
    }
  } else if (playback->format == SND_PCM_FORMAT_DSD_U32_LE) {
    uint32_t* dst = (uint32_t*)playback->interleaved_buf;
    for (size_t f = 0; f < frames_to_write; f++) {
      for (size_t c = 0; c < (size_t)playback->channels; c++) {
        double val = audio_chunk_get_channel(chunk, c)[f];
        uint32_t encoded = pcm_sample_u32_from_f32((float)val);
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && \
    __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        dst[f * playback->channels + c] = __builtin_bswap32(encoded);
#else
        dst[f * playback->channels + c] = encoded;
#endif
      }
    }
  } else if (playback->format == SND_PCM_FORMAT_DSD_U32_BE) {
    uint32_t* dst = (uint32_t*)playback->interleaved_buf;
    for (size_t f = 0; f < frames_to_write; f++) {
      for (size_t c = 0; c < (size_t)playback->channels; c++) {
        double val = audio_chunk_get_channel(chunk, c)[f];
        uint32_t encoded = pcm_sample_u32_from_f32((float)val);
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && \
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        dst[f * playback->channels + c] = __builtin_bswap32(encoded);
#else
        dst[f * playback->channels + c] = encoded;
#endif
      }
    }
  }

  bool paused = atomic_load_explicit(&playback->paused, memory_order_acquire);
  if (paused) {
    if (!playback->currently_paused) {
      snd_pcm_pause(playback->pcm, 1);
      playback->currently_paused = true;
    }
    return true;
  } else {
    if (playback->currently_paused) {
      snd_pcm_pause(playback->pcm, 0);
      playback->currently_paused = false;
    }
  }

  // Write interleaved samples to ALSA device.
  // In case of write failure (e.g., underrun), attempt to recover and retry the
  // write once.
  snd_pcm_sframes_t rc =
      snd_pcm_writei(playback->pcm, playback->interleaved_buf, frames_to_write);
  if (rc < 0) {
    rc = snd_pcm_recover(playback->pcm, rc, 0);
    if (rc >= 0) {
      rc = snd_pcm_writei(playback->pcm, playback->interleaved_buf,
                          frames_to_write);
    }
    if (rc < 0) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_WRITE_ERROR, snd_strerror(rc));
      return false;
    }
  }

  return true;
}

/**
 * @brief Close the ALSA playback device.
 *
 * @param ctx Pointer to the ALSA playback instance.
 */
static void alsa_playback_close(void* ctx) {
  alsa_playback_t* playback = (alsa_playback_t*)ctx;
  if (!playback) return;
  pthread_mutex_lock(&g_alsa_mutex);
  if (playback->pcm) {
    if (!playback->stopped) {
      snd_pcm_drain(playback->pcm);
    }
    snd_pcm_close(playback->pcm);
    playback->pcm = NULL;
  }
  pthread_mutex_unlock(&g_alsa_mutex);
  if (playback->interleaved_buf) {
    free(playback->interleaved_buf);
    playback->interleaved_buf = NULL;
  }
  pthread_mutex_lock(&playback->mixer_mutex);
  if (playback->mixer) {
    snd_mixer_close(playback->mixer);
    playback->mixer = NULL;
    playback->pitch_elem = NULL;
  }
  pthread_mutex_unlock(&playback->mixer_mutex);
}

/**
 * @brief Get the current buffer level of the ALSA device.
 *
 * @param ctx Pointer to the ALSA playback instance.
 * @return The buffer level in frames.
 */
static size_t alsa_playback_get_buffer_level(void* ctx) {
  alsa_playback_t* playback = (alsa_playback_t*)ctx;
  if (!playback || !playback->pcm) return 0;
  snd_pcm_sframes_t delay = 0;
  int err = snd_pcm_delay(playback->pcm, &delay);
  if (err < 0) {
    if (err == -EPIPE) {
      snd_pcm_prepare(playback->pcm);
    }
    return 0;
  }
  return delay < 0 ? 0 : (size_t)delay;
}

/**
 * @brief Check if there is a pending sample rate change on the ALSA device.
 *
 * @param ctx Pointer to the ALSA playback instance.
 * @param[out] out_rate Pointer to store the new sample rate if a change is
 * pending.
 * @return true if a rate change was detected, false otherwise.
 */
static bool alsa_playback_get_pending_rate_change(void* ctx, double* out_rate) {
  (void)ctx;
  (void)out_rate;
  return false;
}

static inline bool alsa_is_dsd_format(snd_pcm_format_t format) {
  if (format == SND_PCM_FORMAT_DSD_U8) return true;
  if (format == SND_PCM_FORMAT_DSD_U16_LE) return true;
  if (format == SND_PCM_FORMAT_DSD_U16_BE) return true;
  if (format == SND_PCM_FORMAT_DSD_U32_LE) return true;
  if (format == SND_PCM_FORMAT_DSD_U32_BE) return true;
  (void)format;
  return false;
}

/**
 * @brief Prefill the ALSA playback buffer with silence.
 *
 * @param ctx Pointer to the ALSA playback instance.
 * @param frames Number of silence frames to write.
 * @param[out] err Pointer to store error details if prefilling fails.
 * @return true on success, false on failure.
 */
static bool alsa_playback_prefill_silence(void* ctx, size_t frames,
                                          backend_error_t* err) {
  alsa_playback_t* playback = (alsa_playback_t*)ctx;
  if (!playback || !playback->pcm) return false;

  int bits = snd_pcm_format_physical_width(playback->format);
  size_t sample_size = (bits > 0) ? ((size_t)bits / 8) : 4;

  size_t zero_buf_size = frames * playback->channels * sample_size;
  void* zero_buf = malloc(zero_buf_size);
  if (!zero_buf) return false;

  if (alsa_is_dsd_format(playback->format)) {
    memset(zero_buf, 0x69, zero_buf_size);
  } else {
    memset(zero_buf, 0, zero_buf_size);
  }

  snd_pcm_sframes_t rc = snd_pcm_writei(playback->pcm, zero_buf, frames);
  free(zero_buf);

  if (rc < 0) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_WRITE_ERROR, snd_strerror(rc));
    return false;
  }
  return true;
}

/**
 * @brief Get the paused status of the ALSA playback.
 *
 * @param ctx Pointer to the ALSA playback instance.
 * @return true if paused, false otherwise.
 */
static bool alsa_playback_get_is_paused(void* ctx) {
  alsa_playback_t* playback = (alsa_playback_t*)ctx;
  if (!playback) return false;
  return atomic_load_explicit(&playback->paused, memory_order_acquire);
}

/**
 * @brief Set the paused status of the ALSA playback.
 *
 * @param ctx Pointer to the ALSA playback instance.
 * @param paused true to pause, false to resume.
 */
static void alsa_playback_set_is_paused(void* ctx, bool paused) {
  alsa_playback_t* playback = (alsa_playback_t*)ctx;
  if (!playback) return;
  atomic_store_explicit(&playback->paused, paused, memory_order_release);
}

/**
 * @brief Check if the ALSA playback backend supports pitch control.
 *
 * @param ctx Pointer to the ALSA playback instance.
 * @return true if supported, false otherwise.
 */
static bool alsa_playback_pitch_control_supported(void* ctx) {
  alsa_playback_t* playback = (alsa_playback_t*)ctx;
  if (!playback) return false;
  pthread_mutex_lock(&playback->mixer_mutex);
  bool res = playback->pitch_elem != NULL;
  pthread_mutex_unlock(&playback->mixer_mutex);
  return res;
}

/**
 * @brief Set the pitch of the ALSA playback device.
 *
 * @param ctx Pointer to the ALSA playback instance.
 * @param multiplier The clock rate multiplier.
 */
static void alsa_playback_set_pitch(void* ctx, double multiplier) {
  alsa_playback_t* playback = (alsa_playback_t*)ctx;
  if (!playback) return;
  pthread_mutex_lock(&playback->mixer_mutex);
  if (!playback->pitch_elem) {
    pthread_mutex_unlock(&playback->mixer_mutex);
    return;
  }
  long value = (long)round(1000000.0 / multiplier);
  if (snd_mixer_selem_has_playback_volume(playback->pitch_elem)) {
    snd_mixer_selem_set_playback_volume_all(playback->pitch_elem, value);
  } else if (snd_mixer_selem_has_capture_volume(playback->pitch_elem)) {
    snd_mixer_selem_set_capture_volume_all(playback->pitch_elem, value);
  }
  pthread_mutex_unlock(&playback->mixer_mutex);
}

/**
 * @brief Stop the ALSA playback device.
 *
 * @param ctx Pointer to the ALSA playback instance.
 */
static void alsa_playback_stop(void* ctx) {
  alsa_playback_t* playback = (alsa_playback_t*)ctx;
  if (!playback) return;
  pthread_mutex_lock(&g_alsa_mutex);
  playback->stopped = true;
  if (playback->pcm) {
    snd_pcm_drop(playback->pcm);
  }
  pthread_mutex_unlock(&g_alsa_mutex);
}

/**
 * @brief Destroy the ALSA playback backend.
 *
 * @param ctx Pointer to the ALSA playback instance.
 */
static void alsa_playback_destroy(void* ctx) {
  alsa_playback_t* playback = (alsa_playback_t*)ctx;
  if (!playback) return;
  alsa_playback_close(playback);
  pthread_mutex_destroy(&playback->mixer_mutex);
  free(playback);
}

/**
 * @brief Create an ALSA playback backend instance.
 *
 * @param config Configuration for the playback device.
 * @param sample_rate The nominal sample rate in Hz.
 * @param chunk_size The size of each audio chunk in frames.
 * @param full_duplex True if running in full duplex mode.
 * @param params Opaque processing parameters pointer.
 * @param[out] err Pointer to store error details if creation fails.
 * @return A pointer to the created playback_backend_t interface wrapper, or
 * NULL on error.
 */
static playback_backend_t* alsa_playback_create(
    const playback_device_config_t* config, int sample_rate, int chunk_size,
    bool full_duplex, processing_parameters_t* params, backend_error_t* err) {
  (void)full_duplex;
  (void)err;
  alsa_playback_t* playback =
      (alsa_playback_t*)calloc(1, sizeof(alsa_playback_t));
  if (!playback) return NULL;

  snprintf(playback->device_name, sizeof(playback->device_name), "%s",
           config->cfg.alsa.device[0] ? config->cfg.alsa.device : "default");

  playback->sample_rate = sample_rate;
  playback->channels = config->cfg.alsa.channels;
  playback->chunk_size = chunk_size;

  playback->has_format = config->cfg.alsa.has_format;
  playback->requested_format = config->cfg.alsa.format;
  playback->params = params;
  atomic_init(&playback->paused, false);
  playback->currently_paused = false;
  pthread_mutex_init(&playback->mixer_mutex, NULL);

  playback_backend_t* backend =
      (playback_backend_t*)calloc(1, sizeof(playback_backend_t));
  if (!backend) {
    free(playback);
    return NULL;
  }
  backend->ctx = playback;
  backend->vtable = &g_alsa_playback_vtable;
  return backend;
}

const playback_backend_vtable_t g_alsa_playback_vtable = {
    .create = alsa_playback_create,
    .open = alsa_playback_open,
    .write = alsa_playback_write,
    .close = alsa_playback_close,
    .get_buffer_level = alsa_playback_get_buffer_level,
    .get_pending_rate_change = alsa_playback_get_pending_rate_change,
    .prefill_silence = alsa_playback_prefill_silence,
    .get_is_paused = alsa_playback_get_is_paused,
    .set_is_paused = alsa_playback_set_is_paused,
    .pitch_control_supported = alsa_playback_pitch_control_supported,
    .set_pitch = alsa_playback_set_pitch,
    .stop = alsa_playback_stop,
    .destroy = alsa_playback_destroy};

#endif  // defined(ENABLE_ALSA)
