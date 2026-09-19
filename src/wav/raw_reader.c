#include "wav/raw_reader.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "audio/sample_conversion.h"
#include "config/config_gen.h"
#include "utils/cdsp_path.h"
#include "wav/wav_types.h"

static void set_error(char *err_buf, size_t err_len, const char *fmt, ...) {
  if (err_buf && err_len > 0) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(err_buf, err_len, fmt, args);
    va_end(args);
  }
}

char *raw_read_dynamic_line(FILE *f) {
  size_t cap = 128;
  size_t len = 0;
  char *buf = (char *)malloc(cap);
  if (!buf)
    return NULL;
  int c;
  while ((c = fgetc(f)) != EOF) {
    if (len + 2 >= cap) {
      size_t new_cap = cap * 2;
      char *new_buf = (char *)realloc(buf, new_cap);
      if (!new_buf) {
        free(buf);
        return NULL;
      }
      buf = new_buf;
      cap = new_cap;
    }
    if (c == '\n') {
      break;
    }
    if (c != '\r') {
      buf[len++] = (char)c;
    }
  }
  if (len == 0 && c == EOF) {
    free(buf);
    return NULL;
  }
  buf[len] = '\0';
  return buf;
}

double *raw_read_text_samples(const char *path, size_t skip_lines,
                              size_t read_lines, size_t *out_count,
                              char *err_buf, size_t err_len) {
  if (err_buf && err_len > 0)
    err_buf[0] = '\0';
  if (!out_count)
    return NULL;
  *out_count = 0;

  FILE *f = cdsp_fopen(path, "r");
  if (!f) {
    set_error(err_buf, err_len, "Could not open coefficient file '%s'", path);
    return NULL;
  }

  for (size_t i = 0; i < skip_lines; i++) {
    char *line = raw_read_dynamic_line(f);
    if (!line) {
      break;
    }
    free(line);
  }

  size_t cap = 1024;
  double *result = (double *)calloc(cap, sizeof(double));
  if (!result) {
    fclose(f);
    return NULL;
  }

  size_t count = 0;
  size_t lines_read = 0;
  while (read_lines == 0 || lines_read < read_lines) {
    char *line = raw_read_dynamic_line(f);
    if (!line)
      break;
    lines_read++;
    size_t line_nbr = skip_lines + lines_read;

    char *p = line;
    while (*p != '\0' && isspace((unsigned char)*p))
      p++;
    size_t len = strlen(p);
    while (len > 0 && isspace((unsigned char)p[len - 1])) {
      p[--len] = '\0';
    }

    if (len == 0) {
      set_error(
          err_buf, err_len,
          "Can't parse value on line %zu of file '%s'. Reason: empty line",
          line_nbr, path);
      free(line);
      free(result);
      fclose(f);
      return NULL;
    }

    const char *check = p;
    if (*check == '+' || *check == '-')
      check++;
    if (check[0] == '0' && (check[1] == 'x' || check[1] == 'X')) {
      set_error(err_buf, err_len,
                "Can't parse value on line %zu of file '%s'. Reason: hex "
                "float not supported",
                line_nbr, path);
      free(line);
      free(result);
      fclose(f);
      return NULL;
    }

    char *endptr = NULL;
    double val = strtod(p, &endptr);
    if (endptr == p || *endptr != '\0') {
      set_error(err_buf, err_len,
                "Can't parse value on line %zu of file '%s'. Reason: "
                "invalid float '%s'",
                line_nbr, path, p);
      free(line);
      free(result);
      fclose(f);
      return NULL;
    }
    free(line);

    if (count >= cap) {
      cap *= 2;
      double *new_res = (double *)realloc(result, cap * sizeof(double));
      if (!new_res) {
        free(result);
        fclose(f);
        return NULL;
      }
      result = new_res;
    }
    result[count++] = val;
  }

  fclose(f);
  *out_count = count;
  return result;
}

double raw_decode_sample(const uint8_t *src, binary_sample_format_t format,
                         bool is_u8) {
  if (is_u8) {
    return ((double)src[0] - 128.0) / 128.0;
  }
  switch (format) {
  case BINARY_SAMPLE_FORMAT_S16_LE:
    return pcm_sample_decode_s16_bytes(src);
  case BINARY_SAMPLE_FORMAT_S24_3_LE:
    return pcm_sample_decode_s24_3bytes(src);
  case BINARY_SAMPLE_FORMAT_S24_4_RJ_LE:
    return pcm_sample_decode_s24_4_rj_bytes(src);
  case BINARY_SAMPLE_FORMAT_S24_4_LJ_LE:
    return pcm_sample_decode_s24_4_lj_bytes(src);
  case BINARY_SAMPLE_FORMAT_S32_LE:
    return pcm_sample_decode_s32_bytes(src);
  case BINARY_SAMPLE_FORMAT_F32_LE:
    return pcm_sample_decode_f32_bytes(src);
  case BINARY_SAMPLE_FORMAT_F64_LE:
    return pcm_sample_decode_f64_bytes(src);
  case BINARY_SAMPLE_FORMAT_DSD_U8:
    return pcm_sample_decode_dsd_u8(src[0]);
  case BINARY_SAMPLE_FORMAT_DSD_U16_LE:
    return pcm_sample_decode_dsd_u16_le_bytes(src);
  case BINARY_SAMPLE_FORMAT_DSD_U16_BE:
    return pcm_sample_decode_dsd_u16_be_bytes(src);
  case BINARY_SAMPLE_FORMAT_DSD_U32_LE:
    return pcm_sample_decode_dsd_u32_le_bytes(src);
  case BINARY_SAMPLE_FORMAT_DSD_U32_BE:
    return pcm_sample_decode_dsd_u32_be_bytes(src);
  case BINARY_SAMPLE_FORMAT_DSD_U32_REVERSED:
    return pcm_sample_decode_dsd_u32_reversed_bytes(src);
  case BINARY_SAMPLE_FORMAT_INVALID:
    CDSP_UNREACHABLE();
    return 0.0;
  }
  CDSP_UNREACHABLE();
  return 0.0;
}

double *raw_read_channel_stream(FILE *f, int channel, size_t channels,
                                size_t container_bytes,
                                binary_sample_format_t format, bool is_u8,
                                size_t num_frames, size_t *out_count,
                                char *err_buf, size_t err_len) {
  if (out_count)
    *out_count = 0;
  if (err_buf && err_len > 0)
    err_buf[0] = '\0';
  if (!f || channels == 0 || container_bytes == 0 || channel < 0 ||
      (size_t)channel >= channels) {
    set_error(err_buf, err_len, "Invalid stream parameters for audio reading");
    return NULL;
  }
  if (num_frames == 0) {
    set_error(err_buf, err_len, "Requested 0 frames to read");
    return NULL;
  }

  size_t bytes_per_frame = channels * container_bytes;
  uint8_t *frame_buf = (uint8_t *)malloc(bytes_per_frame);
  if (!frame_buf) {
    set_error(err_buf, err_len, "Out of memory allocating frame buffer");
    return NULL;
  }

  double *result = (double *)calloc(num_frames, sizeof(double));
  if (!result) {
    set_error(err_buf, err_len, "Out of memory allocating sample buffer");
    free(frame_buf);
    return NULL;
  }

  size_t channel_offset = (size_t)channel * container_bytes;
  size_t read_frames = 0;
  for (size_t i = 0; i < num_frames; i++) {
    if (fread(frame_buf, 1, bytes_per_frame, f) != bytes_per_frame) {
      break;
    }
    result[read_frames++] =
        raw_decode_sample(frame_buf + channel_offset, format, is_u8);
  }

  free(frame_buf);

  if (read_frames == 0) {
    set_error(err_buf, err_len, "No usable samples decoded from stream");
    free(result);
    return NULL;
  }

  if (out_count)
    *out_count = read_frames;
  return result;
}

double *raw_read_binary_samples(const char *path, binary_sample_format_t format,
                                size_t skip_bytes, size_t read_bytes,
                                size_t *out_count, char *err_buf,
                                size_t err_len) {
  if (err_buf && err_len > 0)
    err_buf[0] = '\0';
  if (!out_count)
    return NULL;
  *out_count = 0;

  if (format == BINARY_SAMPLE_FORMAT_INVALID) {
    set_error(err_buf, err_len, "Invalid binary sample format for file '%s'",
              path);
    return NULL;
  }

  size_t sample_size = sample_format_bytes_per_sample(format);
  if (sample_size == 0) {
    set_error(err_buf, err_len, "Invalid sample size for format in file '%s'",
              path);
    return NULL;
  }

  FILE *f = cdsp_fopen(path, "rb");
  if (!f) {
    set_error(err_buf, err_len, "Could not open coefficient file '%s'", path);
    return NULL;
  }

  cdsp_fseek64(f, 0, SEEK_END);
  int64_t total_file_size = cdsp_ftell64(f);
  if (total_file_size < 0 ||
      (uint64_t)total_file_size <= (uint64_t)skip_bytes) {
    set_error(err_buf, err_len,
              "Coefficient file '%s' is empty or skip offset (%zu) exceeds "
              "file size (%lld)",
              path, skip_bytes, (long long)total_file_size);
    fclose(f);
    return NULL;
  }

  size_t file_size = (size_t)total_file_size - skip_bytes;
  if (cdsp_fseek64(f, (int64_t)skip_bytes, SEEK_SET) != 0) {
    set_error(err_buf, err_len, "Could not seek in coefficient file '%s'",
              path);
    fclose(f);
    return NULL;
  }

  size_t max_read = file_size;
  if (read_bytes > 0) {
    size_t requested_samples = read_bytes / sample_size;
    if (requested_samples > 0) {
      size_t requested_bytes = requested_samples * sample_size;
      if (requested_bytes < file_size) {
        max_read = requested_bytes;
      }
    }
  }

  size_t num_samples = max_read / sample_size;
  if (num_samples == 0) {
    set_error(err_buf, err_len, "No coefficients could be read from '%s'",
              path);
    fclose(f);
    return NULL;
  }

  double *result =
      raw_read_channel_stream(f, 0, 1, sample_size, format, false, num_samples,
                              out_count, err_buf, err_len);
  fclose(f);

  if (!result && (!err_buf || err_buf[0] == '\0')) {
    set_error(err_buf, err_len, "No coefficients could be read from '%s'",
              path);
  }
  return result;
}

double *raw_read_samples(const char *path, const char *format_str,
                         size_t skip_bytes_lines, size_t read_bytes_lines,
                         size_t *out_count, char *err_buf, size_t err_len) {
  if (err_buf && err_len > 0)
    err_buf[0] = '\0';
  if (!path || !format_str) {
    set_error(err_buf, err_len, "Invalid arguments to raw_read_samples");
    return NULL;
  }

  if (strcmp(format_str, "TEXT") == 0) {
    return raw_read_text_samples(path, skip_bytes_lines, read_bytes_lines,
                                 out_count, err_buf, err_len);
  }

  binary_sample_format_t format = file_sample_format_from_string(format_str);
  if (format == BINARY_SAMPLE_FORMAT_INVALID) {
    set_error(err_buf, err_len,
              "Unsupported sample format '%s' for raw file '%s'", format_str,
              path);
    return NULL;
  }

  return raw_read_binary_samples(path, format, skip_bytes_lines,
                                 read_bytes_lines, out_count, err_buf, err_len);
}
