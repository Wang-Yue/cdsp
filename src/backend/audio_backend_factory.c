#include "backend/audio_backend_factory.h"

#include <stdio.h>

#include "backend/audio_backend.h"
#include "backend/backend_error.h"
#include "config/config_gen.h"
#include "logging/app_logger.h"

static const logger_t g_logger = {"dsp.backend.factory"};

#include <assert.h>

#include "utils/cdsp_macros.h"

static audio_backend_error_type_t
map_backend_error_type(backend_error_type_t type) {
  switch (type) {
  case BACKEND_ERROR_NONE:
    return AUDIO_BACKEND_ERR_COMMAND_SEND;
  case BACKEND_ERROR_DEVICE_NOT_FOUND:
    return AUDIO_BACKEND_ERR_DEVICE_NOT_FOUND;
  case BACKEND_ERROR_DEVICE_BUSY:
    return AUDIO_BACKEND_ERR_DEVICE_BUSY;
  case BACKEND_ERROR_INITIALIZATION_FAILED:
  case BACKEND_ERROR_INVALID_CHANNELS:
    return AUDIO_BACKEND_ERR_CONFIG_PARSE;
  case BACKEND_ERROR_READ_EOF:
  case BACKEND_ERROR_READ_ERROR:
  case BACKEND_ERROR_WRITE_ERROR:
    return AUDIO_BACKEND_ERR_COMMAND_SEND;
  }
  CDSP_UNREACHABLE();
  return AUDIO_BACKEND_ERR_COMMAND_SEND;
}

/// Fill @p out_err from a backend error. A backend that returns NULL without
/// setting an error would otherwise surface as "Command send error: " with an
/// empty message (F11), so substitute a descriptive fallback.
static void fill_factory_error(audio_backend_error_t *out_err,
                               const backend_error_t *berr,
                               const char *fallback) {
  if (!out_err)
    return;
  out_err->type = map_backend_error_type(berr->type);
  snprintf(out_err->message, sizeof(out_err->message), "%s",
           berr->message[0] != '\0' ? berr->message : fallback);
}

capture_backend_t *audio_backend_factory_create_capture(
    const capture_device_config_t *config, size_t sample_rate,
    size_t chunk_size, bool full_duplex, processing_parameters_t *params,
    audio_backend_error_t *out_err) {
  backend_error_t berr;
  backend_error_init(&berr, BACKEND_ERROR_NONE, "");

  capture_backend_t *backend = create_capture_backend(
      config, (int)sample_rate, (int)chunk_size, full_duplex, params, &berr);
  if (!backend || berr.type != BACKEND_ERROR_NONE) {
    logger_error(&g_logger, "Failed to create capture backend: %s",
                 berr.message[0] != '\0' ? berr.message
                                         : "(no detail from backend)");
    fill_factory_error(out_err, &berr, "Failed to create capture backend");
    if (backend) {
      capture_backend_free(backend);
    }
    return NULL;
  }

  return backend;
}

playback_backend_t *audio_backend_factory_create_playback(
    const playback_device_config_t *config, size_t sample_rate,
    size_t chunk_size, bool full_duplex, processing_parameters_t *params,
    audio_backend_error_t *out_err) {
  backend_error_t berr;
  backend_error_init(&berr, BACKEND_ERROR_NONE, "");

  playback_backend_t *backend = create_playback_backend(
      config, (int)sample_rate, (int)chunk_size, full_duplex, params, &berr);
  if (!backend || berr.type != BACKEND_ERROR_NONE) {
    logger_error(&g_logger, "Failed to create playback backend: %s",
                 berr.message[0] != '\0' ? berr.message
                                         : "(no detail from backend)");
    fill_factory_error(out_err, &berr, "Failed to create playback backend");
    if (backend) {
      playback_backend_free(backend);
    }
    return NULL;
  }

  return backend;
}
