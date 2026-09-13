#include "Wav/wav_writer.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "Audio/sample_conversion.h"
#include "Utils/cdsp_path.h"
#include "Wav/raw_writer.h"

bool wav_can_write_bytes(uint64_t total_written, size_t bytes_to_add,
                         bool is_wav, bool is_seekable, bool use_rf64) {
  if (is_wav && is_seekable && !use_rf64) {
    uint64_t max_bytes = 0xFFFFFFFFULL - 256;
    if (total_written > max_bytes ||
        bytes_to_add > (max_bytes - total_written)) {
      return false;
    }
  }
  return true;
}

bool wav_write_header(FILE* f, size_t channels, binary_sample_format_t format,
                      uint32_t sample_rate, uint32_t data_bytes,
                      bool is_seekable) {
  if (!f) return false;

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
    static const uint8_t guid_suffix[14] = {0x00, 0x00, 0x00, 0x00, 0x10,
                                            0x00, 0x80, 0x00, 0x00, 0xAA,
                                            0x00, 0x38, 0x9B, 0x71};
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
    cdsp_fseek64(f, 0, SEEK_SET);
  }
  return fwrite(header, 1, header_size, f) == header_size;
}

bool wav_write_rf64_header(FILE* f, size_t channels,
                           binary_sample_format_t format, uint32_t sample_rate,
                           uint64_t data_bytes) {
  if (!f) return false;

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
    static const uint8_t guid_suffix[14] = {0x00, 0x00, 0x00, 0x00, 0x10,
                                            0x00, 0x80, 0x00, 0x00, 0xAA,
                                            0x00, 0x38, 0x9B, 0x71};
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

  cdsp_fseek64(f, 0, SEEK_SET);
  return fwrite(header, 1, header_size, f) == header_size;
}

bool wav_update_header(FILE* f, size_t channels, binary_sample_format_t format,
                       uint32_t sample_rate, uint64_t total_bytes_written,
                       bool use_rf64) {
  if (use_rf64) {
    return wav_write_rf64_header(f, channels, format, sample_rate,
                                 total_bytes_written);
  } else {
    return wav_write_header(f, channels, format, sample_rate,
                            (uint32_t)total_bytes_written, true);
  }
}

bool wav_write_audio_chunk(FILE* f, const audio_chunk_t* chunk, size_t channels,
                           binary_sample_format_t format, bool is_wav,
                           bool is_seekable, bool use_rf64,
                           uint64_t* total_bytes_written, uint8_t** raw_buf,
                           size_t* raw_buf_capacity, bool* reached_4gb_limit,
                           char* err_msg, size_t err_msg_len) {
  if (reached_4gb_limit) *reached_4gb_limit = false;
  if (!f || !chunk || !total_bytes_written || !raw_buf || !raw_buf_capacity) {
    if (err_msg && err_msg_len > 0) {
      snprintf(err_msg, err_msg_len,
               "Invalid arguments to wav_write_audio_chunk");
    }
    return false;
  }

  size_t frames = audio_chunk_get_valid_frames(chunk);
  size_t sample_size = sample_format_bytes_per_sample(format);
  size_t required_bytes = frames * channels * sample_size;

  if (!wav_can_write_bytes(*total_bytes_written, required_bytes, is_wav,
                           is_seekable, use_rf64)) {
    if (reached_4gb_limit) *reached_4gb_limit = true;
    if (err_msg && err_msg_len > 0) {
      snprintf(err_msg, err_msg_len, "Plain WAV 4 GB limit reached");
    }
    return false;
  }

  return raw_write_audio_chunk(f, chunk, channels, format, total_bytes_written,
                               raw_buf, raw_buf_capacity, err_msg, err_msg_len);
}

bool wav_write_file(const char* path, const double* const* channel_data,
                    size_t channels, size_t frames, uint32_t sample_rate,
                    binary_sample_format_t format, bool use_rf64, char* err_msg,
                    size_t err_msg_len) {
  if (!path || !channel_data || channels == 0) {
    if (err_msg && err_msg_len > 0)
      snprintf(err_msg, err_msg_len, "Invalid arguments");
    return false;
  }
  size_t sample_size = sample_format_bytes_per_sample(format);
  if (sample_size == 0) {
    if (err_msg && err_msg_len > 0)
      snprintf(err_msg, err_msg_len, "Invalid sample format");
    return false;
  }

  FILE* f = cdsp_fopen(path, "wb");
  if (!f) {
    if (err_msg && err_msg_len > 0) {
      snprintf(err_msg, err_msg_len, "Could not open file '%s' for writing: %s",
               path, strerror(errno));
    }
    return false;
  }

  uint64_t total_payload_bytes =
      (uint64_t)frames * (uint64_t)channels * (uint64_t)sample_size;
  bool ok = false;
  if (use_rf64 || total_payload_bytes >= 0xFFFFFFFFULL - 256) {
    ok = wav_write_rf64_header(f, channels, format, sample_rate,
                               total_payload_bytes);
  } else {
    ok = wav_write_header(f, channels, format, sample_rate,
                          (uint32_t)total_payload_bytes, false);
  }
  if (!ok) {
    if (err_msg && err_msg_len > 0)
      snprintf(err_msg, err_msg_len, "Failed to write WAV header");
    fclose(f);
    return false;
  }

  bool write_ok = raw_write_interleaved_stream(
      f, channel_data, channels, frames, format, err_msg, err_msg_len);
  fclose(f);
  return write_ok;
}

bool wav_write_channel_samples(const char* path, const double* samples,
                               size_t num_samples, uint32_t sample_rate,
                               binary_sample_format_t format, char* err_msg,
                               size_t err_msg_len) {
  const double* channel_ptrs[1] = {samples};
  return wav_write_file(path, channel_ptrs, 1, num_samples, sample_rate, format,
                        false, err_msg, err_msg_len);
}
