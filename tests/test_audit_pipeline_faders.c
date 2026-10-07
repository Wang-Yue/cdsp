// Regression tests for the Volume/Loudness/fader audit items (reports 02, 05,
// 06, 07).
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "audio/audio_chunk.h"
#include "audio/processing_parameters.h"
#include "config/config_error.h"
#include "config/config_gen.h"
#include "config/configuration.h"
#include "filters/filter.h"
#include "filters/loudness.h"
#include "filters/volume.h"
#include "pipeline/pipeline.h"
#include "pipeline/pipeline_faders.h"
#include "pipeline/pipeline_internal.h"
#include "test_support.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int validate_volume(double ramp, bool has_ramp, double limit,
                           bool has_limit) {
  filter_config_t cfg = {.type = FILTER_TYPE_VOLUME};
  cfg.parameters.volume.fader = FADER_AUX1;
  cfg.parameters.volume.ramp_time_ms = ramp;
  cfg.parameters.volume.has_ramp_time_ms = has_ramp;
  cfg.parameters.volume.limit = limit;
  cfg.parameters.volume.has_limit = has_limit;
  config_error_t err;
  config_error_init(&err);
  return g_volume_vtable.validate(&cfg, 48000, &err);
}

// 02-1.6: non-finite ramp_time_ms / limit are rejected (upstream FiniteF32).
TEST(AuditVolumeRejectsNonFiniteParameters) {
  ASSERT_EQ(0, validate_volume(200.0, true, -10.0, true));
  ASSERT_EQ(-1, validate_volume(NAN, true, 0.0, false));
  ASSERT_EQ(-1, validate_volume(INFINITY, true, 0.0, false));
  ASSERT_EQ(-1, validate_volume(-1.0, true, 0.0, false));
  ASSERT_EQ(-1, validate_volume(0.0, false, NAN, true));
  ASSERT_EQ(-1, validate_volume(0.0, false, -INFINITY, true));
  ASSERT_EQ(-1, validate_volume(0.0, false, INFINITY, true));
}

static int validate_loudness(loudness_config_t lp) {
  filter_config_t cfg = {.type = FILTER_TYPE_LOUDNESS};
  cfg.parameters.loudness = lp;
  config_error_t err;
  config_error_init(&err);
  return g_loudness_vtable.validate(&cfg, 48000, &err);
}

// 02-2.1: NaN in any loudness field is rejected.
TEST(AuditLoudnessRejectsNonFiniteParameters) {
  loudness_config_t base = {.has_reference_level = true,
                            .reference_level = -20.0};
  ASSERT_EQ(0, validate_loudness(base));

  loudness_config_t lp = base;
  lp.reference_level = NAN;
  ASSERT_EQ(-1, validate_loudness(lp));
  lp = base;
  lp.has_high_boost = true;
  lp.high_boost = NAN;
  ASSERT_EQ(-1, validate_loudness(lp));
  lp = base;
  lp.has_low_boost = true;
  lp.low_boost = NAN;
  ASSERT_EQ(-1, validate_loudness(lp));
  lp = base;
  lp.has_low_freq = true;
  lp.low_freq = NAN;
  ASSERT_EQ(-1, validate_loudness(lp));
  lp = base;
  lp.has_high_freq = true;
  lp.high_freq = NAN;
  ASSERT_EQ(-1, validate_loudness(lp));
  lp = base;
  lp.has_low_q = true;
  lp.low_q = NAN;
  ASSERT_EQ(-1, validate_loudness(lp));
  lp = base;
  lp.has_high_q = true;
  lp.high_q = NAN;
  ASSERT_EQ(-1, validate_loudness(lp));
}

// ---------------------------------------------------------------------------
// Pipeline-level fader tests
// ---------------------------------------------------------------------------

#define FR 256
#define SR 48000
#define CHUNK_MS (1000.0 * FR / SR)

static void base_config(dsp_config_t *config, size_t channels) {
  memset(config, 0, sizeof(*config));
  config->devices.samplerate = SR;
  config->devices.chunksize = FR;
  config->devices.capture.type = AUDIO_BACKEND_TYPE_FILE;
  config->devices.capture.cfg.raw_file.channels = channels;
  config->devices.playback.type = AUDIO_BACKEND_TYPE_FILE;
  config->devices.playback.cfg.raw_file.channels = channels;
  config->devices.volume_ramp_time_ms = 0.0;
  config->devices.has_volume_ramp_time_ms = true;
}

static void fill_sine(audio_chunk_t *chunk, size_t channels, double freq,
                      size_t chunk_idx) {
  for (size_t ch = 0; ch < channels; ch++) {
    mutable_waveform_t w = audio_chunk_get_channel(chunk, ch);
    for (size_t t = 0; t < FR; t++) {
      double n = (double)(chunk_idx * FR + t);
      w[t] = 0.5 * sin(2.0 * M_PI * freq * n / SR);
    }
  }
  audio_chunk_set_valid_frames(chunk, FR);
}

static double rms(waveform_t w, size_t n) {
  double acc = 0.0;
  for (size_t i = 0; i < n; i++)
    acc += w[i] * w[i];
  return sqrt(acc / (double)n);
}

// 05-2.1 / 02-1.1(2): an Aux fader without a Volume filter still tracks its
// target, so a Loudness filter on it responds to volume changes.
TEST(AuditFaderUnusedAuxTracksTargetAndDrivesLoudness) {
  dsp_config_t config;
  base_config(&config, 1);
  named_filter_config_t fcfg;
  memset(&fcfg, 0, sizeof(fcfg));
  strcpy(fcfg.name, "loud");
  fcfg.filter.type = FILTER_TYPE_LOUDNESS;
  fcfg.filter.parameters.loudness.has_reference_level = true;
  fcfg.filter.parameters.loudness.reference_level = -20.0;
  fcfg.filter.parameters.loudness.fader = FADER_AUX1;
  config.filters = &fcfg;
  config.filters_count = 1;
  char *names[1] = {"loud"};
  pipeline_step_config_t step;
  memset(&step, 0, sizeof(step));
  step.type = PIPELINE_STEP_TYPE_FILTER;
  step.names = names;
  step.names_count = 1;
  config.pipeline = &step;
  config.pipeline_count = 1;

  processing_parameters_t *params = processing_parameters_create(1, 1);
  pipeline_t *p = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(p != NULL);
  audio_chunk_t *in = audio_chunk_create(FR, 1);
  audio_chunk_t *out = audio_chunk_create(FR, 1);

  fill_sine(in, 1, 40.0, 0);
  ASSERT_EQ(PIPELINE_OK, pipeline_process(p, in, out));
  double rms_flat = rms(audio_chunk_get_channel(out, 0), FR);

  processing_parameters_set_target_volume_for_fader(params, -45.0, FADER_AUX1);
  double boosted = 0.0;
  for (size_t c = 1; c < 20; c++) {
    fill_sine(in, 1, 40.0, c);
    ASSERT_EQ(PIPELINE_OK, pipeline_process(p, in, out));
    boosted = rms(audio_chunk_get_channel(out, 0), FR);
  }
  ASSERT_NEAR(
      -45.0,
      processing_parameters_get_current_volume_for_fader(params, FADER_AUX1),
      1e-9);
  // Full 10 dB low-shelf boost at 40 Hz: well above the flat level.
  ASSERT_TRUE(boosted > 2.0 * rms_flat);

  audio_chunk_free(in);
  audio_chunk_free(out);
  pipeline_free(p);
  processing_parameters_free(params);
}

// 05-2.2 / 02-1.1(1,3): Loudness placed before Volume on the same Aux fader,
// on two channels, multithreaded: both channels see the same level for the
// same chunk, so identical inputs give identical outputs.
TEST(AuditFaderAllChannelsSeeTheSameChunk) {
  dsp_config_t config;
  base_config(&config, 2);
  config.devices.has_multithreaded = true;
  config.devices.multithreaded = true;
  named_filter_config_t fcfg[2];
  memset(fcfg, 0, sizeof(fcfg));
  strcpy(fcfg[0].name, "loud");
  fcfg[0].filter.type = FILTER_TYPE_LOUDNESS;
  fcfg[0].filter.parameters.loudness.has_reference_level = true;
  fcfg[0].filter.parameters.loudness.reference_level = -10.0;
  fcfg[0].filter.parameters.loudness.fader = FADER_AUX1;
  strcpy(fcfg[1].name, "vol");
  fcfg[1].filter.type = FILTER_TYPE_VOLUME;
  fcfg[1].filter.parameters.volume.fader = FADER_AUX1;
  fcfg[1].filter.parameters.volume.has_ramp_time_ms = true;
  fcfg[1].filter.parameters.volume.ramp_time_ms = 6.0 * CHUNK_MS;
  config.filters = fcfg;
  config.filters_count = 2;
  char *names[2] = {"loud", "vol"};
  pipeline_step_config_t step;
  memset(&step, 0, sizeof(step));
  step.type = PIPELINE_STEP_TYPE_FILTER;
  step.names = names;
  step.names_count = 2;
  config.pipeline = &step;
  config.pipeline_count = 1;

  processing_parameters_t *params = processing_parameters_create(2, 2);
  pipeline_t *p = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(p != NULL);
  audio_chunk_t *in = audio_chunk_create(FR, 2);
  audio_chunk_t *out = audio_chunk_create(FR, 2);

  fill_sine(in, 2, 60.0, 0);
  ASSERT_EQ(PIPELINE_OK, pipeline_process(p, in, out));
  processing_parameters_set_target_volume_for_fader(params, -40.0, FADER_AUX1);
  for (size_t c = 1; c < 10; c++) {
    fill_sine(in, 2, 60.0, c);
    ASSERT_EQ(PIPELINE_OK, pipeline_process(p, in, out));
    waveform_t a = audio_chunk_get_channel(out, 0);
    waveform_t b = audio_chunk_get_channel(out, 1);
    for (size_t t = 0; t < FR; t++)
      ASSERT_NEAR(a[t], b[t], 1e-15);
  }
  audio_chunk_free(in);
  audio_chunk_free(out);
  pipeline_free(p);
  processing_parameters_free(params);
}

// 05-2.3 / 02-1.2: Loudness follows the mute ramp instead of snapping to the
// mute level on the first muted chunk. With the main fader ramping 0 -> -100
// dB over 10 chunks and a reference of -20 dB, the first muted chunk ends at
// -10 dB, so loudness must still be inactive: output == volume ramp alone.
TEST(AuditFaderLoudnessFollowsMuteRamp) {
  named_filter_config_t fcfg;
  memset(&fcfg, 0, sizeof(fcfg));
  strcpy(fcfg.name, "loud");
  fcfg.filter.type = FILTER_TYPE_LOUDNESS;
  fcfg.filter.parameters.loudness.has_reference_level = true;
  fcfg.filter.parameters.loudness.reference_level = -20.0;
  fcfg.filter.parameters.loudness.attenuate_mid = true;
  fcfg.filter.parameters.loudness.fader = FADER_MAIN;
  char *names[1] = {"loud"};
  pipeline_step_config_t step;
  memset(&step, 0, sizeof(step));
  step.type = PIPELINE_STEP_TYPE_FILTER;
  step.names = names;
  step.names_count = 1;

  dsp_config_t with;
  base_config(&with, 1);
  with.devices.volume_ramp_time_ms = 10.0 * CHUNK_MS;
  with.filters = &fcfg;
  with.filters_count = 1;
  with.pipeline = &step;
  with.pipeline_count = 1;
  dsp_config_t without;
  base_config(&without, 1);
  without.devices.volume_ramp_time_ms = 10.0 * CHUNK_MS;

  processing_parameters_t *pa = processing_parameters_create(1, 1);
  processing_parameters_t *pb = processing_parameters_create(1, 1);
  pipeline_t *a = pipeline_create(&with, pa, 0, NULL);
  pipeline_t *b = pipeline_create(&without, pb, 0, NULL);
  ASSERT_TRUE(a && b);
  audio_chunk_t *in = audio_chunk_create(FR, 1);
  audio_chunk_t *oa = audio_chunk_create(FR, 1);
  audio_chunk_t *ob = audio_chunk_create(FR, 1);
  fill_sine(in, 1, 1000.0, 0);
  ASSERT_EQ(PIPELINE_OK, pipeline_process(a, in, oa));
  ASSERT_EQ(PIPELINE_OK, pipeline_process(b, in, ob));

  processing_parameters_set_muted(pa, true);
  processing_parameters_set_muted(pb, true);
  fill_sine(in, 1, 1000.0, 1);
  ASSERT_EQ(PIPELINE_OK, pipeline_process(a, in, oa));
  ASSERT_EQ(PIPELINE_OK, pipeline_process(b, in, ob));
  waveform_t wa = audio_chunk_get_channel(oa, 0);
  waveform_t wb = audio_chunk_get_channel(ob, 0);
  for (size_t t = 0; t < FR; t++)
    ASSERT_NEAR(wb[t], wa[t], 1e-12);
  ASSERT_NEAR(-10.0, a->faders.levels.faders[FADER_MAIN].end_db, 1e-9);

  audio_chunk_free(in);
  audio_chunk_free(oa);
  audio_chunk_free(ob);
  pipeline_free(a);
  pipeline_free(b);
  processing_parameters_free(pa);
  processing_parameters_free(pb);
}

// 05-2.4 / 02-1.3: a ramp processed over fewer samples than the chunk size
// still ends on the chunk's end level.
TEST(AuditVolumeRampUsesProcessedCount) {
  processing_parameters_t *params = processing_parameters_create(1, 1);
  filter_config_t cfg = {.type = FILTER_TYPE_VOLUME};
  cfg.parameters.volume.fader = FADER_MAIN;
  cfg.parameters.volume.has_ramp_time_ms = true;
  cfg.parameters.volume.ramp_time_ms = 2.0 * 1000.0 * 8.0 / SR;
  volume_filter_t *f =
      (volume_filter_t *)g_volume_vtable.create("v", &cfg, SR, 8, params, NULL);
  ASSERT_TRUE(f != NULL);
  double w[8];
  for (int i = 0; i < 8; i++)
    w[i] = 1.0;
  g_volume_vtable.process(f, w, 8);
  processing_parameters_set_target_volume_for_fader(params, -20.0, FADER_MAIN);
  for (int i = 0; i < 4; i++)
    w[i] = 1.0;
  g_volume_vtable.process(f, w, 4);
  // First of two ramp chunks: 0 -> -10 dB laid out over the 4 samples.
  ASSERT_NEAR(1.0, w[0], 1e-12);
  ASSERT_NEAR(pow(10.0, -7.5 / 20.0), w[3], 1e-12);
  g_volume_vtable.free(f);
  processing_parameters_free(params);
}

// 05-3.2(1) / 02-1.5(1): a self-driven Volume filter does not take over the
// state of a filter on a different fader.
TEST(AuditVolumeTransferIgnoresDifferentFader) {
  processing_parameters_t *params = processing_parameters_create(1, 1);
  processing_parameters_set_target_volume_for_fader(params, -30.0, FADER_AUX1);
  processing_parameters_set_current_volume_for_fader(params, -30.0, FADER_AUX1);
  filter_config_t c1 = {.type = FILTER_TYPE_VOLUME};
  c1.parameters.volume.fader = FADER_AUX1;
  c1.parameters.volume.has_ramp_time_ms = true;
  c1.parameters.volume.ramp_time_ms = 0.0;
  filter_config_t c2 = c1;
  c2.parameters.volume.fader = FADER_AUX2;
  volume_filter_t *src =
      (volume_filter_t *)g_volume_vtable.create("v", &c1, SR, 8, params, NULL);
  volume_filter_t *dst =
      (volume_filter_t *)g_volume_vtable.create("v", &c2, SR, 8, params, NULL);
  g_volume_vtable.transfer_state(dst, src);
  double w[8];
  for (int i = 0; i < 8; i++)
    w[i] = 1.0;
  g_volume_vtable.process(dst, w, 8);
  ASSERT_NEAR(1.0, w[7], 1e-12);
  ASSERT_NEAR(
      0.0,
      processing_parameters_get_current_volume_for_fader(params, FADER_AUX2),
      1e-12);
  g_volume_vtable.free(src);
  g_volume_vtable.free(dst);
  processing_parameters_free(params);
}

// 05-3.2(2) / 02-1.5(2): ramp time change mid-ramp restarts from the current
// level at the new speed, or settles at once when ramping is disabled
// (upstream Faders::update_parameters).
TEST(AuditFaderTransferRampTimeChange) {
  processing_parameters_t *params = processing_parameters_create(1, 1);
  fader_settings_t slow[FADER_COUNT], none[FADER_COUNT], fast[FADER_COUNT];
  for (int i = 0; i < FADER_COUNT; i++) {
    slow[i] = (fader_settings_t){.ramp_time_ms = 10.0 * CHUNK_MS, .limit = 50};
    none[i] = (fader_settings_t){.ramp_time_ms = 0.0, .limit = 50};
    fast[i] = (fader_settings_t){.ramp_time_ms = 4.0 * CHUNK_MS, .limit = 50};
  }
  pipeline_faders_t a, b, c;
  pipeline_faders_init(&a, slow, params, FR, SR);
  processing_parameters_set_target_volume(params, -20.0);
  for (int i = 0; i < 3; i++)
    pipeline_faders_prepare_chunk(&a);
  ASSERT_NEAR(-6.0, a.faders[0].current_db, 1e-9);

  pipeline_faders_init(&b, none, params, FR, SR);
  pipeline_faders_transfer(&b, &a);
  ASSERT_EQ(0, b.faders[0].ramp_step);
  ASSERT_NEAR(-20.0, b.faders[0].current_db, 1e-9);

  pipeline_faders_init(&c, fast, params, FR, SR);
  pipeline_faders_transfer(&c, &a);
  ASSERT_EQ(1, c.faders[0].ramp_step);
  ASSERT_NEAR(-6.0, c.faders[0].ramp_start, 1e-9);
  pipeline_faders_prepare_chunk(&c);
  ASSERT_NEAR(-6.0, c.levels.faders[0].start_db, 1e-9);
  ASSERT_NEAR(-9.5, c.levels.faders[0].end_db, 1e-9);
  processing_parameters_free(params);
}

// 07-1.1: a structural reload during a ramp starts the new pipeline settled at
// the target instead of restarting a full-length ramp.
TEST(AuditFaderStructuralReloadStartsSettled) {
  dsp_config_t config;
  base_config(&config, 1);
  config.devices.volume_ramp_time_ms = 20.0 * CHUNK_MS;
  processing_parameters_t *params = processing_parameters_create(1, 1);
  pipeline_t *a = pipeline_create(&config, params, 0, NULL);
  audio_chunk_t *in = audio_chunk_create(FR, 1);
  audio_chunk_t *out = audio_chunk_create(FR, 1);
  for (size_t t = 0; t < FR; t++)
    audio_chunk_get_channel(in, 0)[t] = 1.0;
  audio_chunk_set_valid_frames(in, FR);
  ASSERT_EQ(PIPELINE_OK, pipeline_process(a, in, out));
  processing_parameters_set_target_volume(params, -20.0);
  for (int i = 0; i < 3; i++)
    ASSERT_EQ(PIPELINE_OK, pipeline_process(a, in, out));
  ASSERT_TRUE(processing_parameters_get_current_volume(params) > -19.0);

  pipeline_t *b = pipeline_create(&config, params, 0, NULL);
  ASSERT_TRUE(b != NULL);
  pipeline_transfer_state(b, a, false);
  pipeline_free(a);
  ASSERT_EQ(PIPELINE_OK, pipeline_process(b, in, out));
  waveform_t w = audio_chunk_get_channel(out, 0);
  ASSERT_NEAR(pow(10.0, -1.0), w[0], 1e-12);
  ASSERT_NEAR(pow(10.0, -1.0), w[FR - 1], 1e-12);

  audio_chunk_free(in);
  audio_chunk_free(out);
  pipeline_free(b);
  processing_parameters_free(params);
}

// 05-2.5: the bank seeds limited levels; the published current volume never
// exceeds the limit.
TEST(AuditFaderSeedRespectsLimit) {
  dsp_config_t config;
  base_config(&config, 1);
  config.devices.has_volume_limit = true;
  config.devices.volume_limit = -6.0;
  processing_parameters_t *params = processing_parameters_create(1, 1);
  processing_parameters_set_target_volume(params, 0.0);
  pipeline_t *p = pipeline_create(&config, params, 0, NULL);
  ASSERT_NEAR(-6.0, p->faders.levels.faders[0].end_db, 1e-12);
  audio_chunk_t *in = audio_chunk_create(FR, 1);
  for (size_t t = 0; t < FR; t++)
    audio_chunk_get_channel(in, 0)[t] = 1.0;
  audio_chunk_set_valid_frames(in, FR);
  ASSERT_EQ(PIPELINE_OK, pipeline_process(p, in, in));
  ASSERT_NEAR(pow(10.0, -6.0 / 20.0), audio_chunk_get_channel(in, 0)[0], 1e-12);
  ASSERT_NEAR(-6.0, processing_parameters_get_current_volume(params), 1e-12);
  audio_chunk_free(in);
  pipeline_free(p);
  processing_parameters_free(params);
}

TEST_MAIN()
