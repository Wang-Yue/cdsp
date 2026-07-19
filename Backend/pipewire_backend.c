#if defined(ENABLE_PIPEWIRE)

#include "pipewire_backend.h"

#include <pipewire/pipewire.h>
#include <pipewire/stream.h>
#include <spa/param/audio/format-utils.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "Audio/sample_conversion.h"
#include "Engine/cdsp_sem.h"
#include "Logging/app_logger.h"
#include "Utils/cdsp_time.h"
#include "Utils/lock_free_ring_buffer.h"

static const logger_t g_logger = {"dsp.backend.pipewire"};

struct pipewire_capture {
  char device[256];
  int sample_rate;
  int channels;
  int chunk_size;

  char node_name[256];
  char node_description[256];
  char node_group_name[256];
  char autoconnect_to[256];
  bool has_node_name;
  bool has_node_description;
  bool has_node_group_name;
  bool has_autoconnect_to;

  struct pw_thread_loop* loop;
  struct pw_context* context;
  struct pw_stream* stream;

  spsc_audio_ring_buffer_t* ring;
  float* decode_buf;
  size_t decode_buf_size;
  cdsp_sem_t semaphore;
  bool stopped;
};

struct pipewire_playback {
  char device[256];
  int sample_rate;
  int channels;
  int chunk_size;

  char node_name[256];
  char node_description[256];
  char node_group_name[256];
  char autoconnect_to[256];
  bool has_node_name;
  bool has_node_description;
  bool has_node_group_name;
  bool has_autoconnect_to;

  struct pw_thread_loop* loop;
  struct pw_context* context;
  struct pw_stream* stream;

  spsc_audio_ring_buffer_t* ring;
  float* encode_buf;
  size_t encode_buf_size;
  _Atomic bool paused;
  bool stopped;
};

// MARK: - PipeWire Callbacks

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
static void on_capture_process(void* data) {
  pipewire_capture_t* c = (pipewire_capture_t*)data;
  struct pw_buffer* b = pw_stream_dequeue_buffer(c->stream);
  if (!b) return;

  struct spa_buffer* buf = b->buffer;
  const float* src = (const float*)buf->datas[0].data;
  if (src) {
    size_t offset = buf->datas[0].chunk->offset;
    size_t size = buf->datas[0].chunk->size;
    size_t frames = size / (sizeof(float) * c->channels);

    spsc_audio_ring_buffer_write(c->ring, src + (offset / sizeof(float)),
                                 frames * c->channels, 1);
    if (c->semaphore) cdsp_sem_signal(c->semaphore);
  }

  pw_stream_queue_buffer(c->stream, b);
}

static void on_capture_stream_state_changed(void* data,
                                            enum pw_stream_state old,
                                            enum pw_stream_state state,
                                            const char* error) {
  (void)data;
  logger_info(&g_logger,
              "Capture stream state changed from %s to %s (error: %s)",
              pw_stream_state_as_string(old), pw_stream_state_as_string(state),
              error ? error : "none");
}

static const struct pw_stream_events capture_stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_capture_stream_state_changed,
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
static void on_playback_process(void* data) {
  pipewire_playback_t* p = (pipewire_playback_t*)data;
  struct pw_buffer* b = pw_stream_dequeue_buffer(p->stream);
  if (!b) return;

  struct spa_buffer* buf = b->buffer;
  float* dst = (float*)buf->datas[0].data;

  if (dst) {
    size_t max_bytes = buf->datas[0].maxsize;
    size_t frame_size = sizeof(float) * p->channels;
    size_t max_frames = max_bytes / frame_size;

    size_t requested = max_frames * p->channels;
    size_t consumed = spsc_audio_ring_buffer_consume(p->ring, dst, requested);

    if (consumed < requested) {
      // Fill remaining with silence
      memset(dst + consumed, 0, (requested - consumed) * sizeof(float));
    }

    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->size = max_frames * frame_size;
    buf->datas[0].chunk->stride = frame_size;
  }

  pw_stream_queue_buffer(p->stream, b);
}

static void on_playback_stream_state_changed(void* data,
                                             enum pw_stream_state old,
                                             enum pw_stream_state state,
                                             const char* error) {
  (void)data;
  logger_info(&g_logger,
              "Playback stream state changed from %s to %s (error: %s)",
              pw_stream_state_as_string(old), pw_stream_state_as_string(state),
              error ? error : "none");
}

static const struct pw_stream_events playback_stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_playback_stream_state_changed,
    .process = on_playback_process,
};

// MARK: - Capture Backend implementation

/**
 * @brief Close the PipeWire capture device.
 *
 * @param ctx Pointer to the PipeWire capture instance.
 */
static void pipewire_capture_close(void* ctx) {
  pipewire_capture_t* capture = (pipewire_capture_t*)ctx;
  if (!capture) return;
  if (capture->loop) {
    pw_thread_loop_lock(capture->loop);
    if (capture->stream) {
      pw_stream_destroy(capture->stream);
      capture->stream = NULL;
    }
    if (capture->context) {
      pw_context_destroy(capture->context);
      capture->context = NULL;
    }
    pw_thread_loop_unlock(capture->loop);

    pw_thread_loop_stop(capture->loop);
    pw_thread_loop_destroy(capture->loop);
    capture->loop = NULL;
  }

  if (capture->ring) {
    spsc_audio_ring_buffer_free(capture->ring);
    capture->ring = NULL;
  }

  if (capture->semaphore) {
    cdsp_sem_destroy(capture->semaphore);
    capture->semaphore = NULL;
  }
  if (capture->decode_buf) {
    free(capture->decode_buf);
    capture->decode_buf = NULL;
  }
}

/**
 * @brief Open the PipeWire capture device.
 *
 * @param ctx Pointer to the PipeWire capture instance.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return true if successful, false otherwise.
 */

static bool pipewire_capture_open(void* ctx, backend_error_t* err) {
  pipewire_capture_t* capture = (pipewire_capture_t*)ctx;
  if (!capture) return false;
  pw_init(NULL, NULL);

  capture->loop = pw_thread_loop_new("CDSP-Capture-Loop", NULL);
  if (!capture->loop) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to create PipeWire thread loop");
    return false;
  }

  if (pw_thread_loop_start(capture->loop) < 0) {
    pw_thread_loop_destroy(capture->loop);
    capture->loop = NULL;
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to start PipeWire thread loop");
    return false;
  }

  pw_thread_loop_lock(capture->loop);

  capture->context =
      pw_context_new(pw_thread_loop_get_loop(capture->loop), NULL, 0);
  if (!capture->context) {
    pw_thread_loop_unlock(capture->loop);
    pw_thread_loop_stop(capture->loop);
    pw_thread_loop_destroy(capture->loop);
    capture->loop = NULL;
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to create PipeWire context");
    return false;
  }

  const char* node_name =
      capture->has_node_name ? capture->node_name : "cdsp-capture";
  const char* node_desc = capture->has_node_description
                              ? capture->node_description
                              : "CDSP Capture";
  const char* node_group =
      capture->has_node_group_name ? capture->node_group_name : "cdsp";

  struct pw_properties* props = pw_properties_new(
      PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
      PW_KEY_MEDIA_ROLE, "DSP", PW_KEY_APP_NAME, "CDSP", PW_KEY_NODE_NAME,
      node_name, PW_KEY_NODE_DESCRIPTION, node_desc, PW_KEY_NODE_GROUP,
      node_group, NULL);

  if (props) {
    char latency_str[64];
    snprintf(latency_str, sizeof(latency_str), "%d/%d", capture->chunk_size,
             capture->sample_rate);
    pw_properties_set(props, PW_KEY_NODE_LATENCY, latency_str);
    if (capture->device[0] != '\0' && strcmp(capture->device, "default") != 0) {
      pw_properties_set(props, "target.object", capture->device);
    } else if (capture->has_autoconnect_to &&
               capture->autoconnect_to[0] != '\0') {
      pw_properties_set(props, "target.object", capture->autoconnect_to);
    }
  }

  capture->stream = pw_stream_new_simple(pw_thread_loop_get_loop(capture->loop),
                                         "CDSP-Capture-Stream", props,
                                         &capture_stream_events, capture);

  if (!capture->stream) {
    pw_context_destroy(capture->context);
    pw_thread_loop_unlock(capture->loop);
    pw_thread_loop_stop(capture->loop);
    pw_thread_loop_destroy(capture->loop);
    capture->loop = NULL;
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to create PipeWire stream");
    return false;
  }

  uint8_t buffer[1024];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  const struct spa_pod* params[1];
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
    pw_thread_loop_lock(capture->loop);
    pw_stream_destroy(capture->stream);
    pw_context_destroy(capture->context);
    pw_thread_loop_unlock(capture->loop);
    pw_thread_loop_stop(capture->loop);
    pw_thread_loop_destroy(capture->loop);
    capture->loop = NULL;
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to connect PipeWire stream");
    return false;
  }

  // Create large SPSC ring buffer (8 blocks headroom)
  capture->ring = spsc_audio_ring_buffer_create(capture->chunk_size *
                                                capture->channels * 64);
  capture->decode_buf_size = capture->chunk_size * capture->channels;
  capture->decode_buf = (float*)calloc(capture->decode_buf_size, sizeof(float));
  capture->semaphore = cdsp_sem_create();

  if (!capture->ring || !capture->decode_buf) {
    pipewire_capture_close(capture);
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to allocate capture buffers");
    return false;
  }

  logger_info(&g_logger,
              "Opened PipeWire capture: device=%s, rate=%d, channels=%d",
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
static bool pipewire_capture_read(void* ctx, size_t frames,
                                  audio_chunk_t* chunk, backend_error_t* err) {
  pipewire_capture_t* capture = (pipewire_capture_t*)ctx;
  if (!capture) return false;
  if (audio_chunk_get_channels(chunk) < (size_t)capture->channels) {
    if (err) {
      backend_error_init(
          err, BACKEND_ERROR_INVALID_CHANNELS,
          "Chunk channels count does not match capture channels");
    }
    return false;
  }
  size_t requested = frames * capture->channels;
  if (requested > capture->decode_buf_size) {
    float* new_buf =
        (float*)realloc(capture->decode_buf, requested * sizeof(float));
    if (!new_buf) {
      if (err) {
        backend_error_init(
            err, BACKEND_ERROR_READ_ERROR,
            "Failed to reallocate PipeWire capture decode buffer");
      }
      return false;
    }
    capture->decode_buf = new_buf;
    capture->decode_buf_size = requested;
  }

  if (spsc_audio_ring_buffer_get_available_to_read(capture->ring) < requested) {
    if (err) {
      backend_error_init(err, BACKEND_ERROR_NONE, "");
    }
    return false;
  }

  size_t consumed = spsc_audio_ring_buffer_consume(
      capture->ring, capture->decode_buf, requested);
  if (consumed < requested) {
    memset(capture->decode_buf + consumed, 0,
           (requested - consumed) * sizeof(float));
  }

  for (size_t f = 0; f < frames; f++) {
    for (int c = 0; c < capture->channels; c++) {
      audio_chunk_get_channel(chunk, c)[f] =
          pcm_sample_decode_f32(capture->decode_buf[f * capture->channels + c]);
    }
  }

  audio_chunk_set_valid_frames(chunk, frames);
  return true;
}

/**
 * @brief Get any pending sample rate change.
 *
 * @param ctx Pointer to the PipeWire capture instance.
 * @param out_rate Pointer to double to store the pending sample rate.
 * @return true if a rate change is pending, false otherwise.
 */
static bool pipewire_capture_get_pending_rate_change(void* ctx,
                                                     double* out_rate) {
  (void)ctx;
  (void)out_rate;
  return false;
}

/**
 * @brief Check if pitch control is supported by the PipeWire capture backend.
 *
 * @param ctx Pointer to the PipeWire capture instance.
 * @return true if supported, false otherwise.
 */
static bool pipewire_capture_pitch_control_supported(void* ctx) {
  (void)ctx;
  return false;
}

/**
 * @brief Set the pitch multiplier for the PipeWire capture backend.
 *
 * @param ctx Pointer to the PipeWire capture instance.
 * @param multiplier The pitch multiplier.
 */
static void pipewire_capture_set_pitch(void* ctx, double multiplier) {
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
static bool pipewire_capture_wait(void* ctx, uint32_t timeout_ms) {
  pipewire_capture_t* capture = (pipewire_capture_t*)ctx;
  if (!capture || !capture->ring || !capture->semaphore) return false;
  if (capture->stopped) return false;
  return cdsp_sem_timedwait(capture->semaphore, timeout_ms);
}

/**
 * @brief Stop the PipeWire capture stream.
 *
 * @param ctx Pointer to the PipeWire capture instance.
 */
static void pipewire_capture_stop(void* ctx) {
  pipewire_capture_t* capture = (pipewire_capture_t*)ctx;
  if (!capture) return;
  if (capture->loop) {
    pw_thread_loop_lock(capture->loop);
    capture->stopped = true;
    if (capture->stream) {
      pw_stream_set_active(capture->stream, false);
    }
    if (capture->semaphore) {
      cdsp_sem_signal(capture->semaphore);
    }
    pw_thread_loop_unlock(capture->loop);
  }
}

/**
 * @brief Destroy the PipeWire capture backend instance.
 *
 * @param ctx Pointer to the PipeWire capture instance to destroy.
 */
static void pipewire_capture_destroy(void* ctx) {
  pipewire_capture_t* capture = (pipewire_capture_t*)ctx;
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
static capture_backend_t* pipewire_capture_create(
    const capture_device_config_t* config, int sample_rate, int chunk_size,
    bool full_duplex, processing_parameters_t* params, backend_error_t* err) {
  (void)full_duplex;
  (void)params;
  (void)err;
  pipewire_capture_t* capture =
      (pipewire_capture_t*)calloc(1, sizeof(pipewire_capture_t));
  if (!capture) return NULL;

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

  capture_backend_t* backend =
      (capture_backend_t*)calloc(1, sizeof(capture_backend_t));
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
static void pipewire_playback_close(void* ctx) {
  pipewire_playback_t* playback = (pipewire_playback_t*)ctx;
  if (!playback) return;
  if (playback->loop) {
    // Wait for the ring buffer to drain before closing the stream,
    // ensuring all remaining audio is played back.
    int retries = 200;  // wait up to 200ms
    while (!playback->stopped && playback->ring &&
           spsc_audio_ring_buffer_get_available_to_read(playback->ring) > 0 &&
           retries-- > 0) {
      cdsp_sleep_ms(1);
    }

    pw_thread_loop_lock(playback->loop);
    if (playback->stream) {
      pw_stream_destroy(playback->stream);
      playback->stream = NULL;
    }
    if (playback->context) {
      pw_context_destroy(playback->context);
      playback->context = NULL;
    }
    pw_thread_loop_unlock(playback->loop);

    pw_thread_loop_stop(playback->loop);
    pw_thread_loop_destroy(playback->loop);
    playback->loop = NULL;
  }

  if (playback->ring) {
    spsc_audio_ring_buffer_free(playback->ring);
    playback->ring = NULL;
  }
  if (playback->encode_buf) {
    free(playback->encode_buf);
    playback->encode_buf = NULL;
  }
}

/**
 * @brief Open the PipeWire playback device.
 *
 * @param ctx Pointer to the PipeWire playback instance.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return true if successful, false otherwise.
 */
static bool pipewire_playback_open(void* ctx, backend_error_t* err) {
  pipewire_playback_t* playback = (pipewire_playback_t*)ctx;
  if (!playback) return false;
  pw_init(NULL, NULL);

  playback->loop = pw_thread_loop_new("CDSP-Playback-Loop", NULL);
  if (!playback->loop) {
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to create PipeWire thread loop");
    return false;
  }

  if (pw_thread_loop_start(playback->loop) < 0) {
    pw_thread_loop_destroy(playback->loop);
    playback->loop = NULL;
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to start PipeWire thread loop");
    return false;
  }

  pw_thread_loop_lock(playback->loop);

  playback->context =
      pw_context_new(pw_thread_loop_get_loop(playback->loop), NULL, 0);
  if (!playback->context) {
    pw_thread_loop_unlock(playback->loop);
    pw_thread_loop_stop(playback->loop);
    pw_thread_loop_destroy(playback->loop);
    playback->loop = NULL;
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to create PipeWire context");
    return false;
  }

  const char* node_name =
      playback->has_node_name ? playback->node_name : "cdsp-playback";
  const char* node_desc = playback->has_node_description
                              ? playback->node_description
                              : "CDSP Playback";
  const char* node_group =
      playback->has_node_group_name ? playback->node_group_name : "cdsp";

  struct pw_properties* props = pw_properties_new(
      PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
      PW_KEY_MEDIA_ROLE, "DSP", PW_KEY_APP_NAME, "CDSP", PW_KEY_NODE_NAME,
      node_name, PW_KEY_NODE_DESCRIPTION, node_desc, PW_KEY_NODE_GROUP,
      node_group, NULL);

  if (props) {
    char latency_str[64];
    snprintf(latency_str, sizeof(latency_str), "%d/%d", playback->chunk_size,
             playback->sample_rate);
    pw_properties_set(props, PW_KEY_NODE_LATENCY, latency_str);
    if (playback->device[0] != '\0' &&
        strcmp(playback->device, "default") != 0) {
      pw_properties_set(props, "target.object", playback->device);
    } else if (playback->has_autoconnect_to &&
               playback->autoconnect_to[0] != '\0') {
      pw_properties_set(props, "target.object", playback->autoconnect_to);
    }
  }

  playback->stream = pw_stream_new_simple(
      pw_thread_loop_get_loop(playback->loop), "CDSP-Playback-Stream", props,
      &playback_stream_events, playback);

  if (!playback->stream) {
    pw_context_destroy(playback->context);
    pw_thread_loop_unlock(playback->loop);
    pw_thread_loop_stop(playback->loop);
    pw_thread_loop_destroy(playback->loop);
    playback->loop = NULL;
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to create PipeWire stream");
    return false;
  }

  uint8_t buffer[1024];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  const struct spa_pod* params[1];
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
    pw_thread_loop_lock(playback->loop);
    pw_stream_destroy(playback->stream);
    pw_context_destroy(playback->context);
    pw_thread_loop_unlock(playback->loop);
    pw_thread_loop_stop(playback->loop);
    pw_thread_loop_destroy(playback->loop);
    playback->loop = NULL;
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to connect PipeWire stream");
    return false;
  }

  playback->ring = spsc_audio_ring_buffer_create(playback->chunk_size *
                                                 playback->channels * 64);
  playback->encode_buf_size = playback->chunk_size * playback->channels;
  playback->encode_buf =
      (float*)calloc(playback->encode_buf_size, sizeof(float));

  if (!playback->ring || !playback->encode_buf) {
    pipewire_playback_close(playback);
    if (err)
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "Failed to allocate playback buffers");
    return false;
  }
  playback->paused = false;

  logger_info(&g_logger,
              "Opened PipeWire playback: device=%s, rate=%d, channels=%d",
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
static bool pipewire_playback_write(void* ctx, const audio_chunk_t* chunk,
                                    backend_error_t* err) {
  pipewire_playback_t* playback = (pipewire_playback_t*)ctx;
  if (!playback) return false;
  if (audio_chunk_get_channels(chunk) < (size_t)playback->channels) {
    if (err) {
      backend_error_init(
          err, BACKEND_ERROR_INVALID_CHANNELS,
          "Chunk channels count does not match playback channels");
    }
    return false;
  }
  if (atomic_load_explicit(&playback->paused, memory_order_acquire))
    return true;
  (void)err;

  size_t frames = audio_chunk_get_valid_frames(chunk);
  size_t requested = frames * playback->channels;
  if (requested > playback->encode_buf_size) {
    float* new_buf =
        (float*)realloc(playback->encode_buf, requested * sizeof(float));
    if (!new_buf) {
      if (err) {
        backend_error_init(
            err, BACKEND_ERROR_WRITE_ERROR,
            "Failed to reallocate PipeWire playback encode buffer");
      }
      return false;
    }
    playback->encode_buf = new_buf;
    playback->encode_buf_size = requested;
  }

  for (size_t f = 0; f < frames; f++) {
    for (int c = 0; c < playback->channels; c++) {
      playback->encode_buf[f * playback->channels + c] =
          pcm_sample_encode_f32(audio_chunk_get_channel(chunk, c)[f]);
    }
  }

  // Wait until there is space in the SPSC ring buffer to prevent overwriting
  // oldest data. This blocks the writer thread (with a timeout) if the consumer
  // (PipeWire thread) is slower.
  int retries = 2000;
  while (!playback->stopped &&
         spsc_audio_ring_buffer_get_available_to_read(playback->ring) +
                 requested >
             spsc_audio_ring_buffer_get_capacity(playback->ring)) {
    if (retries-- <= 0) {
      if (err) {
        backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                           "PipeWire ring buffer write timeout (PipeWire "
                           "thread not consuming samples)");
      }
      return false;
    }
    cdsp_sleep_ms(1);
  }
  if (playback->stopped) {
    if (err) {
      backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                         "Playback stream stopped");
    }
    return false;
  }

  spsc_audio_ring_buffer_write(playback->ring, playback->encode_buf, requested,
                               1);
  return true;
}

/**
 * @brief Get the current buffer level of the PipeWire playback backend.
 *
 * @param ctx Pointer to the PipeWire playback instance.
 * @return The buffer level in samples.
 */
static size_t pipewire_playback_get_buffer_level(void* ctx) {
  pipewire_playback_t* playback = (pipewire_playback_t*)ctx;
  if (!playback || !playback->ring) return 0;
  return spsc_audio_ring_buffer_get_available_to_read(playback->ring) /
         playback->channels;
}

/**
 * @brief Get any pending sample rate change.
 *
 * @param ctx Pointer to the PipeWire playback instance.
 * @param out_rate Pointer to double to store the pending sample rate.
 * @return true if a rate change is pending, false otherwise.
 */
static bool pipewire_playback_get_pending_rate_change(void* ctx,
                                                      double* out_rate) {
  (void)ctx;
  (void)out_rate;
  return false;
}

/**
 * @brief Prefill the PipeWire playback buffer with silence.
 *
 * @param ctx Pointer to the PipeWire playback instance.
 * @param frames Number of frames of silence to prefill.
 * @param err Pointer to a backend_error_t struct to report errors.
 * @return true if successful, false otherwise.
 */
static bool pipewire_playback_prefill_silence(void* ctx, size_t frames,
                                              backend_error_t* err) {
  pipewire_playback_t* playback = (pipewire_playback_t*)ctx;
  (void)err;
  if (!playback || !playback->ring) return false;

  spsc_audio_ring_buffer_write_silence(playback->ring,
                                       frames * playback->channels);
  return true;
}

/**
 * @brief Check if PipeWire playback is currently paused.
 *
 * @param ctx Pointer to the PipeWire playback instance.
 * @return true if paused, false otherwise.
 */
static bool pipewire_playback_get_is_paused(void* ctx) {
  pipewire_playback_t* playback = (pipewire_playback_t*)ctx;
  if (!playback) return false;
  return atomic_load_explicit(&playback->paused, memory_order_acquire);
}

/**
 * @brief Set the paused state of the PipeWire playback backend.
 *
 * @param ctx Pointer to the PipeWire playback instance.
 * @param paused true to pause, false to resume.
 */
static void pipewire_playback_set_is_paused(void* ctx, bool paused) {
  pipewire_playback_t* playback = (pipewire_playback_t*)ctx;
  if (!playback) return;
  atomic_store_explicit(&playback->paused, paused, memory_order_release);
}

/**
 * @brief Stop the PipeWire playback stream.
 *
 * @param ctx Pointer to the PipeWire playback instance.
 */
static void pipewire_playback_stop(void* ctx) {
  pipewire_playback_t* playback = (pipewire_playback_t*)ctx;
  if (!playback) return;
  if (playback->loop) {
    pw_thread_loop_lock(playback->loop);
    playback->stopped = true;
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
static void pipewire_playback_destroy(void* ctx) {
  pipewire_playback_t* playback = (pipewire_playback_t*)ctx;
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
static playback_backend_t* pipewire_playback_create(
    const playback_device_config_t* config, int sample_rate, int chunk_size,
    bool full_duplex, processing_parameters_t* params, backend_error_t* err) {
  (void)full_duplex;
  (void)params;
  (void)err;
  pipewire_playback_t* playback =
      (pipewire_playback_t*)calloc(1, sizeof(pipewire_playback_t));
  if (!playback) return NULL;

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

  atomic_init(&playback->paused, false);
  playback_backend_t* backend =
      (playback_backend_t*)calloc(1, sizeof(playback_backend_t));
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

#endif  // ENABLE_PIPEWIRE
