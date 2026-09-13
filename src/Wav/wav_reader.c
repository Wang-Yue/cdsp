#include "Wav/wav_reader.h"

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "Audio/sample_conversion.h"
#include "Utils/cdsp_path.h"
#include "Wav/raw_reader.h"

static void set_error(char* err_msg, size_t err_msg_len, const char* fmt, ...) {
  if (err_msg && err_msg_len > 0) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(err_msg, err_msg_len, fmt, args);
    va_end(args);
  }
}

bool wav_read_header(FILE* f, wav_info_t* info, char* err_msg,
                     size_t err_msg_len) {
  if (!f || !info) {
    set_error(err_msg, err_msg_len, "Invalid arguments to wav_read_header");
    return false;
  }
  if (err_msg && err_msg_len > 0) {
    err_msg[0] = '\0';
  }

  memset(info, 0, sizeof(*info));
  info->format = BINARY_SAMPLE_FORMAT_INVALID;

  uint8_t header[12];
  if (fread(header, 1, 12, f) != 12) {
    set_error(err_msg, err_msg_len, "Failed to read WAV header");
    return false;
  }

  bool is_rf64 = false;
  if (memcmp(header, "RF64", 4) == 0 || memcmp(header, "BW64", 4) == 0) {
    is_rf64 = true;
  } else if (memcmp(header, "RIFF", 4) != 0) {
    set_error(err_msg, err_msg_len, "Not a RIFF, RF64 or BW64 file");
    return false;
  }

  if (memcmp(header + 8, "WAVE", 4) != 0) {
    set_error(err_msg, err_msg_len, "Not a WAVE file");
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
  uint16_t audio_format = 0;
  uint16_t bits_per_sample = 0;
  uint16_t block_align = 0;
  uint16_t valid_bits = 0;
  size_t container_bytes = 0;

  uint8_t chunk_id[4];
  while (fread(chunk_id, 1, 4, f) == 4) {
    uint8_t size_bytes[4];
    if (fread(size_bytes, 1, 4, f) != 4) {
      break;
    }
    uint32_t chunk_size =
        (uint32_t)size_bytes[0] | ((uint32_t)size_bytes[1] << 8) |
        ((uint32_t)size_bytes[2] << 16) | ((uint32_t)size_bytes[3] << 24);

    if (memcmp(chunk_id, "ds64", 4) == 0) {
      if (chunk_size < 24) {
        uint32_t skip = (chunk_size + 1) & ~1;
        if (cdsp_fseek64(f, skip, SEEK_CUR) != 0) {
          set_error(err_msg, err_msg_len,
                    "Failed to seek past short ds64 chunk");
          return false;
        }
        continue;
      }
      uint8_t ds64_payload[24];
      if (fread(ds64_payload, 1, 24, f) != 24) {
        set_error(err_msg, err_msg_len, "Failed to read ds64 chunk payload");
        return false;
      }
      rf64_data_size = (uint64_t)ds64_payload[8] |
                       ((uint64_t)ds64_payload[9] << 8) |
                       ((uint64_t)ds64_payload[10] << 16) |
                       ((uint64_t)ds64_payload[11] << 24) |
                       ((uint64_t)ds64_payload[12] << 32) |
                       ((uint64_t)ds64_payload[13] << 40) |
                       ((uint64_t)ds64_payload[14] << 48) |
                       ((uint64_t)ds64_payload[15] << 56);

      if (chunk_size > 24) {
        uint32_t remaining = chunk_size - 24;
        uint32_t pad = (chunk_size & 1);
        if (cdsp_fseek64(f, remaining + pad, SEEK_CUR) != 0) {
          set_error(err_msg, err_msg_len, "Failed to seek past ds64 chunk");
          return false;
        }
      }
    } else if (memcmp(chunk_id, "fmt ", 4) == 0) {
      found_fmt = true;
      if (chunk_size < 16) {
        set_error(err_msg, err_msg_len,
                  "Invalid fmt chunk size %u (must be at least 16)",
                  chunk_size);
        return false;
      }
      uint8_t fmt_payload[40];
      size_t to_read = chunk_size < 40 ? chunk_size : 40;
      if (fread(fmt_payload, 1, to_read, f) != to_read) {
        set_error(err_msg, err_msg_len, "Failed to read fmt chunk payload");
        return false;
      }
      audio_format = fmt_payload[0] | (fmt_payload[1] << 8);
      channels = fmt_payload[2] | (fmt_payload[3] << 8);
      sample_rate = fmt_payload[4] | (fmt_payload[5] << 8) |
                    (fmt_payload[6] << 16) | (fmt_payload[7] << 24);
      block_align = fmt_payload[12] | (fmt_payload[13] << 8);
      bits_per_sample = fmt_payload[14] | (fmt_payload[15] << 8);
      valid_bits = bits_per_sample;

      if (audio_format != 1 && audio_format != 3 && audio_format != 0xFFFE) {
        set_error(err_msg, err_msg_len,
                  "Unsupported WAV format code %d (only PCM/Float supported)",
                  audio_format);
        return false;
      }

      bool is_extended = (audio_format == 0xFFFE);
      if (is_extended) {
        if (chunk_size < 40) {
          set_error(err_msg, err_msg_len,
                    "extended fmt chunk must be at least 40 bytes, got %u",
                    chunk_size);
          return false;
        }
        static const uint8_t guid_suffix[14] = {0x00, 0x00, 0x00, 0x00, 0x10,
                                                0x00, 0x80, 0x00, 0x00, 0xAA,
                                                0x00, 0x38, 0x9B, 0x71};
        if (memcmp(&fmt_payload[26], guid_suffix, 14) != 0) {
          set_error(err_msg, err_msg_len,
                    "Unsupported sub-format GUID in EXTENSIBLE");
          return false;
        }
        uint16_t sub_format = fmt_payload[24] | (fmt_payload[25] << 8);
        if (sub_format == 1) {
          audio_format = 1;
        } else if (sub_format == 3) {
          audio_format = 3;
        } else {
          set_error(err_msg, err_msg_len,
                    "Unsupported sub-format %d in EXTENSIBLE", sub_format);
          return false;
        }
        uint16_t v = fmt_payload[18] | (fmt_payload[19] << 8);
        if (v > 0) valid_bits = v;
      }

      if (channels == 0) {
        set_error(err_msg, err_msg_len, "Invalid channel count 0 in fmt chunk");
        return false;
      }
      if (block_align == 0 || (block_align % channels) != 0) {
        set_error(err_msg, err_msg_len,
                  "Invalid block align %d for %d channels", block_align,
                  channels);
        return false;
      }
      container_bytes = block_align / channels;
      uint16_t bytes_per_sample = (uint16_t)container_bytes;
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
        bool is_u8 =
            (audio_format == 1 && bits_per_sample == 8 && container_bytes == 1);
        if (!is_u8) {
          set_error(err_msg, err_msg_len,
                    "Unsupported WAV sample format: format %d, %d bits in %d "
                    "bytes per sample",
                    audio_format, bits_per_sample, bytes_per_sample);
          return false;
        }
      }

      if (chunk_size > to_read) {
        uint32_t pad = chunk_size & 1;
        if (cdsp_fseek64(f, (int64_t)(chunk_size - to_read) + pad, SEEK_CUR) !=
            0) {
          set_error(err_msg, err_msg_len, "Failed to seek past fmt chunk");
          return false;
        }
      } else if ((chunk_size & 1) != 0) {
        if (cdsp_fseek64(f, 1, SEEK_CUR) != 0) {
          set_error(err_msg, err_msg_len, "Failed to seek past fmt padding");
          return false;
        }
      }

      if (found_data) {
        break;
      }
    } else if (memcmp(chunk_id, "data", 4) == 0) {
      if (!found_data) {
        found_data = true;
        data_start_offset = (uint64_t)cdsp_ftell64(f);
        if (is_rf64 && chunk_size == 0xFFFFFFFF) {
          data_bytes = rf64_data_size;
        } else if (chunk_size == 0xFFFFFFFF) {
          // Plain RIFF streaming placeholder: determine file length if seekable
          int64_t cur_pos = cdsp_ftell64(f);
          int64_t file_size = -1;
          if (cur_pos >= 0 && cdsp_fseek64(f, 0, SEEK_END) == 0) {
            file_size = cdsp_ftell64(f);
            cdsp_fseek64(f, cur_pos, SEEK_SET);
          }
          if (file_size > cur_pos) {
            data_bytes = (uint64_t)(file_size - cur_pos);
          } else {
            data_bytes = 0xFFFFFFFF;
          }
        } else {
          data_bytes = chunk_size;
        }
      }
      if (found_fmt) {
        break;
      }
      // Plain RIFF streaming placeholder: data runs to EOF, cannot have fmt
      // after.
      if (!is_rf64 && chunk_size == 0xFFFFFFFF) {
        break;
      }
      uint64_t skip_len =
          (chunk_size == 0xFFFFFFFF) ? rf64_data_size : (uint64_t)chunk_size;
      uint64_t skip_padded = (skip_len + 1) & ~1ULL;
      if (cdsp_fseek64(f, (int64_t)data_start_offset + (int64_t)skip_padded,
                       SEEK_SET) != 0) {
        break;
      }
    } else {
      uint32_t pad = chunk_size & 1;
      if (cdsp_fseek64(f, (int64_t)chunk_size + pad, SEEK_CUR) != 0) {
        set_error(err_msg, err_msg_len, "Failed to seek past unknown chunk");
        return false;
      }
    }
  }

  if (!found_fmt) {
    set_error(err_msg, err_msg_len, "Missing 'fmt ' chunk");
    return false;
  }
  if (!found_data) {
    set_error(err_msg, err_msg_len, "Missing 'data' chunk");
    return false;
  }

  info->sample_rate = sample_rate;
  info->channels = channels;
  info->format = format;
  info->data_bytes = data_bytes;
  info->data_start_offset = data_start_offset;
  info->is_rf64 = is_rf64;
  info->audio_format = audio_format;
  info->bits_per_sample = bits_per_sample;
  info->block_align = block_align;
  info->valid_bits = valid_bits;
  info->container_bytes = container_bytes;

  cdsp_fseek64(f, (int64_t)data_start_offset, SEEK_SET);
  return true;
}

bool wav_read_info_from_file(const char* filename, wav_info_t* info,
                             char* err_msg, size_t err_msg_len) {
  if (!filename || !info) {
    set_error(err_msg, err_msg_len, "Invalid arguments");
    return false;
  }
  FILE* f = cdsp_fopen(filename, "rb");
  if (!f) {
    set_error(err_msg, err_msg_len, "Could not open WAV file '%s': %s",
              filename, strerror(errno));
    return false;
  }
  bool ok = wav_read_header(f, info, err_msg, err_msg_len);
  fclose(f);
  return ok;
}

bool cdsp_wav_file_read_info(const char* filename, cdsp_wav_info_t* info,
                             char* err_msg, size_t err_msg_len) {
  return wav_read_info_from_file(filename, info, err_msg, err_msg_len);
}

double* wav_read_channel_samples(const char* path, int channel,
                                 size_t* out_count, char* err_msg,
                                 size_t err_msg_len) {
  if (out_count) *out_count = 0;
  if (err_msg && err_msg_len > 0) err_msg[0] = '\0';

  if (channel < 0) {
    set_error(err_msg, err_msg_len,
              "Conv channel must be non-negative (got %d)", channel);
    return NULL;
  }

  FILE* f = cdsp_fopen(path, "rb");
  if (!f) {
    set_error(err_msg, err_msg_len, "Could not open WAV file '%s'", path);
    return NULL;
  }

  wav_info_t info;
  char header_err[256] = {0};
  if (!wav_read_header(f, &info, header_err, sizeof(header_err))) {
    set_error(err_msg, err_msg_len, "Unable to parse wav file '%s', %s", path,
              header_err);
    fclose(f);
    return NULL;
  }

  if (info.channels == 0) {
    set_error(err_msg, err_msg_len, "WAV file '%s' has 0 channels", path);
    fclose(f);
    return NULL;
  }

  if (channel >= (int)info.channels) {
    set_error(err_msg, err_msg_len,
              "Cant read channel %d of file '%s' which contains %u channels.",
              channel, path, (unsigned int)info.channels);
    fclose(f);
    return NULL;
  }

  bool is_u8 = (info.audio_format == 1 && info.bits_per_sample == 8 &&
                info.container_bytes == 1);
  if (info.format == BINARY_SAMPLE_FORMAT_INVALID && !is_u8) {
    set_error(err_msg, err_msg_len,
              "Unsupported wav format in '%s' (audio_format=%u, bits=%u, "
              "container_bytes=%zu, valid_bits=%u)",
              path, info.audio_format, info.bits_per_sample,
              info.container_bytes, info.valid_bits);
    fclose(f);
    return NULL;
  }

  size_t bytes_per_frame = (size_t)info.channels * info.container_bytes;
  if (bytes_per_frame == 0) {
    set_error(err_msg, err_msg_len, "Invalid frame byte size in '%s'", path);
    fclose(f);
    return NULL;
  }

  size_t num_frames = (size_t)(info.data_bytes / bytes_per_frame);
  if (num_frames == 0) {
    set_error(err_msg, err_msg_len, "WAV file '%s' has 0 audio frames", path);
    fclose(f);
    return NULL;
  }

  if (cdsp_fseek64(f, (int64_t)info.data_start_offset, SEEK_SET) != 0) {
    set_error(err_msg, err_msg_len,
              "Unable to seek to audio data in WAV file '%s'", path);
    fclose(f);
    return NULL;
  }

  double* result = raw_read_channel_stream(
      f, channel, (size_t)info.channels, info.container_bytes, info.format,
      is_u8, num_frames, out_count, err_msg, err_msg_len);
  fclose(f);

  if (!result && (!err_msg || err_msg[0] == '\0')) {
    set_error(err_msg, err_msg_len,
              "No usable samples decoded from WAV file '%s'", path);
  }
  return result;
}
