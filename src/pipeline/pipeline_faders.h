/**
 * @file pipeline_faders.h
 * @brief Centralized per-chunk fader ramps (upstream fader.rs `Faders`).
 *
 * The pipeline owns one fader bank holding the ramp state of all five faders
 * (Main, Aux1..Aux4). pipeline_process() advances every fader exactly once
 * per chunk, before any pipeline step runs, publishes the resulting per-chunk
 * gains into a fader_levels_t and writes each fader's level back to
 * processing_parameters_t as its current volume. Volume and Loudness filters
 * are pure readers of those levels, so every channel, every filter and every
 * worker thread sees the same level for the same chunk regardless of step
 * order or multithreading, and an Aux fader without a Volume filter still
 * tracks its target (upstream UNUSED_AUX_FADER).
 *
 * Thread safety: the bank is written only by the thread running
 * pipeline_process() (or pipeline_transfer_state(), on the same thread), and
 * only before the steps run. Worker threads that execute filter steps
 * (libdispatch dispatch_apply_f / OpenMP parallel for) only read the levels,
 * and both runtimes order the dispatching thread's earlier writes before the
 * workers' reads. No locks, no allocation.
 */
#ifndef CLIB_PIPELINE_PIPELINE_FADERS_H
#define CLIB_PIPELINE_PIPELINE_FADERS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio/processing_parameters.h"
#include "config/config_error.h"
#include "config/configuration.h"
#include "filters/volume.h"

/// Ramp time and volume limit of one fader (upstream FaderSettings).
typedef struct {
  double ramp_time_ms;
  double limit;
} fader_settings_t;

/// Ramp state of one fader (upstream FaderRamp).
typedef struct {
  int ramp_chunks;
  double limit;
  double target_db; ///< Target in dB, already limited.
  bool mute;
  double current_db; ///< Level at the start of the next chunk.
  double ramp_start; ///< Level at the start of the current ramp.
  int ramp_step;     ///< Next ramp chunk, counting from 1; 0 when settled.
} fader_ramp_t;

/// The ramp state of all faders, owned by the pipeline (upstream Faders).
typedef struct {
  fader_ramp_t faders[FADER_COUNT];
  fader_levels_t levels;
  processing_parameters_t *params;
  size_t chunk_size;
  int sample_rate;
  uint64_t last_pause_count;
} pipeline_faders_t;

/**
 * @brief Collect the settings of every fader from @p config.
 *
 * Main takes devices.volume_ramp_time_ms / volume_limit; each Aux fader takes
 * them from the first Volume filter using it in a non-bypassed filter step
 * (an unused Aux fader gets ramp 0 ms, limit 50 dB). When two Volume filters
 * on one fader disagree, the first wins and, if @p err is given, a conflict
 * message is written to it.
 *
 * @return false if a conflict was found, true otherwise.
 */
bool pipeline_faders_collect_settings(const dsp_config_t *config,
                                      fader_settings_t out[FADER_COUNT],
                                      config_error_t *err);

/**
 * @brief Start every fader at its current shared level, with no ramp running
 * (upstream Faders::new), and publish those levels.
 */
void pipeline_faders_init(pipeline_faders_t *faders,
                          const fader_settings_t settings[FADER_COUNT],
                          processing_parameters_t *params, size_t chunk_size,
                          int sample_rate);

/**
 * @brief Re-seed every fader from the shared parameters, keeping the ramp
 * times and limits. Used after a structural reload has synced the current
 * volumes to their targets.
 */
void pipeline_faders_reseed(pipeline_faders_t *faders);

/**
 * @brief Carry the ramp state of @p src over to @p dest, applying the ramp
 * times and limits @p dest was built with (upstream Faders::update_parameters).
 * Also copies the levels @p src published last.
 */
void pipeline_faders_transfer(pipeline_faders_t *dest,
                              const pipeline_faders_t *src);

/**
 * @brief Advance every fader by one chunk and publish the levels for it. Call
 * once per chunk, before any filter processes it.
 */
void pipeline_faders_prepare_chunk(pipeline_faders_t *faders);

#endif // CLIB_PIPELINE_PIPELINE_FADERS_H
