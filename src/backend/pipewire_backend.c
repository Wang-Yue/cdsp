#include "backend/pipewire_backend.h"

#if defined(ENABLE_PIPEWIRE)

#include <math.h>
#include <pipewire/core.h>
#include <pipewire/keys.h>
#include <pipewire/pipewire.h>
#include <pipewire/port.h>
#include <pipewire/properties.h>
#include <pipewire/stream.h>
#include <pipewire/thread-loop.h>
#include <pthread.h>
#include <spa/buffer/buffer.h>
#include <spa/node/io.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/format.h>
#include <spa/param/audio/raw-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/param/format.h>
#include <spa/param/param.h>
#include <spa/pod/builder.h>
#include <spa/pod/pod.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/audio_chunk.h"
#include "backend/audio_backend.h"
#include "backend/backend_buffer.h"
#include "backend/backend_error.h"
#include "backend/pipewire_internal.h"
#include "config/config_gen.h"
#include "logging/app_logger.h"
#include "utils/cdsp_time.h"

static const logger_t g_logger = {"dsp.backend.pipewire"};

/** Playback write retries, as upstream ringbuffer.rs PUSH_RETRIES. */
#define PIPEWIRE_PLAYBACK_PUSH_RETRIES 16u

static pthread_once_t g_pw_init_once = PTHREAD_ONCE_INIT;

static void do_pw_init(void) { pw_init(NULL, NULL); }

static void ensure_pw_init(void) { pthread_once(&g_pw_init_once, do_pw_init); }

struct pipewire_capture {
  char device[256];
  int sample_rate;
  size_t channels;
  int chunk_size;

  char node_name[256];
  char node_description[256];
  char node_group_name[256];
  char autoconnect_to[256];
  bool has_node_name;
  bool has_node_description;
  bool has_node_group_name;
  bool has_autoconnect_to;
  bool loopback;

  struct pw_thread_loop *loop;
  struct pw_stream *stream;

  backend_buffer_t *buffer;
  size_t blockalign;

  /* Pending graph/format rate in Hz, 0 when none. Written by param_changed on
   * the PipeWire loop thread, consumed lock-free by the engine worker thread
   * (AGENTS §1.2: no mutex on audio threads). */
  _Atomic uint32_t pending_rate_hz;
  /* Graph position IO (SPA_IO_Position), set by io_changed. Its
   * clock.rate.denom is the actual graph rate; the negotiated Format stays at
   * the requested rate because the adapter resamples (audit 05 F-04). */
  struct spa_io_position *_Atomic position;
  /* Last graph rate observed by the RT process callback (RT thread only). */
  uint32_t graph_rate_seen;
  processing_parameters_t *params;
};

struct pipewire_playback {
  char device[256];
  int sample_rate;
  size_t channels;
  int chunk_size;
  size_t target_level;

  char node_name[256];
  char node_description[256];
  char node_group_name[256];
  char autoconnect_to[256];
  bool has_node_name;
  bool has_node_description;
  bool has_node_group_name;
  bool has_autoconnect_to;

  struct pw_thread_loop *loop;
  struct pw_stream *stream;

  backend_buffer_t *buffer;

  /* Pending graph/format rate in Hz, 0 when none. Written by param_changed on
   * the PipeWire loop thread, consumed lock-free by the engine worker thread
   * (AGENTS §1.2: no mutex on audio threads). */
  _Atomic uint32_t pending_rate_hz;
  /* Graph position IO (SPA_IO_Position), set by io_changed. Its
   * clock.rate.denom is the actual graph rate; the negotiated Format stays at
   * the requested rate because the adapter resamples (audit 05 F-04). */
  struct spa_io_position *_Atomic position;
  /* Last graph rate observed by the RT process callback (RT thread only). */
  uint32_t graph_rate_seen;
  /* True while the stream is in PW_STREAM_STATE_STREAMING (process callbacks
   * are running and draining the ring). Set on the PipeWire loop thread. */
  _Atomic bool streaming;
  processing_parameters_t *params;
};

// MARK: - PipeWire Callbacks

/**
 * @brief Report graph clock-rate changes observed in the RT process callback.
 *
 * Reads the graph rate from the position IO area (lock-free, wait-free) and
 * publishes a pending rate when it changes during the session. The first
 * observation only records the baseline so that a graph already running at a
 * different rate at startup keeps working through PipeWire's adapter
 * resampling, as before; the engine compares any reported rate with the
 * configured one.
 */
static inline void pw_check_graph_rate(struct spa_io_position *_Atomic *pos_ptr,
                                       uint32_t *seen,
                                       _Atomic uint32_t *pending,
                                       backend_buffer_t *buffer) {
  struct spa_io_position *pos =
      atomic_load_explicit(pos_ptr, memory_order_acquire);
  if (!pos)
    return;
  uint32_t rate = pipewire_graph_rate_update(pos->clock.rate.denom, seen);
  if (rate != 0) {
    atomic_store_explicit(pending, rate, memory_order_release);
    backend_buffer_set_pending_rate_change(buffer, true);
    backend_buffer_signal(buffer);
  }
}

static void on_capture_io_changed(void *data, uint32_t id, void *area,
                                  uint32_t size) {
  (void)size;
  pipewire_capture_t *c = (pipewire_capture_t *)data;
  if (c && id == SPA_IO_Position)
    atomic_store_explicit(&c->position, (struct spa_io_position *)area,
                          memory_order_release);
}

static void on_playback_io_changed(void *data, uint32_t id, void *area,
                                   uint32_t size) {
  (void)size;
  pipewire_playback_t *p = (pipewire_playback_t *)data;
  if (p && id == SPA_IO_Position)
    atomic_store_explicit(&p->position, (struct spa_io_position *)area,
                          memory_order_release);
}

/**
 * @brief PipeWire stream process callback for capture.
 *
 * Called by the PipeWire thread loop when new capture data is available.
 * Dequeues the buffer, copies data to the internal SPSC ring buffer, and queues
 * it back.
 *
 * @note Runs in a real-time thread context. Must be non-blocking.
 *
 * @param data Pointer to pipewire_capture_t.
 */
static void on_capture_process(void *data) {
  pipewire_capture_t *c = (pipewire_capture_t *)data;
  pw_check_graph_rate(&c->position, &c->graph_rate_seen, &c->pending_rate_hz,
                      c->buffer);
  struct pw_buffer *b = pw_stream_dequeue_buffer(c->stream);
  if (!b)
    return;

  struct spa_buffer *buf = b->buffer;
  if (buf && buf->n_datas > 0 && buf->datas[0].data && buf->datas[0].chunk) {
    // Never trust chunk offset/size from the graph: clamp them to the mapped
    // region so a misbehaving peer cannot make the RT thread read out of
    // bounds (upstream device.rs:871-872 would panic on the slice instead).
    size_t offset = 0;
    size_t frames = pipewire_capture_chunk_frames(
        buf->datas[0].chunk->offset, buf->datas[0].chunk->size,
        buf->datas[0].maxsize, c->blockalign, &offset);
    if (frames > 0) {
      const uint8_t *src = (const uint8_t *)buf->datas[0].data;
      backend_buffer_push(c->buffer, src + offset, frames);
    }
  }

  pw_stream_queue_buffer(c->stream, b);
}

static void on_capture_stream_state_changed(void *data,
                                            enum pw_stream_state old,
                                            enum pw_stream_state state,
                                            const char *error) {
  (void)data;
  logger_debug(&g_logger,
               "Capture stream state changed from %s to %s (error: %s)",
               pw_stream_state_as_string(old), pw_stream_state_as_string(state),
               error ? error : "none");
  if (state == PW_STREAM_STATE_ERROR) {
    // Upstream only logs at debug (device.rs:392-394 / 825-827). Surface
    // stream errors at error level; propagating them to the engine is
    // deferred (audit 05 F-14) to keep upstream's keep-running behaviour.
    logger_error(&g_logger, "PipeWire capture stream error: %s",
                 error ? error : "unknown");
  }
}

static void on_capture_param_changed(void *data, uint32_t id,
                                     const struct spa_pod *param) {
  pipewire_capture_t *c = (pipewire_capture_t *)data;
  if (!c || id != SPA_PARAM_Format || !param)
    return;
  struct spa_audio_info info;
  memset(&info, 0, sizeof(info));
  if (spa_format_audio_parse(param, &info) >= 0) {
    if (info.media_type == SPA_MEDIA_TYPE_audio &&
        info.media_subtype == SPA_MEDIA_SUBTYPE_raw) {
      uint32_t rate = info.info.raw.rate;
      if (rate > 0 && (int)rate != c->sample_rate) {
        atomic_store_explicit(&c->pending_rate_hz, rate, memory_order_release);
        backend_buffer_set_pending_rate_change(c->buffer, true);
        backend_buffer_signal(c->buffer);
      }
    }
  }
}

static const struct pw_stream_events capture_stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_capture_stream_state_changed,
    .param_changed = on_capture_param_changed,
    .io_changed = on_capture_io_changed,
    .process = on_capture_process,
};

/**
 * @brief PipeWire stream process callback for playback.
 *
 * Called by the PipeWire thread loop when the stream needs more data for
 * playback. Dequeues the buffer, fills it from the internal SPSC ring buffer,
 * and queues it back. Fills with silence in case of buffer underflow.
 *
 * @note Runs in a real-time thread context. Must be non-blocking.
 *
 * @param data Pointer to pipewire_playback_t.
 */
static void on_playback_process(void *data) {
  pipewire_playback_t *p = (pipewire_playback_t *)data;
  pw_check_graph_rate(&p->position, &p->graph_rate_seen, &p->pending_rate_hz,
                      p->buffer);
  struct pw_buffer *b = pw_stream_dequeue_buffer(p->stream);
  if (!b)
    return;

  struct spa_buffer *buf = b->buffer;
  if (buf->n_datas == 0) {
    pw_stream_queue_buffer(p->stream, b);
    return;
  }

  uint8_t *dst = (uint8_t *)buf->datas[0].data;
  struct spa_chunk *chunk = buf->datas[0].chunk;

  if (dst && chunk) {
    size_t stride = sizeof(float) * p->channels;
    size_t callback_bytes = pipewire_playback_callback_bytes(
        chunk->size, buf->datas[0].maxsize, stride, (size_t)p->chunk_size);

    if (p->buffer) {
      backend_buffer_render(p->buffer, dst, callback_bytes / stride, 0x00);
    } else {
      // Defence in depth: never hand PipeWire a buffer we did not write.
      memset(dst, 0, callback_bytes);
    }

    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->size = (uint32_t)callback_bytes;
    buf->datas[0].chunk->stride = (int32_t)stride;
  }

  pw_stream_queue_buffer(p->stream, b);
}

static void on_playback_stream_state_changed(void *data,
                                             enum pw_stream_state old,
                                             enum pw_stream_state state,
                                             const char *error) {
  pipewire_playback_t *p = (pipewire_playback_t *)data;
  if (p) {
    atomic_store_explicit(&p->streaming, state == PW_STREAM_STATE_STREAMING,
                          memory_order_release);
  }
  logger_debug(&g_logger,
               "Playback stream state changed from %s to %s (error: %s)",
               pw_stream_state_as_string(old), pw_stream_state_as_string(state),
               error ? error : "none");
  if (state == PW_STREAM_STATE_ERROR) {
    // Upstream only logs at debug (device.rs:392-394 / 825-827). Surface
    // stream errors at error level; propagating them to the engine is
    // deferred (audit 05 F-14) to keep upstream's keep-running behaviour.
    logger_error(&g_logger, "PipeWire playback stream error: %s",
                 error ? error : "unknown");
  }
}

static void on_playback_param_changed(void *data, uint32_t id,
                                      const struct spa_pod *param) {
  pipewire_playback_t *p = (pipewire_playback_t *)data;
  if (!p || id != SPA_PARAM_Format || !param)
    return;
  struct spa_audio_info info;
  memset(&info, 0, sizeof(info));
  if (spa_format_audio_parse(param, &info) >= 0) {
    if (info.media_type == SPA_MEDIA_TYPE_audio &&
        info.media_subtype == SPA_MEDIA_SUBTYPE_raw) {
      uint32_t rate = info.info.raw.rate;
      if (rate > 0 && (int)rate != p->sample_rate) {
        atomic_store_explicit(&p->pending_rate_hz, rate, memory_order_release);
        backend_buffer_set_pending_rate_change(p->buffer, true);
        backend_buffer_signal(p->buffer);
      }
    }
  }
}

static const struct pw_stream_events playback_stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_playback_stream_state_changed,
    .param_changed = on_playback_param_changed,
    .io_changed = on_playback_io_changed,
    .process = on_playback_process,
};

// MARK: - Capture Backend implementation

/**
 * @brief Close the PipeWire capture device.
 *
 * @param ctx Pointer to the PipeWire capture instance.
 */
static void pipewire_capture_close(void *ctx) {
  pipewire_capture_t *capture = (pipewire_capture_t *)ctx;
  if (!capture)
    return;
  if (capture->loop) {
    pw_thread_loop_lock(capture->loop);
    if (capture->stream) {
      pw_stream_destroy(capture->stream);
      capture->stream = NULL;
    }
    pw_thread_loop_unlock(capture->loop);

    pw_thread_loop_stop(capture->loop);
    pw_thread_loop_destroy(capture->loop);
    capture->loop = NULL;
  }

  backend_buffer_set_state(capture->buffer, BACKEND_STREAM_STOPPED);
  backend_buffer_free(capture->buffer);
  capture->buffer = NULL;
}

/**
 * @brief Open the PipeWire capture device.
 *
 * @param ctx Pointer to the PipeWire capture instance.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return true if successful, false otherwise.
 */

static bool pipewire_capture_open(void *ctx, backend_error_t *err) {
  pipewire_capture_t *capture = (pipewire_capture_t *)ctx;
  if (!capture)
    return false;
  ensure_pw_init();

  // The ring buffer must exist before the stream is connected: with
  // PW_STREAM_FLAG_RT_PROCESS the process callback runs on the PipeWire data
  // thread as soon as negotiation completes, independently of this thread and
  // of the thread-loop lock. Upstream likewise builds its ring before
  // registering the listener and connecting (device.rs:804-809).
  capture->blockalign = (size_t)capture->channels * sizeof(float);
  size_t cap_min_frames = (size_t)ceil((double)capture->sample_rate * 0.025);
  size_t cap_frames_needed = (size_t)(4 * capture->chunk_size);
  if (cap_frames_needed < cap_min_frames)
    cap_frames_needed = cap_min_frames;
  capture->buffer = backend_buffer_create(
      cap_frames_needed, BINARY_SAMPLE_FORMAT_F32_LE, capture->channels,
      capture->sample_rate, false, capture->params);
  if (!capture->buffer) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to allocate capture buffer");
    return false;
  }

  capture->loop = pw_thread_loop_new("CDSP-Capture-Loop", NULL);
  if (!capture->loop) {
    pipewire_capture_close(capture);
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to create PipeWire thread loop");
    return false;
  }

  if (pw_thread_loop_start(capture->loop) < 0) {
    pw_thread_loop_destroy(capture->loop);
    capture->loop = NULL;
    pipewire_capture_close(capture);
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to start PipeWire thread loop");
    return false;
  }

  pw_thread_loop_lock(capture->loop);

  const char *node_name =
      capture->has_node_name ? capture->node_name : "cdsp-capture";
  const char *node_desc = capture->has_node_description
                              ? capture->node_description
                              : "CDSP Capture";
  const char *node_group =
      capture->has_node_group_name ? capture->node_group_name : "cdsp";

  struct pw_properties *props = pw_properties_new(
      PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
      PW_KEY_MEDIA_ROLE, "DSP", PW_KEY_APP_NAME, "CDSP", PW_KEY_NODE_NAME,
      node_name, PW_KEY_NODE_DESCRIPTION, node_desc, PW_KEY_NODE_GROUP,
      node_group, NULL);

  if (props) {
    char latency_str[64];
    snprintf(latency_str, sizeof(latency_str), "%d/%d", capture->chunk_size,
             capture->sample_rate);
    pw_properties_set(props, PW_KEY_NODE_LATENCY, latency_str);
    char rate_str[64];
    snprintf(rate_str, sizeof(rate_str), "1/%d", capture->sample_rate);
    pw_properties_set(props, PW_KEY_NODE_RATE, rate_str);
    // `device` ("default" is already mapped to "" in create) is a cdsp
    // extension: target.object with normal fallback. It takes precedence over
    // `autoconnect_to` (upstream semantics: dont-fallback + linger).
    if (capture->device[0] != '\0') {
      pw_properties_set(props, "target.object", capture->device);
    } else if (capture->has_autoconnect_to &&
               capture->autoconnect_to[0] != '\0') {
      pw_properties_set(props, "target.object", capture->autoconnect_to);
      pw_properties_set(props, "node.dont-fallback", "true");
      pw_properties_set(props, "node.linger", "true");
    }
    if (capture->loopback) {
      pw_properties_set(props, "stream.capture.sink", "true");
    }
  }

  capture->stream = pw_stream_new_simple(pw_thread_loop_get_loop(capture->loop),
                                         "CDSP-Capture-Stream", props,
                                         &capture_stream_events, capture);

  if (!capture->stream) {
    pw_thread_loop_unlock(capture->loop);
    pipewire_capture_close(capture);
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to create PipeWire stream");
    return false;
  }

  uint8_t buffer[1024];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  const struct spa_pod *params[1];
  struct spa_audio_info_raw info = {.format = SPA_AUDIO_FORMAT_F32_LE,
                                    .rate = (uint32_t)capture->sample_rate,
                                    .channels = (uint32_t)capture->channels};
  params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);

  uint32_t flags = PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS;
  if (capture->device[0] != '\0' || capture->has_autoconnect_to) {
    flags |= PW_STREAM_FLAG_AUTOCONNECT;
  }

  int rc = pw_stream_connect(capture->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                             flags, params, 1);

  pw_thread_loop_unlock(capture->loop);

  if (rc < 0) {
    // close() destroys the stream under the loop lock, stops and destroys the
    // loop and frees the buffer, leaving no dangling pointers behind.
    pipewire_capture_close(capture);
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to connect PipeWire stream");
    return false;
  }

  logger_info(&g_logger,
              "Opened PipeWire capture: device=%s, rate=%d, channels=%zu",
              capture->device[0] != '\0' ? capture->device : "default",
              capture->sample_rate, capture->channels);

  return true;
}

/**
 * @brief Read audio frames from the PipeWire capture device.
 *
 * @param ctx Pointer to the PipeWire capture instance.
 * @param frames Number of frames to read.
 * @param chunk Pointer to the audio chunk to fill.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return true if successful, false otherwise.
 */
static bool pipewire_capture_read(void *ctx, size_t frames,
                                  audio_chunk_t *chunk, backend_error_t *err) {
  pipewire_capture_t *capture = (pipewire_capture_t *)ctx;
  if (!capture)
    return false;
  return backend_buffer_read_chunk(capture->buffer, frames, chunk, err);
}

/**
 * @brief Get any pending sample rate change.
 *
 * @param ctx Pointer to the PipeWire capture instance.
 * @param out_rate Pointer to double to store the pending sample rate.
 * @return true if a rate change is pending, false otherwise.
 */
static bool pipewire_capture_get_pending_rate_change(void *ctx,
                                                     double *out_rate) {
  pipewire_capture_t *capture = (pipewire_capture_t *)ctx;
  if (!capture)
    return false;
  // Lock-free consume: called from the engine audio thread on every
  // iteration, so it must not take pw_thread_loop_lock().
  uint32_t rate = atomic_exchange_explicit(&capture->pending_rate_hz, 0u,
                                           memory_order_acq_rel);
  if (rate == 0)
    return false;
  backend_buffer_set_pending_rate_change(capture->buffer, false);
  if (out_rate)
    *out_rate = (double)rate;
  return true;
}

/**
 * @brief Check if pitch control is supported by the PipeWire capture backend.
 *
 * @param ctx Pointer to the PipeWire capture instance.
 * @return true if supported, false otherwise.
 */
static bool pipewire_capture_pitch_control_supported(void *ctx) {
  (void)ctx;
  return false;
}

/**
 * @brief Set the pitch multiplier for the PipeWire capture backend.
 *
 * @param ctx Pointer to the PipeWire capture instance.
 * @param multiplier The pitch multiplier.
 */
static void pipewire_capture_set_pitch(void *ctx, double multiplier) {
  (void)ctx;
  (void)multiplier;
}

/**
 * @brief Wait for the PipeWire capture device to have data available.
 *
 * @param ctx Pointer to the PipeWire capture instance.
 * @param timeout_ms Timeout in milliseconds.
 * @return true if data is available, false on timeout or error.
 */
static bool pipewire_capture_wait(void *ctx, uint32_t timeout_ms) {
  pipewire_capture_t *capture = (pipewire_capture_t *)ctx;
  if (!capture)
    return false;
  return backend_buffer_wait(capture->buffer, timeout_ms);
}

/**
 * @brief Stop the PipeWire capture stream.
 *
 * @param ctx Pointer to the PipeWire capture instance.
 */
static void pipewire_capture_stop(void *ctx) {
  pipewire_capture_t *capture = (pipewire_capture_t *)ctx;
  if (!capture)
    return;
  backend_buffer_set_state(capture->buffer, BACKEND_STREAM_STOPPED);
  if (capture->loop) {
    pw_thread_loop_lock(capture->loop);
    if (capture->stream) {
      pw_stream_set_active(capture->stream, false);
    }
    pw_thread_loop_unlock(capture->loop);
  }
}

/**
 * @brief Destroy the PipeWire capture backend instance.
 *
 * @param ctx Pointer to the PipeWire capture instance to destroy.
 */
static void pipewire_capture_destroy(void *ctx) {
  pipewire_capture_t *capture = (pipewire_capture_t *)ctx;
  if (capture) {
    pipewire_capture_close(capture);
    free(capture);
  }
}

/**
 * @brief Create a PipeWire capture backend instance.
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
pipewire_capture_create(const capture_device_config_t *config, int sample_rate,
                        int chunk_size, bool full_duplex,
                        processing_parameters_t *params, backend_error_t *err) {
  (void)full_duplex;
  (void)err;
  pipewire_capture_t *capture =
      (pipewire_capture_t *)calloc(1, sizeof(pipewire_capture_t));
  if (!capture)
    return NULL;

  if (config->cfg.pipewire.has_device &&
      strcmp(config->cfg.pipewire.device, "default") != 0) {
    snprintf(capture->device, sizeof(capture->device), "%s",
             config->cfg.pipewire.device);
  } else {
    capture->device[0] = '\0';
  }

  capture->sample_rate = sample_rate;
  capture->channels = config->cfg.pipewire.channels;
  capture->chunk_size = chunk_size;

  if (config->cfg.pipewire.has_node_name) {
    snprintf(capture->node_name, sizeof(capture->node_name), "%s",
             config->cfg.pipewire.node_name);
    capture->has_node_name = true;
  }
  if (config->cfg.pipewire.has_node_description) {
    snprintf(capture->node_description, sizeof(capture->node_description), "%s",
             config->cfg.pipewire.node_description);
    capture->has_node_description = true;
  }
  if (config->cfg.pipewire.has_node_group_name) {
    snprintf(capture->node_group_name, sizeof(capture->node_group_name), "%s",
             config->cfg.pipewire.node_group_name);
    capture->has_node_group_name = true;
  }
  if (config->cfg.pipewire.has_autoconnect_to) {
    snprintf(capture->autoconnect_to, sizeof(capture->autoconnect_to), "%s",
             config->cfg.pipewire.autoconnect_to);
    capture->has_autoconnect_to = true;
  }
  capture->loopback = config->cfg.pipewire.loopback;
  if (capture->device[0] != '\0' && capture->has_autoconnect_to &&
      capture->autoconnect_to[0] != '\0') {
    logger_warn(
        &g_logger,
        "PipeWire capture: both device '%s' and autoconnect_to '%s' set; "
        "using device (autoconnect_to ignored)",
        capture->device, capture->autoconnect_to);
  }
  capture->params = params;

  capture_backend_t *backend =
      (capture_backend_t *)calloc(1, sizeof(capture_backend_t));
  if (!backend) {
    free(capture);
    return NULL;
  }
  backend->ctx = capture;
  backend->vtable = &g_pipewire_capture_vtable;
  backend->is_realtime = true;
  return backend;
}

const capture_backend_vtable_t g_pipewire_capture_vtable = {
    .create = pipewire_capture_create,
    .open = pipewire_capture_open,
    .read = pipewire_capture_read,
    .close = pipewire_capture_close,
    .get_pending_rate_change = pipewire_capture_get_pending_rate_change,
    .is_pitch_control_supported = pipewire_capture_pitch_control_supported,
    .set_pitch = pipewire_capture_set_pitch,
    .wait_for_data = pipewire_capture_wait,
    .stop = pipewire_capture_stop,
    .destroy = pipewire_capture_destroy};

// MARK: - Playback Backend implementation

/**
 * @brief Close the PipeWire playback device.
 *
 * @param ctx Pointer to the PipeWire playback instance.
 */
static void pipewire_playback_close(void *ctx) {
  pipewire_playback_t *playback = (pipewire_playback_t *)ctx;
  if (!playback)
    return;
  if (playback->loop) {
    // Wait for the ring buffer to drain before closing the stream, ensuring
    // all remaining audio is played back (cdsp addition; upstream discards the
    // ring on EndOfStream). Only wait while something can actually drain it:
    // a paused buffer renders silence without consuming, and a stream that is
    // not STREAMING (e.g. unlinked with node.dont-fallback) has no callbacks.
    int retries = 200; // wait up to 200ms
    while (backend_buffer_get_state(playback->buffer) ==
               BACKEND_STREAM_RUNNING &&
           atomic_load_explicit(&playback->streaming, memory_order_acquire) &&
           backend_buffer_get_available_read_frames(playback->buffer) > 0 &&
           retries-- > 0) {
      cdsp_sleep_ms(1);
    }

    backend_buffer_set_state(playback->buffer, BACKEND_STREAM_STOPPED);

    pw_thread_loop_lock(playback->loop);
    if (playback->stream) {
      pw_stream_destroy(playback->stream);
      playback->stream = NULL;
    }
    pw_thread_loop_unlock(playback->loop);

    pw_thread_loop_stop(playback->loop);
    pw_thread_loop_destroy(playback->loop);
    playback->loop = NULL;
  }

  backend_buffer_free(playback->buffer);
  playback->buffer = NULL;
}

/**
 * @brief Open the PipeWire playback device.
 *
 * @param ctx Pointer to the PipeWire playback instance.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return true if successful, false otherwise.
 */
static bool pipewire_playback_open(void *ctx, backend_error_t *err) {
  pipewire_playback_t *playback = (pipewire_playback_t *)ctx;
  if (!playback)
    return false;
  ensure_pw_init();

  // Create and configure the ring buffer before the stream exists: with
  // PW_STREAM_FLAG_RT_PROCESS the process callback runs on the PipeWire data
  // thread as soon as negotiation completes, independently of this thread and
  // of the thread-loop lock. Rendering from a missing buffer would publish a
  // PipeWire buffer that was never written. Upstream likewise builds its ring
  // before registering the listener and connecting (device.rs:374-383).
  size_t pb_min_frames = (size_t)ceil((double)playback->sample_rate * 0.025);
  size_t target_level = playback->target_level > 0
                            ? playback->target_level
                            : (size_t)playback->chunk_size;
  size_t pb_prefill_frames = target_level > (size_t)(3 * playback->chunk_size)
                                 ? target_level
                                 : (size_t)(3 * playback->chunk_size);
  size_t pb_frames_needed =
      pb_prefill_frames + (size_t)(4 * playback->chunk_size);
  if (pb_frames_needed < pb_min_frames)
    pb_frames_needed = pb_min_frames;

  playback->buffer = backend_buffer_create(
      pb_frames_needed, BINARY_SAMPLE_FORMAT_F32_LE, playback->channels,
      playback->sample_rate, false, playback->params);

  if (!playback->buffer) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to allocate playback buffer");
    return false;
  }
  backend_buffer_set_state(playback->buffer, BACKEND_STREAM_RUNNING);
  backend_buffer_set_target_level(playback->buffer, target_level);

  playback->loop = pw_thread_loop_new("CDSP-Playback-Loop", NULL);
  if (!playback->loop) {
    pipewire_playback_close(playback);
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to create PipeWire thread loop");
    return false;
  }

  if (pw_thread_loop_start(playback->loop) < 0) {
    pw_thread_loop_destroy(playback->loop);
    playback->loop = NULL;
    pipewire_playback_close(playback);
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to start PipeWire thread loop");
    return false;
  }

  pw_thread_loop_lock(playback->loop);

  const char *node_name =
      playback->has_node_name ? playback->node_name : "cdsp-playback";
  const char *node_desc = playback->has_node_description
                              ? playback->node_description
                              : "CDSP Playback";
  const char *node_group =
      playback->has_node_group_name ? playback->node_group_name : "cdsp";

  struct pw_properties *props = pw_properties_new(
      PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
      PW_KEY_MEDIA_ROLE, "DSP", PW_KEY_APP_NAME, "CDSP", PW_KEY_NODE_NAME,
      node_name, PW_KEY_NODE_DESCRIPTION, node_desc, PW_KEY_NODE_GROUP,
      node_group, NULL);

  if (props) {
    char latency_str[64];
    snprintf(latency_str, sizeof(latency_str), "%d/%d", playback->chunk_size,
             playback->sample_rate);
    pw_properties_set(props, PW_KEY_NODE_LATENCY, latency_str);
    char rate_str[64];
    snprintf(rate_str, sizeof(rate_str), "1/%d", playback->sample_rate);
    pw_properties_set(props, PW_KEY_NODE_RATE, rate_str);
    // `device` ("default" is already mapped to "" in create) is a cdsp
    // extension: target.object with normal fallback. It takes precedence over
    // `autoconnect_to` (upstream semantics: dont-fallback + linger).
    if (playback->device[0] != '\0') {
      pw_properties_set(props, "target.object", playback->device);
    } else if (playback->has_autoconnect_to &&
               playback->autoconnect_to[0] != '\0') {
      pw_properties_set(props, "target.object", playback->autoconnect_to);
      pw_properties_set(props, "node.dont-fallback", "true");
      pw_properties_set(props, "node.linger", "true");
    }
  }

  playback->stream = pw_stream_new_simple(
      pw_thread_loop_get_loop(playback->loop), "CDSP-Playback-Stream", props,
      &playback_stream_events, playback);

  if (!playback->stream) {
    pw_thread_loop_unlock(playback->loop);
    pipewire_playback_close(playback);
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to create PipeWire stream");
    return false;
  }

  uint8_t buffer[1024];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  const struct spa_pod *params[1];
  struct spa_audio_info_raw info = {.format = SPA_AUDIO_FORMAT_F32_LE,
                                    .rate = (uint32_t)playback->sample_rate,
                                    .channels = (uint32_t)playback->channels};
  params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);

  uint32_t flags = PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS;
  if (playback->device[0] != '\0' || playback->has_autoconnect_to) {
    flags |= PW_STREAM_FLAG_AUTOCONNECT;
  }

  int rc = pw_stream_connect(playback->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                             flags, params, 1);

  pw_thread_loop_unlock(playback->loop);

  if (rc < 0) {
    // close() destroys the stream under the loop lock, stops and destroys the
    // loop and frees the buffer, leaving no dangling pointers behind.
    pipewire_playback_close(playback);
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to connect PipeWire stream");
    return false;
  }

  logger_info(&g_logger,
              "Opened PipeWire playback: device=%s, rate=%d, channels=%zu",
              playback->device[0] != '\0' ? playback->device : "default",
              playback->sample_rate, playback->channels);

  return true;
}

/**
 * @brief Write an audio chunk to the PipeWire playback device.
 *
 * @param ctx Pointer to the PipeWire playback instance.
 * @param chunk Pointer to the audio chunk to write.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return true if successful, false otherwise.
 */
static bool pipewire_playback_write(void *ctx, const audio_chunk_t *chunk,
                                    backend_error_t *err) {
  pipewire_playback_t *playback = (pipewire_playback_t *)ctx;
  if (!playback)
    return false;
  // Match upstream RingBufferFeeder (ringbuffer.rs:22, 37-68): up to
  // PUSH_RETRIES = 16 waits of chunksize / samplerate / 2 before dropping the
  // chunk. The wait is a semaphore timed wait that the RT callback signals on
  // every consume, so whole-millisecond rounding only bounds the worst case.
  uint32_t sleep_ms = (uint32_t)lround((double)playback->chunk_size * 1000.0 /
                                       (double)playback->sample_rate / 2.0);
  if (sleep_ms < 1)
    sleep_ms = 1;
  return backend_buffer_write_chunk(playback->buffer, chunk, sleep_ms,
                                    PIPEWIRE_PLAYBACK_PUSH_RETRIES, err);
}

/**
 * @brief Get the current buffer level of the PipeWire playback backend.
 *
 * @param ctx Pointer to the PipeWire playback instance.
 * @return The buffer level in frames.
 */
static size_t pipewire_playback_get_buffer_level(void *ctx) {
  pipewire_playback_t *playback = (pipewire_playback_t *)ctx;
  if (!playback)
    return 0;
  return backend_buffer_get_level(playback->buffer);
}

/**
 * @brief Get any pending sample rate change.
 *
 * @param ctx Pointer to the PipeWire playback instance.
 * @param out_rate Pointer to double to store the pending sample rate.
 * @return true if a rate change is pending, false otherwise.
 */
static bool pipewire_playback_get_pending_rate_change(void *ctx,
                                                      double *out_rate) {
  pipewire_playback_t *playback = (pipewire_playback_t *)ctx;
  if (!playback)
    return false;
  // Lock-free consume: called from the engine audio thread on every
  // iteration, so it must not take pw_thread_loop_lock().
  uint32_t rate = atomic_exchange_explicit(&playback->pending_rate_hz, 0u,
                                           memory_order_acq_rel);
  if (rate == 0)
    return false;
  backend_buffer_set_pending_rate_change(playback->buffer, false);
  if (out_rate)
    *out_rate = (double)rate;
  return true;
}

/**
 * @brief Prefill the PipeWire playback buffer with silence.
 *
 * @param ctx Pointer to the PipeWire playback instance.
 * @param frames Number of frames of silence to prefill.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return true if successful, false otherwise.
 */
static bool pipewire_playback_prefill_silence(void *ctx, size_t frames,
                                              backend_error_t *err) {
  pipewire_playback_t *playback = (pipewire_playback_t *)ctx;
  (void)err;
  if (!playback)
    return false;

  backend_buffer_prefill_silence(playback->buffer, frames, 0x00);
  return true;
}

/**
 * @brief Check if PipeWire playback is currently paused.
 *
 * @param ctx Pointer to the PipeWire playback instance.
 * @return true if paused, false otherwise.
 */
static bool pipewire_playback_get_is_paused(void *ctx) {
  pipewire_playback_t *playback = (pipewire_playback_t *)ctx;
  if (!playback)
    return false;
  return backend_buffer_get_state(playback->buffer) == BACKEND_STREAM_PAUSED;
}

/**
 * @brief Set the paused state of the PipeWire playback backend.
 *
 * @param ctx Pointer to the PipeWire playback instance.
 * @param paused true to pause, false to resume.
 */
static void pipewire_playback_set_is_paused(void *ctx, bool paused) {
  pipewire_playback_t *playback = (pipewire_playback_t *)ctx;
  if (!playback)
    return;
  backend_buffer_set_state(playback->buffer, paused ? BACKEND_STREAM_PAUSED
                                                    : BACKEND_STREAM_RUNNING);
}

/**
 * @brief Stop the PipeWire playback stream.
 *
 * @param ctx Pointer to the PipeWire playback instance.
 */
static void pipewire_playback_stop(void *ctx) {
  pipewire_playback_t *playback = (pipewire_playback_t *)ctx;
  if (!playback)
    return;
  backend_buffer_set_state(playback->buffer, BACKEND_STREAM_STOPPED);
  if (playback->loop) {
    pw_thread_loop_lock(playback->loop);
    if (playback->stream) {
      pw_stream_set_active(playback->stream, false);
    }
    pw_thread_loop_unlock(playback->loop);
  }
}

/**
 * @brief Destroy the PipeWire playback backend instance.
 *
 * @param ctx Pointer to the PipeWire playback instance to destroy.
 */
static void pipewire_playback_destroy(void *ctx) {
  pipewire_playback_t *playback = (pipewire_playback_t *)ctx;
  if (playback) {
    pipewire_playback_close(playback);
    free(playback);
  }
}

/**
 * @brief Create a PipeWire playback backend instance.
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
static playback_backend_t *pipewire_playback_create(
    const playback_device_config_t *config, int sample_rate, int chunk_size,
    bool full_duplex, processing_parameters_t *params, backend_error_t *err) {
  (void)full_duplex;
  (void)err;
  pipewire_playback_t *playback =
      (pipewire_playback_t *)calloc(1, sizeof(pipewire_playback_t));
  if (!playback)
    return NULL;

  if (config->cfg.pipewire.has_device &&
      strcmp(config->cfg.pipewire.device, "default") != 0) {
    snprintf(playback->device, sizeof(playback->device), "%s",
             config->cfg.pipewire.device);
  } else {
    playback->device[0] = '\0';
  }

  playback->sample_rate = sample_rate;
  playback->channels = config->cfg.pipewire.channels;
  playback->chunk_size = chunk_size;
  playback->target_level = config->cfg.pipewire.has_target_level
                               ? (size_t)config->cfg.pipewire.target_level
                               : (size_t)chunk_size;

  if (config->cfg.pipewire.has_node_name) {
    snprintf(playback->node_name, sizeof(playback->node_name), "%s",
             config->cfg.pipewire.node_name);
    playback->has_node_name = true;
  }
  if (config->cfg.pipewire.has_node_description) {
    snprintf(playback->node_description, sizeof(playback->node_description),
             "%s", config->cfg.pipewire.node_description);
    playback->has_node_description = true;
  }
  if (config->cfg.pipewire.has_node_group_name) {
    snprintf(playback->node_group_name, sizeof(playback->node_group_name), "%s",
             config->cfg.pipewire.node_group_name);
    playback->has_node_group_name = true;
  }
  if (config->cfg.pipewire.has_autoconnect_to) {
    snprintf(playback->autoconnect_to, sizeof(playback->autoconnect_to), "%s",
             config->cfg.pipewire.autoconnect_to);
    playback->has_autoconnect_to = true;
  }
  if (playback->device[0] != '\0' && playback->has_autoconnect_to &&
      playback->autoconnect_to[0] != '\0') {
    logger_warn(
        &g_logger,
        "PipeWire playback: both device '%s' and autoconnect_to '%s' set; "
        "using device (autoconnect_to ignored)",
        playback->device, playback->autoconnect_to);
  }
  playback->params = params;

  playback_backend_t *backend =
      (playback_backend_t *)calloc(1, sizeof(playback_backend_t));
  if (!backend) {
    free(playback);
    return NULL;
  }
  backend->ctx = playback;
  backend->vtable = &g_pipewire_playback_vtable;
  return backend;
}

const playback_backend_vtable_t g_pipewire_playback_vtable = {
    .create = pipewire_playback_create,
    .open = pipewire_playback_open,
    .write = pipewire_playback_write,
    .close = pipewire_playback_close,
    .get_buffer_level = pipewire_playback_get_buffer_level,
    .get_pending_rate_change = pipewire_playback_get_pending_rate_change,
    .prefill_silence = pipewire_playback_prefill_silence,
    .get_is_paused = pipewire_playback_get_is_paused,
    .set_is_paused = pipewire_playback_set_is_paused,
    .stop = pipewire_playback_stop,
    .destroy = pipewire_playback_destroy};

#endif // ENABLE_PIPEWIRE
