#include "wav/raw_writer.h"

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "audio/sample_conversion.h"
#include "config/config_gen.h"
#include "utils/cdsp_path.h"

static void set_error(char *err_buf, size_t err_len, const char *fmt, ...) {
  if (err_buf && err_len > 0) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(err_buf, err_len, fmt, args);
    va_end(args);
  }
}

void raw_encode_sample(double val, uint8_t *dst,
                       binary_sample_format_t format) {
  switch (format) {
  case BINARY_SAMPLE_FORMAT_S16_LE:
    pcm_sample_encode_s16_bytes(val, dst);
    break;
  case BINARY_SAMPLE_FORMAT_S24_3_LE:
    pcm_sample_encode_s24_3bytes(val, dst);
    break;
  case BINARY_SAMPLE_FORMAT_S24_4_RJ_LE:
    pcm_sample_encode_s24_4_rj_bytes(val, dst);
    break;
  case BINARY_SAMPLE_FORMAT_S24_4_LJ_LE:
    pcm_sample_encode_s24_4_lj_bytes(val, dst);
    break;
  case BINARY_SAMPLE_FORMAT_S32_LE:
    pcm_sample_encode_s32_bytes(val, dst);
    break;
  case BINARY_SAMPLE_FORMAT_F32_LE:
    pcm_sample_encode_f32_bytes(val, dst);
    break;
  case BINARY_SAMPLE_FORMAT_F64_LE:
    pcm_sample_encode_f64_bytes(val, dst);
    break;
  case BINARY_SAMPLE_FORMAT_DSD_U8:
    dst[0] = pcm_sample_encode_dsd_u8(val);
    break;
  case BINARY_SAMPLE_FORMAT_DSD_U16_LE:
    pcm_sample_encode_dsd_u16_le_bytes(val, dst);
    break;
  case BINARY_SAMPLE_FORMAT_DSD_U16_BE:
    pcm_sample_encode_dsd_u16_be_bytes(val, dst);
    break;
  case BINARY_SAMPLE_FORMAT_DSD_U32_LE:
    pcm_sample_encode_dsd_u32_le_bytes(val, dst);
    break;
  case BINARY_SAMPLE_FORMAT_DSD_U32_BE:
    pcm_sample_encode_dsd_u32_be_bytes(val, dst);
    break;
  case BINARY_SAMPLE_FORMAT_DSD_U32_REVERSED:
    pcm_sample_encode_dsd_u32_reversed_bytes(val, dst);
    break;
  case BINARY_SAMPLE_FORMAT_INVALID:
    CDSP_UNREACHABLE();
    break;
  }
}

bool raw_write_interleaved_stream(FILE *f, const double *const *channel_data,
                                  size_t channels, size_t frames,
                                  binary_sample_format_t format, char *err_msg,
                                  size_t err_msg_len) {
  if (err_msg && err_msg_len > 0)
    err_msg[0] = '\0';
  if (!f || !channel_data || channels == 0) {
    set_error(err_msg, err_msg_len,
              "Invalid arguments to raw_write_interleaved_stream");
    return false;
  }
  size_t sample_size = sample_format_bytes_per_sample(format);
  if (sample_size == 0) {
    set_error(err_msg, err_msg_len,
              "Invalid binary sample format for stream write");
    return false;
  }

  size_t block_frames = 4096;
  size_t frame_bytes = channels * sample_size;
  uint8_t *block_buf = (uint8_t *)malloc(block_frames * frame_bytes);
  if (!block_buf) {
    set_error(err_msg, err_msg_len,
              "Memory allocation failure in raw_write_interleaved_stream");
    return false;
  }

  size_t remaining = frames;
  size_t frame_offset = 0;
  while (remaining > 0) {
    size_t chunk = remaining < block_frames ? remaining : block_frames;
    uint8_t *p = block_buf;
    for (size_t i = 0; i < chunk; i++) {
      size_t f_idx = frame_offset + i;
      for (size_t ch = 0; ch < channels; ch++) {
        double val = channel_data[ch] ? channel_data[ch][f_idx] : 0.0;
        raw_encode_sample(val, p, format);
        p += sample_size;
      }
    }

    size_t bytes_to_write = chunk * frame_bytes;
    if (fwrite(block_buf, 1, bytes_to_write, f) != bytes_to_write) {
      set_error(err_msg, err_msg_len,
                "Error writing audio frames to stream: %s", strerror(errno));
      free(block_buf);
      return false;
    }
    frame_offset += chunk;
    remaining -= chunk;
  }

  free(block_buf);
  return true;
}

bool raw_write_audio_chunk(FILE *f, const audio_chunk_t *chunk, size_t channels,
                           binary_sample_format_t format,
                           uint64_t *total_bytes_written, uint8_t **raw_buf,
                           size_t *raw_buf_capacity, char *err_msg,
                           size_t err_msg_len) {
  if (!f || !chunk || !total_bytes_written || !raw_buf || !raw_buf_capacity) {
    set_error(err_msg, err_msg_len,
              "Invalid arguments to raw_write_audio_chunk");
    return false;
  }
  if (audio_chunk_get_channels(chunk) < channels) {
    set_error(err_msg, err_msg_len,
              "Chunk channels count does not match playback channels");
    return false;
  }

  size_t frames = audio_chunk_get_valid_frames(chunk);
  size_t sample_size = sample_format_bytes_per_sample(format);
  size_t required_bytes = frames * channels * sample_size;

  if (required_bytes > *raw_buf_capacity) {
    uint8_t *new_buf = (uint8_t *)realloc(*raw_buf, required_bytes);
    if (!new_buf) {
      set_error(err_msg, err_msg_len, "Failed to reallocate write buffer");
      return false;
    }
    *raw_buf = new_buf;
    *raw_buf_capacity = required_bytes;
  }

  audio_chunk_encode_interleaved(chunk, format, channels, frames, *raw_buf);

  size_t bytes_written = fwrite(*raw_buf, 1, required_bytes, f);
  *total_bytes_written += bytes_written;

  if (bytes_written != required_bytes) {
    set_error(err_msg, err_msg_len, "Write error (%zu of %zu bytes written)",
              bytes_written, required_bytes);
    return false;
  }
  return true;
}

bool raw_write_interleaved_file(const char *path,
                                const double *const *channel_data,
                                size_t channels, size_t frames,
                                binary_sample_format_t format, char *err_msg,
                                size_t err_msg_len) {
  if (err_msg && err_msg_len > 0)
    err_msg[0] = '\0';
  if (!path || !channel_data || channels == 0) {
    set_error(err_msg, err_msg_len,
              "Invalid arguments to raw_write_interleaved_file");
    return false;
  }

  FILE *f = cdsp_fopen(path, "wb");
  if (!f) {
    set_error(err_msg, err_msg_len, "Could not open file '%s' for writing: %s",
              path, strerror(errno));
    return false;
  }

  bool ok = raw_write_interleaved_stream(f, channel_data, channels, frames,
                                         format, err_msg, err_msg_len);
  fclose(f);
  return ok;
}

bool raw_write_samples(const char *path, const double *samples,
                       size_t num_samples, binary_sample_format_t format,
                       char *err_msg, size_t err_msg_len) {
  const double *channel_ptrs[1] = {samples};
  return raw_write_interleaved_file(path, channel_ptrs, 1, num_samples, format,
                                    err_msg, err_msg_len);
}

bool raw_write_text_samples(const char *path, const double *samples,
                            size_t num_samples, char *err_msg,
                            size_t err_msg_len) {
  if (err_msg && err_msg_len > 0)
    err_msg[0] = '\0';
  if (!path || (!samples && num_samples > 0)) {
    set_error(err_msg, err_msg_len,
              "Invalid arguments to raw_write_text_samples");
    return false;
  }

  FILE *f = cdsp_fopen(path, "w");
  if (!f) {
    set_error(err_msg, err_msg_len,
              "Could not open text file '%s' for writing: %s", path,
              strerror(errno));
    return false;
  }

  for (size_t i = 0; i < num_samples; i++) {
    if (fprintf(f, "%.17g\n", samples[i]) < 0) {
      set_error(err_msg, err_msg_len, "Failed writing sample %zu to '%s': %s",
                i, path, strerror(errno));
      fclose(f);
      return false;
    }
  }

  fclose(f);
  return true;
}

bool raw_write_file(const char *path, const char *format_str,
                    const double *samples, size_t num_samples, char *err_msg,
                    size_t err_msg_len) {
  if (err_msg && err_msg_len > 0)
    err_msg[0] = '\0';
  if (!path || !format_str) {
    set_error(err_msg, err_msg_len, "Invalid arguments to raw_write_file");
    return false;
  }

  if (strcmp(format_str, "TEXT") == 0) {
    return raw_write_text_samples(path, samples, num_samples, err_msg,
                                  err_msg_len);
  }

  binary_sample_format_t format = file_sample_format_from_string(format_str);
  if (format == BINARY_SAMPLE_FORMAT_INVALID) {
    set_error(err_msg, err_msg_len,
              "Unsupported sample format '%s' for raw write to '%s'",
              format_str, path);
    return false;
  }

  return raw_write_samples(path, samples, num_samples, format, err_msg,
                           err_msg_len);
}
