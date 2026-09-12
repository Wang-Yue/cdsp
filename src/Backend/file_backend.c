
#include "Backend/file_backend.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <poll.h>
#include <sys/stat.h>
#endif

#include "Audio/audio_chunk.h"
#include "Backend/backend_error.h"
#include "Config/engine_config_types.h"
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
  size_t channels;
  int chunk_size;
  binary_sample_format_t format;
  bool is_wav;
  uint64_t skip_bytes;
  uint64_t read_bytes;
  uint64_t total_bytes_read;
  size_t extra_samples;
  size_t extra_samples_generated;
  int playback_sample_rate;
  double resampling_ratio;
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
  size_t channels;
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
typedef cdsp_wav_info_t wav_info_t;

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
  if (memcmp(header, "RF64", 4) == 0 || memcmp(header, "BW64", 4) == 0) {
    is_rf64 = true;
  } else if (memcmp(header, "RIFF", 4) != 0) {
    snprintf(err_msg, err_msg_len, "Not a RIFF, RF64 or BW64 file");
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
    uint8_t size_bytes[4];
    if (fread(size_bytes, 1, 4, f) != 4) {
      break;
    }
    chunk_size = (uint32_t)size_bytes[0] | ((uint32_t)size_bytes[1] << 8) |
                 ((uint32_t)size_bytes[2] << 16) |
                 ((uint32_t)size_bytes[3] << 24);

    if (memcmp(chunk_id, "ds64", 4) == 0) {
      if (chunk_size < 24) {
        uint32_t skip = (chunk_size + 1) & ~1;
        if (fseek_64(f, skip, SEEK_CUR) != 0) {
          snprintf(err_msg, err_msg_len,
                   "Failed to seek past short ds64 chunk");
          return false;
        }
        continue;
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
        uint32_t remaining = chunk_size - 24;
        uint32_t pad = (chunk_size & 1);
        if (fseek_64(f, remaining + pad, SEEK_CUR) != 0) {
          snprintf(err_msg, err_msg_len, "Failed to seek past ds64 chunk");
          return false;
        }
      }
    } else if (memcmp(chunk_id, "fmt ", 4) == 0) {
      found_fmt = true;
      if (chunk_size != 16 && chunk_size != 18 && chunk_size != 40) {
        snprintf(err_msg, err_msg_len,
                 "Invalid fmt chunk size %u (must be 16, 18, or 40)",
                 chunk_size);
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
      uint16_t block_align = fmt_payload[12] | (fmt_payload[13] << 8);
      uint16_t bits_per_sample = fmt_payload[14] | (fmt_payload[15] << 8);

      if (audio_format != 1 && audio_format != 3 && audio_format != 0xFFFE) {
        snprintf(err_msg, err_msg_len,
                 "Unsupported WAV format code %d (only PCM/Float supported)",
                 audio_format);
        return false;
      }

      bool is_extended = (audio_format == 0xFFFE);
      if (is_extended) {
        if (chunk_size != 40) {
          snprintf(err_msg, err_msg_len,
                   "extended fmt chunk must be 40 bytes, got %u", chunk_size);
          return false;
        }
        static const uint8_t guid_suffix[14] = {0x00, 0x00, 0x00, 0x00, 0x10,
                                                0x00, 0x80, 0x00, 0x00, 0xAA,
                                                0x00, 0x38, 0x9B, 0x71};
        if (memcmp(&fmt_payload[26], guid_suffix, 14) != 0) {
          snprintf(err_msg, err_msg_len,
                   "Unsupported sub-format GUID in EXTENSIBLE");
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

      // The container size is decided by nBlockAlign, not by wBitsPerSample:
      // 24 bits may be stored in either 3 or 4 bytes. Upstream (waveadapter's
      // `look_up_format`) matches on the full
      // (format code, bits, bytes per sample) tuple and rejects anything else,
      // so inferring the stride from the bit depth alone silently mis-decodes
      // 24-in-4 files.
      if (channels == 0) {
        snprintf(err_msg, err_msg_len, "Invalid channel count 0 in fmt chunk");
        return false;
      }
      if (block_align == 0 || (block_align % channels) != 0) {
        snprintf(err_msg, err_msg_len, "Invalid block align %d for %d channels",
                 block_align, channels);
        return false;
      }
      uint16_t bytes_per_sample = (uint16_t)(block_align / channels);
      uint16_t valid_bits = (to_read >= 20)
                                ? (fmt_payload[18] | (fmt_payload[19] << 8))
                                : bits_per_sample;
      if (valid_bits == 0) valid_bits = bits_per_sample;

      if (audio_format == 1) {
        if (bits_per_sample == 16 && bytes_per_sample == 2)
          format = BINARY_SAMPLE_FORMAT_S16_LE;
        else if (bits_per_sample == 24 && bytes_per_sample == 3)
          format = BINARY_SAMPLE_FORMAT_S24_3_LE;
        else if (bits_per_sample == 24 && bytes_per_sample == 4)
          format = BINARY_SAMPLE_FORMAT_S24_4_LJ_LE;
        else if (bits_per_sample == 32 && bytes_per_sample == 4) {
          if (is_extended && valid_bits == 24) {
            format = BINARY_SAMPLE_FORMAT_S24_4_LJ_LE;
          } else {
            format = BINARY_SAMPLE_FORMAT_S32_LE;
          }
        }
      } else if (audio_format == 3) {
        if (bits_per_sample == 32 && bytes_per_sample == 4)
          format = BINARY_SAMPLE_FORMAT_F32_LE;
        else if (bits_per_sample == 64 && bytes_per_sample == 8)
          format = BINARY_SAMPLE_FORMAT_F64_LE;
      }

      if (format == BINARY_SAMPLE_FORMAT_INVALID) {
        snprintf(err_msg, err_msg_len,
                 "Unsupported WAV sample format: format %d, %d bits in %d "
                 "bytes per sample",
                 audio_format, bits_per_sample, bytes_per_sample);
        return false;
      }

      if (chunk_size > to_read) {
        if (fseek_64(f, chunk_size - to_read, SEEK_CUR) != 0) {
          snprintf(err_msg, err_msg_len, "Failed to seek past fmt chunk");
          return false;
        }
      }
    } else if (memcmp(chunk_id, "data", 4) == 0) {
      if (!found_data) {
        found_data = true;
        data_start_offset = (uint64_t)ftell_64(f);
        if (is_rf64 && chunk_size == 0xFFFFFFFF) {
          data_bytes = rf64_data_size;
        } else {
          data_bytes = chunk_size;
        }
      }
      if (!is_rf64 && chunk_size == 0xFFFFFFFF) {
        // Plain RIFF streaming placeholder: break early because audio follows
        // until EOF.
        break;
      }
      uint32_t pad = chunk_size & 1;
      if (fseek_64(f, (int64_t)chunk_size + pad, SEEK_CUR) != 0) {
        // Reached EOF or unseekable, stop chunk walking
        break;
      }
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

bool cdsp_wav_file_read_info(const char* filename, cdsp_wav_info_t* info,
                             char* err_msg, size_t err_msg_len) {
  if (!filename || !info) {
    if (err_msg && err_msg_len > 0) {
      snprintf(err_msg, err_msg_len, "Invalid arguments");
    }
    return false;
  }
  FILE* f = cdsp_fopen(filename, "rb");
  if (!f) {
    if (err_msg && err_msg_len > 0) {
      snprintf(err_msg, err_msg_len, "Could not open WAV file '%s': %s",
               filename, strerror(errno));
    }
    return false;
  }
  char msg[256] = {0};
  bool ok = parse_wav_header(f, info, msg, sizeof(msg));
  fclose(f);
  if (!ok && err_msg && err_msg_len > 0) {
    snprintf(err_msg, err_msg_len, "%s", msg);
  }
  return ok;
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
  bool extensible =
      (format == BINARY_SAMPLE_FORMAT_S24_4_LJ_LE ||
       format == BINARY_SAMPLE_FORMAT_S24_4_RJ_LE || channels > 2);
  bool is_float = (format == BINARY_SAMPLE_FORMAT_F32_LE ||
                   format == BINARY_SAMPLE_FORMAT_F64_LE);
  bool needs_fact = is_float || extensible;
  size_t fmt_size = extensible ? 40 : 16;
  size_t header_size = 12 + (8 + fmt_size) + (needs_fact ? 12 : 0) + 8;
  uint8_t header[128];
  memset(header, 0, sizeof(header));

  uint8_t* p = header;
  memcpy(p, "RIFF", 4);
  p += 4;
  uint32_t file_size = (data_bytes >= (0xFFFFFFFF - header_size + 8))
                           ? 0xFFFFFFFF
                           : (uint32_t)(data_bytes + header_size - 8);
  p[0] = file_size & 0xFF;
  p[1] = (file_size >> 8) & 0xFF;
  p[2] = (file_size >> 16) & 0xFF;
  p[3] = (file_size >> 24) & 0xFF;
  p += 4;
  memcpy(p, "WAVE", 4);
  p += 4;

  // fmt chunk
  memcpy(p, "fmt ", 4);
  p += 4;
  p[0] = (uint8_t)(fmt_size & 0xFF);
  p[1] = (uint8_t)((fmt_size >> 8) & 0xFF);
  p[2] = (uint8_t)((fmt_size >> 16) & 0xFF);
  p[3] = (uint8_t)((fmt_size >> 24) & 0xFF);
  p += 4;

  size_t sample_size = sample_format_bytes_per_sample(format);
  uint32_t byte_rate = sample_rate * (uint32_t)channels * (uint32_t)sample_size;
  uint16_t block_align = (uint16_t)(channels * sample_size);
  uint16_t bits_per_sample = (uint16_t)(sample_size * 8);

  if (extensible) {
    uint16_t format_tag = 0xFFFE;
    p[0] = format_tag & 0xFF;
    p[1] = (format_tag >> 8) & 0xFF;
    p[2] = channels & 0xFF;
    p[3] = (channels >> 8) & 0xFF;
    p[4] = sample_rate & 0xFF;
    p[5] = (sample_rate >> 8) & 0xFF;
    p[6] = (sample_rate >> 16) & 0xFF;
    p[7] = (sample_rate >> 24) & 0xFF;
    p[8] = byte_rate & 0xFF;
    p[9] = (byte_rate >> 8) & 0xFF;
    p[10] = (byte_rate >> 16) & 0xFF;
    p[11] = (byte_rate >> 24) & 0xFF;
    p[12] = block_align & 0xFF;
    p[13] = (block_align >> 8) & 0xFF;
    p[14] = bits_per_sample & 0xFF;
    p[15] = (bits_per_sample >> 8) & 0xFF;
    // cbSize = 22
    p[16] = 22;
    p[17] = 0;
    // ValidBitsPerSample
    uint16_t valid_bits = (format == BINARY_SAMPLE_FORMAT_S24_4_LJ_LE ||
                           format == BINARY_SAMPLE_FORMAT_S24_4_RJ_LE)
                              ? 24
                              : bits_per_sample;
    p[18] = valid_bits & 0xFF;
    p[19] = (valid_bits >> 8) & 0xFF;
    // dwChannelMask = 0
    p[20] = 0;
    p[21] = 0;
    p[22] = 0;
    p[23] = 0;
    // SubFormat GUID
    static const uint8_t guid_suffix[14] = {
        0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80,
        0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
    uint16_t sub_format = is_float ? 3 : 1;
    p[24] = sub_format & 0xFF;
    p[25] = (sub_format >> 8) & 0xFF;
    memcpy(p + 26, guid_suffix, 14);
    p += 40;
  } else {
    uint16_t format_tag = is_float ? 3 : 1;
    p[0] = format_tag & 0xFF;
    p[1] = (format_tag >> 8) & 0xFF;
    p[2] = channels & 0xFF;
    p[3] = (channels >> 8) & 0xFF;
    p[4] = sample_rate & 0xFF;
    p[5] = (sample_rate >> 8) & 0xFF;
    p[6] = (sample_rate >> 16) & 0xFF;
    p[7] = (sample_rate >> 24) & 0xFF;
    p[8] = byte_rate & 0xFF;
    p[9] = (byte_rate >> 8) & 0xFF;
    p[10] = (byte_rate >> 16) & 0xFF;
    p[11] = (byte_rate >> 24) & 0xFF;
    p[12] = block_align & 0xFF;
    p[13] = (block_align >> 8) & 0xFF;
    p[14] = bits_per_sample & 0xFF;
    p[15] = (bits_per_sample >> 8) & 0xFF;
    p += 16;
  }

  // fact chunk
  if (needs_fact) {
    memcpy(p, "fact", 4);
    p += 4;
    p[0] = 4;
    p[1] = 0;
    p[2] = 0;
    p[3] = 0;
    p += 4;
    uint32_t sample_frames = 0xFFFFFFFF;
    if (data_bytes != 0xFFFFFFFF && channels > 0 && sample_size > 0) {
      sample_frames = (uint32_t)(data_bytes / (channels * sample_size));
    }
    p[0] = sample_frames & 0xFF;
    p[1] = (sample_frames >> 8) & 0xFF;
    p[2] = (sample_frames >> 16) & 0xFF;
    p[3] = (sample_frames >> 24) & 0xFF;
    p += 4;
  }

  // data chunk header
  memcpy(p, "data", 4);
  p += 4;
  p[0] = data_bytes & 0xFF;
  p[1] = (data_bytes >> 8) & 0xFF;
  p[2] = (data_bytes >> 16) & 0xFF;
  p[3] = (data_bytes >> 24) & 0xFF;
  p += 4;

  if (is_seekable) {
    fseek_64(f, 0, SEEK_SET);
  }
  fwrite(header, 1, header_size, f);
}

/**
 * @brief Write RF64 WAV header to the file.
 */
static void write_rf64_header_to_file(FILE* f, size_t channels,
                                      binary_sample_format_t format,
                                      uint32_t sample_rate,
                                      uint64_t data_bytes) {
  bool extensible =
      (format == BINARY_SAMPLE_FORMAT_S24_4_LJ_LE ||
       format == BINARY_SAMPLE_FORMAT_S24_4_RJ_LE || channels > 2);
  bool is_float = (format == BINARY_SAMPLE_FORMAT_F32_LE ||
                   format == BINARY_SAMPLE_FORMAT_F64_LE);
  size_t fmt_size = extensible ? 40 : 16;
  size_t header_size = 12 + 36 + (8 + fmt_size) + 8;
  uint8_t header[128];
  memset(header, 0, sizeof(header));
  uint8_t* p = header;

  memcpy(p, "RF64", 4);
  p += 4;
  p[0] = 0xFF;
  p[1] = 0xFF;
  p[2] = 0xFF;
  p[3] = 0xFF;
  p += 4;
  memcpy(p, "WAVE", 4);
  p += 4;

  // ds64 chunk (4 bytes id, 4 bytes size=28, 28 bytes body)
  memcpy(p, "ds64", 4);
  p += 4;
  p[0] = 28;
  p[1] = 0;
  p[2] = 0;
  p[3] = 0;
  p += 4;

  uint64_t riff_size = data_bytes + header_size - 8;
  for (int i = 0; i < 8; i++) {
    p[i] = (riff_size >> (i * 8)) & 0xFF;
  }
  p += 8;

  for (int i = 0; i < 8; i++) {
    p[i] = (data_bytes >> (i * 8)) & 0xFF;
  }
  p += 8;

  size_t sample_size = sample_format_bytes_per_sample(format);
  uint64_t sample_count = (channels > 0 && sample_size > 0)
                              ? (data_bytes / (channels * sample_size))
                              : 0;
  for (int i = 0; i < 8; i++) {
    p[i] = (sample_count >> (i * 8)) & 0xFF;
  }
  p += 8;
  // table length = 0
  p[0] = 0;
  p[1] = 0;
  p[2] = 0;
  p[3] = 0;
  p += 4;

  // fmt chunk
  memcpy(p, "fmt ", 4);
  p += 4;
  p[0] = (uint8_t)(fmt_size & 0xFF);
  p[1] = (uint8_t)((fmt_size >> 8) & 0xFF);
  p[2] = (uint8_t)((fmt_size >> 16) & 0xFF);
  p[3] = (uint8_t)((fmt_size >> 24) & 0xFF);
  p += 4;

  uint32_t byte_rate = sample_rate * (uint32_t)channels * (uint32_t)sample_size;
  uint16_t block_align = (uint16_t)(channels * sample_size);
  uint16_t bits_per_sample = (uint16_t)(sample_size * 8);

  if (extensible) {
    uint16_t format_tag = 0xFFFE;
    p[0] = format_tag & 0xFF;
    p[1] = (format_tag >> 8) & 0xFF;
    p[2] = channels & 0xFF;
    p[3] = (channels >> 8) & 0xFF;
    p[4] = sample_rate & 0xFF;
    p[5] = (sample_rate >> 8) & 0xFF;
    p[6] = (sample_rate >> 16) & 0xFF;
    p[7] = (sample_rate >> 24) & 0xFF;
    p[8] = byte_rate & 0xFF;
    p[9] = (byte_rate >> 8) & 0xFF;
    p[10] = (byte_rate >> 16) & 0xFF;
    p[11] = (byte_rate >> 24) & 0xFF;
    p[12] = block_align & 0xFF;
    p[13] = (block_align >> 8) & 0xFF;
    p[14] = bits_per_sample & 0xFF;
    p[15] = (bits_per_sample >> 8) & 0xFF;
    p[16] = 22;
    p[17] = 0;
    uint16_t valid_bits = (format == BINARY_SAMPLE_FORMAT_S24_4_LJ_LE ||
                           format == BINARY_SAMPLE_FORMAT_S24_4_RJ_LE)
                              ? 24
                              : bits_per_sample;
    p[18] = valid_bits & 0xFF;
    p[19] = (valid_bits >> 8) & 0xFF;
    p[20] = 0;
    p[21] = 0;
    p[22] = 0;
    p[23] = 0;
    static const uint8_t guid_suffix[14] = {
        0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80,
        0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
    uint16_t sub_format = is_float ? 3 : 1;
    p[24] = sub_format & 0xFF;
    p[25] = (sub_format >> 8) & 0xFF;
    memcpy(p + 26, guid_suffix, 14);
    p += 40;
  } else {
    uint16_t format_tag = is_float ? 3 : 1;
    p[0] = format_tag & 0xFF;
    p[1] = (format_tag >> 8) & 0xFF;
    p[2] = channels & 0xFF;
    p[3] = (channels >> 8) & 0xFF;
    p[4] = sample_rate & 0xFF;
    p[5] = (sample_rate >> 8) & 0xFF;
    p[6] = (sample_rate >> 16) & 0xFF;
    p[7] = (sample_rate >> 24) & 0xFF;
    p[8] = byte_rate & 0xFF;
    p[9] = (byte_rate >> 8) & 0xFF;
    p[10] = (byte_rate >> 16) & 0xFF;
    p[11] = (byte_rate >> 24) & 0xFF;
    p[12] = block_align & 0xFF;
    p[13] = (block_align >> 8) & 0xFF;
    p[14] = bits_per_sample & 0xFF;
    p[15] = (bits_per_sample >> 8) & 0xFF;
    p += 16;
  }

  // data chunk header
  memcpy(p, "data", 4);
  p += 4;
  p[0] = 0xFF;
  p[1] = 0xFF;
  p[2] = 0xFF;
  p[3] = 0xFF;
  p += 4;

  fseek_64(f, 0, SEEK_SET);
  fwrite(header, 1, header_size, f);
}

// MARK: - File Capture Backend implementation

/**
 * @brief Open the file capture device.
 *
 * @param ctx Pointer to the file_capture_t instance.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return true if successful, false otherwise.
 */
static bool file_capture_open(void* ctx, backend_error_t* err) {
  file_capture_t* capture = (file_capture_t*)ctx;
  if (!capture) return false;
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
    if (info.sample_rate > 0 && capture->playback_sample_rate > 0) {
      capture->resampling_ratio =
          (double)capture->playback_sample_rate / (double)info.sample_rate;
    }

    logger_info(&g_logger,
                "Parsed input WAV file: rate=%d Hz, channels=%d, format=%s",
                info.sample_rate, info.channels,
                file_sample_format_to_string(info.format));

    fseek_64(capture->f, info.data_start_offset, SEEK_SET);
  } else {
    if (capture->skip_bytes > 0) {
      logger_debug(&g_logger, "skipping the first %llu bytes",
                   (unsigned long long)capture->skip_bytes);
      if (capture->is_stdin ||
          fseek_64(capture->f, (int64_t)capture->skip_bytes, SEEK_SET) != 0) {
        uint64_t remaining = capture->skip_bytes;
        uint8_t discard_buf[4096];
        while (remaining > 0) {
          size_t to_read = remaining < sizeof(discard_buf)
                               ? (size_t)remaining
                               : sizeof(discard_buf);
          size_t n = fread(discard_buf, 1, to_read, capture->f);
          if (n == 0) break;
          remaining -= n;
        }
      }
    }
  }

  size_t sample_size = sample_format_bytes_per_sample(capture->format);
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
  if (capture->raw_buf) {
    free(capture->raw_buf);
    capture->raw_buf = NULL;
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

/**
 * @brief Read audio frames from the file capture device.
 *
 * @param ctx Pointer to the file_capture_t instance.
 * @param frames Number of frames to read.
 * @param chunk Pointer to the audio chunk to fill.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return true if successful, false otherwise.
 */
static bool file_capture_read(void* ctx, size_t frames, audio_chunk_t* chunk,
                              backend_error_t* err) {
  file_capture_t* capture = (file_capture_t*)ctx;
  if (!capture) return false;
#ifdef CDSP_TEST
  if (capture->realtime &&
      atomic_load_explicit(&capture->is_paused, memory_order_acquire)) {
    cdsp_sleep_ms(10);
  }
#endif
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
    struct pollfd pfd = {
        .fd = fileno(capture->f), .events = POLLIN, .revents = 0};
    int poll_ret = poll(&pfd, 1, 50);
    if (poll_ret == 0) {
      audio_chunk_set_valid_frames(chunk, 0);
      return true;
    } else if (poll_ret < 0) {
      if (err) {
        backend_error_init(err, BACKEND_ERROR_READ_ERROR, "Poll error");
      }
      audio_chunk_set_valid_frames(chunk, 0);
      return false;
    }
  }
#endif

  size_t sample_size = sample_format_bytes_per_sample(capture->format);
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

  audio_chunk_decode_interleaved(capture->raw_buf, capture->format,
                                 (size_t)capture->channels, frames_read, chunk);

  if (frames_read < frames) {
    size_t remaining_frames = frames - frames_read;
    double ratio =
        capture->resampling_ratio > 0.0 ? capture->resampling_ratio : 1.0;
    size_t missing = (size_t)((double)remaining_frames * ratio);
    size_t extra_left =
        capture->extra_samples > capture->extra_samples_generated
            ? capture->extra_samples - capture->extra_samples_generated
            : 0;
    size_t extra_to_generate = 0;
    if (extra_left > missing) {
      extra_to_generate = remaining_frames;
      capture->extra_samples_generated += missing;
    } else {
      extra_to_generate = (size_t)((double)extra_left / ratio);
      capture->extra_samples_generated = capture->extra_samples;
    }

    if (extra_to_generate > 0) {
      for (size_t c = 0; c < capture->channels; c++) {
        double* ch_data = audio_chunk_get_channel(chunk, c);
        if (ch_data) {
          memset(ch_data + frames_read, 0, extra_to_generate * sizeof(double));
        }
      }
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

/**
 * @brief Close the file capture device.
 *
 * @param ctx Pointer to the file_capture_t instance.
 */
static void file_capture_close(void* ctx) {
  file_capture_t* capture = (file_capture_t*)ctx;
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

/**
 * @brief Get any pending sample rate change.
 *
 * @param ctx Pointer to the file_capture_t instance.
 * @param out_rate Pointer to double to store the pending sample rate.
 * @return true if a rate change is pending, false otherwise.
 */
static bool file_capture_get_pending_rate_change(void* ctx, double* out_rate) {
  (void)ctx;
  (void)out_rate;
  return false;
}

/**
 * @brief Check if pitch control is supported by the file capture backend.
 *
 * @param ctx Pointer to the file_capture_t instance.
 * @return true if supported, false otherwise.
 */
static bool file_capture_pitch_control_supported(void* ctx) {
  (void)ctx;
  return false;
}

/**
 * @brief Set the pitch multiplier for the file capture backend.
 *
 * @param ctx Pointer to the file_capture_t instance.
 * @param multiplier The pitch multiplier.
 */
static void file_capture_set_pitch(void* ctx, double multiplier) {
  (void)ctx;
  (void)multiplier;
}

/**
 * @brief Wait for the file capture device to have data available.
 *
 * @param ctx Pointer to the file_capture_t instance.
 * @param timeout_ms Timeout in milliseconds.
 * @return true if data is available, false on timeout or error.
 */
static bool file_capture_wait(void* ctx, uint32_t timeout_ms) {
#ifdef CDSP_TEST
  file_capture_t* capture = (file_capture_t*)ctx;
  if (capture && capture->realtime) {
    cdsp_sleep_ms(timeout_ms);
  }
#else
  (void)ctx;
  (void)timeout_ms;
#endif
  return true;
}

/**
 * @brief Set the paused state of the file capture backend.
 *
 * @param ctx Pointer to the file_capture_t instance.
 * @param paused true to pause, false to resume.
 */
static void file_capture_set_is_paused(void* ctx, bool paused) {
  file_capture_t* capture = (file_capture_t*)ctx;
  if (capture) {
    atomic_store_explicit(&capture->is_paused, paused, memory_order_release);
  }
}

/**
 * @brief Stop the file capture device.
 *
 * @param ctx Pointer to the file_capture_t instance.
 */
static void file_capture_stop(void* ctx) { (void)ctx; }

/**
 * @brief Destroy the file capture backend instance.
 *
 * @param ctx Pointer to the file_capture_t instance to destroy.
 */
static void file_capture_destroy(void* ctx) {
  file_capture_t* capture = (file_capture_t*)ctx;
  if (!capture) return;
  file_capture_close(capture);
  free(capture);
}

/**
 * @brief Create a file capture backend instance.
 *
 * @param config Pointer to the capture device configuration.
 * @param sample_rate The sample rate in Hz.
 * @param chunk_size The size of each audio chunk in frames.
 * @param full_duplex True if running in full duplex mode.
 * @param params Pointer to processing parameters.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return Pointer to the created capture_backend_t instance, or NULL on
 * failure.
 */
static capture_backend_t* file_capture_create(
    const capture_device_config_t* config, int sample_rate, int chunk_size,
    bool full_duplex, processing_parameters_t* params, backend_error_t* err) {
  (void)full_duplex;
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
  capture->playback_sample_rate = sample_rate;
  capture->resampling_ratio = 1.0;
  capture->chunk_size = chunk_size;

  capture_backend_t* backend =
      (capture_backend_t*)calloc(1, sizeof(capture_backend_t));
  if (!backend) {
    free(capture);
    return NULL;
  }
  backend->ctx = capture;
  backend->vtable = &g_file_capture_vtable;
#ifdef CDSP_TEST
  backend->is_realtime = capture->realtime;
#else
  backend->is_realtime = false;
#endif
  return backend;
}

const capture_backend_vtable_t g_file_capture_vtable = {
    .create = file_capture_create,
    .open = file_capture_open,
    .read = file_capture_read,
    .close = file_capture_close,
    .get_pending_rate_change = file_capture_get_pending_rate_change,
    .is_pitch_control_supported = file_capture_pitch_control_supported,
    .set_pitch = file_capture_set_pitch,
    .wait_for_data = file_capture_wait,
    .set_is_paused = file_capture_set_is_paused,
    .stop = file_capture_stop,
    .destroy = file_capture_destroy};

// MARK: - File Playback Backend implementation

/**
 * @brief Open the file playback device.
 *
 * @param ctx Pointer to the file_playback_t instance.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return true if successful, false otherwise.
 */
static bool file_playback_open(void* ctx, backend_error_t* err) {
  file_playback_t* playback = (file_playback_t*)ctx;
  if (!playback) return false;
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

  size_t sample_size = sample_format_bytes_per_sample(playback->format);
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
  if (playback->raw_buf) {
    free(playback->raw_buf);
    playback->raw_buf = NULL;
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
    if (!playback->is_seekable && playback->use_rf64) {
      logger_warn(&g_logger,
                  "RF64 output requires a seekable file, writing a "
                  "streaming wav header instead");
      write_wav_header_to_file(playback->f, playback->channels,
                               playback->format, playback->sample_rate,
                               0xFFFFFFFF, false);
    } else {
      if (playback->use_rf64) {
        write_rf64_header_to_file(playback->f, playback->channels,
                                  playback->format, playback->sample_rate, 0);
      } else {
        write_wav_header_to_file(playback->f, playback->channels,
                                 playback->format, playback->sample_rate,
                                 0xFFFFFFFF, playback->is_seekable);
      }
    }
  } else {
    if (playback->use_rf64) {
      logger_warn(&g_logger,
                  "RF64 output requires a wav header, ignoring `use_rf64`");
    }
  }

  return true;
}

/**
 * @brief Write an audio chunk to the file playback device.
 *
 * @param ctx Pointer to the file_playback_t instance.
 * @param chunk Pointer to the audio chunk to write.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return true if successful, false otherwise.
 */
static bool file_playback_write(void* ctx, const audio_chunk_t* chunk,
                                backend_error_t* err) {
  file_playback_t* playback = (file_playback_t*)ctx;
  if (!playback) return false;
#ifdef CDSP_TEST
  if (atomic_load_explicit(&playback->stopped, memory_order_acquire)) {
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
  size_t sample_size = sample_format_bytes_per_sample(playback->format);

  size_t required_bytes = frames * playback->channels * sample_size;

  // Parity with CamillaDSP FilePlayback::write:
  // Standard RIFF WAV format headers use 32-bit unsigned integers for chunk
  // sizes, imposing a hard 4 GB (0xFFFFFFFF bytes) ceiling on total file size.
  // When use_rf64 is false and adding the next chunk would exceed this limit,
  // we stop writing to preserve a valid, well-formed WAV file header rather
  // than producing an invalid or truncated file. We return false with
  // BACKEND_ERROR_NONE so the engine playback loop treats this as clean
  // End-Of-Stream (EOS) instead of an I/O hardware failure (Ref:
  // engine_state_management.md §4.2).
  if (playback->is_wav && playback->is_seekable && !playback->use_rf64) {
    uint64_t max_bytes = 0xFFFFFFFFULL - 256;
    if (playback->total_bytes_written > max_bytes ||
        required_bytes > (max_bytes - playback->total_bytes_written)) {
      logger_warn(&g_logger,
                  "Wav file reached the maximum size of a plain wav file. "
                  "Stopping playback to avoid writing an invalid file.");
      if (err) {
        backend_error_init(err, BACKEND_ERROR_NONE,
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

  audio_chunk_encode_interleaved(chunk, playback->format,
                                 (size_t)playback->channels, frames,
                                 playback->raw_buf);

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

/**
 * @brief Close the file playback device.
 *
 * @param ctx Pointer to the file_playback_t instance.
 */
static void file_playback_close(void* ctx) {
  file_playback_t* playback = (file_playback_t*)ctx;
  if (!playback) return;
  if (playback->f) {
    if (playback->is_wav && playback->is_seekable && !playback->is_stdout) {
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

/**
 * @brief Get the current buffer level of the file playback backend.
 *
 * @param ctx Pointer to the file_playback_t instance.
 * @return The buffer level in samples.
 */
static size_t file_playback_get_buffer_level(void* ctx) {
  (void)ctx;
  return 0;
}

/**
 * @brief Get any pending sample rate change.
 *
 * @param ctx Pointer to the file_playback_t instance.
 * @param out_rate Pointer to double to store the pending sample rate.
 * @return true if a rate change is pending, false otherwise.
 */
static bool file_playback_get_pending_rate_change(void* ctx, double* out_rate) {
  (void)ctx;
  (void)out_rate;
  return false;
}

/**
 * @brief Prefill the file playback buffer with silence.
 *
 * @param ctx Pointer to the file_playback_t instance.
 * @param frames Number of frames of silence to prefill.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return true if successful, false otherwise.
 */
static bool file_playback_prefill_silence(void* ctx, size_t frames,
                                          backend_error_t* err) {
  (void)ctx;
  (void)frames;
  (void)err;
  return true;
}

/**
 * @brief Check if file playback is currently paused.
 *
 * @param ctx Pointer to the file_playback_t instance.
 * @return true if paused, false otherwise.
 */
static bool file_playback_get_is_paused(void* ctx) {
  (void)ctx;
  return false;
}

/**
 * @brief Set the paused state of the file playback backend.
 *
 * @param ctx Pointer to the file_playback_t instance.
 * @param paused true to pause, false to resume.
 */
static void file_playback_set_is_paused(void* ctx, bool paused) {
  (void)ctx;
  (void)paused;
}

/**
 * @brief Stop the file playback device.
 *
 * @param ctx Pointer to the file_playback_t instance.
 */
static void file_playback_stop(void* ctx) {
#ifdef CDSP_TEST
  file_playback_t* playback = (file_playback_t*)ctx;
  if (playback) {
    atomic_store_explicit(&playback->stopped, true, memory_order_release);
  }
#else
  (void)ctx;
#endif
}

/**
 * @brief Destroy the file playback backend instance.
 *
 * @param ctx Pointer to the file_playback_t instance to destroy.
 */
static void file_playback_destroy(void* ctx) {
  file_playback_t* playback = (file_playback_t*)ctx;
  if (!playback) return;
  file_playback_close(playback);
  free(playback);
}

/**
 * @brief Create a file playback backend instance.
 *
 * @param config Pointer to the playback device configuration.
 * @param sample_rate The sample rate in Hz.
 * @param chunk_size The size of each audio chunk in frames.
 * @param full_duplex True if running in full duplex mode.
 * @param params Pointer to processing parameters.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return Pointer to the created playback_backend_t instance, or NULL on
 * failure.
 */
static playback_backend_t* file_playback_create(
    const playback_device_config_t* config, int sample_rate, int chunk_size,
    bool full_duplex, processing_parameters_t* params, backend_error_t* err) {
  (void)sample_rate;
  (void)full_duplex;
  (void)params;
  (void)err;
  bool has_wav_header = false;
  binary_sample_format_t fmt = BINARY_SAMPLE_FORMAT_INVALID;
  if (config->type == AUDIO_BACKEND_TYPE_FILE) {
    has_wav_header = config->cfg.raw_file.wav_header;
    fmt = config->cfg.raw_file.format;
  } else if (config->type == AUDIO_BACKEND_TYPE_STDIN_OUT) {
    has_wav_header = config->cfg.stdout_out.wav_header;
    fmt = config->cfg.stdout_out.format;
  }
  if (has_wav_header && fmt == BINARY_SAMPLE_FORMAT_S24_4_RJ_LE) {
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
#ifdef CDSP_TEST
  atomic_init(&playback->stopped, false);
#endif

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
  backend->vtable = &g_file_playback_vtable;
  return backend;
}

const playback_backend_vtable_t g_file_playback_vtable = {
    .create = file_playback_create,
    .open = file_playback_open,
    .write = file_playback_write,
    .close = file_playback_close,
    .get_buffer_level = file_playback_get_buffer_level,
    .get_pending_rate_change = file_playback_get_pending_rate_change,
    .prefill_silence = file_playback_prefill_silence,
    .get_is_paused = file_playback_get_is_paused,
    .set_is_paused = file_playback_set_is_paused,
    .stop = file_playback_stop,
    .destroy = file_playback_destroy};
