#ifndef ENGINE_DSP_ENGINE_CORE_INTERNAL_H
#define ENGINE_DSP_ENGINE_CORE_INTERNAL_H

/**
 * @file dsp_engine_core_internal.h
 * @brief Internal structural layout of `struct dsp_engine_core` shared across
 * Engine sub-modules.
 */

#include "dsp_engine_core.h"

#include "DoP/dsd_encoder.h"

struct dsp_engine_core {
  // MARK: - Configuration
  /** Current configuration. */
  dsp_config_t* current_config;
  /** Processing parameters derived from configuration. */
  processing_parameters_t* processing_params;

  // MARK: - Shared state
  /** Shared state between threads. */
  engine_shared_state_t* shared;

  // MARK: - Components built per run
  /** Capture audio backend. */
  capture_backend_t* capture;
  /** Playback audio backend. */
  playback_backend_t* playback;
  /** Processing loop instance. */
  engine_processing_loop_t* processing_loop;
  /** Capture loop instance. */
  engine_capture_loop_t* capture_loop;
  /** Playback loop instance. */
  engine_playback_loop_t* playback_loop;
  /** DoP decoder instance. */
  dop_decoder_t* dop_decoder;
  /** DSD encoder instance. */
  dsd_encoder_t* dsd_encoder;

  /**
   * Playback-side chunk size — `resampler.maxOutputFrames` when a
   * resampler is in use, otherwise `effectiveChunkSize`.
   */
  size_t effective_playback_chunk_size;
  /** Scratch buffer for resampler. */
  audio_chunk_t* resampler_scratch;
  /** Scratch buffer for pipeline. */
  audio_chunk_t* pipeline_scratch;
  /** Pool of chunks for capture. */
  round_robin_chunk_pool_t* capture_chunk_pool;
  /** Pool of scratch chunks for processing. */
  round_robin_chunk_pool_t* processing_scratch_pool;
  /** Audio resampler. */
  audio_resampler_t* resampler;
  /** Audio processing pipeline. */
  pipeline_t* pipeline;

  // MARK: - Threading
  /** Capture thread handle. */
  pthread_t capture_thread;
  /** Processing thread handle. */
  pthread_t processing_thread;
  /** Playback thread handle. */
  pthread_t playback_thread;
  /** True if threads were successfully created. */
  bool threads_created;

  // MARK: - Optional taps for visualisation
  /**
   * Optional callback invoked before pipeline processing.
   * Set before `start()` and treated as immutable thereafter.
   */
  chunk_callback_t on_chunk_captured;
  /** Context for capture callback. */
  void* on_chunk_captured_ctx;

  /**
   * Optional callback invoked after pipeline processing.
   * Set before `start()` and treated as immutable thereafter.
   */
  chunk_callback_t on_chunk_processed;
  /** Context for processing callback. */
  void* on_chunk_processed_ctx;
};

#endif  // ENGINE_DSP_ENGINE_CORE_INTERNAL_H
