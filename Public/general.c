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
#if defined(ENABLE_WASAPI)
  cap_count++;
#endif
#if defined(ENABLE_ASIO)
  cap_count++;
#endif
  cap_count += 3;  // File, Stdin, Generator

  char** pb_arr = (char**)malloc(pb_count * sizeof(char*));
  char** cap_arr = (char**)malloc(cap_count * sizeof(char*));

  size_t pb_idx = 0;
#if defined(ENABLE_COREAUDIO)
  pb_arr[pb_idx++] = strdup("CoreAudio");
#endif
#if defined(ENABLE_ALSA)
  pb_arr[pb_idx++] = strdup("ALSA");
#endif
#if defined(ENABLE_WASAPI)
  pb_arr[pb_idx++] = strdup("WASAPI");
#endif
#if defined(ENABLE_ASIO)
  pb_arr[pb_idx++] = strdup("ASIO");
#endif
  pb_arr[pb_idx++] = strdup("File");
  pb_arr[pb_idx++] = strdup("Stdout");

  size_t cap_idx = 0;
#if defined(ENABLE_COREAUDIO)
  cap_arr[cap_idx++] = strdup("CoreAudio");
#endif
#if defined(ENABLE_ALSA)
  cap_arr[cap_idx++] = strdup("ALSA");
#endif
#if defined(ENABLE_WASAPI)
  cap_arr[cap_idx++] = strdup("WASAPI");
#endif
#if defined(ENABLE_ASIO)
  cap_arr[cap_idx++] = strdup("ASIO");
#endif
  cap_arr[cap_idx++] = strdup("File");
  cap_arr[cap_idx++] = strdup("Stdin");
  cap_arr[cap_idx++] = strdup("Generator");

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
