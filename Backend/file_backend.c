
#include "file_backend.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if !defined(_WIN32)
#include <poll.h>
#include <sys/stat.h>
#endif

#include <stdatomic.h>

#include "Audio/sample_conversion.h"
#include "Logging/app_logger.h"
#include "Utils/cdsp_path.h"
#include "Utils/cdsp_time.h"

#ifdef _WIN32
#define fseek_64 _fseeki64
#define ftell_64 _ftelli64
#else
#define fseek_64 fseeko
#define ftell_64 ftello
#endif

static const logger_t g_logger = {"dsp.backend.file"};

/**
 * @brief Helper to get monotonic time in nanoseconds.
 *
 * @return Monotonic time in nanoseconds.
 */
static uint64_t get_time_ns(void) { return cdsp_time_now_ns(); }

struct file_capture {
  char filename[512];
  bool is_stdin;
  FILE* f;
  int sample_rate;
  int channels;
  int chunk_size;
  binary_sample_format_t format;
  bool is_wav;
  uint64_t skip_bytes;
  uint64_t read_bytes;
  uint64_t total_bytes_read;
  size_t extra_samples;
  size_t extra_samples_generated;
  uint8_t* raw_buf;
  size_t raw_buf_capacity;
  uint64_t last_read_time_ns;
  _Atomic bool is_paused;
#ifdef CDSP_TEST
  bool realtime;
  uint64_t start_time_ns;
  size_t total_frames_read;
#endif
};

struct file_playback {
  char filename[512];
  bool is_stdout;
  FILE* f;
  int sample_rate;
  int channels;
  int chunk_size;
  binary_sample_format_t format;
  bool is_wav;
  bool use_rf64;
  bool is_seekable;
  uint64_t total_bytes_written;
  uint8_t* raw_buf;
  size_t raw_buf_capacity;
#ifdef CDSP_TEST
  bool realtime;
  uint64_t start_time_ns;
  size_t total_frames_written;
  _Atomic bool stopped;
#endif
};

// WAV parsing header
typedef struct {
  uint32_t sample_rate;
  uint16_t channels;
  binary_sample_format_t format;
  uint64_t data_bytes;
  uint64_t data_start_offset;
} wav_info_t;

/**
 * @brief Get the size in bytes of a sample for a given format.
 *
 * @param format The binary sample format.
 * @return Size in bytes, or 0 if format is invalid.
 */
static size_t get_sample_size(binary_sample_format_t format) {
  switch (format) {
    case BINARY_SAMPLE_FORMAT_S16_LE:
      return 2;
    case BINARY_SAMPLE_FORMAT_S24_3_LE:
      return 3;
    case BINARY_SAMPLE_FORMAT_S24_4_RJ_LE:
      return 4;
    case BINARY_SAMPLE_FORMAT_S24_4_LJ_LE:
      return 4;
    case BINARY_SAMPLE_FORMAT_S32_LE:
      return 4;
    case BINARY_SAMPLE_FORMAT_F32_LE:
      return 4;
    case BINARY_SAMPLE_FORMAT_F64_LE:
      return 8;
    default:
      return 0;
  }
}

/**
 * @brief Parse WAV header from a file.
 *
 * Extracts sample rate, channels, format, and data chunk size/offset.
 * Handles files where the 'data' chunk is not immediately after the 'fmt '
 * chunk.
 *
 * @param f File pointer.
 * @param info Output structure to store parsed WAV info.
 * @param err_msg Output buffer for error message if parsing fails.
 * @param err_msg_len Size of err_msg buffer.
 * @return true if parsing succeeded, false otherwise.
 */
static bool parse_wav_header(FILE* f, wav_info_t* info, char* err_msg,
                             size_t err_msg_len) {
  uint8_t header[12];
  if (fread(header, 1, 12, f) != 12) {
    snprintf(err_msg, err_msg_len, "Failed to read WAV header");
    return false;
  }

  bool is_rf64 = false;
  if (memcmp(header, "RF64", 4) == 0) {
    is_rf64 = true;
  } else if (memcmp(header, "RIFF", 4) != 0) {
    snprintf(err_msg, err_msg_len, "Not a RIFF or RF64 file");
    return false;
  }

  if (memcmp(header + 8, "WAVE", 4) != 0) {
    snprintf(err_msg, err_msg_len, "Not a WAVE file");
    return false;
  }

  bool found_fmt = false;
  bool found_data = false;
  uint32_t sample_rate = 0;
  uint16_t channels = 0;
  binary_sample_format_t format = BINARY_SAMPLE_FORMAT_INVALID;
  uint64_t rf64_data_size = 0;
  uint64_t data_bytes = 0;
  uint64_t data_start_offset = 0;

  uint8_t chunk_id[4];
  uint32_t chunk_size;

  while (fread(chunk_id, 1, 4, f) == 4) {
    if (fread(&chunk_size, 4, 1, f) != 1) {
      break;
    }

    if (memcmp(chunk_id, "ds64", 4) == 0) {
      if (chunk_size < 24) {
        snprintf(err_msg, err_msg_len, "Invalid ds64 chunk size %u",
                 chunk_size);
        return false;
      }
      uint8_t ds64_payload[24];
      if (fread(ds64_payload, 1, 24, f) != 24) {
        snprintf(err_msg, err_msg_len, "Failed to read ds64 chunk payload");
        return false;
      }
      rf64_data_size = ds64_payload[8] | ((uint64_t)ds64_payload[9] << 8) |
                       ((uint64_t)ds64_payload[10] << 16) |
                       ((uint64_t)ds64_payload[11] << 24) |
                       ((uint64_t)ds64_payload[12] << 32) |
                       ((uint64_t)ds64_payload[13] << 40) |
                       ((uint64_t)ds64_payload[14] << 48) |
                       ((uint64_t)ds64_payload[15] << 56);

      if (chunk_size > 24) {
        if (fseek_64(f, chunk_size - 24, SEEK_CUR) != 0) {
          snprintf(err_msg, err_msg_len, "Failed to seek past ds64 chunk");
          return false;
        }
      }
    } else if (memcmp(chunk_id, "fmt ", 4) == 0) {
      found_fmt = true;
      if (chunk_size < 16) {
        snprintf(err_msg, err_msg_len, "Invalid fmt chunk size %u", chunk_size);
        return false;
      }
      uint8_t fmt_payload[40];
      size_t to_read = chunk_size < 40 ? chunk_size : 40;
      if (fread(fmt_payload, 1, to_read, f) != to_read) {
        snprintf(err_msg, err_msg_len, "Failed to read fmt chunk payload");
        return false;
      }
      uint16_t audio_format = fmt_payload[0] | (fmt_payload[1] << 8);
      channels = fmt_payload[2] | (fmt_payload[3] << 8);
      sample_rate = fmt_payload[4] | (fmt_payload[5] << 8) |
                    (fmt_payload[6] << 16) | (fmt_payload[7] << 24);
      uint16_t bits_per_sample = fmt_payload[14] | (fmt_payload[15] << 8);

      if (audio_format != 1 && audio_format != 3 && audio_format != 0xFFFE) {
        snprintf(err_msg, err_msg_len,
                 "Unsupported WAV format code %d (only PCM/Float supported)",
                 audio_format);
        return false;
      }

      if (audio_format == 0xFFFE) {
        if (to_read < 24) {
          snprintf(err_msg, err_msg_len,
                   "fmt chunk too short for EXTENSIBLE format");
          return false;
        }
        uint16_t sub_format = fmt_payload[24] | (fmt_payload[25] << 8);
        if (sub_format == 1) {
          audio_format = 1;
        } else if (sub_format == 3) {
          audio_format = 3;
        } else {
          snprintf(err_msg, err_msg_len,
                   "Unsupported sub-format %d in EXTENSIBLE", sub_format);
          return false;
        }
      }

      if (audio_format == 1) {
        if (bits_per_sample == 16)
          format = BINARY_SAMPLE_FORMAT_S16_LE;
        else if (bits_per_sample == 24)
          format = BINARY_SAMPLE_FORMAT_S24_3_LE;
        else if (bits_per_sample == 32)
          format = BINARY_SAMPLE_FORMAT_S32_LE;
      } else if (audio_format == 3) {
        if (bits_per_sample == 32)
          format = BINARY_SAMPLE_FORMAT_F32_LE;
        else if (bits_per_sample == 64)
          format = BINARY_SAMPLE_FORMAT_F64_LE;
      }

      if (format == BINARY_SAMPLE_FORMAT_INVALID) {
        snprintf(err_msg, err_msg_len,
                 "Unsupported bits per sample %d for format %d",
                 bits_per_sample, audio_format);
        return false;
      }

      if (chunk_size > to_read) {
        if (fseek_64(f, chunk_size - to_read, SEEK_CUR) != 0) {
          snprintf(err_msg, err_msg_len, "Failed to seek past fmt chunk");
          return false;
        }
      }
    } else if (memcmp(chunk_id, "data", 4) == 0) {
      found_data = true;
      data_start_offset = (uint64_t)ftell_64(f);
      if (is_rf64 && chunk_size == 0xFFFFFFFF) {
        data_bytes = rf64_data_size;
      } else {
        data_bytes = chunk_size;
      }
      break;
    } else {
      uint32_t pad = chunk_size & 1;
      if (fseek_64(f, (int64_t)chunk_size + pad, SEEK_CUR) != 0) {
        snprintf(err_msg, err_msg_len, "Failed to seek past unknown chunk");
        return false;
      }
    }
  }

  if (!found_fmt) {
    snprintf(err_msg, err_msg_len, "Missing 'fmt ' chunk");
    return false;
  }
  if (!found_data) {
    snprintf(err_msg, err_msg_len, "Missing 'data' chunk");
    return false;
  }

  info->sample_rate = sample_rate;
  info->channels = channels;
  info->format = format;
  info->data_bytes = data_bytes;
  info->data_start_offset = data_start_offset;
  return true;
}

/**
 * @brief Write a standard 44-byte WAV header to the file.
 *
 * Used for file playback when WAV header is requested.
 *
 * @param f File pointer.
 * @param channels Number of channels.
 * @param format Sample format.
 * @param sample_rate Sample rate.
 * @param data_bytes Size of data payload in bytes.
 */
static void write_wav_header_to_file(FILE* f, size_t channels,
                                     binary_sample_format_t format,
                                     uint32_t sample_rate, uint32_t data_bytes,
                                     bool is_seekable) {
  uint8_t header[44];
  memset(header, 0, 44);
  memcpy(header, "RIFF", 4);
  uint32_t file_size = data_bytes + 36;
  header[4] = file_size & 0xFF;
  header[5] = (file_size >> 8) & 0xFF;
  header[6] = (file_size >> 16) & 0xFF;
  header[7] = (file_size >> 24) & 0xFF;
  memcpy(header + 8, "WAVE", 4);
  memcpy(header + 12, "fmt ", 4);
  header[16] = 16;
  uint16_t format_tag = (format == BINARY_SAMPLE_FORMAT_F32_LE ||
                         format == BINARY_SAMPLE_FORMAT_F64_LE)
                            ? 3
                            : 1;
  header[20] = format_tag & 0xFF;
  header[21] = (format_tag >> 8) & 0xFF;
  header[22] = channels & 0xFF;
  header[23] = (channels >> 8) & 0xFF;
  header[24] = sample_rate & 0xFF;
  header[25] = (sample_rate >> 8) & 0xFF;
  header[26] = (sample_rate >> 16) & 0xFF;
  header[27] = (sample_rate >> 24) & 0xFF;
  size_t sample_size = get_sample_size(format);
  uint32_t byte_rate = sample_rate * channels * sample_size;
  header[28] = byte_rate & 0xFF;
  header[29] = (byte_rate >> 8) & 0xFF;
  header[30] = (byte_rate >> 16) & 0xFF;
  header[31] = (byte_rate >> 24) & 0xFF;
  uint16_t block_align = channels * sample_size;
  header[32] = block_align & 0xFF;
  header[33] = (block_align >> 8) & 0xFF;
  uint16_t bits_per_sample = sample_size * 8;
  header[34] = bits_per_sample & 0xFF;
  header[35] = (bits_per_sample >> 8) & 0xFF;
  memcpy(header + 36, "data", 4);
  header[40] = data_bytes & 0xFF;
  header[41] = (data_bytes >> 8) & 0xFF;
  header[42] = (data_bytes >> 16) & 0xFF;
  header[43] = (data_bytes >> 24) & 0xFF;
  if (is_seekable) {
    fseek_64(f, 0, SEEK_SET);
  }
  fwrite(header, 1, 44, f);
}

/**
 * @brief Write an 80-byte RF64 WAV header to the file.
 */
static void write_rf64_header_to_file(FILE* f, size_t channels,
                                      binary_sample_format_t format,
                                      uint32_t sample_rate,
                                      uint64_t data_bytes) {
  uint8_t header[80];
  memset(header, 0, 80);
  memcpy(header, "RF64", 4);
  // RIFF size placeholder
  header[4] = 0xFF;
  header[5] = 0xFF;
  header[6] = 0xFF;
  header[7] = 0xFF;
  memcpy(header + 8, "WAVE", 4);

  // ds64 chunk
  memcpy(header + 12, "ds64", 4);
  uint32_t ds64_size = 28;
  header[16] = ds64_size & 0xFF;
  header[17] = (ds64_size >> 8) & 0xFF;
  header[18] = (ds64_size >> 16) & 0xFF;
  header[19] = (ds64_size >> 24) & 0xFF;

  // 64-bit RIFF size (36 + 8 + 28 + 8 + 8 + 16 + data_bytes)
  uint64_t riff_size = data_bytes + 72;
  for (int i = 0; i < 8; i++) {
    header[20 + i] = (riff_size >> (i * 8)) & 0xFF;
  }

  // 64-bit Data size
  for (int i = 0; i < 8; i++) {
    header[28 + i] = (data_bytes >> (i * 8)) & 0xFF;
  }

  // 64-bit Sample count
  size_t sample_size = get_sample_size(format);
  uint64_t sample_count = data_bytes / (channels * sample_size);
  for (int i = 0; i < 8; i++) {
    header[36 + i] = (sample_count >> (i * 8)) & 0xFF;
  }

  // table entry count is 0 (header[44..47])

  // fmt chunk
  memcpy(header + 48, "fmt ", 4);
  header[52] = 16;  // fmt chunk size
  uint16_t format_tag = (format == BINARY_SAMPLE_FORMAT_F32_LE ||
                         format == BINARY_SAMPLE_FORMAT_F64_LE)
                            ? 3
                            : 1;
  header[56] = format_tag & 0xFF;
  header[57] = (format_tag >> 8) & 0xFF;
  header[58] = channels & 0xFF;
  header[59] = (channels >> 8) & 0xFF;
  header[60] = sample_rate & 0xFF;
  header[61] = (sample_rate >> 8) & 0xFF;
  header[62] = (sample_rate >> 16) & 0xFF;
  header[63] = (sample_rate >> 24) & 0xFF;

  uint32_t byte_rate = sample_rate * channels * sample_size;
  header[64] = byte_rate & 0xFF;
  header[65] = (byte_rate >> 8) & 0xFF;
  header[66] = (byte_rate >> 16) & 0xFF;
  header[67] = (byte_rate >> 24) & 0xFF;

  uint16_t block_align = channels * sample_size;
  header[68] = block_align & 0xFF;
  header[69] = (block_align >> 8) & 0xFF;
  uint16_t bits_per_sample = sample_size * 8;
  header[70] = bits_per_sample & 0xFF;
  header[71] = (bits_per_sample >> 8) & 0xFF;

  // data chunk header
  memcpy(header + 72, "data", 4);
  header[76] = 0xFF;
  header[77] = 0xFF;
  header[78] = 0xFF;
  header[79] = 0xFF;

  fseek_64(f, 0, SEEK_SET);
  fwrite(header, 1, 80, f);
}

/**
 * @brief Decode multiple interleaved binary samples to deinterleaved double
 * channels in range [-1.0, 1.0].
 *
 * @param dst_channels Array of pointers to destination channel buffers.
 * @param src Pointer to the binary source data buffer.
 * @param frames Number of frames to decode.
 * @param channels Number of audio channels.
 * @param format The source sample format.
 */
static inline void decode_samples_deinterleave(double* const* dst_channels,
                                               const uint8_t* src,
                                               size_t frames, int channels,
                                               binary_sample_format_t format) {
  switch (format) {
    case BINARY_SAMPLE_FORMAT_S16_LE: {
      for (size_t f = 0; f < frames; f++) {
        for (int c = 0; c < channels; c++) {
          size_t offset = (f * channels + c) * 2;
          dst_channels[c][f] = pcm_sample_decode_s16_bytes(src + offset);
        }
      }
      break;
    }
    case BINARY_SAMPLE_FORMAT_S24_3_LE: {
      for (size_t f = 0; f < frames; f++) {
        for (int c = 0; c < channels; c++) {
          size_t offset = (f * channels + c) * 3;
          dst_channels[c][f] = pcm_sample_decode_s24_3bytes(src + offset);
        }
      }
      break;
    }
    case BINARY_SAMPLE_FORMAT_S24_4_RJ_LE: {
      for (size_t f = 0; f < frames; f++) {
        for (int c = 0; c < channels; c++) {
          size_t offset = (f * channels + c) * 4;
          dst_channels[c][f] = pcm_sample_decode_s24_4_rj_bytes(src + offset);
        }
      }
      break;
    }
    case BINARY_SAMPLE_FORMAT_S24_4_LJ_LE: {
      for (size_t f = 0; f < frames; f++) {
        for (int c = 0; c < channels; c++) {
          size_t offset = (f * channels + c) * 4;
          dst_channels[c][f] = pcm_sample_decode_s24_4_lj_bytes(src + offset);
        }
      }
      break;
    }
    case BINARY_SAMPLE_FORMAT_S32_LE: {
      for (size_t f = 0; f < frames; f++) {
        for (int c = 0; c < channels; c++) {
          size_t offset = (f * channels + c) * 4;
          dst_channels[c][f] = pcm_sample_decode_s32_bytes(src + offset);
        }
      }
      break;
    }
    case BINARY_SAMPLE_FORMAT_F32_LE: {
      for (size_t f = 0; f < frames; f++) {
        for (int c = 0; c < channels; c++) {
          size_t offset = (f * channels + c) * 4;
          dst_channels[c][f] = pcm_sample_decode_f32_bytes(src + offset);
        }
      }
      break;
    }
    case BINARY_SAMPLE_FORMAT_F64_LE: {
      for (size_t f = 0; f < frames; f++) {
        for (int c = 0; c < channels; c++) {
          size_t offset = (f * channels + c) * 8;
          dst_channels[c][f] = pcm_sample_decode_f64_bytes(src + offset);
        }
      }
      break;
    }
    default:
      break;
  }
}

/**
 * @brief Encode multiple deinterleaved double channels in range [-1.0, 1.0] to
 * interleaved binary format.
 *
 * @param dst Pointer to destination interleaved binary buffer.
 * @param src_channels Array of pointers to source double channel buffers.
 * @param frames Number of frames to encode.
 * @param channels Number of audio channels.
 * @param format The target sample format.
 */
static inline void encode_samples_interleave(uint8_t* dst,
                                             const double* const* src_channels,
                                             size_t frames, int channels,
                                             binary_sample_format_t format) {
  switch (format) {
    case BINARY_SAMPLE_FORMAT_S16_LE: {
      for (size_t f = 0; f < frames; f++) {
        for (int c = 0; c < channels; c++) {
          size_t offset = (f * channels + c) * 2;
          pcm_sample_encode_s16_bytes(src_channels[c][f], dst + offset);
        }
      }
      break;
    }
    case BINARY_SAMPLE_FORMAT_S24_3_LE: {
      for (size_t f = 0; f < frames; f++) {
        for (int c = 0; c < channels; c++) {
          size_t offset = (f * channels + c) * 3;
          pcm_sample_encode_s24_3bytes(src_channels[c][f], dst + offset);
        }
      }
      break;
    }
    case BINARY_SAMPLE_FORMAT_S24_4_RJ_LE: {
      for (size_t f = 0; f < frames; f++) {
        for (int c = 0; c < channels; c++) {
          size_t offset = (f * channels + c) * 4;
          pcm_sample_encode_s24_4_rj_bytes(src_channels[c][f], dst + offset);
        }
      }
      break;
    }
    case BINARY_SAMPLE_FORMAT_S24_4_LJ_LE: {
      for (size_t f = 0; f < frames; f++) {
        for (int c = 0; c < channels; c++) {
          size_t offset = (f * channels + c) * 4;
          pcm_sample_encode_s24_4_lj_bytes(src_channels[c][f], dst + offset);
        }
      }
      break;
    }
    case BINARY_SAMPLE_FORMAT_S32_LE: {
      for (size_t f = 0; f < frames; f++) {
        for (int c = 0; c < channels; c++) {
          size_t offset = (f * channels + c) * 4;
          pcm_sample_encode_s32_bytes(src_channels[c][f], dst + offset);
        }
      }
      break;
    }
    case BINARY_SAMPLE_FORMAT_F32_LE: {
      for (size_t f = 0; f < frames; f++) {
        for (int c = 0; c < channels; c++) {
          size_t offset = (f * channels + c) * 4;
          pcm_sample_encode_f32_bytes(src_channels[c][f], dst + offset);
        }
      }
      break;
    }
    case BINARY_SAMPLE_FORMAT_F64_LE: {
      for (size_t f = 0; f < frames; f++) {
        for (int c = 0; c < channels; c++) {
          size_t offset = (f * channels + c) * 8;
          pcm_sample_encode_f64_bytes(src_channels[c][f], dst + offset);
        }
      }
      break;
    }
    default:
      break;
  }
}

// MARK: - File Capture Backend implementation

/** @brief Vtable wrapper for file_capture_open. */
static bool cap_vtable_open(void* ctx, backend_error_t* err) {
  return file_capture_open((file_capture_t*)ctx, err);
}
/** @brief Vtable wrapper for file_capture_read. */
static bool cap_vtable_read(void* ctx, size_t frames, audio_chunk_t* chunk,
                            backend_error_t* err) {
  return file_capture_read((file_capture_t*)ctx, frames, chunk, err);
}
/** @brief Vtable wrapper for file_capture_close. */
static void cap_vtable_close(void* ctx) {
  file_capture_close((file_capture_t*)ctx);
}
/** @brief Vtable wrapper for file_capture_get_pending_rate_change. */
static bool cap_vtable_get_pending_rate_change(void* ctx, double* out_rate) {
  return file_capture_get_pending_rate_change((file_capture_t*)ctx, out_rate);
}
/** @brief Vtable wrapper for file_capture_pitch_control_supported. */
static bool cap_vtable_is_pitch_control_supported(void* ctx) {
  return file_capture_pitch_control_supported((file_capture_t*)ctx);
}
/** @brief Vtable wrapper for file_capture_set_pitch. */
static void cap_vtable_set_pitch(void* ctx, double multiplier) {
  file_capture_set_pitch((file_capture_t*)ctx, multiplier);
}
/** @brief Vtable wrapper for file_capture_wait. */
static bool cap_vtable_wait_for_data(void* ctx, uint32_t timeout_ms) {
  return file_capture_wait((file_capture_t*)ctx, timeout_ms);
}
/** @brief Vtable wrapper for file_capture_destroy. */
static void cap_vtable_destroy(void* ctx) {
  file_capture_destroy((file_capture_t*)ctx);
}
/** @brief Vtable wrapper for file_capture_set_is_paused. */
static void cap_vtable_set_is_paused(void* ctx, bool paused) {
  file_capture_set_is_paused((file_capture_t*)ctx, paused);
}

static const capture_backend_vtable_t file_capture_vtable = {
    .open = cap_vtable_open,
    .read = cap_vtable_read,
    .close = cap_vtable_close,
    .get_pending_rate_change = cap_vtable_get_pending_rate_change,
    .is_pitch_control_supported = cap_vtable_is_pitch_control_supported,
    .set_pitch = cap_vtable_set_pitch,
    .wait_for_data = cap_vtable_wait_for_data,
    .set_is_paused = cap_vtable_set_is_paused,
    .destroy = cap_vtable_destroy};

capture_backend_t* file_capture_create(const capture_device_config_t* config,
                                       int sample_rate, int chunk_size,
                                       processing_parameters_t* params,
                                       backend_error_t* err) {
  (void)params;
  (void)err;
  file_capture_t* capture = (file_capture_t*)calloc(1, sizeof(file_capture_t));
  if (!capture) return NULL;

  if (config->type == AUDIO_BACKEND_TYPE_STDIN_OUT) {
    capture->is_stdin = true;
    capture->format = config->cfg.stdin_in.format;
    capture->channels = config->cfg.stdin_in.channels;
    capture->skip_bytes = config->cfg.stdin_in.has_skip_bytes
                              ? (size_t)config->cfg.stdin_in.skip_bytes
                              : 0;
    capture->read_bytes = config->cfg.stdin_in.has_read_bytes
                              ? (size_t)config->cfg.stdin_in.read_bytes
                              : 0;
    capture->extra_samples = config->cfg.stdin_in.has_extra_samples
                                 ? (size_t)config->cfg.stdin_in.extra_samples
                                 : 0;
  } else {
    capture->is_wav = config->is_wav;
    if (config->is_wav) {
      snprintf(capture->filename, sizeof(capture->filename), "%s",
               config->cfg.wav_file.filename);
      capture->format = BINARY_SAMPLE_FORMAT_INVALID;
      capture->channels = 0;
      capture->skip_bytes = 0;
      capture->read_bytes = 0;
      capture->extra_samples = config->cfg.wav_file.has_extra_samples
                                   ? (size_t)config->cfg.wav_file.extra_samples
                                   : 0;
#ifdef CDSP_TEST
      capture->realtime = config->cfg.wav_file.has_realtime
                              ? config->cfg.wav_file.realtime
                              : false;
#endif
    } else {
      snprintf(capture->filename, sizeof(capture->filename), "%s",
               config->cfg.raw_file.filename);
      capture->format = config->cfg.raw_file.format;
      capture->channels = config->cfg.raw_file.channels;
      capture->skip_bytes = config->cfg.raw_file.has_skip_bytes
                                ? (size_t)config->cfg.raw_file.skip_bytes
                                : 0;
      capture->read_bytes = config->cfg.raw_file.has_read_bytes
                                ? (size_t)config->cfg.raw_file.read_bytes
                                : 0;
      capture->extra_samples = config->cfg.raw_file.has_extra_samples
                                   ? (size_t)config->cfg.raw_file.extra_samples
                                   : 0;
#ifdef CDSP_TEST
      capture->realtime = config->cfg.raw_file.has_realtime
                              ? config->cfg.raw_file.realtime
                              : false;
#endif
    }
  }

  capture->sample_rate = sample_rate;
  capture->chunk_size = chunk_size;

  capture_backend_t* backend =
      (capture_backend_t*)calloc(1, sizeof(capture_backend_t));
  if (!backend) {
    free(capture);
    return NULL;
  }
  backend->ctx = capture;
  backend->vtable = &file_capture_vtable;
#ifdef CDSP_TEST
  backend->is_realtime = capture->realtime;
#else
  backend->is_realtime = false;
#endif
  return backend;
}

bool file_capture_open(file_capture_t* capture, backend_error_t* err) {
  if (capture->is_stdin) {
    capture->f = stdin;
  } else {
    capture->f = cdsp_fopen(capture->filename, "rb");
    if (!capture->f) {
      if (err) {
        char err_msg[1024];
        snprintf(err_msg, sizeof(err_msg), "Failed to open input file '%s': %s",
                 capture->filename, strerror(errno));
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, err_msg);
      }
      return false;
    }
  }

  if (capture->is_wav && !capture->is_stdin) {
    wav_info_t info;
    char msg[256];
    if (!parse_wav_header(capture->f, &info, msg, sizeof(msg))) {
      fclose(capture->f);
      capture->f = NULL;
      if (err)
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
      return false;
    }
    capture->sample_rate = info.sample_rate;
    capture->channels = info.channels;
    capture->format = info.format;
    capture->read_bytes = info.data_bytes;

    logger_info(&g_logger,
                "Parsed input WAV file: rate=%d Hz, channels=%d, format=%s",
                info.sample_rate, info.channels,
                binary_sample_format_to_string(info.format));

    fseek_64(capture->f, info.data_start_offset, SEEK_SET);
  } else {
    if (capture->skip_bytes > 0 && !capture->is_stdin) {
      fseek_64(capture->f, capture->skip_bytes, SEEK_SET);
    }
  }

  size_t sample_size = get_sample_size(capture->format);
  if (sample_size == 0 || capture->channels <= 0 || capture->chunk_size <= 0) {
    if (err) {
      backend_error_init(
          err, BACKEND_ERROR_INITIALIZATION_FAILED,
          "Invalid format, channels, or chunk size for file capture");
    }
    if (!capture->is_stdin && capture->f) {
      fclose(capture->f);
      capture->f = NULL;
    }
    return false;
  }
  capture->raw_buf_capacity =
      capture->chunk_size * capture->channels * sample_size * 4;
  capture->raw_buf =
      (uint8_t*)calloc(capture->raw_buf_capacity, sizeof(uint8_t));
  if (!capture->raw_buf) {
    if (!capture->is_stdin) {
      fclose(capture->f);
      capture->f = NULL;
    }
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Memory allocation failure");
    return false;
  }

  capture->total_bytes_read = 0;
  capture->extra_samples_generated = 0;
  capture->last_read_time_ns = get_time_ns();
#ifdef CDSP_TEST
  capture->start_time_ns = get_time_ns();
  capture->total_frames_read = 0;
#endif
  return true;
}

bool file_capture_read(file_capture_t* capture, size_t frames,
                       audio_chunk_t* chunk, backend_error_t* err) {
  if (atomic_load_explicit(&capture->is_paused, memory_order_acquire)) {
    cdsp_sleep_ms(10);
  }
  if (audio_chunk_get_channels(chunk) < (size_t)capture->channels) {
    if (err) {
      backend_error_init(
          err, BACKEND_ERROR_INVALID_CHANNELS,
          "Chunk channels count does not match capture channels");
    }
    return false;
  }

#if !defined(_WIN32)
  bool should_poll = true;
  struct stat st;
  if (fstat(fileno(capture->f), &st) == 0 && S_ISREG(st.st_mode)) {
    should_poll = false;
  }

  if (should_poll) {
    // Poll the file descriptor to see if data is readable.
    // Timeout is set to 50ms. If no data, return false (no data read) so that
    // the engine capture loop doesn't block forever and can check stop
    // requests.
    struct pollfd pfd = {
        .fd = fileno(capture->f), .events = POLLIN, .revents = 0};
    int poll_ret = poll(&pfd, 1, 50);
    if (poll_ret == 0) {
      // Timeout
      audio_chunk_set_valid_frames(chunk, 0);
      return false;
    } else if (poll_ret < 0) {
      if (err) {
        backend_error_init(err, BACKEND_ERROR_READ_ERROR, "Poll error");
      }
      audio_chunk_set_valid_frames(chunk, 0);
      return false;
    }
  }
#endif

  size_t sample_size = get_sample_size(capture->format);
  if (sample_size == 0 || capture->channels == 0) {
    audio_chunk_set_valid_frames(chunk, 0);
    return false;
  }
  size_t frames_to_read = frames;

  size_t bytes_to_read = frames_to_read * capture->channels * sample_size;
  if (capture->read_bytes > 0 &&
      (capture->total_bytes_read + bytes_to_read) > capture->read_bytes) {
    bytes_to_read = capture->read_bytes - capture->total_bytes_read;
  }

  size_t bytes_read = 0;
  if (bytes_to_read > 0) {
    if (bytes_to_read > capture->raw_buf_capacity) {
      uint8_t* new_buf = (uint8_t*)realloc(capture->raw_buf, bytes_to_read);
      if (!new_buf) {
        if (err) {
          backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                             "Failed to reallocate file capture raw buffer");
        }
        audio_chunk_set_valid_frames(chunk, 0);
        return false;
      }
      capture->raw_buf = new_buf;
      capture->raw_buf_capacity = bytes_to_read;
    }
    bytes_read = fread(capture->raw_buf, 1, bytes_to_read, capture->f);
    capture->total_bytes_read += bytes_read;
  }

  size_t frames_read = bytes_read / (capture->channels * sample_size);

  // Decode read frames
  double* dst_channels[capture->channels];
  for (int c = 0; c < capture->channels; c++) {
    dst_channels[c] = audio_chunk_get_channel(chunk, c);
  }
  decode_samples_deinterleave(dst_channels, capture->raw_buf, frames_read,
                              capture->channels, capture->format);

  // Check EOF and generate extra samples if configured
  // If we read fewer frames than requested (e.g., EOF), and we have configured
  // extra samples to generate, append silence up to the requested number of
  // frames or until the extra samples limit is reached.
  if (frames_read < frames) {
    size_t remaining_frames = frames - frames_read;
    size_t extra_to_generate =
        capture->extra_samples - capture->extra_samples_generated;
    if (extra_to_generate > remaining_frames) {
      extra_to_generate = remaining_frames;
    }

    if (extra_to_generate > 0) {
      for (size_t f = frames_read; f < (frames_read + extra_to_generate); f++) {
        for (int c = 0; c < capture->channels; c++) {
          audio_chunk_get_channel(chunk, c)[f] = 0.0;
        }
      }
      capture->extra_samples_generated += extra_to_generate;
      frames_read += extra_to_generate;
    }
  }

  audio_chunk_set_valid_frames(chunk, frames_read);

#ifdef CDSP_TEST
  if (frames_read > 0) {
    if (capture->total_frames_read == 0) {
      capture->start_time_ns = get_time_ns();
    }
    capture->total_frames_read += frames_read;
    if (capture->realtime) {
      uint64_t target_elapsed_ns = (uint64_t)capture->total_frames_read *
                                   1000000000ULL /
                                   (uint64_t)capture->sample_rate;
      uint64_t actual_elapsed_ns = get_time_ns() - capture->start_time_ns;
      if (target_elapsed_ns > actual_elapsed_ns) {
        uint64_t sleep_ns = target_elapsed_ns - actual_elapsed_ns;
        cdsp_sleep_us(sleep_ns / 1000ULL);
      }
    }
  }
#endif

  if (frames_read == 0) {
    if (err) {
      backend_error_init(err, BACKEND_ERROR_READ_EOF,
                         "End of file/stream reached");
    }
  }

  return (frames_read > 0);
}

void file_capture_close(file_capture_t* capture) {
  if (!capture) return;
  if (capture->f && !capture->is_stdin) {
    fclose(capture->f);
    capture->f = NULL;
  }
  if (capture->raw_buf) {
    free(capture->raw_buf);
    capture->raw_buf = NULL;
  }
}

bool file_capture_get_pending_rate_change(file_capture_t* capture,
                                          double* out_rate) {
  (void)capture;
  (void)out_rate;
  return false;
}

bool file_capture_pitch_control_supported(file_capture_t* capture) {
  (void)capture;
  return false;
}

void file_capture_set_pitch(file_capture_t* capture, double multiplier) {
  (void)capture;
  (void)multiplier;
}

bool file_capture_wait(file_capture_t* capture, uint32_t timeout_ms) {
  (void)capture;
  cdsp_sleep_ms(timeout_ms);
  return true;
}

void file_capture_destroy(file_capture_t* capture) {
  if (!capture) return;
  file_capture_close(capture);
  free(capture);
}

void file_capture_set_is_paused(file_capture_t* capture, bool paused) {
  if (capture) {
    atomic_store_explicit(&capture->is_paused, paused, memory_order_release);
  }
}

// MARK: - File Playback Backend implementation

/** @brief Vtable wrapper for file_playback_open. */
static bool play_vtable_open(void* ctx, backend_error_t* err) {
  return file_playback_open((file_playback_t*)ctx, err);
}
/** @brief Vtable wrapper for file_playback_write. */
static bool play_vtable_write(void* ctx, const audio_chunk_t* chunk,
                              backend_error_t* err) {
  return file_playback_write((file_playback_t*)ctx, chunk, err);
}
/** @brief Vtable wrapper for file_playback_close. */
static void play_vtable_close(void* ctx) {
  file_playback_close((file_playback_t*)ctx);
}
/** @brief Vtable wrapper for file_playback_get_buffer_level. */
static size_t play_vtable_get_buffer_level(void* ctx) {
  return file_playback_get_buffer_level((file_playback_t*)ctx);
}
/** @brief Vtable wrapper for file_playback_get_pending_rate_change. */
static bool play_vtable_get_pending_rate_change(void* ctx, double* out_rate) {
  return file_playback_get_pending_rate_change((file_playback_t*)ctx, out_rate);
}
/** @brief Vtable wrapper for file_playback_prefill_silence. */
static bool play_vtable_prefill_silence(void* ctx, size_t frames,
                                        backend_error_t* err) {
  return file_playback_prefill_silence((file_playback_t*)ctx, frames, err);
}
/** @brief Vtable wrapper for file_playback_get_is_paused. */
static bool play_vtable_get_is_paused(void* ctx) {
  return file_playback_get_is_paused((file_playback_t*)ctx);
}
/** @brief Vtable wrapper for file_playback_set_is_paused. */
static void play_vtable_set_is_paused(void* ctx, bool paused) {
  file_playback_set_is_paused((file_playback_t*)ctx, paused);
}
/** @brief Vtable wrapper for file_playback_destroy. */
/** @brief Vtable wrapper for file_playback_stop. */
static void play_vtable_stop(void* ctx) {
#ifdef CDSP_TEST
  file_playback_t* playback = (file_playback_t*)ctx;
  if (playback) {
    playback->stopped = true;
  }
#else
  (void)ctx;
#endif
}
/** @brief Vtable wrapper for file_playback_destroy. */
static void play_vtable_destroy(void* ctx) {
  file_playback_destroy((file_playback_t*)ctx);
}

static const playback_backend_vtable_t file_playback_vtable = {
    .open = play_vtable_open,
    .write = play_vtable_write,
    .close = play_vtable_close,
    .get_buffer_level = play_vtable_get_buffer_level,
    .get_pending_rate_change = play_vtable_get_pending_rate_change,
    .prefill_silence = play_vtable_prefill_silence,
    .get_is_paused = play_vtable_get_is_paused,
    .set_is_paused = play_vtable_set_is_paused,
    .stop = play_vtable_stop,
    .destroy = play_vtable_destroy};

playback_backend_t* file_playback_create(const playback_device_config_t* config,
                                         int sample_rate, int chunk_size,
                                         processing_parameters_t* params,
                                         backend_error_t* err) {
  (void)sample_rate;
  (void)params;
  (void)err;
  if (config->type == AUDIO_BACKEND_TYPE_FILE &&
      config->cfg.raw_file.wav_header &&
      config->cfg.raw_file.format == BINARY_SAMPLE_FORMAT_S24_4_RJ_LE) {
    if (err) {
      backend_error_init(
          err, BACKEND_ERROR_INITIALIZATION_FAILED,
          "Wav files do not support the S24_4_RJ_LE sample format");
    }
    return NULL;
  }

  file_playback_t* playback =
      (file_playback_t*)calloc(1, sizeof(file_playback_t));
  if (!playback) return NULL;

  if (config->type == AUDIO_BACKEND_TYPE_STDIN_OUT) {
    playback->is_stdout = true;
    playback->format = config->cfg.stdout_out.format;
    playback->is_wav = config->cfg.stdout_out.wav_header;
    playback->use_rf64 = false;
    playback->channels = config->cfg.stdout_out.channels;
#ifdef CDSP_TEST
    playback->realtime = false;
#endif
  } else {
    snprintf(playback->filename, sizeof(playback->filename), "%s",
             config->cfg.raw_file.filename);
    playback->format = config->cfg.raw_file.format;
    playback->is_wav = config->cfg.raw_file.wav_header;
    playback->use_rf64 = config->cfg.raw_file.has_use_rf64
                             ? config->cfg.raw_file.use_rf64
                             : false;
    playback->channels = config->cfg.raw_file.channels;
#ifdef CDSP_TEST
    playback->realtime = config->cfg.raw_file.has_realtime
                             ? config->cfg.raw_file.realtime
                             : false;
#endif
  }
  playback->chunk_size = chunk_size;
  playback->sample_rate = sample_rate;

  playback_backend_t* backend =
      (playback_backend_t*)calloc(1, sizeof(playback_backend_t));
  if (!backend) {
    free(playback);
    return NULL;
  }
  backend->ctx = playback;
  backend->vtable = &file_playback_vtable;
  return backend;
}

bool file_playback_open(file_playback_t* playback, backend_error_t* err) {
  if (playback->is_stdout) {
    playback->f = stdout;
  } else {
    playback->f = cdsp_fopen(playback->filename, "wb");
    if (!playback->f) {
      if (err) {
        char err_msg[1024];
        snprintf(err_msg, sizeof(err_msg),
                 "Failed to open output file '%s': %s", playback->filename,
                 strerror(errno));
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, err_msg);
      }
      return false;
    }
  }

  size_t sample_size = get_sample_size(playback->format);
  if (sample_size == 0 || playback->channels <= 0 ||
      playback->chunk_size <= 0) {
    if (err) {
      backend_error_init(
          err, BACKEND_ERROR_INITIALIZATION_FAILED,
          "Invalid format, channels, or chunk size for file playback");
    }
    if (!playback->is_stdout && playback->f) {
      fclose(playback->f);
      playback->f = NULL;
    }
    return false;
  }
  playback->raw_buf_capacity =
      playback->chunk_size * playback->channels * sample_size * 4;
  playback->raw_buf =
      (uint8_t*)calloc(playback->raw_buf_capacity, sizeof(uint8_t));
  if (!playback->raw_buf) {
    if (!playback->is_stdout) {
      fclose(playback->f);
      playback->f = NULL;
    }
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Memory allocation failure");
    return false;
  }

  playback->total_bytes_written = 0;
#ifdef CDSP_TEST
  playback->start_time_ns = get_time_ns();
  playback->total_frames_written = 0;
#endif

  playback->is_seekable = false;
  if (!playback->is_stdout && playback->f) {
    if (fseek_64(playback->f, 0, SEEK_CUR) == 0) {
      playback->is_seekable = true;
    }
  }

  if (playback->is_wav) {
    if (playback->is_seekable) {
      size_t header_size = playback->use_rf64 ? 80 : 44;
      uint8_t zero_header[80];
      memset(zero_header, 0, header_size);
      fwrite(zero_header, 1, header_size, playback->f);
    } else {
      if (playback->use_rf64) {
        logger_warn(&g_logger,
                    "RF64 WAV output requires a seekable file, writing a "
                    "streaming WAV header instead");
      }
      write_wav_header_to_file(playback->f, playback->channels,
                               playback->format, playback->sample_rate,
                               0xFFFFFFFF, false);
    }
  }

  return true;
}

bool file_playback_write(file_playback_t* playback, const audio_chunk_t* chunk,
                         backend_error_t* err) {
#ifdef CDSP_TEST
  if (playback->stopped) {
    if (err) {
      backend_error_init(err, BACKEND_ERROR_WRITE_ERROR, "Playback stopped");
    }
    return false;
  }
#endif
  if (audio_chunk_get_channels(chunk) < (size_t)playback->channels) {
    if (err) {
      backend_error_init(
          err, BACKEND_ERROR_INVALID_CHANNELS,
          "Chunk channels count does not match playback channels");
    }
    return false;
  }
  size_t frames = audio_chunk_get_valid_frames(chunk);
  size_t sample_size = get_sample_size(playback->format);

  // Allocate larger buffer if chunk size exceeds chunk_size
  // Dynamically reallocate the raw buffer if the incoming chunk has more frames
  // than our pre-allocated capacity (which was based on chunk_size).
  size_t required_bytes = frames * playback->channels * sample_size;

  if (playback->is_wav && playback->is_seekable && !playback->use_rf64) {
    uint64_t max_bytes = 0xFFFFFFFFULL - 256;
    if (playback->total_bytes_written > max_bytes ||
        required_bytes > (max_bytes - playback->total_bytes_written)) {
      logger_warn(&g_logger,
                  "Wav file reached the maximum size of a plain wav file. "
                  "Stopping write to avoid writing an invalid file.");
      if (err) {
        backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                           "Plain WAV 4 GB limit reached");
      }
      return false;
    }
  }

  if (required_bytes > playback->raw_buf_capacity) {
    uint8_t* new_buf = (uint8_t*)realloc(playback->raw_buf, required_bytes);
    if (!new_buf) {
      if (err)
        backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                           "Failed to reallocate file playback buffer");
      return false;
    }
    playback->raw_buf = new_buf;
    playback->raw_buf_capacity = required_bytes;
  }

  // Encode samples
  const double* src_channels[playback->channels];
  for (int c = 0; c < playback->channels; c++) {
    src_channels[c] = audio_chunk_get_channel((audio_chunk_t*)chunk, c);
  }
  encode_samples_interleave(playback->raw_buf, src_channels, frames,
                            playback->channels, playback->format);

  size_t bytes_written =
      fwrite(playback->raw_buf, 1, required_bytes, playback->f);
  playback->total_bytes_written += bytes_written;

  bool success = (bytes_written == required_bytes);
#ifdef CDSP_TEST
  if (success && frames > 0) {
    if (playback->total_frames_written == 0) {
      playback->start_time_ns = get_time_ns();
    }
    playback->total_frames_written += frames;
    if (playback->realtime) {
      uint64_t target_elapsed_ns = (uint64_t)playback->total_frames_written *
                                   1000000000ULL /
                                   (uint64_t)playback->sample_rate;
      uint64_t actual_elapsed_ns = get_time_ns() - playback->start_time_ns;
      if (target_elapsed_ns > actual_elapsed_ns) {
        cdsp_sleep_us((target_elapsed_ns - actual_elapsed_ns) / 1000ULL);
      }
    }
  }
#endif

  return success;
}

void file_playback_close(file_playback_t* playback) {
  if (!playback) return;
  if (playback->f) {
    if (playback->is_wav && playback->is_seekable && !playback->is_stdout) {
      // Write completed WAV header
      if (playback->use_rf64) {
        write_rf64_header_to_file(playback->f, playback->channels,
                                  playback->format, playback->sample_rate,
                                  playback->total_bytes_written);
      } else {
        write_wav_header_to_file(playback->f, playback->channels,
                                 playback->format, playback->sample_rate,
                                 (uint32_t)playback->total_bytes_written, true);
      }
    }
    if (!playback->is_stdout) {
      fclose(playback->f);
    }
    playback->f = NULL;
  }
  if (playback->raw_buf) {
    free(playback->raw_buf);
    playback->raw_buf = NULL;
  }
}

size_t file_playback_get_buffer_level(file_playback_t* playback) {
  (void)playback;
  return 0;
}

bool file_playback_get_pending_rate_change(file_playback_t* playback,
                                           double* out_rate) {
  (void)playback;
  (void)out_rate;
  return false;
}

bool file_playback_prefill_silence(file_playback_t* playback, size_t frames,
                                   backend_error_t* err) {
  (void)err;
  // For file playback, we don't need to prefill silence because it is not
  // real-time audio. However, to keep it clean and avoid errors, we just return
  // true.
  (void)playback;
  (void)frames;
  return true;
}

bool file_playback_get_is_paused(file_playback_t* playback) {
  (void)playback;
  return false;
}

void file_playback_set_is_paused(file_playback_t* playback, bool paused) {
  (void)playback;
  (void)paused;
}

void file_playback_destroy(file_playback_t* playback) {
  if (!playback) return;
  file_playback_close(playback);
  free(playback);
}
