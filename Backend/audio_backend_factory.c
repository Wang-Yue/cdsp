#include "audio_backend_factory.h"

#include <stdio.h>
#include <string.h>

#include "Logging/app_logger.h"

static const logger_t g_logger = {"dsp.backend.factory"};

static audio_backend_error_type_t map_backend_error_type(
    backend_error_type_t type) {
  switch (type) {
    case BACKEND_ERROR_DEVICE_NOT_FOUND:
      return AUDIO_BACKEND_ERR_DEVICE_NOT_FOUND;
    case BACKEND_ERROR_DEVICE_BUSY:
      return AUDIO_BACKEND_ERR_DEVICE_BUSY;
    default:
      return AUDIO_BACKEND_ERR_COMMAND_SEND;
  }
}

capture_backend_t* audio_backend_factory_create_capture(
    const capture_device_config_t* config, int sample_rate, int chunk_size,
    bool full_duplex, processing_parameters_t* params,
    audio_backend_error_t* out_err) {
  backend_error_t berr;
  backend_error_init(&berr, BACKEND_ERROR_NONE, "");

  capture_backend_t* backend = create_capture_backend(
      config, sample_rate, chunk_size, full_duplex, params, &berr);
  if (!backend || berr.type != BACKEND_ERROR_NONE) {
    logger_error(&g_logger, "Failed to create capture backend: %s",
                 berr.message);
    if (out_err) {
      out_err->type = map_backend_error_type(berr.type);
      snprintf(out_err->message, sizeof(out_err->message), "%s", berr.message);
    }
    if (backend) {
      capture_backend_free(backend);
    }
    return NULL;
  }

  return backend;
}

playback_backend_t* audio_backend_factory_create_playback(
    const playback_device_config_t* config, int sample_rate, int chunk_size,
    bool full_duplex, processing_parameters_t* params,
    audio_backend_error_t* out_err) {
  backend_error_t berr;
  backend_error_init(&berr, BACKEND_ERROR_NONE, "");

  playback_backend_t* backend = create_playback_backend(
      config, sample_rate, chunk_size, full_duplex, params, &berr);
  if (!backend || berr.type != BACKEND_ERROR_NONE) {
    logger_error(&g_logger, "Failed to create playback backend: %s",
                 berr.message);
    if (out_err) {
      out_err->type = map_backend_error_type(berr.type);
      snprintf(out_err->message, sizeof(out_err->message), "%s", berr.message);
    }
    if (backend) {
      playback_backend_free(backend);
    }
    return NULL;
  }

  return backend;
}
