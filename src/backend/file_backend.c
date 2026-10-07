
#include "backend/file_backend.h"

#include <errno.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <fcntl.h>
#include <io.h>
#endif

#include "audio/audio_chunk.h"
#include "audio/processing_parameters.h"
#include "backend/audio_backend.h"
#include "backend/backend_clip.h"
#include "backend/backend_error.h"
#include "config/config_gen.h"
#include "logging/app_logger.h"
#include "utils/cdsp_path.h"
#include "utils/cdsp_time.h"
#include "wav/wav_writer.h"

#define fseek_64 cdsp_fseek64
#define ftell_64 cdsp_ftell64

static const logger_t g_logger = {"dsp.backend.file"};

#ifndef CDSP_TEST
/** The `realtime` file option is test-only; say so instead of ignoring it. */
static void file_backend_warn_realtime_ignored(bool requested) {
  if (requested) {
    logger_warn(&g_logger, "File backend option 'realtime' is only supported "
                           "in test builds and is ignored");
  }
}
#endif

/**
 * @brief Helper to get monotonic time in nanoseconds.
 *
 * @return Monotonic time in nanoseconds.
 */
static uint64_t get_time_ns(void) { return cdsp_time_now_ns(); }

struct file_capture {
  char filename[512];
  bool is_stdin;
  FILE *f;
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
  /// Owning backend handle, to flag pure extra_samples tail chunks as not
  /// silence-gated (capture_backend_t.skip_silence_detection, 06 F-10).
  capture_backend_t *owner;
  int playback_sample_rate;
  double resampling_ratio;
  uint8_t *raw_buf;
  size_t raw_buf_capacity;
  /// Bytes of an incomplete frame left over from a timed-out read. They were
  /// already consumed from the fd, so they are prepended to the next read to
  /// keep the stream frame aligned. Sized to one frame at open.
  uint8_t *carry;
  size_t carry_len;
  /// skip_bytes still to discard on a non-seekable source. Done inside read()
  /// with the same poll timeout as audio, so a silent producer cannot block
  /// open() (and the engine's stop/reload) indefinitely.
  uint64_t skip_remaining;
  bool is_regular; ///< Regular file: blocking fread, no poll timeout.
  bool eof;        ///< End of stream seen (sticky).
#ifdef CDSP_TEST
  bool realtime;
  uint64_t start_time_ns;
  size_t total_frames_read;
#endif
};

struct file_playback {
  char filename[512];
  bool is_stdout;
  FILE *f;
  int sample_rate;
  size_t channels;
  int chunk_size;
  binary_sample_format_t format;
  bool is_wav;
  bool use_rf64;
  bool is_seekable;
  uint64_t total_bytes_written;
  uint8_t *raw_buf;
  size_t raw_buf_capacity;
  processing_parameters_t *params;
  uint64_t last_clip_warn_ns; ///< Rate limit for the clipping warning.
#ifdef CDSP_TEST
  bool realtime;
  uint64_t start_time_ns;
  size_t total_frames_written;
  _Atomic bool stopped;
#endif
};

// MARK: - File Capture Backend implementation

/**
 * @brief Open the file capture device.
 *
 * @param ctx Pointer to the file_capture_t instance.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return true if successful, false otherwise.
 */
static bool file_capture_open(void *ctx, backend_error_t *err) {
  file_capture_t *capture = (file_capture_t *)ctx;
  if (!capture)
    return false;
  if (capture->is_stdin) {
    capture->f = stdin;
#if defined(_WIN32)
    // The CRT opens stdin in text mode (CRLF -> LF, ^Z = EOF), which would
    // corrupt binary PCM. Upstream reads the raw handle.
    _setmode(_fileno(stdin), _O_BINARY);
#endif
  } else {
#if !defined(_WIN32)
    char expanded[1024];
    cdsp_expand_path(capture->filename, expanded, sizeof(expanded));
    int fd = open(expanded, O_RDONLY | O_NONBLOCK);
    if (fd >= 0) {
      capture->f = fdopen(fd, "rb");
      if (!capture->f) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
      }
    } else {
      capture->f = NULL;
    }
#else
    capture->f = cdsp_fopen(capture->filename, "rb");
#endif
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

  if (capture->f) {
    setvbuf(capture->f, NULL, _IONBF, 0);
  }

  capture->is_regular = true;
#if !defined(_WIN32)
  {
    int fd = capture->f ? fileno(capture->f) : -1;
    struct stat st;
    capture->is_regular =
        fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode);
    if (capture->is_regular && !capture->is_stdin) {
      int flags = fcntl(fd, F_GETFL, 0);
      if (flags >= 0 && (flags & O_NONBLOCK)) {
        (void)fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
      }
    }
  }
#endif
  capture->skip_remaining = 0;
  capture->carry_len = 0;
  capture->eof = false;

  if (capture->is_wav && !capture->is_stdin) {
    wav_info_t info;
    char msg[256];
    if (!wav_read_header(capture->f, &info, msg, sizeof(msg)) ||
        info.format == BINARY_SAMPLE_FORMAT_INVALID) {
      if (info.format == BINARY_SAMPLE_FORMAT_INVALID && msg[0] == '\0') {
        snprintf(msg, sizeof(msg), "Unsupported WAV sample format");
      }
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

    if (fseek_64(capture->f, (int64_t)info.data_start_offset, SEEK_SET) != 0 &&
        capture->is_regular) {
      char seek_msg[1024];
      snprintf(seek_msg, sizeof(seek_msg),
               "Failed to seek to the audio data of '%s': %s",
               capture->filename, strerror(errno));
      fclose(capture->f);
      capture->f = NULL;
      if (err)
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, seek_msg);
      return false;
    }
  } else {
    if (capture->skip_bytes > 0) {
      logger_debug(&g_logger, "skipping the first %llu bytes",
                   (unsigned long long)capture->skip_bytes);
      if (capture->is_stdin || !capture->is_regular ||
          fseek_64(capture->f, (int64_t)capture->skip_bytes, SEEK_SET) != 0) {
        // Non-seekable: discard inside read(), bounded by the poll timeout.
        capture->skip_remaining = capture->skip_bytes;
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
  size_t cap_frames = (size_t)capture->chunk_size;
  if (capture->resampling_ratio > 0.0 && capture->resampling_ratio < 1.0) {
    size_t scaled =
        (size_t)ceil((double)cap_frames / capture->resampling_ratio);
    if (scaled > cap_frames) {
      cap_frames = scaled;
    }
  }
  capture->raw_buf_capacity = cap_frames * capture->channels * sample_size * 4;
  capture->raw_buf =
      (uint8_t *)calloc(capture->raw_buf_capacity, sizeof(uint8_t));
  free(capture->carry);
  capture->carry =
      (uint8_t *)calloc(capture->channels * sample_size, sizeof(uint8_t));
  if (!capture->raw_buf || !capture->carry) {
    free(capture->raw_buf);
    capture->raw_buf = NULL;
    free(capture->carry);
    capture->carry = NULL;
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
#ifdef CDSP_TEST
  capture->start_time_ns = get_time_ns();
  capture->total_frames_read = 0;
#endif
  return true;
}

/// Outcome of one bounded read from the capture source.
typedef enum {
  FILE_READ_COMPLETE, ///< All requested bytes were read.
  FILE_READ_TIMEOUT,  ///< The poll timeout expired first (non-regular fds).
  FILE_READ_EOF,      ///< End of stream (read() returned 0 / short fread).
  FILE_READ_ERROR     ///< poll()/read()/fread() failed.
} file_read_status_t;

/**
 * @brief Read up to @p len bytes from the capture source.
 *
 * Regular files (and every source on Windows) use a blocking fread. Pipes,
 * FIFOs, ttys and sockets are polled with an overall time limit of
 * @p timeout_ms, mirroring upstream `NonBlockingReader`
 * (filereader_nonblock.rs:55-91).
 *
 * @param capture The capture instance.
 * @param buf Destination buffer.
 * @param len Number of bytes wanted.
 * @param timeout_ms Overall time limit for non-regular sources.
 * @param out_n Receives the number of bytes actually read.
 * @return The read outcome.
 */
static file_read_status_t file_capture_fill(file_capture_t *capture,
                                            uint8_t *buf, size_t len,
                                            uint64_t timeout_ms,
                                            size_t *out_n) {
  size_t got = 0;
#if !defined(_WIN32)
  if (!capture->is_regular) {
    int fd = fileno(capture->f);
    struct pollfd pfd = {.fd = fd, .events = POLLIN, .revents = 0};
    uint64_t start_ns = get_time_ns();
    uint64_t timeout_ns = timeout_ms * 1000000ULL;
    while (got < len) {
      // Always poll at least once (with 0 ms once the budget is spent), so
      // data that is already queued is never reported as a timeout.
      uint64_t now_ns = get_time_ns();
      uint64_t elapsed_ns = (now_ns >= start_ns) ? (now_ns - start_ns) : 0;
      int wait_ms = 0;
      if (elapsed_ns < timeout_ns) {
        wait_ms = (int)((timeout_ns - elapsed_ns + 999999ULL) / 1000000ULL);
      }
      int poll_ret = poll(&pfd, 1, wait_ms);
      if (poll_ret == 0) {
        *out_n = got;
        return FILE_READ_TIMEOUT;
      }
      if (poll_ret < 0) {
        if (errno == EINTR)
          continue;
        *out_n = got;
        return FILE_READ_ERROR;
      }
      ssize_t n = read(fd, buf + got, len - got);
      if (n == 0) {
        *out_n = got;
        return FILE_READ_EOF;
      }
      if (n < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
          continue;
        *out_n = got;
        return FILE_READ_ERROR;
      }
      got += (size_t)n;
    }
    *out_n = got;
    return FILE_READ_COMPLETE;
  }
#else
  (void)timeout_ms;
#endif
  got = fread(buf, 1, len, capture->f);
  *out_n = got;
  if (got < len) {
    return ferror(capture->f) ? FILE_READ_ERROR : FILE_READ_EOF;
  }
  return FILE_READ_COMPLETE;
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
static bool file_capture_read(void *ctx, size_t frames, audio_chunk_t *chunk,
                              backend_error_t *err) {
  file_capture_t *capture = (file_capture_t *)ctx;
  if (!capture)
    return false;
  if (audio_chunk_get_channels(chunk) < (size_t)capture->channels) {
    if (err) {
      backend_error_init(
          err, BACKEND_ERROR_INVALID_CHANNELS,
          "Chunk channels count does not match capture channels");
    }
    return false;
  }

  size_t sample_size = sample_format_bytes_per_sample(capture->format);
  if (sample_size == 0 || capture->channels == 0 || !capture->carry) {
    audio_chunk_set_valid_frames(chunk, 0);
    return false;
  }
  if (frames > audio_chunk_get_frames(chunk)) {
    frames = audio_chunk_get_frames(chunk);
  }
  size_t frame_bytes = capture->channels * sample_size;
  size_t want_bytes = frames * frame_bytes;

  if (want_bytes > capture->raw_buf_capacity) {
    uint8_t *new_buf = (uint8_t *)realloc(capture->raw_buf, want_bytes);
    if (!new_buf) {
      if (err) {
        backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                           "Failed to reallocate file capture raw buffer");
      }
      audio_chunk_set_valid_frames(chunk, 0);
      return false;
    }
    capture->raw_buf = new_buf;
    capture->raw_buf_capacity = want_bytes;
  }

  uint64_t timeout_ms =
      capture->sample_rate > 0
          ? ((uint64_t)2000 * (uint64_t)frames / (uint64_t)capture->sample_rate)
          : 50;
  if (timeout_ms == 0)
    timeout_ms = 1;

  // Deferred skip_bytes on a non-seekable source. A timeout returns "no data
  // yet" so the engine stays responsive to stop/reload.
  while (capture->skip_remaining > 0 && !capture->eof) {
    size_t to_skip = capture->skip_remaining < capture->raw_buf_capacity
                         ? (size_t)capture->skip_remaining
                         : capture->raw_buf_capacity;
    size_t n = 0;
    file_read_status_t st =
        file_capture_fill(capture, capture->raw_buf, to_skip, timeout_ms, &n);
    capture->skip_remaining -= n;
    if (st == FILE_READ_TIMEOUT) {
      audio_chunk_set_valid_frames(chunk, 0);
      return false;
    }
    if (st == FILE_READ_ERROR) {
      if (err) {
        backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                           "Read error from stream");
      }
      audio_chunk_set_valid_frames(chunk, 0);
      return false;
    }
    if (st == FILE_READ_EOF) {
      capture->skip_remaining = 0;
      capture->eof = true;
    }
  }

  // Start with the incomplete frame carried over from a timed-out read.
  size_t bytes_read = 0;
  if (capture->carry_len > 0 && capture->carry_len <= want_bytes) {
    memcpy(capture->raw_buf, capture->carry, capture->carry_len);
    bytes_read = capture->carry_len;
  }
  capture->carry_len = 0;

  size_t new_bytes = want_bytes - bytes_read;
  if (capture->read_bytes > 0) {
    uint64_t left = capture->read_bytes > capture->total_bytes_read
                        ? capture->read_bytes - capture->total_bytes_read
                        : 0;
    if ((uint64_t)new_bytes > left) {
      new_bytes = (size_t)left;
    }
  }

  if (new_bytes > 0 && !capture->eof) {
    size_t n = 0;
    file_read_status_t st = file_capture_fill(
        capture, capture->raw_buf + bytes_read, new_bytes, timeout_ms, &n);
    bytes_read += n;
    capture->total_bytes_read += n;
    if (st == FILE_READ_ERROR) {
      if (err) {
        backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                           "Read error from stream");
      }
      audio_chunk_set_valid_frames(chunk, 0);
      return false;
    }
    if (st == FILE_READ_EOF) {
      capture->eof = true;
    }
  }

  // End of stream: real EOF, or the read_bytes limit was reached. A timeout is
  // never treated as end of stream (upstream device.rs:501-532).
  bool at_end =
      capture->eof || (capture->read_bytes > 0 &&
                       capture->total_bytes_read >= capture->read_bytes);

  size_t frames_read = bytes_read / frame_bytes;
  size_t residual = bytes_read - frames_read * frame_bytes;
  if (residual > 0 && !at_end) {
    // Keep the partial frame for the next read so the stream stays aligned.
    memcpy(capture->carry, capture->raw_buf + frames_read * frame_bytes,
           residual);
    capture->carry_len = residual;
  }
  if (frames_read == 0 && !at_end) {
    // Timed out before a complete frame arrived: no data yet, not EOF.
    audio_chunk_set_valid_frames(chunk, 0);
    return false;
  }

  audio_chunk_decode_interleaved(capture->raw_buf, capture->format,
                                 (size_t)capture->channels, frames_read, chunk);

  // A chunk made only of the EOF extra_samples tail is sent unconditionally
  // upstream (file_backend/device.rs send_silence), so it must not be
  // silence-gated (06 F-10). Chunks with file data are gated as usual.
  const size_t file_frames = frames_read;
  if (capture->owner)
    capture->owner->skip_silence_detection = false;

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
    } else if (extra_left > 0) {
      extra_to_generate = (size_t)ceil((double)extra_left / ratio);
      if (extra_to_generate > remaining_frames) {
        extra_to_generate = remaining_frames;
      }
      capture->extra_samples_generated = capture->extra_samples;
    }

    if (extra_to_generate > 0) {
      for (size_t c = 0; c < capture->channels; c++) {
        double *ch_data = audio_chunk_get_channel(chunk, c);
        if (ch_data) {
          memset(ch_data + frames_read, 0, extra_to_generate * sizeof(double));
        }
      }
      frames_read += extra_to_generate;
      if (file_frames == 0 && capture->owner)
        capture->owner->skip_silence_detection = true;
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
static void file_capture_close(void *ctx) {
  file_capture_t *capture = (file_capture_t *)ctx;
  if (!capture)
    return;
  if (capture->f && !capture->is_stdin) {
    fclose(capture->f);
    capture->f = NULL;
  }
  if (capture->raw_buf) {
    free(capture->raw_buf);
    capture->raw_buf = NULL;
  }
  free(capture->carry);
  capture->carry = NULL;
  capture->carry_len = 0;
}

/**
 * @brief Get any pending sample rate change.
 *
 * @param ctx Pointer to the file_capture_t instance.
 * @param out_rate Pointer to double to store the pending sample rate.
 * @return true if a rate change is pending, false otherwise.
 */
static bool file_capture_get_pending_rate_change(void *ctx, double *out_rate) {
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
static bool file_capture_pitch_control_supported(void *ctx) {
  (void)ctx;
  return false;
}

/**
 * @brief Set the pitch multiplier for the file capture backend.
 *
 * @param ctx Pointer to the file_capture_t instance.
 * @param multiplier The pitch multiplier.
 */
static void file_capture_set_pitch(void *ctx, double multiplier) {
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
static bool file_capture_wait(void *ctx, uint32_t timeout_ms) {
#ifdef CDSP_TEST
  file_capture_t *capture = (file_capture_t *)ctx;
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
 * @brief Stop the file capture device.
 *
 * @param ctx Pointer to the file_capture_t instance.
 */
static void file_capture_stop(void *ctx) { (void)ctx; }

/**
 * @brief Destroy the file capture backend instance.
 *
 * @param ctx Pointer to the file_capture_t instance to destroy.
 */
static void file_capture_destroy(void *ctx) {
  file_capture_t *capture = (file_capture_t *)ctx;
  if (!capture)
    return;
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
static capture_backend_t *
file_capture_create(const capture_device_config_t *config, int sample_rate,
                    int chunk_size, bool full_duplex,
                    processing_parameters_t *params, backend_error_t *err) {
  (void)full_duplex;
  (void)params;
  file_capture_t *capture = (file_capture_t *)calloc(1, sizeof(file_capture_t));
  if (!capture) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Memory allocation failure");
    return NULL;
  }

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
#else
      file_backend_warn_realtime_ignored(config->cfg.wav_file.has_realtime &&
                                         config->cfg.wav_file.realtime);
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
#else
      file_backend_warn_realtime_ignored(config->cfg.raw_file.has_realtime &&
                                         config->cfg.raw_file.realtime);
#endif
    }
  }

  capture->sample_rate = sample_rate;
  capture->playback_sample_rate = sample_rate;
  capture->resampling_ratio = 1.0;
  capture->chunk_size = chunk_size;

  capture_backend_t *backend =
      (capture_backend_t *)calloc(1, sizeof(capture_backend_t));
  if (!backend) {
    free(capture);
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Memory allocation failure");
    return NULL;
  }
  backend->ctx = capture;
  backend->vtable = &g_file_capture_vtable;
  capture->owner = backend;
#ifdef CDSP_TEST
  backend->is_realtime = capture->realtime;
#else
  backend->is_realtime = false;
#endif
  return backend;
}

// Pipeline (playback) sample rate, delivered by the engine through the
// optional set_pipeline_sample_rate vtable hook, so resampling_ratio and
// extra_samples scaling match upstream when capture rate != pipeline rate.
static void file_capture_set_pipeline_sample_rate(void *ctx,
                                                  int pipeline_sample_rate) {
  file_capture_t *capture = (file_capture_t *)ctx;
  if (!capture)
    return;
  capture->playback_sample_rate = pipeline_sample_rate;
  if (capture->sample_rate > 0 && capture->playback_sample_rate > 0) {
    capture->resampling_ratio =
        (double)capture->playback_sample_rate / (double)capture->sample_rate;
  }
}

const capture_backend_vtable_t g_file_capture_vtable = {
    .create = file_capture_create,
    .open = file_capture_open,
    .read = file_capture_read,
    .close = file_capture_close,
    .get_pending_rate_change = file_capture_get_pending_rate_change,
    .is_pitch_control_supported = file_capture_pitch_control_supported,
    .set_pitch = file_capture_set_pitch,
    .set_pipeline_sample_rate = file_capture_set_pipeline_sample_rate,
    .wait_for_data = file_capture_wait,
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
static bool file_playback_open(void *ctx, backend_error_t *err) {
  file_playback_t *playback = (file_playback_t *)ctx;
  if (!playback)
    return false;
  if (playback->is_stdout) {
    playback->f = stdout;
#if defined(_WIN32)
    // Text-mode stdout would expand every 0x0A byte to CR LF.
    _setmode(_fileno(stdout), _O_BINARY);
#endif
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
      (uint8_t *)calloc(playback->raw_buf_capacity, sizeof(uint8_t));
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
    bool header_ok;
    if (!playback->is_seekable && playback->use_rf64) {
      logger_warn(&g_logger, "RF64 output requires a seekable file, writing a "
                             "streaming wav header instead");
      header_ok =
          wav_write_header(playback->f, playback->channels, playback->format,
                           playback->sample_rate, 0xFFFFFFFF, false);
    } else {
      if (playback->use_rf64) {
        header_ok =
            wav_write_rf64_header(playback->f, playback->channels,
                                  playback->format, playback->sample_rate, 0);
      } else {
        header_ok = wav_write_header(playback->f, playback->channels,
                                     playback->format, playback->sample_rate,
                                     0xFFFFFFFF, playback->is_seekable);
      }
    }
    if (!header_ok) {
      if (err) {
        char err_msg[1024];
        snprintf(err_msg, sizeof(err_msg), "Failed to write wav header to '%s'",
                 playback->is_stdout ? "stdout" : playback->filename);
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, err_msg);
      }
      if (!playback->is_stdout) {
        fclose(playback->f);
      }
      playback->f = NULL;
      free(playback->raw_buf);
      playback->raw_buf = NULL;
      return false;
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
static bool file_playback_write(void *ctx, const audio_chunk_t *chunk,
                                backend_error_t *err) {
  file_playback_t *playback = (file_playback_t *)ctx;
  if (!playback)
    return false;
#ifdef CDSP_TEST
  if (atomic_load_explicit(&playback->stopped, memory_order_acquire)) {
    if (err) {
      backend_error_init(err, BACKEND_ERROR_WRITE_ERROR, "Playback stopped");
    }
    return false;
  }
#endif
  size_t frames = audio_chunk_get_valid_frames(chunk);
  (void)frames;

  bool reached_4gb = false;
  char err_msg[256] = {0};

  bool success = wav_write_audio_chunk(
      playback->f, chunk, (size_t)playback->channels, playback->format,
      playback->is_wav, playback->is_seekable, playback->use_rf64,
      &playback->total_bytes_written, &playback->raw_buf,
      &playback->raw_buf_capacity, &reached_4gb, err_msg, sizeof(err_msg));

  if (!success) {
    if (reached_4gb) {
      logger_warn(&g_logger,
                  "Wav file reached the maximum size of a plain wav file. "
                  "Stopping playback to avoid writing an invalid file.");
      if (err) {
        backend_error_init(err, BACKEND_ERROR_NONE,
                           "Plain WAV 4 GB limit reached");
      }
    } else {
      if (err) {
        backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                           err_msg[0] ? err_msg
                                      : "Failed to write audio chunk");
      }
    }
    return false;
  }
  // stdout is fully buffered when redirected to a pipe; flush every chunk so
  // the consumer sees audio with chunk latency and a closed reader is
  // reported now rather than at some later write (upstream flushes on
  // finish and Rust's Stdout is a LineWriter).
  if (playback->is_stdout && fflush(playback->f) != 0) {
    if (err) {
      char msg[256];
      snprintf(msg, sizeof(msg), "Failed to flush stdout: %s", strerror(errno));
      backend_error_init(err, BACKEND_ERROR_WRITE_ERROR, msg);
    }
    return false;
  }

  if (playback->params && !sample_format_is_float(playback->format) &&
      !sample_format_is_dsd(playback->format)) {
    backend_count_clipped(playback->params, chunk, &playback->last_clip_warn_ns,
                          &g_logger);
  }
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
static void file_playback_close(void *ctx) {
  file_playback_t *playback = (file_playback_t *)ctx;
  if (!playback)
    return;
  if (playback->f) {
    if (playback->is_wav && playback->is_seekable && !playback->is_stdout) {
      if (!wav_update_header(playback->f, playback->channels, playback->format,
                             playback->sample_rate,
                             playback->total_bytes_written,
                             playback->use_rf64)) {
        logger_error(&g_logger,
                     "Failed to finalize wav header of '%s'; the file may be "
                     "truncated or corrupt",
                     playback->filename);
      }
    }
    if (!playback->is_stdout) {
      if (fclose(playback->f) != 0) {
        logger_error(&g_logger, "Failed to close output file '%s': %s",
                     playback->filename, strerror(errno));
      }
    } else if (fflush(playback->f) != 0) {
      logger_error(&g_logger, "Failed to flush stdout: %s", strerror(errno));
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
static size_t file_playback_get_buffer_level(void *ctx) {
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
static bool file_playback_get_pending_rate_change(void *ctx, double *out_rate) {
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
static bool file_playback_prefill_silence(void *ctx, size_t frames,
                                          backend_error_t *err) {
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
static bool file_playback_get_is_paused(void *ctx) {
  (void)ctx;
  return false;
}

/**
 * @brief Set the paused state of the file playback backend.
 *
 * @param ctx Pointer to the file_playback_t instance.
 * @param paused true to pause, false to resume.
 */
static void file_playback_set_is_paused(void *ctx, bool paused) {
  (void)ctx;
  (void)paused;
}

/**
 * @brief Stop the file playback device.
 *
 * @param ctx Pointer to the file_playback_t instance.
 */
static void file_playback_stop(void *ctx) {
#ifdef CDSP_TEST
  file_playback_t *playback = (file_playback_t *)ctx;
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
static void file_playback_destroy(void *ctx) {
  file_playback_t *playback = (file_playback_t *)ctx;
  if (!playback)
    return;
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
static playback_backend_t *
file_playback_create(const playback_device_config_t *config, int sample_rate,
                     int chunk_size, bool full_duplex,
                     processing_parameters_t *params, backend_error_t *err) {
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
  if (has_wav_header && sample_format_is_dsd(fmt)) {
    // A WAV header can only describe PCM; DSD bit patterns tagged as PCM play
    // back as full-scale noise (speaker hazard). Raw DSD output stays allowed.
    if (err) {
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Wav files do not support DSD sample formats");
    }
    return NULL;
  }

  file_playback_t *playback =
      (file_playback_t *)calloc(1, sizeof(file_playback_t));
  if (!playback) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Memory allocation failure");
    return NULL;
  }
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
#else
    file_backend_warn_realtime_ignored(config->cfg.raw_file.has_realtime &&
                                       config->cfg.raw_file.realtime);
#endif
  }
  playback->chunk_size = chunk_size;
  playback->sample_rate = sample_rate;
  playback->params = params;

  playback_backend_t *backend =
      (playback_backend_t *)calloc(1, sizeof(playback_backend_t));
  if (!backend) {
    free(playback);
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Memory allocation failure");
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

void file_capture_set_resampling_ratio(capture_backend_t *backend,
                                       double ratio) {
  if (!backend || backend->vtable != &g_file_capture_vtable || !backend->ctx) {
    return;
  }
  file_capture_t *capture = (file_capture_t *)backend->ctx;
  capture->resampling_ratio = ratio;
}

double file_capture_get_resampling_ratio(const capture_backend_t *backend) {
  if (!backend || backend->vtable != &g_file_capture_vtable || !backend->ctx) {
    return 1.0;
  }
  const file_capture_t *capture = (const file_capture_t *)backend->ctx;
  return capture->resampling_ratio;
}
