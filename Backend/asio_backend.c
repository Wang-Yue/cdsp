#include "Backend/asio_backend.h"

/**
 * @file asio_backend.c
 * @brief ASIO capture and playback backend matching upstream CamillaDSP
 * src/asio_backend/.
 */

#if defined(ENABLE_ASIO)

#define WIN32_LEAN_AND_MEAN

#include <initguid.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unknwn.h>
#include <windows.h>

#include "Audio/sample_conversion.h"
#include "Engine/cdsp_sem.h"
#include "Logging/app_logger.h"
#include "Utils/cdsp_time.h"
#include "Utils/lock_free_ring_buffer.h"

static const logger_t g_logger = {"dsp.backend.asio"};

// COM Release helper
#define SAFE_RELEASE(punk)         \
  if ((punk) != NULL) {            \
    (punk)->lpVtbl->Release(punk); \
    (punk) = NULL;                 \
  }

// ASIO type definitions
typedef int32_t ASIOBool;
#define ASIOFalse 0
#define ASIOTrue 1

typedef double ASIOSampleRate;
typedef long ASIOError;

// ASIO sample types matching utils.rs lines 91-111
#define ASIO_ST_INT16_MSB 0
#define ASIO_ST_INT24_MSB 1
#define ASIO_ST_INT32_MSB 2
#define ASIO_ST_FLOAT32_MSB 3
#define ASIO_ST_FLOAT64_MSB 4
#define ASIO_ST_INT32_MSB_16 8
#define ASIO_ST_INT32_MSB_18 9
#define ASIO_ST_INT32_MSB_20 10
#define ASIO_ST_INT32_MSB_24 11
#define ASIO_ST_INT16_LSB 16
#define ASIO_ST_INT24_LSB 17
#define ASIO_ST_INT32_LSB 18
#define ASIO_ST_FLOAT32_LSB 19
#define ASIO_ST_FLOAT64_LSB 20
#define ASIO_ST_INT32_LSB_16 24
#define ASIO_ST_INT32_LSB_18 25
#define ASIO_ST_INT32_LSB_20 26
#define ASIO_ST_INT32_LSB_24 27
#define ASIO_ST_DSD_INT8_LSB_1 32
#define ASIO_ST_DSD_INT8_MSB_1 33
#define ASIO_ST_DSD_INT8_NER8 40

// Standard ASIO message selectors matching device.rs lines 485-493
#define K_ASIO_SELECTOR_SUPPORTED 1
#define K_ASIO_ENGINE_VERSION 2
#define K_ASIO_SUPPORTS_TIME_INFO 3
#define K_ASIO_SUPPORTS_TIME_CODE 4
#define K_ASIO_RESET_REQUEST 5
#define K_ASIO_BUFFER_SIZE_CHANGE 6
#define K_ASIO_RESYNC_REQUEST 7
#define K_ASIO_LATENCIES_CHANGED 8

// ASIO DSD Future Selectors (Steinberg ASIO SDK 2.3)
#define kAsioSetIoFormat 0x23111961L
#define kAsioGetIoFormat 0x23111983L
#define kAsioCanDoIoFormat 0x23112004L
#define ASE_SUCCESS 0x3f4847a0L

typedef enum {
  kASIOFormatInvalid = -1,
  kASIOFormatPCM = 0,
  kASIOFormatDSD = 1
} ASIOSampleFormatType;

typedef struct {
  ASIOSampleFormatType FormatType;
  char future[512 - sizeof(ASIOSampleFormatType)];
} ASIOIoFormat;

typedef struct {
  long asioVersion;
  long driverVersion;
  char name[32];
  char errorMessage[124];
  void* sysRef;
} ASIODriverInfo;

typedef struct {
  int32_t channel;
  ASIOBool isInput;
  ASIOBool isActive;
  int32_t channelGroup;
  int32_t type;
  char name[32];
} ASIOChannelInfo;

typedef struct {
  ASIOBool isInput;
  int32_t channelNum;
  void* buffers[2];
} ASIOBufferInfo;

typedef struct {
  void (*bufferSwitch)(long doubleBufferIndex, ASIOBool directProcess);
  void (*sampleRateDidChange)(ASIOSampleRate sRate);
  long (*asioMessage)(long selector, long value, void* message, double* opt);
  void* (*bufferSwitchTimeInfo)(void* params, long doubleBufferIndex,
                                ASIOBool directProcess);
} ASIOCallbacks;

typedef struct IASIO IASIO;
typedef struct IASIOVtbl {
  HRESULT(STDMETHODCALLTYPE* QueryInterface)(IASIO* This, REFIID riid,
                                             void** ppvObject);
  ULONG(STDMETHODCALLTYPE* AddRef)(IASIO* This);
  ULONG(STDMETHODCALLTYPE* Release)(IASIO* This);
  ASIOBool(STDMETHODCALLTYPE* init)(IASIO* This, void* sysHandle);
  void(STDMETHODCALLTYPE* getDriverName)(IASIO* This, char* name);
  long(STDMETHODCALLTYPE* getDriverVersion)(IASIO* This);
  void(STDMETHODCALLTYPE* getErrorMessage)(IASIO* This, char* string);
  ASIOError(STDMETHODCALLTYPE* start)(IASIO* This);
  ASIOError(STDMETHODCALLTYPE* stop)(IASIO* This);
  ASIOError(STDMETHODCALLTYPE* getChannels)(IASIO* This, long* numInputChannels,
                                            long* numOutputChannels);
  ASIOError(STDMETHODCALLTYPE* getLatencies)(IASIO* This, long* inputLatency,
                                             long* outputLatency);
  ASIOError(STDMETHODCALLTYPE* getBufferSize)(IASIO* This, long* minSize,
                                              long* maxSize,
                                              long* preferredSize,
                                              long* granularity);
  ASIOError(STDMETHODCALLTYPE* canSampleRate)(IASIO* This, double sampleRate);
  ASIOError(STDMETHODCALLTYPE* getSampleRate)(IASIO* This, double* sampleRate);
  ASIOError(STDMETHODCALLTYPE* setSampleRate)(IASIO* This, double sampleRate);
  ASIOError(STDMETHODCALLTYPE* getClockSources)(IASIO* This, void* clocks,
                                                long* numSources);
  ASIOError(STDMETHODCALLTYPE* setClockSource)(IASIO* This, long reference);
  ASIOError(STDMETHODCALLTYPE* getSamplePosition)(IASIO* This, int64_t* sPos,
                                                  int64_t* tStamp);
  ASIOError(STDMETHODCALLTYPE* getChannelInfo)(IASIO* This, void* info);
  ASIOError(STDMETHODCALLTYPE* createBuffers)(IASIO* This, void* bufferInfos,
                                              long numChannels, long bufferSize,
                                              void* callbacks);
  ASIOError(STDMETHODCALLTYPE* disposeBuffers)(IASIO* This);
  ASIOError(STDMETHODCALLTYPE* controlPanel)(IASIO* This);
  ASIOError(STDMETHODCALLTYPE* future)(IASIO* This, long selector, void* opt);
  ASIOError(STDMETHODCALLTYPE* outputReady)(IASIO* This);
} IASIOVtbl;

struct IASIO {
  const IASIOVtbl* lpVtbl;
};

static const GUID g_IID_IASIO_VAL = {
    0x9333b620,
    0x1f0b,
    0x11d2,
    {0x98, 0xbc, 0x00, 0x00, 0xf8, 0x75, 0xac, 0x12}};

// MARK: - ASIO Utils matching CamillaDSP utils.rs

/**
 * @brief read_current_asio_sample_rate_hz matching CamillaDSP utils.rs lines
 * 28-36.
 */
static int read_current_asio_sample_rate_hz(IASIO* iasio) {
  if (!iasio) return 0;
  double rate = 0.0;
  long res = iasio->lpVtbl->getSampleRate(iasio, &rate);
  if (res == 0 && isfinite(rate) && rate > 0.0) {
    return (int)round(rate);
  }
  return 0;
}

/**
 * @brief make_buffer_infos matching CamillaDSP utils.rs lines 57-65.
 */
static ASIOBufferInfo* make_buffer_infos(size_t num_channels, bool is_input) {
  ASIOBufferInfo* infos =
      (ASIOBufferInfo*)calloc(num_channels, sizeof(ASIOBufferInfo));
  if (!infos) return NULL;
  for (size_t ch = 0; ch < num_channels; ch++) {
    infos[ch].isInput = is_input ? ASIOTrue : ASIOFalse;
    infos[ch].channelNum = (int32_t)ch;
    infos[ch].buffers[0] = NULL;
    infos[ch].buffers[1] = NULL;
  }
  return infos;
}

/**
 * @brief asio_format_to_str matching CamillaDSP utils.rs lines 80-89.
 */
static const char* asio_format_to_str(asio_sample_format_t fmt) {
  switch (fmt) {
    case ASIO_SAMPLE_FORMAT_S16_LE:
      return "S16_LE";
    case ASIO_SAMPLE_FORMAT_S24_4_LE:
      return "S24_4_LE";
    case ASIO_SAMPLE_FORMAT_S24_3_LE:
      return "S24_3_LE";
    case ASIO_SAMPLE_FORMAT_S32_LE:
      return "S32_LE";
    case ASIO_SAMPLE_FORMAT_F32_LE:
      return "F32_LE";
    case ASIO_SAMPLE_FORMAT_F64_LE:
      return "F64_LE";
    case ASIO_SAMPLE_FORMAT_DSD_INT8:
      return "DSD_INT8";
    default:
      return "Unknown";
  }
}

/**
 * @brief asio_sample_type_name matching CamillaDSP utils.rs lines 115-140.
 */
static const char* asio_sample_type_name(int type_id) {
  switch (type_id) {
    case ASIO_ST_INT16_MSB:
      return "Int16 MSB (big-endian)";
    case ASIO_ST_INT24_MSB:
      return "Int24 MSB (3-byte packed, big-endian)";
    case ASIO_ST_INT32_MSB:
      return "Int32 MSB (big-endian)";
    case ASIO_ST_FLOAT32_MSB:
      return "Float32 MSB (big-endian)";
    case ASIO_ST_FLOAT64_MSB:
      return "Float64 MSB (big-endian)";
    case ASIO_ST_INT32_MSB_16:
      return "Int32 MSB 16-bit (big-endian)";
    case ASIO_ST_INT32_MSB_18:
      return "Int32 MSB 18-bit (big-endian)";
    case ASIO_ST_INT32_MSB_20:
      return "Int32 MSB 20-bit (big-endian)";
    case ASIO_ST_INT32_MSB_24:
      return "Int32 MSB 24-bit (big-endian)";
    case ASIO_ST_INT16_LSB:
      return "Int16 LSB";
    case ASIO_ST_INT24_LSB:
      return "Int24 LSB (3-byte packed)";
    case ASIO_ST_INT32_LSB:
      return "Int32 LSB";
    case ASIO_ST_FLOAT32_LSB:
      return "Float32 LSB";
    case ASIO_ST_FLOAT64_LSB:
      return "Float64 LSB";
    case ASIO_ST_INT32_LSB_16:
      return "Int32 LSB 16-bit";
    case ASIO_ST_INT32_LSB_18:
      return "Int32 LSB 18-bit";
    case ASIO_ST_INT32_LSB_20:
      return "Int32 LSB 20-bit";
    case ASIO_ST_INT32_LSB_24:
      return "Int32 LSB 24-bit";
    case ASIO_ST_DSD_INT8_LSB_1:
      return "DSD Int8 LSB 1";
    case ASIO_ST_DSD_INT8_MSB_1:
      return "DSD Int8 MSB 1";
    case ASIO_ST_DSD_INT8_NER8:
      return "DSD Int8 NER8";
    default:
      return "Unknown";
  }
}

/**
 * @brief asio_sample_type_to_format matching CamillaDSP utils.rs lines 145-158.
 */
static asio_sample_format_t asio_sample_type_to_format(int type_id) {
  switch (type_id) {
    case ASIO_ST_INT16_LSB:
      return ASIO_SAMPLE_FORMAT_S16_LE;
    case ASIO_ST_INT24_LSB:
      return ASIO_SAMPLE_FORMAT_S24_3_LE;
    case ASIO_ST_INT32_LSB:
    case ASIO_ST_INT32_LSB_16:
    case ASIO_ST_INT32_LSB_18:
    case ASIO_ST_INT32_LSB_20:
      return ASIO_SAMPLE_FORMAT_S32_LE;
    case ASIO_ST_INT32_LSB_24:
      return ASIO_SAMPLE_FORMAT_S24_4_LE;
    case ASIO_ST_FLOAT32_LSB:
      return ASIO_SAMPLE_FORMAT_F32_LE;
    case ASIO_ST_FLOAT64_LSB:
      return ASIO_SAMPLE_FORMAT_F64_LE;
    case ASIO_ST_DSD_INT8_LSB_1:
    case ASIO_ST_DSD_INT8_MSB_1:
      return ASIO_SAMPLE_FORMAT_DSD_INT8;
    default:
      return ASIO_SAMPLE_FORMAT_INVALID;
  }
}

static size_t asio_format_bytes_per_sample(asio_sample_format_t fmt) {
  switch (fmt) {
    case ASIO_SAMPLE_FORMAT_S16_LE:
      return 2;
    case ASIO_SAMPLE_FORMAT_S24_3_LE:
      return 3;
    case ASIO_SAMPLE_FORMAT_S24_4_LE:
    case ASIO_SAMPLE_FORMAT_S32_LE:
    case ASIO_SAMPLE_FORMAT_F32_LE:
    case ASIO_SAMPLE_FORMAT_DSD_INT8:
      return 4;
    case ASIO_SAMPLE_FORMAT_F64_LE:
      return 8;
    default:
      return 4;
  }
}

/**
 * @brief query_device_format matching CamillaDSP utils.rs lines 172-195.
 */
static bool query_device_format(IASIO* iasio, bool is_input, int* out_type,
                                backend_error_t* err) {
  ASIOChannelInfo info = {0};
  info.channel = 0;
  info.isInput = is_input ? ASIOTrue : ASIOFalse;
  long res = iasio->lpVtbl->getChannelInfo(iasio, &info);
  if (res != 0) {
    const char* direction = is_input ? "input" : "output";
    if (err) {
      char msg[256];
      snprintf(msg, sizeof(msg),
               "ASIOGetChannelInfo failed for %s channel 0 (error code %ld)",
               direction, res);
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
    return false;
  }
  logger_debug(&g_logger, "ASIO channel 0 (%s): type=%d (%s)",
               is_input ? "input" : "output", info.type,
               asio_sample_type_name(info.type));
  *out_type = info.type;
  return true;
}

/**
 * @brief resolve_format matching CamillaDSP utils.rs lines 206-240.
 */
static bool resolve_format(IASIO* iasio, asio_sample_format_t configured,
                           bool has_configured, bool is_input,
                           asio_sample_format_t* out_format,
                           backend_error_t* err) {
  int device_type = 0;
  if (!query_device_format(iasio, is_input, &device_type, err)) {
    return false;
  }
  asio_sample_format_t native_format = asio_sample_type_to_format(device_type);
  const char* direction = is_input ? "capture" : "playback";

  if (native_format == ASIO_SAMPLE_FORMAT_INVALID) {
    if (err) {
      char msg[256];
      snprintf(msg, sizeof(msg),
               "ASIO %s: device uses unsupported sample type %d (%s)",
               direction, device_type, asio_sample_type_name(device_type));
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
    return false;
  }

  if (has_configured && configured != ASIO_SAMPLE_FORMAT_INVALID) {
    if (configured != native_format) {
      if (err) {
        char msg[512];
        snprintf(
            msg, sizeof(msg),
            "ASIO %s: configured format %s does not match device native "
            "format %s (%s). ASIO drivers do not convert sample formats. "
            "Please remove the format setting to auto-detect, or set it to %s",
            direction, asio_format_to_str(configured),
            asio_format_to_str(native_format),
            asio_sample_type_name(device_type),
            asio_format_to_str(native_format));
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
      }
      return false;
    }
    logger_debug(&g_logger,
                 "ASIO %s: configured format %s matches device native format.",
                 direction, asio_format_to_str(configured));
  } else {
    logger_debug(&g_logger, "ASIO %s: auto-detected format %s from device.",
                 direction, asio_format_to_str(native_format));
  }

  *out_format = native_format;
  return true;
}

/**
 * @brief get_preferred_buffer_size matching CamillaDSP utils.rs lines 274-297.
 */
static bool get_preferred_buffer_size(IASIO* iasio, long* out_preferred,
                                      backend_error_t* err) {
  long min_buf = 0, max_buf = 0, preferred_buf = 0, granularity = 0;
  long res = iasio->lpVtbl->getBufferSize(iasio, &min_buf, &max_buf,
                                          &preferred_buf, &granularity);
  if (res != 0) {
    if (err) {
      char msg[256];
      snprintf(msg, sizeof(msg), "ASIOGetBufferSize failed with error code %ld",
               res);
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
    return false;
  }
  logger_trace(&g_logger,
               "ASIOGetBufferSize: min=%ld, max=%ld, preferred=%ld, "
               "granularity=%ld",
               min_buf, max_buf, preferred_buf, granularity);
  *out_preferred = preferred_buf;
  return true;
}

/**
 * @brief create_asio_buffers matching CamillaDSP utils.rs lines 243-271.
 */
static bool create_asio_buffers(IASIO* iasio, ASIOBufferInfo* buffer_infos,
                                long num_channels, long buffer_size,
                                ASIOCallbacks* callbacks,
                                backend_error_t* err) {
  logger_trace(
      &g_logger,
      "Calling ASIOCreateBuffers: infos_ptr=%p, channels=%ld, buffer_size=%ld, "
      "callbacks_ptr=%p",
      (int64_t)(uintptr_t)buffer_infos, num_channels, buffer_size,
      (int64_t)(uintptr_t)callbacks);
  long res = iasio->lpVtbl->createBuffers(iasio, buffer_infos, num_channels,
                                          buffer_size, callbacks);
  logger_trace(&g_logger, "ASIOCreateBuffers returned %ld.", res);
  if (res != 0) {
    if (err) {
      char msg[256];
      snprintf(msg, sizeof(msg), "ASIOCreateBuffers failed with error code %ld",
               res);
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
    return false;
  }
  return true;
}

// MARK: - Internal Contexts and Global Atomics matching CamillaDSP device.rs
// lines 103-157

typedef struct {
  spsc_byte_ring_buffer_t* ring_buffer;
  ASIOBufferInfo* buffer_infos;
  size_t num_channels;
  size_t buffer_size;
  size_t bytes_per_sample;
  uint8_t* transfer_buf;
  size_t transfer_buf_cap;
  size_t target_level;
  size_t silence_frames_to_insert;
  uint8_t silence_byte;
  _Atomic double buffer_fill;
  bool running;
} asio_playback_context_t;

typedef struct {
  spsc_byte_ring_buffer_t* ring_buffer;
  cdsp_sem_t semaphore;
  ASIOBufferInfo* buffer_infos;
  size_t num_channels;
  size_t buffer_size;
  size_t bytes_per_sample;
  uint8_t* transfer_buf;
  uint64_t chunk_counter;
} asio_capture_context_t;

static _Atomic(asio_playback_context_t*) PLAYBACK_CONTEXT = NULL;
static _Atomic(asio_capture_context_t*) CAPTURE_CONTEXT = NULL;
static _Atomic bool ASIO_DRIVER_INITIALIZED = false;
static _Atomic bool ASIO_PLAYBACK_RATE_CHANGED = false;
static _Atomic bool ASIO_CAPTURE_RATE_CHANGED = false;

static void clear_playback_rate_change_event(void) {
  atomic_store_explicit(&ASIO_PLAYBACK_RATE_CHANGED, false,
                        memory_order_release);
}

static void clear_capture_rate_change_event(void) {
  atomic_store_explicit(&ASIO_CAPTURE_RATE_CHANGED, false,
                        memory_order_release);
}

static bool take_playback_rate_change_event(void) {
  return atomic_exchange_explicit(&ASIO_PLAYBACK_RATE_CHANGED, false,
                                  memory_order_acq_rel);
}

static bool take_capture_rate_change_event(void) {
  return atomic_exchange_explicit(&ASIO_CAPTURE_RATE_CHANGED, false,
                                  memory_order_acq_rel);
}

bool asio_driver_is_initialized(void) {
  return atomic_load_explicit(&ASIO_DRIVER_INITIALIZED, memory_order_acquire);
}

// MARK: - Startup callback gate (PLAYBACK_CALLBACK_SEEN) matching device.rs
// lines 193-222

static struct {
  SRWLOCK lock;
  CONDITION_VARIABLE cond;
  bool seen;
} g_playback_callback_seen = {
    .lock = SRWLOCK_INIT, .cond = CONDITION_VARIABLE_INIT, .seen = false};

static void reset_playback_callback_seen(void) {
  AcquireSRWLockExclusive(&g_playback_callback_seen.lock);
  g_playback_callback_seen.seen = false;
  ReleaseSRWLockExclusive(&g_playback_callback_seen.lock);
}

static void mark_playback_callback_seen(void) {
  AcquireSRWLockExclusive(&g_playback_callback_seen.lock);
  if (!g_playback_callback_seen.seen) {
    g_playback_callback_seen.seen = true;
    WakeAllConditionVariable(&g_playback_callback_seen.cond);
  }
  ReleaseSRWLockExclusive(&g_playback_callback_seen.lock);
}

static bool wait_for_playback_callback(DWORD timeout_ms) {
  AcquireSRWLockExclusive(&g_playback_callback_seen.lock);
  if (g_playback_callback_seen.seen) {
    ReleaseSRWLockExclusive(&g_playback_callback_seen.lock);
    return true;
  }
  SleepConditionVariableSRW(&g_playback_callback_seen.cond,
                            &g_playback_callback_seen.lock, timeout_ms, 0);
  bool seen = g_playback_callback_seen.seen;
  ReleaseSRWLockExclusive(&g_playback_callback_seen.lock);
  return seen;
}

// MARK: - ASIO Callbacks matching CamillaDSP device.rs lines 228-545

static void buffer_switch_combined(long buffer_index, ASIOBool direct_process);

/**
 * @brief buffer_switch_playback matching CamillaDSP device.rs lines 234-325.
 * Uses contiguous transfer buffer for direct sample consumption and
 * de-interleaving.
 */
static void buffer_switch_playback(long buffer_index, ASIOBool direct_process) {
  (void)direct_process;
  asio_playback_context_t* ctx =
      atomic_load_explicit(&PLAYBACK_CONTEXT, memory_order_acquire);
  if (!ctx) {
    return;
  }
  if (buffer_index < 0 || buffer_index > 1) {
    logger_debug(
        &g_logger,
        "ASIO playback callback got invalid buffer index %ld, ignoring.",
        buffer_index);
    return;
  }
  if (!ctx->buffer_infos || !ctx->transfer_buf) {
    return;
  }
  mark_playback_callback_seen();

  size_t frames = ctx->buffer_size;
  size_t channels = ctx->num_channels;
  size_t bytes_per_sample = ctx->bytes_per_sample;
  size_t bytes_per_frame = bytes_per_sample * channels;

  size_t avail_bytes =
      spsc_byte_ring_buffer_get_available_to_read(ctx->ring_buffer);
  size_t avail_frames = avail_bytes / bytes_per_frame;

  if (!ctx->running && avail_frames > 0) {
    ctx->running = true;
    size_t prefill_frames = (ctx->target_level > ctx->buffer_size)
                                ? ctx->target_level
                                : ctx->buffer_size;
    ctx->silence_frames_to_insert = prefill_frames;
  }

  size_t silence_frames = 0;
  if (ctx->silence_frames_to_insert > 0) {
    silence_frames = (ctx->silence_frames_to_insert < frames)
                         ? ctx->silence_frames_to_insert
                         : frames;
    memset(ctx->transfer_buf, ctx->silence_byte,
           silence_frames * bytes_per_frame);
    ctx->silence_frames_to_insert -= silence_frames;
  }

  size_t frames_from_ring = frames - silence_frames;
  size_t bytes_from_ring = frames_from_ring * bytes_per_frame;
  size_t consumed_bytes = 0;
  if (bytes_from_ring > 0) {
    consumed_bytes = spsc_byte_ring_buffer_consume(
        ctx->ring_buffer, ctx->transfer_buf + silence_frames * bytes_per_frame,
        bytes_from_ring);
  }

  if (consumed_bytes < bytes_from_ring) {
    size_t missing_bytes = bytes_from_ring - consumed_bytes;
    memset(
        ctx->transfer_buf + silence_frames * bytes_per_frame + consumed_bytes,
        ctx->silence_byte, missing_bytes);
    if (ctx->running) {
      ctx->running = false;
      logger_warn(
          &g_logger,
          "ASIO playback callback: underrun, filled %zu bytes of silence.",
          missing_bytes + silence_frames * bytes_per_frame);
    }
  }

  // De-interleave transfer_buf into per-channel ASIO buffers
  for (size_t frame = 0; frame < frames; frame++) {
    for (size_t ch = 0; ch < channels; ch++) {
      void* out_ptr = ctx->buffer_infos[ch].buffers[buffer_index];
      if (out_ptr) {
        uint8_t* dst = (uint8_t*)out_ptr + frame * bytes_per_sample;
        const uint8_t* src =
            ctx->transfer_buf + (frame * channels + ch) * bytes_per_sample;
        memcpy(dst, src, bytes_per_sample);
      }
    }
  }

  // Update buffer fill estimate
  size_t curr_buffer_fill =
      (ctx->silence_frames_to_insert * bytes_per_frame +
       spsc_byte_ring_buffer_get_available_to_read(ctx->ring_buffer)) /
      bytes_per_frame;
  atomic_store_explicit(&ctx->buffer_fill, (double)curr_buffer_fill,
                        memory_order_relaxed);
}

/**
 * @brief buffer_switch_capture matching CamillaDSP device.rs lines 333-414.
 */
static void buffer_switch_capture(long buffer_index, ASIOBool direct_process) {
  (void)direct_process;
  asio_capture_context_t* ctx =
      atomic_load_explicit(&CAPTURE_CONTEXT, memory_order_acquire);
  if (!ctx) {
    return;
  }
  if (buffer_index < 0 || buffer_index > 1) {
    logger_debug(
        &g_logger,
        "ASIO capture callback got invalid buffer index %ld, ignoring.",
        buffer_index);
    return;
  }
  if (!ctx->buffer_infos || !ctx->transfer_buf) {
    return;
  }

  // Read from per-channel ASIO input buffers and interleave into transfer_buf
  for (size_t frame = 0; frame < ctx->buffer_size; frame++) {
    for (size_t ch = 0; ch < ctx->num_channels; ch++) {
      void* in_ptr = ctx->buffer_infos[ch].buffers[buffer_index];
      if (in_ptr) {
        const uint8_t* src =
            (const uint8_t*)in_ptr + frame * ctx->bytes_per_sample;
        size_t offset =
            (frame * ctx->num_channels + ch) * ctx->bytes_per_sample;
        memcpy(&ctx->transfer_buf[offset], src, ctx->bytes_per_sample);
      }
    }
  }

  size_t total_bytes =
      ctx->buffer_size * ctx->num_channels * ctx->bytes_per_sample;
  size_t pushed_bytes = spsc_byte_ring_buffer_write(
      ctx->ring_buffer, ctx->transfer_buf, total_bytes);
  if (pushed_bytes < total_bytes) {
    logger_warn(
        &g_logger,
        "ASIO capture callback: ringbuffer full, dropped %zu of %zu bytes.",
        total_bytes - pushed_bytes, total_bytes);
  }
  if (ctx->semaphore) {
    cdsp_sem_signal(ctx->semaphore);
  }
  ctx->chunk_counter++;
}

static void* buffer_switch_timeinfo_playback(void* params,
                                             long doubleBufferIndex,
                                             ASIOBool directProcess) {
  buffer_switch_playback(doubleBufferIndex, directProcess);
  return params;
}

static void* buffer_switch_timeinfo_capture(void* params,
                                            long doubleBufferIndex,
                                            ASIOBool directProcess) {
  buffer_switch_capture(doubleBufferIndex, directProcess);
  return params;
}

static void* buffer_switch_timeinfo_combined(void* params,
                                             long doubleBufferIndex,
                                             ASIOBool directProcess) {
  buffer_switch_combined(doubleBufferIndex, directProcess);
  return params;
}

static void buffer_switch_combined(long buffer_index, ASIOBool direct_process) {
  buffer_switch_playback(buffer_index, direct_process);
  buffer_switch_capture(buffer_index, direct_process);
}

/**
 * @brief sample_rate_changed_callback matching CamillaDSP device.rs lines
 * 466-470.
 */
static void sample_rate_changed_callback(ASIOSampleRate s_rate) {
  (void)s_rate;
  atomic_store_explicit(&ASIO_PLAYBACK_RATE_CHANGED, true,
                        memory_order_release);
  atomic_store_explicit(&ASIO_CAPTURE_RATE_CHANGED, true, memory_order_release);
  logger_warn(&g_logger, "ASIO sampleRateDidChange callback received.");
}

/**
 * @brief asio_message_callback matching CamillaDSP device.rs lines 478-532.
 */
static long asio_message_callback(long selector, long value, void* message,
                                  double* opt) {
  (void)message;
  (void)opt;
  switch (selector) {
    case K_ASIO_SELECTOR_SUPPORTED:
      switch (value) {
        case K_ASIO_ENGINE_VERSION:
        case K_ASIO_RESYNC_REQUEST:
        case K_ASIO_LATENCIES_CHANGED:
        case K_ASIO_RESET_REQUEST:
        case K_ASIO_BUFFER_SIZE_CHANGE:
        case K_ASIO_SUPPORTS_TIME_INFO:
        case K_ASIO_SELECTOR_SUPPORTED:
          return 1;
        case K_ASIO_SUPPORTS_TIME_CODE:
        default:
          return 0;
      }
    case K_ASIO_ENGINE_VERSION:
      return 2;  // ASIO 2.0
    case K_ASIO_SUPPORTS_TIME_INFO:
      return 1;
    case K_ASIO_RESET_REQUEST:
      logger_warn(&g_logger,
                  "ASIO reset request received. A stream restart may be "
                  "required by the driver.");
      return 1;
    case K_ASIO_BUFFER_SIZE_CHANGE:
      logger_warn(&g_logger,
                  "ASIO buffer size change request received. Dynamic resize is "
                  "not implemented in this backend.");
      return 1;
    case K_ASIO_RESYNC_REQUEST:
      logger_debug(&g_logger, "ASIO resync request received.");
      return 1;
    case K_ASIO_LATENCIES_CHANGED:
      logger_debug(&g_logger, "ASIO latencies changed notification.");
      return 1;
    default:
      return 0;
  }
}

// MARK: - Full-Duplex Coordination matching CamillaDSP device.rs lines 548-792

typedef struct {
  char driver_name[256];
  long num_inputs;
  long num_outputs;
  long preferred_buf_size;
  IASIO* iasio;

  ASIOBufferInfo* pending_output;
  size_t pending_output_channels;

  ASIOBufferInfo* pending_input;
  size_t pending_input_channels;

  bool stream_started;
  char setup_error[256];
  uint8_t active_count;

  ASIOBufferInfo* buffer_infos_for_driver;
  size_t buffer_infos_for_driver_count;
  ASIOCallbacks callbacks_for_driver;
} asio_shared_state_t;

static struct {
  SRWLOCK lock;
  CONDITION_VARIABLE cond;
  asio_shared_state_t* state;
} g_asio_shared = {
    .lock = SRWLOCK_INIT, .cond = CONDITION_VARIABLE_INIT, .state = NULL};

static bool find_asio_driver_clsid(const char* driver_name, CLSID* out_clsid);
static void teardown_asio_driver(IASIO** p_iasio);
static bool load_driver_by_name(const char* name, IASIO** out_iasio,
                                backend_error_t* err);
static bool open_asio_device(const char* devname, int samplerate, bool is_dsd,
                             IASIO** out_iasio, long* out_inputs,
                             long* out_outputs, backend_error_t* err);

/**
 * @brief init_shared_asio matching CamillaDSP device.rs lines 554-620.
 */
static bool init_shared_asio(const char* devname, int samplerate, bool is_dsd,
                             long* out_inputs, long* out_outputs,
                             long* out_preferred_buf, backend_error_t* err) {
  logger_trace(&g_logger,
               "init_shared_asio: dev='%s', samplerate=%d, is_dsd=%d", devname,
               samplerate, (int)is_dsd);
  AcquireSRWLockExclusive(&g_asio_shared.lock);

  if (g_asio_shared.state) {
    if (strcmp(g_asio_shared.state->driver_name, devname) != 0) {
      ReleaseSRWLockExclusive(&g_asio_shared.lock);
      if (err) {
        backend_error_init(
            err, BACKEND_ERROR_INITIALIZATION_FAILED,
            "Different ASIO driver names for capture and playback are not "
            "supported");
      }
      return false;
    }
    logger_trace(&g_logger,
                 "init_shared_asio: reusing existing shared state for '%s'",
                 g_asio_shared.state->driver_name);
    *out_inputs = g_asio_shared.state->num_inputs;
    *out_outputs = g_asio_shared.state->num_outputs;
    *out_preferred_buf = g_asio_shared.state->preferred_buf_size;
    ReleaseSRWLockExclusive(&g_asio_shared.lock);
    return true;
  }

  IASIO* iasio = NULL;
  long num_inputs = 0, num_outputs = 0;
  if (!open_asio_device(devname, samplerate, is_dsd, &iasio, &num_inputs,
                        &num_outputs, err)) {
    ReleaseSRWLockExclusive(&g_asio_shared.lock);
    return false;
  }

  long preferred_buf = 0;
  if (!get_preferred_buffer_size(iasio, &preferred_buf, err)) {
    teardown_asio_driver(&iasio);
    ReleaseSRWLockExclusive(&g_asio_shared.lock);
    return false;
  }

  g_asio_shared.state =
      (asio_shared_state_t*)calloc(1, sizeof(asio_shared_state_t));
  snprintf(g_asio_shared.state->driver_name,
           sizeof(g_asio_shared.state->driver_name), "%s", devname);
  g_asio_shared.state->num_inputs = num_inputs;
  g_asio_shared.state->num_outputs = num_outputs;
  g_asio_shared.state->preferred_buf_size = preferred_buf;
  g_asio_shared.state->iasio = iasio;
  g_asio_shared.state->stream_started = false;
  g_asio_shared.state->active_count = 0;

  *out_inputs = num_inputs;
  *out_outputs = num_outputs;
  *out_preferred_buf = preferred_buf;

  ReleaseSRWLockExclusive(&g_asio_shared.lock);
  return true;
}

/**
 * @brief register_and_wait matching CamillaDSP device.rs lines 627-754.
 */
static bool register_and_wait(bool is_input, size_t num_channels,
                              ASIOBufferInfo** out_buffer_infos,
                              long* out_buf_size, IASIO** out_iasio,
                              backend_error_t* err) {
  logger_trace(&g_logger, "register_and_wait: is_input=%d, num_channels=%zu",
               is_input, num_channels);
  AcquireSRWLockExclusive(&g_asio_shared.lock);

  if (!g_asio_shared.state) {
    ReleaseSRWLockExclusive(&g_asio_shared.lock);
    if (err) {
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                         "ASIO shared state not initialized");
    }
    return false;
  }

  if (g_asio_shared.state->setup_error[0] != '\0') {
    if (err) {
      char msg[384];
      snprintf(msg, sizeof(msg), "ASIO full-duplex setup aborted: %s",
               g_asio_shared.state->setup_error);
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
    ReleaseSRWLockExclusive(&g_asio_shared.lock);
    return false;
  }

  ASIOBufferInfo* my_infos = make_buffer_infos(num_channels, is_input);
  if (is_input) {
    g_asio_shared.state->pending_input = my_infos;
    g_asio_shared.state->pending_input_channels = num_channels;
  } else {
    g_asio_shared.state->pending_output = my_infos;
    g_asio_shared.state->pending_output_channels = num_channels;
  }

  bool both_ready = (g_asio_shared.state->pending_input != NULL &&
                     g_asio_shared.state->pending_output != NULL);

  if (both_ready) {
    size_t out_ch = g_asio_shared.state->pending_output_channels;
    size_t in_ch = g_asio_shared.state->pending_input_channels;
    long preferred_buf = g_asio_shared.state->preferred_buf_size;
    size_t total_ch = out_ch + in_ch;

    ASIOBufferInfo* combined =
        (ASIOBufferInfo*)calloc(total_ch, sizeof(ASIOBufferInfo));
    memcpy(combined, g_asio_shared.state->pending_output,
           out_ch * sizeof(ASIOBufferInfo));
    memcpy(combined + out_ch, g_asio_shared.state->pending_input,
           in_ch * sizeof(ASIOBufferInfo));

    g_asio_shared.state->callbacks_for_driver.bufferSwitch =
        buffer_switch_combined;
    g_asio_shared.state->callbacks_for_driver.sampleRateDidChange =
        sample_rate_changed_callback;
    g_asio_shared.state->callbacks_for_driver.asioMessage =
        asio_message_callback;
    g_asio_shared.state->callbacks_for_driver.bufferSwitchTimeInfo =
        buffer_switch_timeinfo_combined;

    if (!create_asio_buffers(g_asio_shared.state->iasio, combined,
                             (long)total_ch, preferred_buf,
                             &g_asio_shared.state->callbacks_for_driver, err)) {
      snprintf(g_asio_shared.state->setup_error,
               sizeof(g_asio_shared.state->setup_error),
               "ASIOCreateBuffers failed in full-duplex setup");
      WakeAllConditionVariable(&g_asio_shared.cond);
      ReleaseSRWLockExclusive(&g_asio_shared.lock);
      return false;
    }

    // Update global playback/capture context buffer_infos
    asio_playback_context_t* pb_ctx =
        atomic_load_explicit(&PLAYBACK_CONTEXT, memory_order_acquire);
    if (pb_ctx) {
      pb_ctx->buffer_infos = combined;
    }
    asio_capture_context_t* cap_ctx =
        atomic_load_explicit(&CAPTURE_CONTEXT, memory_order_acquire);
    if (cap_ctx) {
      cap_ctx->buffer_infos = combined + out_ch;
    }

    g_asio_shared.state->buffer_infos_for_driver = combined;
    g_asio_shared.state->buffer_infos_for_driver_count = total_ch;

    // Start the stream
    long start_res =
        g_asio_shared.state->iasio->lpVtbl->start(g_asio_shared.state->iasio);
    if (start_res != 0) {
      snprintf(g_asio_shared.state->setup_error,
               sizeof(g_asio_shared.state->setup_error),
               "ASIOStart failed with error code %ld", start_res);
      if (err) {
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED,
                           g_asio_shared.state->setup_error);
      }
      WakeAllConditionVariable(&g_asio_shared.cond);
      ReleaseSRWLockExclusive(&g_asio_shared.lock);
      return false;
    }

    logger_debug(&g_logger, "Full-duplex ASIO stream started.");
    g_asio_shared.state->stream_started = true;
    g_asio_shared.state->setup_error[0] = '\0';
    g_asio_shared.state->active_count = 2;
    WakeAllConditionVariable(&g_asio_shared.cond);
  } else {
    logger_debug(&g_logger,
                 "Waiting for other ASIO side to register for full-duplex...");
    while (!g_asio_shared.state->stream_started &&
           g_asio_shared.state->setup_error[0] == '\0') {
      SleepConditionVariableSRW(&g_asio_shared.cond, &g_asio_shared.lock,
                                INFINITE, 0);
    }
    if (g_asio_shared.state->setup_error[0] != '\0') {
      if (err) {
        char msg[384];
        snprintf(msg, sizeof(msg), "ASIO full-duplex setup aborted: %s",
                 g_asio_shared.state->setup_error);
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
      }
      ReleaseSRWLockExclusive(&g_asio_shared.lock);
      return false;
    }
    logger_debug(&g_logger, "Full-duplex ASIO setup complete, proceeding.");
  }

  size_t out_ch = g_asio_shared.state->pending_output_channels;
  *out_buffer_infos =
      is_input ? (g_asio_shared.state->buffer_infos_for_driver + out_ch)
               : g_asio_shared.state->buffer_infos_for_driver;
  *out_buf_size = g_asio_shared.state->preferred_buf_size;
  *out_iasio = g_asio_shared.state->iasio;

  ReleaseSRWLockExclusive(&g_asio_shared.lock);
  return true;
}

/**
 * @brief release_shared_asio matching CamillaDSP device.rs lines 763-792.
 */
static void release_shared_asio(void) {
  AcquireSRWLockExclusive(&g_asio_shared.lock);
  if (g_asio_shared.state) {
    if (g_asio_shared.state->active_count > 0) {
      g_asio_shared.state->active_count--;
    }
    if (g_asio_shared.state->active_count == 1) {
      logger_debug(&g_logger, "First ASIO side exiting, stopping stream.");
      atomic_store_explicit(&PLAYBACK_CONTEXT, NULL, memory_order_release);
      atomic_store_explicit(&CAPTURE_CONTEXT, NULL, memory_order_release);
      if (g_asio_shared.state->iasio) {
        g_asio_shared.state->iasio->lpVtbl->stop(g_asio_shared.state->iasio);
      }
    } else if (g_asio_shared.state->active_count == 0) {
      logger_debug(&g_logger, "Last ASIO side exiting, disposing driver.");
      if (g_asio_shared.state->iasio) {
        g_asio_shared.state->iasio->lpVtbl->disposeBuffers(
            g_asio_shared.state->iasio);
        teardown_asio_driver(&g_asio_shared.state->iasio);
      }
      if (g_asio_shared.state->buffer_infos_for_driver) {
        free(g_asio_shared.state->buffer_infos_for_driver);
      }
      if (g_asio_shared.state->pending_output) {
        free(g_asio_shared.state->pending_output);
      }
      if (g_asio_shared.state->pending_input) {
        free(g_asio_shared.state->pending_input);
      }
      free(g_asio_shared.state);
      g_asio_shared.state = NULL;
    }
  }
  ReleaseSRWLockExclusive(&g_asio_shared.lock);
}

// MARK: - Low-level ASIO helpers matching CamillaDSP device.rs lines 798-1188

static bool asio_check_drv_path(const char* clsid_str, char* out_dll_path,
                                size_t max_path) {
  char clsid_key[384];
  snprintf(clsid_key, sizeof(clsid_key), "clsid\\%s\\InprocServer32",
           clsid_str);
  HKEY hkpath;
  if (RegOpenKeyExA(HKEY_CLASSES_ROOT, clsid_key, 0, KEY_READ, &hkpath) ==
      ERROR_SUCCESS) {
    DWORD datatype = REG_SZ;
    DWORD datasize = (DWORD)max_path;
    LONG cr = RegQueryValueExA(hkpath, NULL, 0, &datatype, (LPBYTE)out_dll_path,
                               &datasize);
    RegCloseKey(hkpath);
    if (cr == ERROR_SUCCESS) {
      if (GetFileAttributesA(out_dll_path) != INVALID_FILE_ATTRIBUTES) {
        return true;
      }
    }
  }
  return false;
}

static bool find_asio_driver_clsid(const char* driver_name, CLSID* out_clsid) {
  HKEY hk;
  if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\ASIO", 0, KEY_READ, &hk) !=
      ERROR_SUCCESS) {
    return false;
  }

  char subkey_name[256];
  DWORD index = 0;
  bool found = false;

  while (RegEnumKeyA(hk, index++, subkey_name, sizeof(subkey_name)) ==
         ERROR_SUCCESS) {
    HKEY hk_driver;
    if (RegOpenKeyExA(hk, subkey_name, 0, KEY_READ, &hk_driver) ==
        ERROR_SUCCESS) {
      char clsid_str[128];
      DWORD size = sizeof(clsid_str);
      if (RegQueryValueExA(hk_driver, "CLSID", NULL, NULL, (LPBYTE)clsid_str,
                           &size) == ERROR_SUCCESS) {
        char dllpath[MAX_PATH];
        if (asio_check_drv_path(clsid_str, dllpath, sizeof(dllpath))) {
          char drv_name[256];
          DWORD desc_size = sizeof(drv_name);
          if (RegQueryValueExA(hk_driver, "description", NULL, NULL,
                               (LPBYTE)drv_name, &desc_size) != ERROR_SUCCESS ||
              drv_name[0] == '\0') {
            snprintf(drv_name, sizeof(drv_name), "%s", subkey_name);
          }

          if (!driver_name || driver_name[0] == '\0' ||
              strcasecmp(driver_name, "default") == 0 ||
              strcasecmp(drv_name, driver_name) == 0 ||
              strcasecmp(subkey_name, driver_name) == 0) {
            wchar_t wclsid_str[128];
            mbstowcs(wclsid_str, clsid_str, 128);
            if (SUCCEEDED(CLSIDFromString(wclsid_str, out_clsid))) {
              found = true;
            }
          }
        }
      }
      RegCloseKey(hk_driver);
    }
    if (found) break;
  }
  RegCloseKey(hk);
  return found;
}

static HRESULT create_asio_com_instance(const CLSID* clsid, IASIO** out_iasio) {
  HRESULT hr = CoCreateInstance(clsid, NULL, CLSCTX_INPROC_SERVER, clsid,
                                (void**)out_iasio);
  if (FAILED(hr)) {
    hr = CoCreateInstance(clsid, NULL, CLSCTX_INPROC_SERVER, &g_IID_IASIO_VAL,
                          (void**)out_iasio);
  }
  if (FAILED(hr)) {
    hr = CoCreateInstance(clsid, NULL, CLSCTX_INPROC_SERVER, &IID_IUnknown,
                          (void**)out_iasio);
  }
  return hr;
}

/**
 * @brief teardown_asio_driver matching CamillaDSP device.rs lines 807-815.
 */
static void teardown_asio_driver(IASIO** p_iasio) {
  if (!atomic_exchange_explicit(&ASIO_DRIVER_INITIALIZED, false,
                                memory_order_acq_rel)) {
    logger_trace(&g_logger,
                 "teardown_asio_driver: no driver initialized, nothing to do");
    if (p_iasio && *p_iasio) {
      SAFE_RELEASE(*p_iasio);
    }
    return;
  }
  logger_trace(&g_logger, "teardown_asio_driver: removing current driver");
  if (p_iasio && *p_iasio) {
    SAFE_RELEASE(*p_iasio);
  }
  logger_trace(&g_logger, "teardown_asio_driver: done");
}

/**
 * @brief load_driver_by_name matching CamillaDSP device.rs lines 821-860.
 */
static bool load_driver_by_name(const char* name, IASIO** out_iasio,
                                backend_error_t* err) {
  logger_trace(&g_logger, "load_driver_by_name: loading '%s'", name);
  teardown_asio_driver(out_iasio);

  HRESULT co_hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  logger_trace(&g_logger,
               "load_driver_by_name: CoInitializeEx returned 0x%08lX",
               (unsigned long)co_hr);

  CLSID clsid;
  if (!find_asio_driver_clsid(name, &clsid)) {
    if (err) {
      char msg[256];
      snprintf(msg, sizeof(msg), "Failed to load ASIO driver '%s'", name);
      backend_error_init(err, BACKEND_ERROR_DEVICE_NOT_FOUND, msg);
    }
    return false;
  }

  IASIO* iasio = NULL;
  HRESULT hr = create_asio_com_instance(&clsid, &iasio);
  if (FAILED(hr) || !iasio) {
    if (err) {
      char msg[256];
      snprintf(msg, sizeof(msg), "Failed to load ASIO driver '%s'", name);
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
    return false;
  }

  if (!iasio->lpVtbl->init(iasio, GetDesktopWindow())) {
    SAFE_RELEASE(iasio);
    if (err) {
      char msg[256];
      snprintf(msg, sizeof(msg), "ASIOInit failed for driver '%s'", name);
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
    return false;
  }

  atomic_store_explicit(&ASIO_DRIVER_INITIALIZED, true, memory_order_release);
  logger_trace(&g_logger, "load_driver_by_name: '%s' loaded and initialised",
               name);
  *out_iasio = iasio;
  return true;
}

/**
 * @brief force_sample_rate_with_dummy_cycle matching CamillaDSP device.rs lines
 * 875-1008.
 */
static bool force_sample_rate_with_dummy_cycle(const char* devname, double rate,
                                               IASIO** p_iasio,
                                               backend_error_t* err) {
  IASIO* iasio = *p_iasio;
  long num_in = 0, num_out = 0;
  long ch_res = iasio->lpVtbl->getChannels(iasio, &num_in, &num_out);
  if (ch_res != 0) {
    if (err) {
      char msg[256];
      snprintf(
          msg, sizeof(msg),
          "ASIOGetChannels failed during rate-change cycle (error code %ld)",
          ch_res);
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
    return false;
  }

  long preferred_buf = 0;
  if (!get_preferred_buffer_size(iasio, &preferred_buf, err)) {
    return false;
  }

  bool is_input = (num_out == 0);
  ASIOBufferInfo dummy_bufs[1];
  dummy_bufs[0].isInput = is_input ? ASIOTrue : ASIOFalse;
  dummy_bufs[0].channelNum = 0;
  dummy_bufs[0].buffers[0] = NULL;
  dummy_bufs[0].buffers[1] = NULL;

  ASIOCallbacks dummy_callbacks = {
      .bufferSwitch = buffer_switch_combined,
      .sampleRateDidChange = NULL,
      .asioMessage = asio_message_callback,
      .bufferSwitchTimeInfo = buffer_switch_timeinfo_combined,
  };

  long create_res = iasio->lpVtbl->createBuffers(
      iasio, dummy_bufs, 1, preferred_buf, &dummy_callbacks);
  if (create_res != 0) {
    if (err) {
      char msg[256];
      snprintf(
          msg, sizeof(msg),
          "ASIOCreateBuffers failed during rate-change cycle (error code %ld)",
          create_res);
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
    return false;
  }

  long start_res = iasio->lpVtbl->start(iasio);
  if (start_res != 0) {
    iasio->lpVtbl->disposeBuffers(iasio);
    if (err) {
      char msg[256];
      snprintf(msg, sizeof(msg),
               "ASIOStart failed during rate-change cycle (error code %ld)",
               start_res);
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
    return false;
  }

  cdsp_sleep_ms(50);
  iasio->lpVtbl->stop(iasio);
  iasio->lpVtbl->disposeBuffers(iasio);

  teardown_asio_driver(p_iasio);
  cdsp_sleep_ms(50);
  if (!load_driver_by_name(devname, p_iasio, err)) {
    return false;
  }
  iasio = *p_iasio;

  if (rate >= 1000000.0) {
    ASIOIoFormat dsd_format = {0};
    dsd_format.FormatType = kASIOFormatDSD;
    iasio->lpVtbl->future(iasio, kAsioSetIoFormat, &dsd_format);
  }

  long set_res = iasio->lpVtbl->setSampleRate(iasio, rate);
  if (set_res != 0) {
    if (err) {
      char msg[256];
      snprintf(
          msg, sizeof(msg),
          "Failed to set sample rate after rate-change cycle (error code %ld)",
          set_res);
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
    return false;
  }

  double verify = 0.0;
  long verify_res = iasio->lpVtbl->getSampleRate(iasio, &verify);
  if (verify_res != 0) {
    if (err) {
      char msg[256];
      snprintf(msg, sizeof(msg),
               "Failed to read ASIO sample rate after rate-change cycle (error "
               "code %ld)",
               verify_res);
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
    return false;
  }

  logger_debug(&g_logger,
               "ASIO sample rate after dummy-stream cycle: %.1f Hz (requested "
               "%.1f Hz).",
               verify, rate);
  if (fabs(verify - rate) > 0.5) {
    if (err) {
      char msg[384];
      snprintf(msg, sizeof(msg),
               "ASIO sample rate is %.1f Hz after rate-change cycle, expected "
               "%.1f Hz. The driver may require the rate to be set via its "
               "control panel.",
               verify, rate);
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
    return false;
  }

  return true;
}

/**
 * @brief open_asio_device matching CamillaDSP device.rs lines 1014-1152.
 */
static bool open_asio_device(const char* devname, int samplerate, bool is_dsd,
                             IASIO** out_iasio, long* out_inputs,
                             long* out_outputs, backend_error_t* err) {
  logger_trace(&g_logger,
               "open_asio_device: dev='%s', samplerate=%d, is_dsd=%d", devname,
               samplerate, (int)is_dsd);

  if (!load_driver_by_name(devname, out_iasio, err)) {
    return false;
  }
  IASIO* iasio = *out_iasio;

  if (is_dsd) {
    ASIOIoFormat dsd_format = {0};
    dsd_format.FormatType = kASIOFormatDSD;
    ASIOError io_res = (ASIOError)(uintptr_t)iasio->lpVtbl->future(
        iasio, kAsioSetIoFormat, &dsd_format);
    if (io_res == 0 || io_res == (ASIOError)ASE_SUCCESS) {
      logger_info(&g_logger,
                  "ASIO driver successfully set to Native DSD format via "
                  "kAsioSetIoFormat");
    } else {
      logger_warn(
          &g_logger,
          "ASIO driver kAsioSetIoFormat DSD returned %ld (FormatType=%ld)",
          io_res, (long)dsd_format.FormatType);
    }
  }

  double current_rate = 0.0;
  long rate_res = iasio->lpVtbl->getSampleRate(iasio, &current_rate);
  if (rate_res != 0) {
    teardown_asio_driver(out_iasio);
    if (err) {
      char msg[256];
      snprintf(msg, sizeof(msg),
               "Failed to read ASIO sample rate (error code %ld)", rate_res);
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
    return false;
  }
  logger_debug(&g_logger, "ASIO current sample rate: %.1f Hz", current_rate);

  double rate = (double)(is_dsd ? (samplerate * 32) : samplerate);
  if (iasio->lpVtbl->canSampleRate(iasio, rate) != 0) {
    teardown_asio_driver(out_iasio);
    if (err) {
      char msg[256];
      snprintf(msg, sizeof(msg),
               "ASIO device does not support sample rate %.0f Hz.", rate);
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
    return false;
  }

  bool already_correct = (fabs(current_rate - rate) <= 0.5);
  if (already_correct) {
    logger_debug(&g_logger,
                 "ASIO sample rate already at %.0f Hz, no change needed.",
                 rate);
  } else {
    long set_res = iasio->lpVtbl->setSampleRate(iasio, rate);
    if (set_res != 0) {
      teardown_asio_driver(out_iasio);
      if (err) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "Failed to set ASIO sample rate to %.0f Hz (error code %ld)",
                 rate, set_res);
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
      }
      return false;
    }

    logger_debug(&g_logger,
                 "Forcing ASIO rate change to %.0f Hz via dummy stream cycle.",
                 rate);
    if (!force_sample_rate_with_dummy_cycle(devname, rate, out_iasio, err)) {
      return false;
    }
    iasio = *out_iasio;
  }

  long num_inputs = 0, num_outputs = 0;
  long channels_res =
      iasio->lpVtbl->getChannels(iasio, &num_inputs, &num_outputs);
  if (channels_res != 0) {
    teardown_asio_driver(out_iasio);
    if (err) {
      char msg[256];
      snprintf(msg, sizeof(msg), "ASIOGetChannels failed (error code %ld)",
               channels_res);
      backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
    }
    return false;
  }
  logger_debug(&g_logger,
               "ASIO device opened: %ld input channels, %ld output channels.",
               num_inputs, num_outputs);

  for (long ch = 0; ch < num_inputs; ch++) {
    ASIOChannelInfo info = {0};
    info.channel = (int32_t)ch;
    info.isInput = ASIOTrue;
    if (iasio->lpVtbl->getChannelInfo(iasio, &info) == 0) {
      logger_debug(&g_logger, "  Input  channel %ld: name='%s', format=%d (%s)",
                   ch, info.name, info.type, asio_sample_type_name(info.type));
    }
  }
  for (long ch = 0; ch < num_outputs; ch++) {
    ASIOChannelInfo info = {0};
    info.channel = (int32_t)ch;
    info.isInput = ASIOFalse;
    if (iasio->lpVtbl->getChannelInfo(iasio, &info) == 0) {
      logger_debug(&g_logger, "  Output channel %ld: name='%s', format=%d (%s)",
                   ch, info.name, info.type, asio_sample_type_name(info.type));
    }
  }

  *out_inputs = num_inputs;
  *out_outputs = num_outputs;
  return true;
}

/**
 * @brief open_asio_playback matching CamillaDSP device.rs lines 1156-1170.
 */
static bool open_asio_playback(const char* devname, size_t num_channels,
                               int samplerate,
                               asio_sample_format_t configured_format,
                               bool has_format, IASIO** out_iasio,
                               asio_sample_format_t* out_resolved_format,
                               backend_error_t* err) {
  long inputs = 0, outputs = 0;
  bool is_dsd = (configured_format == ASIO_SAMPLE_FORMAT_DSD_INT8);
  if (!open_asio_device(devname, samplerate, is_dsd, out_iasio, &inputs,
                        &outputs, err)) {
    return false;
  }
  if (num_channels > (size_t)outputs) {
    teardown_asio_driver(out_iasio);
    if (err) {
      char msg[256];
      snprintf(msg, sizeof(msg),
               "Requested %zu output channels but device only has %ld",
               num_channels, outputs);
      backend_error_init(err, BACKEND_ERROR_INVALID_CHANNELS, msg);
    }
    return false;
  }
  if (!resolve_format(*out_iasio, configured_format, has_format, false,
                      out_resolved_format, err)) {
    teardown_asio_driver(out_iasio);
    return false;
  }
  return true;
}

/**
 * @brief open_asio_capture matching CamillaDSP device.rs lines 1174-1188.
 */
static bool open_asio_capture(const char* devname, size_t num_channels,
                              int samplerate,
                              asio_sample_format_t configured_format,
                              bool has_format, IASIO** out_iasio,
                              asio_sample_format_t* out_resolved_format,
                              backend_error_t* err) {
  long inputs = 0, outputs = 0;
  bool is_dsd = (configured_format == ASIO_SAMPLE_FORMAT_DSD_INT8);
  if (!open_asio_device(devname, samplerate, is_dsd, out_iasio, &inputs,
                        &outputs, err)) {
    return false;
  }
  if (num_channels > (size_t)inputs) {
    teardown_asio_driver(out_iasio);
    if (err) {
      char msg[256];
      snprintf(msg, sizeof(msg),
               "Requested %zu input channels but device only has %ld",
               num_channels, inputs);
      backend_error_init(err, BACKEND_ERROR_INVALID_CHANNELS, msg);
    }
    return false;
  }
  if (!resolve_format(*out_iasio, configured_format, has_format, true,
                      out_resolved_format, err)) {
    teardown_asio_driver(out_iasio);
    return false;
  }
  return true;
}

// MARK: - Playback Backend Struct and VTable Methods

struct asio_playback {
  char device[256];
  int sample_rate;
  int channels;
  int chunk_size;
  asio_sample_format_t format;
  bool has_format;
  bool full_duplex;
  int target_level;

  asio_sample_format_t resolved_format;
  bool is_lsb;
  size_t bytes_per_sample;
  long actual_buffer_size;

  IASIO* iasio;
  ASIOBufferInfo* buffer_infos;
  bool single_mode_allocated_infos;
  ASIOCallbacks callbacks_for_driver;

  spsc_byte_ring_buffer_t* ring_buffer;
  uint8_t* encode_buf;
  size_t encode_buf_size;

  asio_playback_context_t* context;
  _Atomic bool is_running;
  _Atomic bool stopped;
  _Atomic bool paused;
  bool com_initialized;
};

static void asio_playback_close(void* ctx) {
  asio_playback_t* playback = (asio_playback_t*)ctx;
  if (!playback) return;

  logger_debug(&g_logger, "Stopping ASIO playback.");
  if (playback->full_duplex) {
    release_shared_asio();
  } else {
    atomic_store_explicit(&PLAYBACK_CONTEXT, NULL, memory_order_release);
    if (playback->iasio) {
      playback->iasio->lpVtbl->stop(playback->iasio);
      playback->iasio->lpVtbl->disposeBuffers(playback->iasio);
      teardown_asio_driver(&playback->iasio);
    }
  }
  atomic_store_explicit(&PLAYBACK_CONTEXT, NULL, memory_order_release);

  if (playback->context) {
    if (playback->context->transfer_buf) {
      free(playback->context->transfer_buf);
    }
    free(playback->context);
    playback->context = NULL;
  }

  if (playback->single_mode_allocated_infos && playback->buffer_infos) {
    free(playback->buffer_infos);
    playback->buffer_infos = NULL;
  }

  if (playback->ring_buffer) {
    spsc_byte_ring_buffer_free(playback->ring_buffer);
    playback->ring_buffer = NULL;
  }
  if (playback->encode_buf) {
    free(playback->encode_buf);
    playback->encode_buf = NULL;
    playback->encode_buf_size = 0;
  }

  if (playback->com_initialized) {
    CoUninitialize();
    playback->com_initialized = false;
  }
}

/**
 * @brief open matching CamillaDSP device.rs AsioPlaybackDevice::start (lines
 * 1333-1604).
 */
static bool asio_playback_open(void* ctx, backend_error_t* err) {
  asio_playback_t* playback = (asio_playback_t*)ctx;
  if (!playback) return false;

  HRESULT co_hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  playback->com_initialized = SUCCEEDED(co_hr);

  asio_sample_format_t resolved_format = ASIO_SAMPLE_FORMAT_S32_LE;
  long asio_buffer_size = 0;

  if (playback->full_duplex) {
    long inputs = 0, outputs = 0, preferred_buf = 0;
    bool is_dsd = (playback->format == ASIO_SAMPLE_FORMAT_DSD_INT8);
    if (!init_shared_asio(playback->device, playback->sample_rate, is_dsd,
                          &inputs, &outputs, &preferred_buf, err)) {
      goto error_cleanup;
    }
    if ((size_t)playback->channels > (size_t)outputs) {
      if (err) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "Requested %d output channels but device only has %ld",
                 playback->channels, outputs);
        backend_error_init(err, BACKEND_ERROR_INVALID_CHANNELS, msg);
      }
      goto error_cleanup;
    }
    if (!resolve_format(g_asio_shared.state->iasio, playback->format,
                        playback->has_format, false, &resolved_format, err)) {
      goto error_cleanup;
    }
    asio_buffer_size = preferred_buf;
  } else {
    if (!open_asio_playback(playback->device, (size_t)playback->channels,
                            playback->sample_rate, playback->format,
                            playback->has_format, &playback->iasio,
                            &resolved_format, err)) {
      goto error_cleanup;
    }
    long preferred_buf = 0;
    if (!get_preferred_buffer_size(playback->iasio, &preferred_buf, err)) {
      goto error_cleanup;
    }
    asio_buffer_size = preferred_buf;
  }

  // Detect whether channel 0 uses LSB-first bit ordering for DSD
  bool is_lsb = false;
  IASIO* active_iasio =
      playback->full_duplex ? g_asio_shared.state->iasio : playback->iasio;
  if (active_iasio) {
    ASIOChannelInfo ch_info = {0};
    ch_info.channel = 0;
    ch_info.isInput = ASIOFalse;
    if (active_iasio->lpVtbl->getChannelInfo(active_iasio, &ch_info) == 0) {
      if (ch_info.type == ASIO_ST_DSD_INT8_LSB_1) {
        is_lsb = true;
      }
    }
  }

  long driver_buffer_size = asio_buffer_size;
  if (resolved_format == ASIO_SAMPLE_FORMAT_DSD_INT8) {
    asio_buffer_size /= 32;
  }

  playback->resolved_format = resolved_format;
  playback->is_lsb = is_lsb;
  playback->bytes_per_sample = asio_format_bytes_per_sample(resolved_format);
  playback->actual_buffer_size = asio_buffer_size;

  // Size ring buffer to fit at least driver's buffer size (issue #498)
  size_t ring_frames = ((size_t)playback->chunk_size > (size_t)asio_buffer_size)
                           ? (size_t)playback->chunk_size
                           : (size_t)asio_buffer_size;
  size_t ring_bytes = (size_t)playback->channels * playback->bytes_per_sample *
                      (2 * ring_frames + 2048);
  playback->ring_buffer = spsc_byte_ring_buffer_create(ring_bytes);

  playback->encode_buf_size = (size_t)playback->channels *
                              (size_t)playback->chunk_size *
                              playback->bytes_per_sample * 2;
  playback->encode_buf = (uint8_t*)malloc(playback->encode_buf_size);

  clear_playback_rate_change_event();
  reset_playback_callback_seen();

  size_t target_level = (playback->target_level > 0)
                            ? (size_t)playback->target_level
                            : (size_t)playback->chunk_size;

  playback->context =
      (asio_playback_context_t*)calloc(1, sizeof(asio_playback_context_t));
  playback->context->ring_buffer = playback->ring_buffer;
  playback->context->num_channels = (size_t)playback->channels;
  playback->context->buffer_size = (size_t)asio_buffer_size;
  playback->context->bytes_per_sample = playback->bytes_per_sample;
  playback->context->transfer_buf_cap = (size_t)asio_buffer_size *
                                        playback->bytes_per_sample *
                                        (size_t)playback->channels;
  playback->context->transfer_buf =
      (uint8_t*)malloc(playback->context->transfer_buf_cap);
  playback->context->target_level = target_level;
  playback->context->silence_frames_to_insert = 0;
  playback->context->running = false;
  playback->context->silence_byte =
      (resolved_format == ASIO_SAMPLE_FORMAT_DSD_INT8) ? 0x69 : 0x00;
  atomic_init(&playback->context->buffer_fill, 0.0);

  if (playback->full_duplex) {
    atomic_store_explicit(&PLAYBACK_CONTEXT, playback->context,
                          memory_order_release);
    if (!register_and_wait(
            false, (size_t)playback->channels, &playback->buffer_infos,
            &playback->actual_buffer_size, &playback->iasio, err)) {
      atomic_store_explicit(&PLAYBACK_CONTEXT, NULL, memory_order_release);
      goto error_cleanup;
    }
    if (resolved_format == ASIO_SAMPLE_FORMAT_DSD_INT8) {
      playback->actual_buffer_size /= 32;
    }
  } else {
    playback->buffer_infos =
        make_buffer_infos((size_t)playback->channels, false);
    playback->single_mode_allocated_infos = true;
    playback->callbacks_for_driver.bufferSwitch = buffer_switch_playback;
    playback->callbacks_for_driver.sampleRateDidChange =
        sample_rate_changed_callback;
    playback->callbacks_for_driver.asioMessage = asio_message_callback;
    playback->callbacks_for_driver.bufferSwitchTimeInfo =
        buffer_switch_timeinfo_playback;

    if (!create_asio_buffers(playback->iasio, playback->buffer_infos,
                             (long)playback->channels, driver_buffer_size,
                             &playback->callbacks_for_driver, err)) {
      goto error_cleanup;
    }

    playback->context->buffer_infos = playback->buffer_infos;
    atomic_store_explicit(&PLAYBACK_CONTEXT, playback->context,
                          memory_order_release);

    logger_trace(&g_logger, "Playback: calling ASIOStart()");
    long start_res = playback->iasio->lpVtbl->start(playback->iasio);
    if (start_res != 0) {
      atomic_store_explicit(&PLAYBACK_CONTEXT, NULL, memory_order_release);
      if (err) {
        char msg[256];
        snprintf(msg, sizeof(msg), "ASIOStart failed with error code %ld",
                 start_res);
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
      }
      goto error_cleanup;
    }
    logger_trace(&g_logger, "Playback: ASIOStart() succeeded");
  }

  logger_debug(&g_logger, "Playback device ready and waiting.");
  bool got_callback = wait_for_playback_callback(500);
  logger_trace(&g_logger,
               "Playback startup callback gate: first_callback_received=%d",
               got_callback);
  logger_debug(&g_logger, "Playback device starts now!");

  atomic_store_explicit(&playback->is_running, true, memory_order_release);
  return true;

error_cleanup:
  asio_playback_close(playback);
  return false;
}

/**
 * @brief write matching CamillaDSP device.rs lines 1606-1709.
 */
static bool asio_playback_write(void* ctx, const audio_chunk_t* chunk,
                                backend_error_t* err) {
  asio_playback_t* playback = (asio_playback_t*)ctx;
  if (!playback) return false;
  if (atomic_load_explicit(&playback->paused, memory_order_acquire))
    return true;

  if (take_playback_rate_change_event()) {
    if (err) {
      backend_error_init(err, BACKEND_ERROR_NONE, "Sample rate changed");
    }
    return false;
  }

  if (atomic_load_explicit(&playback->stopped, memory_order_acquire) ||
      !atomic_load_explicit(&playback->is_running, memory_order_acquire)) {
    if (err) {
      backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                         "Playback stream stopped");
    }
    return false;
  }

  size_t valid_frames = audio_chunk_get_valid_frames(chunk);
  size_t bytes_to_write =
      valid_frames * (size_t)playback->channels * playback->bytes_per_sample;

  if (bytes_to_write > playback->encode_buf_size) {
    if (err) {
      backend_error_init(err, BACKEND_ERROR_WRITE_ERROR,
                         "Frame count exceeds playback buffer capacity");
    }
    return false;
  }

  // Convert chunk to interleaved raw bytes
  const double* src_channels[playback->channels];
  for (int c = 0; c < playback->channels; c++) {
    src_channels[c] = audio_chunk_get_channel(chunk, c);
  }

  for (size_t f = 0; f < valid_frames; f++) {
    for (int c = 0; c < playback->channels; c++) {
      double sample = src_channels[c][f];
      uint8_t* dst =
          playback->encode_buf + (f * (size_t)playback->channels + (size_t)c) *
                                     playback->bytes_per_sample;
      switch (playback->resolved_format) {
        case ASIO_SAMPLE_FORMAT_S16_LE:
          pcm_sample_encode_s16_bytes(sample, dst);
          break;
        case ASIO_SAMPLE_FORMAT_S24_3_LE:
          pcm_sample_encode_s24_3bytes(sample, dst);
          break;
        case ASIO_SAMPLE_FORMAT_S24_4_LE:
          pcm_sample_encode_s24_4_lj_bytes(sample, dst);
          break;
        case ASIO_SAMPLE_FORMAT_S32_LE:
          pcm_sample_encode_s32_bytes(sample, dst);
          break;
        case ASIO_SAMPLE_FORMAT_F32_LE:
          pcm_sample_encode_f32_bytes(sample, dst);
          break;
        case ASIO_SAMPLE_FORMAT_F64_LE:
          pcm_sample_encode_f64_bytes(sample, dst);
          break;
        case ASIO_SAMPLE_FORMAT_DSD_INT8:
          if (playback->is_lsb) {
            pcm_sample_encode_dsd_u32_reversed_bytes(sample, dst);
          } else {
            pcm_sample_encode_dsd_u32_be_bytes(sample, dst);
          }
          break;
        default:
          pcm_sample_encode_s32_bytes(sample, dst);
          break;
      }
    }
  }

  // Sleep duration based on time to play back one chunksize (lines 1670-1679)
  size_t sleep_duration_us =
      (size_t)(1000000ULL * (unsigned long long)playback->chunk_size /
               (unsigned long long)playback->sample_rate / 2ULL);
  DWORD sleep_duration_ms = (DWORD)(sleep_duration_us / 1000);
  if (sleep_duration_ms == 0) sleep_duration_ms = 1;

  for (int retry = 0; retry < 8; retry++) {
    if (spsc_byte_ring_buffer_get_available_to_write(playback->ring_buffer) >=
        bytes_to_write) {
      break;
    }
    cdsp_sleep_ms(sleep_duration_ms);
  }

  size_t pushed_bytes = spsc_byte_ring_buffer_write(
      playback->ring_buffer, playback->encode_buf, bytes_to_write);
  if (pushed_bytes < bytes_to_write) {
    logger_debug(&g_logger,
                 "Playback ring buffer is full, dropped %zu out of %zu bytes.",
                 bytes_to_write - pushed_bytes, bytes_to_write);
  }

  return true;
}

static size_t asio_playback_get_buffer_level(void* ctx) {
  asio_playback_t* playback = (asio_playback_t*)ctx;
  if (!playback || !playback->context) return 0;
  return (size_t)atomic_load_explicit(&playback->context->buffer_fill,
                                      memory_order_relaxed);
}

static bool asio_playback_get_pending_rate_change(void* ctx, double* out_rate) {
  asio_playback_t* playback = (asio_playback_t*)ctx;
  if (!playback) return false;
  if (take_playback_rate_change_event()) {
    int new_rate = read_current_asio_sample_rate_hz(playback->iasio);
    if (out_rate) {
      *out_rate = (double)new_rate;
    }
    return true;
  }
  return false;
}

static bool asio_playback_prefill_silence(void* ctx, size_t frames,
                                          backend_error_t* err) {
  (void)err;
  asio_playback_t* playback = (asio_playback_t*)ctx;
  if (!playback) return false;
  playback->target_level = (int)frames;
  return true;
}

static void asio_playback_stop(void* ctx) {
  asio_playback_t* playback = (asio_playback_t*)ctx;
  if (!playback) return;
  atomic_store_explicit(&playback->stopped, true, memory_order_release);
}

static void asio_playback_destroy(void* ctx) {
  asio_playback_t* playback = (asio_playback_t*)ctx;
  if (playback) {
    asio_playback_close(playback);
    free(playback);
  }
}

static playback_backend_t* asio_playback_create(
    const playback_device_config_t* config, int sample_rate, int chunk_size,
    bool full_duplex, processing_parameters_t* params, backend_error_t* err) {
  (void)params;
  (void)err;
  asio_playback_t* playback =
      (asio_playback_t*)calloc(1, sizeof(asio_playback_t));
  if (!playback) return NULL;

  snprintf(playback->device, sizeof(playback->device), "%s",
           config->cfg.asio.device);

  playback->sample_rate = sample_rate;
  playback->channels = config->cfg.asio.channels;
  playback->chunk_size = chunk_size;
  playback->format = config->cfg.asio.format;
  playback->has_format =
      (config->cfg.asio.format != ASIO_SAMPLE_FORMAT_INVALID);
  playback->full_duplex = full_duplex;

  atomic_init(&playback->is_running, false);
  atomic_init(&playback->stopped, false);
  atomic_init(&playback->paused, false);

  playback_backend_t* backend =
      (playback_backend_t*)calloc(1, sizeof(playback_backend_t));
  if (!backend) {
    free(playback);
    return NULL;
  }
  backend->ctx = playback;
  backend->vtable = &g_asio_playback_vtable;
  return backend;
}

const playback_backend_vtable_t g_asio_playback_vtable = {
    .create = asio_playback_create,
    .open = asio_playback_open,
    .write = asio_playback_write,
    .close = asio_playback_close,
    .get_buffer_level = asio_playback_get_buffer_level,
    .get_pending_rate_change = asio_playback_get_pending_rate_change,
    .prefill_silence = asio_playback_prefill_silence,
    .get_is_paused = NULL,
    .set_is_paused = NULL,
    .pitch_control_supported = NULL,
    .set_pitch = NULL,
    .stop = asio_playback_stop,
    .destroy = asio_playback_destroy,
};

// MARK: - Capture Backend Struct and VTable Methods

struct asio_capture {
  char device[256];
  int sample_rate;
  int channels;
  int chunk_size;
  asio_sample_format_t format;
  bool has_format;
  bool full_duplex;

  asio_sample_format_t resolved_format;
  bool is_lsb;
  size_t bytes_per_sample;
  long actual_buffer_size;

  IASIO* iasio;
  ASIOBufferInfo* buffer_infos;
  bool single_mode_allocated_infos;
  ASIOCallbacks callbacks_for_driver;

  spsc_byte_ring_buffer_t* ring_buffer;
  cdsp_sem_t semaphore;
  uint8_t* decode_buf;
  size_t decode_buf_size;

  asio_capture_context_t* context;
  _Atomic bool is_running;
  _Atomic bool stopped;
  bool com_initialized;
};

static void asio_capture_close(void* ctx) {
  asio_capture_t* capture = (asio_capture_t*)ctx;
  if (!capture) return;

  logger_debug(&g_logger, "Stopping ASIO capture.");
  if (capture->full_duplex) {
    release_shared_asio();
  } else {
    atomic_store_explicit(&CAPTURE_CONTEXT, NULL, memory_order_release);
    if (capture->iasio) {
      capture->iasio->lpVtbl->stop(capture->iasio);
      capture->iasio->lpVtbl->disposeBuffers(capture->iasio);
      teardown_asio_driver(&capture->iasio);
    }
  }
  atomic_store_explicit(&CAPTURE_CONTEXT, NULL, memory_order_release);

  if (capture->context) {
    if (capture->context->transfer_buf) {
      free(capture->context->transfer_buf);
    }
    free(capture->context);
    capture->context = NULL;
  }

  if (capture->single_mode_allocated_infos && capture->buffer_infos) {
    free(capture->buffer_infos);
    capture->buffer_infos = NULL;
  }

  if (capture->semaphore) {
    cdsp_sem_destroy(capture->semaphore);
    capture->semaphore = NULL;
  }
  if (capture->ring_buffer) {
    spsc_byte_ring_buffer_free(capture->ring_buffer);
    capture->ring_buffer = NULL;
  }
  if (capture->decode_buf) {
    free(capture->decode_buf);
    capture->decode_buf = NULL;
    capture->decode_buf_size = 0;
  }

  if (capture->com_initialized) {
    CoUninitialize();
    capture->com_initialized = false;
  }
}

/**
 * @brief open matching CamillaDSP device.rs AsioCaptureDevice::start (lines
 * 1744-2030).
 */
static bool asio_capture_open(void* ctx, backend_error_t* err) {
  asio_capture_t* capture = (asio_capture_t*)ctx;
  if (!capture) return false;

  HRESULT co_hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  capture->com_initialized = SUCCEEDED(co_hr);

  asio_sample_format_t resolved_format = ASIO_SAMPLE_FORMAT_S32_LE;
  long asio_buffer_size = 0;

  if (capture->full_duplex) {
    long inputs = 0, outputs = 0, preferred_buf = 0;
    bool is_dsd = (capture->format == ASIO_SAMPLE_FORMAT_DSD_INT8);
    if (!init_shared_asio(capture->device, capture->sample_rate, is_dsd,
                          &inputs, &outputs, &preferred_buf, err)) {
      goto error_cleanup;
    }
    if ((size_t)capture->channels > (size_t)inputs) {
      if (err) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "Requested %d input channels but device only has %ld",
                 capture->channels, inputs);
        backend_error_init(err, BACKEND_ERROR_INVALID_CHANNELS, msg);
      }
      goto error_cleanup;
    }
    if (!resolve_format(g_asio_shared.state->iasio, capture->format,
                        capture->has_format, true, &resolved_format, err)) {
      goto error_cleanup;
    }
    asio_buffer_size = preferred_buf;
  } else {
    if (!open_asio_capture(capture->device, (size_t)capture->channels,
                           capture->sample_rate, capture->format,
                           capture->has_format, &capture->iasio,
                           &resolved_format, err)) {
      goto error_cleanup;
    }
    long preferred_buf = 0;
    if (!get_preferred_buffer_size(capture->iasio, &preferred_buf, err)) {
      goto error_cleanup;
    }
    asio_buffer_size = preferred_buf;
  }

  // Detect whether channel 0 uses LSB-first bit ordering for DSD
  bool is_lsb = false;
  IASIO* active_iasio =
      capture->full_duplex ? g_asio_shared.state->iasio : capture->iasio;
  if (active_iasio) {
    ASIOChannelInfo ch_info = {0};
    ch_info.channel = 0;
    ch_info.isInput = ASIOTrue;
    if (active_iasio->lpVtbl->getChannelInfo(active_iasio, &ch_info) == 0) {
      if (ch_info.type == ASIO_ST_DSD_INT8_LSB_1) {
        is_lsb = true;
      }
    }
  }

  long driver_buffer_size = asio_buffer_size;
  if (resolved_format == ASIO_SAMPLE_FORMAT_DSD_INT8) {
    asio_buffer_size /= 32;
  }

  capture->resolved_format = resolved_format;
  capture->is_lsb = is_lsb;
  capture->bytes_per_sample = asio_format_bytes_per_sample(resolved_format);
  capture->actual_buffer_size = asio_buffer_size;

  size_t ring_frames = ((size_t)capture->chunk_size > (size_t)asio_buffer_size)
                           ? (size_t)capture->chunk_size
                           : (size_t)asio_buffer_size;
  size_t ring_bytes = (size_t)capture->channels * capture->bytes_per_sample *
                      (2 * ring_frames + 2048);
  capture->ring_buffer = spsc_byte_ring_buffer_create(ring_bytes);
  capture->semaphore = cdsp_sem_create();

  capture->decode_buf_size = (size_t)capture->channels *
                             (size_t)capture->chunk_size *
                             capture->bytes_per_sample * 2;
  capture->decode_buf = (uint8_t*)malloc(capture->decode_buf_size);

  clear_capture_rate_change_event();

  capture->context =
      (asio_capture_context_t*)calloc(1, sizeof(asio_capture_context_t));
  capture->context->ring_buffer = capture->ring_buffer;
  capture->context->semaphore = capture->semaphore;
  capture->context->num_channels = (size_t)capture->channels;
  capture->context->buffer_size = (size_t)asio_buffer_size;
  capture->context->bytes_per_sample = capture->bytes_per_sample;
  capture->context->transfer_buf =
      (uint8_t*)malloc((size_t)asio_buffer_size * capture->bytes_per_sample *
                       (size_t)capture->channels);
  capture->context->chunk_counter = 0;

  if (capture->full_duplex) {
    atomic_store_explicit(&CAPTURE_CONTEXT, capture->context,
                          memory_order_release);
    if (!register_and_wait(true, (size_t)capture->channels,
                           &capture->buffer_infos, &capture->actual_buffer_size,
                           &capture->iasio, err)) {
      atomic_store_explicit(&CAPTURE_CONTEXT, NULL, memory_order_release);
      goto error_cleanup;
    }
    if (resolved_format == ASIO_SAMPLE_FORMAT_DSD_INT8) {
      capture->actual_buffer_size /= 32;
    }
  } else {
    capture->buffer_infos = make_buffer_infos((size_t)capture->channels, true);
    capture->single_mode_allocated_infos = true;
    capture->callbacks_for_driver.bufferSwitch = buffer_switch_capture;
    capture->callbacks_for_driver.sampleRateDidChange =
        sample_rate_changed_callback;
    capture->callbacks_for_driver.asioMessage = asio_message_callback;
    capture->callbacks_for_driver.bufferSwitchTimeInfo =
        buffer_switch_timeinfo_capture;

    if (!create_asio_buffers(capture->iasio, capture->buffer_infos,
                             (long)capture->channels, driver_buffer_size,
                             &capture->callbacks_for_driver, err)) {
      goto error_cleanup;
    }

    capture->context->buffer_infos = capture->buffer_infos;
    atomic_store_explicit(&CAPTURE_CONTEXT, capture->context,
                          memory_order_release);

    logger_trace(&g_logger, "Capture: calling ASIOStart()");
    long start_res = capture->iasio->lpVtbl->start(capture->iasio);
    if (start_res != 0) {
      atomic_store_explicit(&CAPTURE_CONTEXT, NULL, memory_order_release);
      if (err) {
        char msg[256];
        snprintf(msg, sizeof(msg), "ASIOStart failed with error code %ld",
                 start_res);
        backend_error_init(err, BACKEND_ERROR_INITIALIZATION_FAILED, msg);
      }
      goto error_cleanup;
    }
    logger_trace(&g_logger, "Capture: ASIOStart() succeeded");
  }

  logger_debug(&g_logger, "Capture device ready and waiting.");
  logger_debug(&g_logger, "Capture device starts now!");

  atomic_store_explicit(&capture->is_running, true, memory_order_release);
  return true;

error_cleanup:
  asio_capture_close(capture);
  return false;
}

/**
 * @brief read matching CamillaDSP device.rs lines 2031-2223.
 */
static bool asio_capture_read(void* ctx, size_t frames, audio_chunk_t* chunk,
                              backend_error_t* err) {
  asio_capture_t* capture = (asio_capture_t*)ctx;
  if (!capture) return false;

  if (take_capture_rate_change_event()) {
    if (err) {
      backend_error_init(err, BACKEND_ERROR_NONE, "Sample rate changed");
    }
    return false;
  }

  if (atomic_load_explicit(&capture->stopped, memory_order_acquire) ||
      !atomic_load_explicit(&capture->is_running, memory_order_acquire)) {
    if (err) {
      backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                         "Capture stream stopped");
    }
    return false;
  }

  size_t capture_bytes =
      frames * (size_t)capture->channels * capture->bytes_per_sample;
  if (capture_bytes > capture->decode_buf_size) {
    if (err) {
      backend_error_init(err, BACKEND_ERROR_READ_ERROR,
                         "Frame count exceeds capture buffer capacity");
    }
    return false;
  }

  // Wait for enough data in the ring buffer (lines 2093-2126)
  while (spsc_byte_ring_buffer_get_available_to_read(capture->ring_buffer) <
         capture_bytes) {
    if (atomic_load_explicit(&capture->stopped, memory_order_acquire)) {
      return false;
    }
    if (!cdsp_sem_timedwait(capture->semaphore, 250)) {
      logger_warn(&g_logger, "Capture, waiting for data timed out.");
      break;
    }
  }

  size_t read_bytes = spsc_byte_ring_buffer_consume(
      capture->ring_buffer, capture->decode_buf, capture_bytes);
  if (read_bytes < capture_bytes) {
    memset(capture->decode_buf + read_bytes, 0, capture_bytes - read_bytes);
  }

  // Convert raw bytes to audio_chunk_t
  double* dst_channels[capture->channels];
  for (int c = 0; c < capture->channels; c++) {
    dst_channels[c] = audio_chunk_get_channel(chunk, c);
  }

  for (size_t f = 0; f < frames; f++) {
    for (int c = 0; c < capture->channels; c++) {
      const uint8_t* src =
          capture->decode_buf + (f * (size_t)capture->channels + (size_t)c) *
                                    capture->bytes_per_sample;
      double sample = 0.0;
      switch (capture->resolved_format) {
        case ASIO_SAMPLE_FORMAT_S16_LE:
          sample = pcm_sample_decode_s16_bytes(src);
          break;
        case ASIO_SAMPLE_FORMAT_S24_3_LE:
          sample = pcm_sample_decode_s24_3bytes(src);
          break;
        case ASIO_SAMPLE_FORMAT_S24_4_LE:
          sample = pcm_sample_decode_s24_4_lj_bytes(src);
          break;
        case ASIO_SAMPLE_FORMAT_S32_LE:
          sample = pcm_sample_decode_s32_bytes(src);
          break;
        case ASIO_SAMPLE_FORMAT_F32_LE:
          sample = pcm_sample_decode_f32_bytes(src);
          break;
        case ASIO_SAMPLE_FORMAT_F64_LE:
          sample = pcm_sample_decode_f64_bytes(src);
          break;
        case ASIO_SAMPLE_FORMAT_DSD_INT8:
          if (capture->is_lsb) {
            sample = pcm_sample_decode_dsd_u32_reversed_bytes(src);
          } else {
            sample = pcm_sample_decode_dsd_u32_be_bytes(src);
          }
          break;
        default:
          sample = pcm_sample_decode_s32_bytes(src);
          break;
      }
      dst_channels[c][f] = sample;
    }
  }
  audio_chunk_set_valid_frames(chunk, frames);
  return true;
}

static bool asio_capture_wait_for_data(void* ctx, uint32_t timeout_ms) {
  asio_capture_t* capture = (asio_capture_t*)ctx;
  if (!capture || !capture->semaphore) return false;
  return cdsp_sem_timedwait(capture->semaphore, timeout_ms);
}

static bool asio_capture_get_pending_rate_change(void* ctx, double* out_rate) {
  asio_capture_t* capture = (asio_capture_t*)ctx;
  if (!capture) return false;
  if (take_capture_rate_change_event()) {
    int new_rate = read_current_asio_sample_rate_hz(capture->iasio);
    if (out_rate) {
      *out_rate = (double)new_rate;
    }
    return true;
  }
  return false;
}

static void asio_capture_stop(void* ctx) {
  asio_capture_t* capture = (asio_capture_t*)ctx;
  if (!capture) return;
  atomic_store_explicit(&capture->stopped, true, memory_order_release);
  if (capture->semaphore) {
    cdsp_sem_signal(capture->semaphore);
  }
}

static void asio_capture_destroy(void* ctx) {
  asio_capture_t* capture = (asio_capture_t*)ctx;
  if (capture) {
    asio_capture_close(capture);
    free(capture);
  }
}

static capture_backend_t* asio_capture_create(
    const capture_device_config_t* config, int sample_rate, int chunk_size,
    bool full_duplex, processing_parameters_t* params, backend_error_t* err) {
  (void)params;
  (void)err;
  asio_capture_t* capture = (asio_capture_t*)calloc(1, sizeof(asio_capture_t));
  if (!capture) return NULL;

  snprintf(capture->device, sizeof(capture->device), "%s",
           config->cfg.asio.device);

  capture->sample_rate = sample_rate;
  capture->channels = config->cfg.asio.channels;
  capture->chunk_size = chunk_size;
  capture->format = config->cfg.asio.format;
  capture->has_format = (config->cfg.asio.format != ASIO_SAMPLE_FORMAT_INVALID);
  capture->full_duplex = full_duplex;

  atomic_init(&capture->is_running, false);
  atomic_init(&capture->stopped, false);

  capture_backend_t* backend =
      (capture_backend_t*)calloc(1, sizeof(capture_backend_t));
  if (!backend) {
    free(capture);
    return NULL;
  }
  backend->ctx = capture;
  backend->vtable = &g_asio_capture_vtable;
  backend->is_realtime = true;
  return backend;
}

const capture_backend_vtable_t g_asio_capture_vtable = {
    .create = asio_capture_create,
    .open = asio_capture_open,
    .read = asio_capture_read,
    .close = asio_capture_close,
    .get_pending_rate_change = asio_capture_get_pending_rate_change,
    .is_pitch_control_supported = NULL,
    .set_pitch = NULL,
    .wait_for_data = asio_capture_wait_for_data,
    .set_is_paused = NULL,
    .stop = asio_capture_stop,
    .destroy = asio_capture_destroy,
};

#endif  // ENABLE_ASIO
