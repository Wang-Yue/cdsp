#include "Public/general.h"

#include <stdlib.h>
#include <string.h>

#include "Engine/dsp_engine.h"
#include "Logging/app_logger.h"

const char* cdsp_get_version(void) { return "CDSP 4.2.0"; }

void cdsp_get_supported_device_types(char*** out_playback_types,
                                     size_t* out_playback_count,
                                     char*** out_capture_types,
                                     size_t* out_capture_count) {
  if (!out_playback_types || !out_playback_count || !out_capture_types ||
      !out_capture_count) {
    return;
  }

  // Count playback types
  size_t pb_count = 0;
#if defined(ENABLE_COREAUDIO)
  pb_count++;
#endif
#if defined(ENABLE_ALSA)
  pb_count++;
#endif
#if defined(ENABLE_PIPEWIRE)
  pb_count++;
#endif
#if defined(ENABLE_WASAPI)
  pb_count++;
#endif
#if defined(ENABLE_ASIO)
  pb_count++;
#endif
  pb_count += 2;  // File, Stdout

  // Count capture types
  size_t cap_count = 0;
#if defined(ENABLE_COREAUDIO)
  cap_count++;
#endif
#if defined(ENABLE_ALSA)
  cap_count++;
#endif
#if defined(ENABLE_PIPEWIRE)
  cap_count++;
#endif
#if defined(ENABLE_WASAPI)
  cap_count++;
#endif
#if defined(ENABLE_ASIO)
  cap_count++;
#endif
  cap_count += 3;  // File, Stdin, Generator

  char** pb_arr = (char**)calloc(pb_count, sizeof(char*));
  char** cap_arr = (char**)calloc(cap_count, sizeof(char*));
  if (!pb_arr || !cap_arr) {
    if (pb_arr) free(pb_arr);
    if (cap_arr) free(cap_arr);
    *out_playback_types = NULL;
    *out_playback_count = 0;
    *out_capture_types = NULL;
    *out_capture_count = 0;
    return;
  }

#define SAFE_STRDUP(arr, idx, str)              \
  do {                                          \
    arr[idx] = strdup(str);                     \
    if (!arr[idx]) {                            \
      cdsp_free_device_types(pb_arr, pb_idx);   \
      cdsp_free_device_types(cap_arr, cap_idx); \
      *out_playback_types = NULL;               \
      *out_playback_count = 0;                  \
      *out_capture_types = NULL;                \
      *out_capture_count = 0;                   \
      return;                                   \
    }                                           \
    idx++;                                      \
  } while (0)

  size_t pb_idx = 0;
  size_t cap_idx = 0;
#if defined(ENABLE_COREAUDIO)
  SAFE_STRDUP(pb_arr, pb_idx, "CoreAudio");
#endif
#if defined(ENABLE_ALSA)
  SAFE_STRDUP(pb_arr, pb_idx, "ALSA");
#endif
#if defined(ENABLE_PIPEWIRE)
  SAFE_STRDUP(pb_arr, pb_idx, "PipeWire");
#endif
#if defined(ENABLE_WASAPI)
  SAFE_STRDUP(pb_arr, pb_idx, "WASAPI");
#endif
#if defined(ENABLE_ASIO)
  SAFE_STRDUP(pb_arr, pb_idx, "ASIO");
#endif
  SAFE_STRDUP(pb_arr, pb_idx, "File");
  SAFE_STRDUP(pb_arr, pb_idx, "Stdout");
#if defined(ENABLE_COREAUDIO)
  SAFE_STRDUP(cap_arr, cap_idx, "CoreAudio");
#endif
#if defined(ENABLE_ALSA)
  SAFE_STRDUP(cap_arr, cap_idx, "ALSA");
#endif
#if defined(ENABLE_PIPEWIRE)
  SAFE_STRDUP(cap_arr, cap_idx, "PipeWire");
#endif
#if defined(ENABLE_WASAPI)
  SAFE_STRDUP(cap_arr, cap_idx, "WASAPI");
#endif
#if defined(ENABLE_ASIO)
  SAFE_STRDUP(cap_arr, cap_idx, "ASIO");
#endif
  SAFE_STRDUP(cap_arr, cap_idx, "File");
  SAFE_STRDUP(cap_arr, cap_idx, "Stdin");
  SAFE_STRDUP(cap_arr, cap_idx, "Generator");

#undef SAFE_STRDUP

  *out_playback_types = pb_arr;
  *out_playback_count = pb_count;
  *out_capture_types = cap_arr;
  *out_capture_count = cap_count;
}

void cdsp_free_device_types(char** types, size_t count) {
  if (!types) return;
  for (size_t i = 0; i < count; i++) {
    free(types[i]);
  }
  free(types);
}

dsp_engine_t* cdsp_engine_create(void) { return dsp_engine_create(); }

void cdsp_engine_free(dsp_engine_t* engine) {
  if (engine && engine->free) engine->free(engine->ctx);
}

void cdsp_engine_poll(dsp_engine_t* engine) {
  if (engine && engine->poll) engine->poll(engine->ctx);
}

void cdsp_set_log_level(const char* level_str) {
  app_logger_set_level(log_level_from_string(level_str));
}

void cdsp_stop(dsp_engine_t* engine) {
  if (engine && engine->stop) {
    engine->stop(engine->ctx);
  }
}
