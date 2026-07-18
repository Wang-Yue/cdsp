#include "engine_session_builder.h"

#ifdef _WIN32
#include <mmsystem.h>
#include <windows.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Audio/audio_chunk.h"
#include "Audio/processing_parameters.h"
#include "Backend/audio_backend.h"
#include "Backend/audio_backend_factory.h"
#include "Config/config_diff.h"
#include "DoP/dop_decoder.h"
#include "DoP/dsd_encoder.h"
#include "Logging/app_logger.h"
#include "Pipeline/pipeline.h"
#include "Resampler/audio_resampler.h"
#include "dsp_session_internal.h"
#include "engine_capture_loop.h"
#include "engine_playback_loop.h"
#include "engine_processing_loop.h"
#include "engine_shared_state.h"

static const logger_t g_logger = {"dsp.session.builder"};

/**
 * @brief Thread entry point wrapper for the audio capture loop.
 *
 * @param arg Pointer to the engine_capture_loop_t instance.
 * @return NULL.
 */
static void* capture_thread_func(void* arg) {
  engine_capture_loop_t* loop = (engine_capture_loop_t*)arg;
  engine_capture_loop_run(loop);
  return NULL;
}

/**
 * @brief Thread entry point wrapper for the DSP processing pipeline loop.
 *
 * @param arg Pointer to the engine_processing_loop_t instance.
 * @return NULL.
 */
static void* processing_thread_func(void* arg) {
  engine_processing_loop_t* loop = (engine_processing_loop_t*)arg;
  engine_processing_loop_run(loop);
  return NULL;
}

/**
 * @brief Thread entry point wrapper for the audio playback loop.
 *
 * @param arg Pointer to the engine_playback_loop_t instance.
 * @return NULL.
 */
static void* playback_thread_func(void* arg) {
  engine_playback_loop_t* loop = (engine_playback_loop_t*)arg;
  engine_playback_loop_run(loop);
  return NULL;
}

/**
 * @brief Step 3: Allocates shared state queues, processing telemetry, and DoP
 * encoder/decoder codecs.
 * Ref: engine_state_management.md - Section 3.1: Startup & Initialization Flow (Step 3)
 */
static bool engine_session_build_shared_state_and_dop(dsp_session_t* core,
                                                      dsp_config_t* config) {
  // 1. Shared state & DoP codec setup.
  int queue_limit =
      config->devices.has_queuelimit ? config->devices.queuelimit : 4;
  core->shared = engine_shared_state_create(queue_limit, queue_limit);
  core->processing_params = processing_parameters_create(
      capture_device_config_get_channels(&config->devices.capture),
      playback_device_config_get_channels(&config->devices.playback));

  bool multithreaded =
      config->devices.has_multithreaded ? config->devices.multithreaded : false;

  double capture_rate = (double)(config->devices.has_capture_samplerate
                                     ? config->devices.capture_samplerate
                                     : config->devices.samplerate);

  core->dop_decoder = dop_decoder_create(
      capture_device_config_get_channels(&config->devices.capture),
      capture_rate,
      capture_device_config_get_bypass_dop(&config->devices.capture),
      capture_device_config_get_dop_cutoff_hz(&config->devices.capture),
      multithreaded);

  size_t playback_rate = config->devices.samplerate;
  sdm_filter_t dop_filter =
      playback_device_config_get_dsd_encoder_filter(&config->devices.playback);
  if (dop_filter == SDM_FILTER_INVALID) {
    dop_filter = SDM_FILTER_SDM6;
  }
  dsd_mode_t dsd_mode = DSD_MODE_PCM;
  bool is_dsd = false;
#if defined(ENABLE_ALSA)
  if (config->devices.playback.type == AUDIO_BACKEND_TYPE_ALSA) {
    is_dsd = config->devices.playback.cfg.alsa.output_dsd;
  }
#endif
#if defined(ENABLE_ASIO)
  if (config->devices.playback.type == AUDIO_BACKEND_TYPE_ASIO) {
    is_dsd = config->devices.playback.cfg.asio.output_dsd;
  }
#endif

  if (is_dsd) {
    dsd_mode = DSD_MODE_NATIVE;
  } else if (config->devices.playback.output_dop) {
    dsd_mode = DSD_MODE_DOP;
  }
  size_t dsd_bit_depth =
      playback_device_config_calculate_carrier_bits(&config->devices.playback);

  core->dsd_encoder = dsd_encoder_create(
      playback_device_config_get_channels(&config->devices.playback),
      playback_rate, dsd_mode, dsd_bit_depth, dop_filter, 20000.0,
      multithreaded);

  return (core->shared && core->processing_params && core->dop_decoder &&
          core->dsd_encoder);
}

/**
 * @brief Step 4: Creates the resampler codec if target pipeline rate differs from capture rate.
 * Ref: engine_state_management.md - Section 3.1: Startup & Initialization Flow (Step 4)
 */
static bool engine_session_build_resampler(
    dsp_session_t* core, dsp_config_t* config, double capture_rate,
    size_t pipeline_rate, audio_backend_error_t* err) {
  if (config->devices.has_resampler) {
    config_error_t cerr;
    config_error_init(&cerr);
    core->resampler = resampler_create_from_config(
        &config->devices.resampler, (size_t)capture_rate, pipeline_rate,
        capture_device_config_get_channels(&config->devices.capture),
        config->devices.chunksize, &cerr);
    if (!core->resampler) {
      if (err) {
        err->type = AUDIO_BACKEND_ERR_COMMAND_SEND;
        strncpy(err->message, cerr.message, sizeof(err->message) - 1);
        err->message[sizeof(err->message) - 1] = '\0';
      }
      return false;
    }
  }
  return true;
}

/**
 * @brief Step 5: Opens audio capture and playback backends via backend factory.
 * Ref: engine_state_management.md - Section 3.1: Startup & Initialization Flow (Step 5)
 */
static bool engine_session_build_backends(
    dsp_session_t* core, dsp_config_t* config, size_t capture_rate,
    size_t pipeline_rate, size_t capture_chunk_size, size_t playback_chunk_size,
    audio_backend_error_t* err) {
  // 2. Open audio capture and playback backends.
  bool full_duplex = false;
#if defined(ENABLE_ASIO)
  if (config->devices.capture.type == AUDIO_BACKEND_TYPE_ASIO &&
      config->devices.playback.type == AUDIO_BACKEND_TYPE_ASIO &&
      strcmp(capture_device_config_get_device(&config->devices.capture),
             playback_device_config_get_device(&config->devices.playback)) ==
          0) {
    full_duplex = true;
  }
#endif

  core->capture = audio_backend_factory_create_capture(
      &config->devices.capture, (int)capture_rate, (int)capture_chunk_size,
      full_duplex, core->processing_params, err);
  if (!core->capture) return false;

  core->playback = audio_backend_factory_create_playback(
      &config->devices.playback, (int)pipeline_rate, (int)playback_chunk_size,
      full_duplex, core->processing_params, err);
  if (!core->playback) return false;

  return true;
}

/**
 * @brief Step 6: Pre-allocates scratch audio chunks and creates the DSP processing pipeline.
 * Ref: engine_state_management.md - Section 3.1: Startup & Initialization Flow (Step 6)
 */
static bool engine_session_build_pipeline_and_scratch(
    dsp_session_t* core, dsp_config_t* config, size_t capture_chunk_size,
    size_t playback_chunk_size, audio_backend_error_t* err) {
  // 5. Allocate scratch chunks for temporary data storage during
  // processing/resampling.
  core->resampler_scratch = audio_chunk_create(
      core->resampler ? resampler_get_max_output_frames(core->resampler)
                      : capture_chunk_size,
      capture_device_config_get_channels(&config->devices.capture));
  core->pipeline_scratch = audio_chunk_create(
      playback_chunk_size,
      playback_device_config_get_channels(&config->devices.playback));
  if (!core->resampler_scratch || !core->pipeline_scratch) {
    if (err) {
      err->type = AUDIO_BACKEND_ERR_COMMAND_SEND;
      snprintf(err->message, sizeof(err->message),
               "Failed to allocate scratch chunks");
    }
    return false;
  }
  audio_chunk_set_valid_frames(core->resampler_scratch, 0);
  audio_chunk_set_valid_frames(core->pipeline_scratch, 0);

  // 6. Create the DSP processing pipeline.
  config_error_t cerr;
  config_error_init(&cerr);
  core->pipeline = pipeline_create(config, core->processing_params,
                                   playback_chunk_size, &cerr);
  if (!core->pipeline) {
    if (err) {
      err->type = AUDIO_BACKEND_ERR_COMMAND_SEND;
      strncpy(err->message, cerr.message, sizeof(err->message) - 1);
      err->message[sizeof(err->message) - 1] = '\0';
    }
    return false;
  }
  return true;
}

/**
 * @brief Step 7: Pre-allocates lock-free chunk pools for thread communication.
 * Ref: engine_state_management.md - Section 3.1: Startup & Initialization Flow (Step 7)
 */
static bool engine_session_build_chunk_pools(
    dsp_session_t* core, dsp_config_t* config, size_t capture_chunk_size,
    size_t playback_chunk_size) {
  // 7. Pre-allocate chunk pools.
  // Allocate memory for chunk pools ahead of time to guarantee that the capture
  // and processing loop threads never perform dynamic memory allocations on the
  // hot path.
  size_t capture_pool_cap =
      spsc_queue_get_capacity(
          engine_shared_state_get_captured_queue(core->shared)) +
      2;
  core->capture_chunk_pool = round_robin_chunk_pool_create(
      capture_pool_cap, capture_chunk_size,
      capture_device_config_get_channels(&config->devices.capture));

  size_t processing_pool_cap =
      spsc_queue_get_capacity(
          engine_shared_state_get_processed_queue(core->shared)) +
      2;
  core->processing_scratch_pool = round_robin_chunk_pool_create(
      processing_pool_cap, playback_chunk_size,
      playback_device_config_get_channels(&config->devices.playback));

  return (core->capture_chunk_pool && core->processing_scratch_pool);
}

/**
 * @brief Step 8: Instantiates engine worker loop orchestrators and spawns worker threads.
 * Ref: engine_state_management.md - Section 3.1: Startup & Initialization Flow (Step 8)
 */
static bool engine_session_spawn_worker_threads(dsp_session_t* core,
                                                dsp_config_t* config,
                                                size_t capture_chunk_size,
                                                size_t playback_chunk_size,
                                                size_t pipeline_rate,
                                                audio_backend_error_t* err) {
  // 8. Instantiate the loop orchestrators.
  engine_capture_loop_config_t cap_cfg = {
      .shared = core->shared,
      .capture = core->capture,
      .playback = core->playback,
      .processing_params = core->processing_params,
      .dop_decoder = core->dop_decoder,
      .chunk_pool = core->capture_chunk_pool,
      .chunk_size = capture_chunk_size,
      .channels = capture_device_config_get_channels(&config->devices.capture),
      .samplerate = (size_t)(config->devices.has_capture_samplerate
                                 ? config->devices.capture_samplerate
                                 : config->devices.samplerate),
      .silence_threshold_db = config->devices.has_silence_threshold
                                  ? config->devices.silence_threshold
                                  : 0.0,
      .silence_timeout_seconds = config->devices.has_silence_timeout_s
                                     ? config->devices.silence_timeout_s
                                     : 0.0,
      .stop_on_rate_change = config->devices.has_stop_on_rate_change
                                 ? config->devices.stop_on_rate_change
                                 : false,
      .rate_measure_interval_s = config->devices.has_rate_measure_interval_s
                                     ? config->devices.rate_measure_interval_s
                                     : 1.0,
  };
  core->capture_loop = engine_capture_loop_create(&cap_cfg);

  engine_processing_loop_config_t proc_cfg = {
      .shared = core->shared,
      .processing_params = core->processing_params,
      .pipeline_rate = pipeline_rate,
      .resampler = core->resampler,
      .pipeline = core->pipeline,
      .dsd_encoder = core->dsd_encoder,
      .resampler_scratch = core->resampler_scratch,
      .pipeline_scratch = core->pipeline_scratch,
      .scratch_pool = core->processing_scratch_pool,
      .on_chunk_captured = core->on_chunk_captured,
      .on_chunk_captured_ctx = core->on_chunk_captured_ctx,
      .on_chunk_processed = core->on_chunk_processed,
      .on_chunk_processed_ctx = core->on_chunk_processed_ctx,
      .is_realtime = capture_backend_is_realtime(core->capture),
  };
  core->processing_loop = engine_processing_loop_create(&proc_cfg);

  bool rate_adjust_enabled = config->devices.has_enable_rate_adjust
                                 ? config->devices.enable_rate_adjust
                                 : false;
  double adjust_period = config->devices.has_adjust_interval_s
                             ? config->devices.adjust_interval_s
                             : 10.0;
  int target_level = config->devices.has_target_level
                         ? config->devices.target_level
                         : (int)playback_chunk_size;

  engine_playback_loop_config_t play_cfg = {
      .shared = core->shared,
      .capture = core->capture,
      .playback = core->playback,
      .processing_params = core->processing_params,
      .dsd_encoder = core->dsd_encoder,
      .pipeline_rate = pipeline_rate,
      .chunk_size = playback_chunk_size,
      .rate_adjust_enabled = rate_adjust_enabled,
      .adjust_period = adjust_period,
      .target_level = target_level,
  };
  core->playback_loop = engine_playback_loop_create(&play_cfg);

  if (!core->capture_loop || !core->processing_loop || !core->playback_loop) {
    if (err) {
      err->type = AUDIO_BACKEND_ERR_COMMAND_SEND;
      snprintf(err->message, sizeof(err->message),
               "Failed to create engine loops");
    }
    return false;
  }

  // 9. Spawn worker threads.
  // The threads are spawned in STARTING state (initialized by
  // engine_shared_state_create) and will transition to RUNNING once they
  // successfully open their backends. Wrap `pthread_create` construction so
  // each spawn shares the same QoS and lifecycle.
  int ret;
  ret = pthread_create(&core->capture_thread, NULL, capture_thread_func,
                       core->capture_loop);
  if (ret != 0) return false;

  ret = pthread_create(&core->processing_thread, NULL, processing_thread_func,
                       core->processing_loop);
  if (ret != 0) {
    engine_shared_state_request_stop(
        core->shared, (processing_stop_reason_t){.type = STOP_REASON_NONE});
    if (core->capture) capture_backend_stop(core->capture);
    if (core->playback) playback_backend_stop(core->playback);
    pthread_join(core->capture_thread, NULL);
    return false;
  }

  ret = pthread_create(&core->playback_thread, NULL, playback_thread_func,
                       core->playback_loop);
  if (ret != 0) {
    engine_shared_state_request_stop(
        core->shared, (processing_stop_reason_t){.type = STOP_REASON_NONE});
    if (core->capture) capture_backend_stop(core->capture);
    if (core->playback) playback_backend_stop(core->playback);
    pthread_join(core->capture_thread, NULL);
    pthread_join(core->processing_thread, NULL);
    return false;
  }

  core->threads_created = true;
  pthread_mutex_lock(&core->config_mutex);
  core->current_config = config;
  pthread_mutex_unlock(&core->config_mutex);
  return true;
}

dsp_session_t* engine_session_build_and_start(dsp_config_t* config,
                                              chunk_callback_t on_captured,
                                              void* captured_ctx,
                                              chunk_callback_t on_processed,
                                              void* processed_ctx,
                                              audio_backend_error_t* err) {
  if (!config) return NULL;

  // Ref: engine_state_management.md - Section 3.1: Startup & Initialization Flow
  // Note on Non-Builder steps:
  //   - Step 1 (Staging & Lock-Free Status Indicator) occurs inside dsp_engine_set_config_json.
  //   - Step 9 (Async Hardware Open & Prefill) occurs inside the worker thread runs.
  //   - Step 10 (Transition to RUNNING) occurs inside engine_capture_loop_run.
  //
  // Step 2: Allocate the core dsp_session_t struct container and initialize its config_mutex.
  dsp_session_t* core = (dsp_session_t*)calloc(1, sizeof(dsp_session_t));
  if (!core) return NULL;

  pthread_mutex_init(&core->config_mutex, NULL);

#ifdef _WIN32
  timeBeginPeriod(1);
#endif

  core->on_chunk_captured = on_captured;
  core->on_chunk_captured_ctx = captured_ctx;
  core->on_chunk_processed = on_processed;
  core->on_chunk_processed_ctx = processed_ctx;

  // Ref: engine_state_management.md - Section 3.1: Startup & Initialization Flow
  // Step 3: Call engine_session_build_shared_state_and_dop to allocate shared state
  // and initialize DoP/DSD codecs.
  if (!engine_session_build_shared_state_and_dop(core, config)) {
    dsp_session_stop_and_free(
        core, (processing_stop_reason_t){.type = STOP_REASON_NONE});
    return NULL;
  }

  size_t pipeline_rate = config->devices.samplerate;
  double capture_rate = (double)(config->devices.has_capture_samplerate
                                     ? config->devices.capture_samplerate
                                     : config->devices.samplerate);

  // Ref: engine_state_management.md - Section 3.1: Startup & Initialization Flow
  // Step 4: Call engine_session_build_resampler to allocate resampler if rates differ.
  if (!engine_session_build_resampler(core, config, capture_rate, pipeline_rate, err)) {
    dsp_session_stop_and_free(
        core, (processing_stop_reason_t){.type = STOP_REASON_NONE});
    return NULL;
  }

  size_t requested_chunk_size = config->devices.chunksize;
  size_t capture_chunk_size = core->resampler
                                  ? resampler_get_chunk_size(core->resampler)
                                  : requested_chunk_size;
  size_t playback_chunk_size =
      core->resampler ? resampler_get_max_output_frames(core->resampler)
                      : capture_chunk_size;
  core->effective_playback_chunk_size = playback_chunk_size;

  // Ref: engine_state_management.md - Section 3.1: Startup & Initialization Flow
  // Step 5: Call engine_session_build_backends to allocate capture/playback backend handles.
  if (!engine_session_build_backends(core, config, (size_t)capture_rate,
                                     pipeline_rate, capture_chunk_size,
                                     playback_chunk_size, err)) {
    dsp_session_stop_and_free(
        core, (processing_stop_reason_t){.type = STOP_REASON_NONE});
    return NULL;
  }

  // Ref: engine_state_management.md - Section 3.1: Startup & Initialization Flow
  // Step 6: Call engine_session_build_pipeline_and_scratch to build DSP pipeline
  // and allocate scratch buffers.
  if (!engine_session_build_pipeline_and_scratch(
          core, config, capture_chunk_size, playback_chunk_size, err)) {
    dsp_session_stop_and_free(
        core, (processing_stop_reason_t){.type = STOP_REASON_NONE});
    return NULL;
  }

  // Ref: engine_state_management.md - Section 3.1: Startup & Initialization Flow
  // Step 7: Call engine_session_build_chunk_pools to pre-allocate lock-free chunk pools.
  if (!engine_session_build_chunk_pools(core, config, capture_chunk_size,
                                        playback_chunk_size)) {
    dsp_session_stop_and_free(
        core, (processing_stop_reason_t){.type = STOP_REASON_NONE});
    return NULL;
  }

  // Ref: engine_state_management.md - Section 3.1: Startup & Initialization Flow
  // Step 8: Call engine_session_spawn_worker_threads to spawn worker threads in parallel.
  if (!engine_session_spawn_worker_threads(core, config, capture_chunk_size,
                                           playback_chunk_size, pipeline_rate,
                                           err)) {
    dsp_session_stop_and_free(
        core, (processing_stop_reason_t){.type = STOP_REASON_NONE});
    return NULL;
  }

  logger_info(&g_logger, "DSP session started: %zuHz, chunk=%zu",
              config->devices.samplerate, capture_chunk_size);
  return core;
}
