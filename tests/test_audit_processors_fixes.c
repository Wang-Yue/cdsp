// Regression tests for the processor/mixer audit fixes (report 04, plus
// report 02 §3.1/§3.2 processor parts and report 05 §3.4).

#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "audio/audio_chunk.h"
#include "config/config_error.h"
#include "config/config_gen.h"
#include "mixer/mixer.h"
#include "processors/processor.h"
#include "test_support.h"

/* ------------------------------------------------------------------------- */
/* LookaheadLimiter processor                                                */
/* ------------------------------------------------------------------------- */

static processor_config_t limiter_cfg(size_t channels, double attack_samples) {
  processor_config_t cfg = {.type = PROCESSOR_TYPE_LOOKAHEAD_LIMITER};
  lookahead_limiter_processor_config_t *p = &cfg.parameters.lookahead_limiter;
  p->channels = channels;
  p->has_limit = true;
  p->limit = -6.0;
  p->attack = attack_samples;
  p->attack_unit = TIME_UNIT_SAMPLES;
  p->release = 100.0;
  p->release_unit = TIME_UNIT_SAMPLES;
  return cfg;
}

TEST(audit_limiter_rejects_non_finite_params) {
  processor_config_t cfg = limiter_cfg(2, 4.0);
  ASSERT_EQ(0, processor_config_validate(&cfg, 48000, NULL));

  cfg.parameters.lookahead_limiter.limit = NAN;
  ASSERT_NE(0, processor_config_validate(&cfg, 48000, NULL));
  cfg.parameters.lookahead_limiter.limit = INFINITY;
  ASSERT_NE(0, processor_config_validate(&cfg, 48000, NULL));
  cfg.parameters.lookahead_limiter.limit = -INFINITY;
  ASSERT_NE(0, processor_config_validate(&cfg, 48000, NULL));
  cfg.parameters.lookahead_limiter.limit = -6.0;

  cfg.parameters.lookahead_limiter.attack = NAN;
  ASSERT_NE(0, processor_config_validate(&cfg, 48000, NULL));
  cfg.parameters.lookahead_limiter.attack = 4.0;

  cfg.parameters.lookahead_limiter.release = NAN;
  ASSERT_NE(0, processor_config_validate(&cfg, 48000, NULL));
  cfg.parameters.lookahead_limiter.release = INFINITY;
  ASSERT_NE(0, processor_config_validate(&cfg, 48000, NULL));
  cfg.parameters.lookahead_limiter.release = 100.0;

  cfg.parameters.lookahead_limiter.attack_unit = TIME_UNIT_INVALID;
  ASSERT_NE(0, processor_config_validate(&cfg, 48000, NULL));
  cfg.parameters.lookahead_limiter.attack_unit = TIME_UNIT_SAMPLES;
  cfg.parameters.lookahead_limiter.release_unit = TIME_UNIT_INVALID;
  ASSERT_NE(0, processor_config_validate(&cfg, 48000, NULL));
  cfg.parameters.lookahead_limiter.release_unit = TIME_UNIT_SAMPLES;

  // create() must report the failure through err.
  cfg.parameters.lookahead_limiter.release = NAN;
  config_error_t err;
  config_error_init(&err);
  ASSERT_TRUE(dsp_processor_create("lim", &cfg, 48000, 16, &err) == NULL);
  ASSERT_NE(CONFIG_ERR_NONE, err.type);
  ASSERT_TRUE(err.message[0] != '\0');
}

// Upstream rebuilds all lookahead delays when `channels` changes. A hot reload
// that changes the channel count must not leak old delayed audio.
TEST(audit_limiter_transfer_channel_change_resets_delays) {
  const size_t frames = 8;
  processor_config_t cfg2 = limiter_cfg(2, 4.0);
  processor_config_t cfg3 = limiter_cfg(3, 4.0);
  dsp_processor_t *old_lim =
      dsp_processor_create("lim", &cfg2, 48000, frames, NULL);
  dsp_processor_t *new_lim =
      dsp_processor_create("lim", &cfg3, 48000, frames, NULL);
  ASSERT_TRUE(old_lim != NULL);
  ASSERT_TRUE(new_lim != NULL);

  audio_chunk_t *c2 = audio_chunk_create(frames, 2);
  for (size_t ch = 0; ch < 2; ch++) {
    double *buf = audio_chunk_get_channel(c2, ch);
    for (size_t i = 0; i < frames; i++)
      buf[i] = 0.25;
  }
  audio_chunk_set_valid_frames(c2, frames);
  dsp_processor_process(old_lim, c2);

  dsp_processor_transfer_state(new_lim, old_lim);

  audio_chunk_t *c3 = audio_chunk_create(frames, 3);
  for (size_t ch = 0; ch < 3; ch++)
    memset(audio_chunk_get_channel(c3, ch), 0, frames * sizeof(double));
  audio_chunk_set_valid_frames(c3, frames);
  dsp_processor_process(new_lim, c3);
  for (size_t ch = 0; ch < 3; ch++) {
    const double *buf = audio_chunk_get_channel(c3, ch);
    for (size_t i = 0; i < frames; i++)
      ASSERT_TRUE(buf[i] == 0.0);
  }

  audio_chunk_free(c2);
  audio_chunk_free(c3);
  dsp_processor_free(old_lim);
  dsp_processor_free(new_lim);
}

// Same channel count: delay contents are still preserved (cdsp keeps state
// across unchanged reloads).
TEST(audit_limiter_transfer_same_channels_keeps_delays) {
  const size_t frames = 8;
  processor_config_t cfg = limiter_cfg(2, 4.0);
  dsp_processor_t *old_lim =
      dsp_processor_create("lim", &cfg, 48000, frames, NULL);
  dsp_processor_t *new_lim =
      dsp_processor_create("lim", &cfg, 48000, frames, NULL);
  ASSERT_TRUE(old_lim != NULL && new_lim != NULL);

  audio_chunk_t *c = audio_chunk_create(frames, 2);
  for (size_t ch = 0; ch < 2; ch++) {
    double *buf = audio_chunk_get_channel(c, ch);
    for (size_t i = 0; i < frames; i++)
      buf[i] = 0.25;
  }
  audio_chunk_set_valid_frames(c, frames);
  dsp_processor_process(old_lim, c);
  dsp_processor_transfer_state(new_lim, old_lim);

  for (size_t ch = 0; ch < 2; ch++)
    memset(audio_chunk_get_channel(c, ch), 0, frames * sizeof(double));
  dsp_processor_process(new_lim, c);
  // 0.25 is below the -6 dB limit, so the 4 delayed samples pass unchanged.
  const double *buf = audio_chunk_get_channel(c, 0);
  ASSERT_TRUE(fabs(buf[0] - 0.25) < 1e-12);
  ASSERT_TRUE(buf[frames - 1] == 0.0);

  audio_chunk_free(c);
  dsp_processor_free(old_lim);
  dsp_processor_free(new_lim);
}

/* ------------------------------------------------------------------------- */
/* Compressor / NoiseGate validation                                         */
/* ------------------------------------------------------------------------- */

static processor_config_t compressor_cfg(void) {
  processor_config_t cfg = {.type = PROCESSOR_TYPE_COMPRESSOR};
  compressor_config_t *p = &cfg.parameters.compressor;
  p->channels = 2;
  p->attack = 0.01;
  p->attack_unit = TIME_UNIT_S;
  p->release = 0.1;
  p->release_unit = TIME_UNIT_S;
  p->threshold = -20.0;
  p->factor = 2.0;
  return cfg;
}

TEST(audit_compressor_validation) {
  processor_config_t cfg = compressor_cfg();
  ASSERT_EQ(0, processor_config_validate(&cfg, 48000, NULL));

  cfg.parameters.compressor.attack = NAN;
  ASSERT_NE(0, processor_config_validate(&cfg, 48000, NULL));
  cfg.parameters.compressor.attack = INFINITY;
  ASSERT_NE(0, processor_config_validate(&cfg, 48000, NULL));
  cfg.parameters.compressor.attack = 0.01;

  cfg.parameters.compressor.release = NAN;
  ASSERT_NE(0, processor_config_validate(&cfg, 48000, NULL));
  cfg.parameters.compressor.release = 0.1;

  cfg.parameters.compressor.threshold = NAN;
  ASSERT_NE(0, processor_config_validate(&cfg, 48000, NULL));
  cfg.parameters.compressor.threshold = -20.0;

  cfg.parameters.compressor.has_makeup_gain = true;
  cfg.parameters.compressor.makeup_gain = INFINITY;
  ASSERT_NE(0, processor_config_validate(&cfg, 48000, NULL));
  cfg.parameters.compressor.makeup_gain = 3.0;
  ASSERT_EQ(0, processor_config_validate(&cfg, 48000, NULL));

  cfg.parameters.compressor.attack_unit = TIME_UNIT_INVALID;
  ASSERT_NE(0, processor_config_validate(&cfg, 48000, NULL));
  cfg.parameters.compressor.attack_unit = TIME_UNIT_S;

  // clip_limit is validated up front, consistent with create().
  cfg.parameters.compressor.has_clip_limit = true;
  cfg.parameters.compressor.clip_limit = -1.0;
  ASSERT_EQ(0, processor_config_validate(&cfg, 48000, NULL));
  cfg.parameters.compressor.clip_limit = NAN;
  ASSERT_NE(0, processor_config_validate(&cfg, 48000, NULL));
  cfg.parameters.compressor.clip_limit = -7000.0; // underflows to 0 linear
  ASSERT_NE(0, processor_config_validate(&cfg, 48000, NULL));
  ASSERT_TRUE(dsp_processor_create("c", &cfg, 48000, 16, NULL) == NULL);
}

TEST(audit_noise_gate_validation) {
  processor_config_t cfg = {.type = PROCESSOR_TYPE_NOISE_GATE};
  noise_gate_config_t *p = &cfg.parameters.noise_gate;
  p->channels = 2;
  p->attack = 0.01;
  p->attack_unit = TIME_UNIT_S;
  p->release = 0.1;
  p->release_unit = TIME_UNIT_S;
  p->threshold = -40.0;
  p->attenuation = 20.0;
  ASSERT_EQ(0, processor_config_validate(&cfg, 48000, NULL));

  p->attack = NAN;
  ASSERT_NE(0, processor_config_validate(&cfg, 48000, NULL));
  p->attack = 0.01;
  p->release = INFINITY;
  ASSERT_NE(0, processor_config_validate(&cfg, 48000, NULL));
  p->release = 0.1;
  p->threshold = NAN;
  ASSERT_NE(0, processor_config_validate(&cfg, 48000, NULL));
  p->threshold = -40.0;
  p->attenuation = INFINITY;
  ASSERT_NE(0, processor_config_validate(&cfg, 48000, NULL));
  p->attenuation = 20.0;
  p->release_unit = TIME_UNIT_INVALID;
  ASSERT_NE(0, processor_config_validate(&cfg, 48000, NULL));
}

/* ------------------------------------------------------------------------- */
/* RACE                                                                      */
/* ------------------------------------------------------------------------- */

static processor_config_t race_cfg(size_t channels, size_t a, size_t b) {
  processor_config_t cfg = {.type = PROCESSOR_TYPE_RACE};
  race_config_t *p = &cfg.parameters.race;
  p->channels = channels;
  p->channel_a = a;
  p->channel_b = b;
  p->delay = 4.0;
  p->delay_unit = DELAY_UNIT_SAMPLES;
  p->attenuation = 3.0;
  return cfg;
}

TEST(audit_race_validate_rejects_oversized_delay) {
  processor_config_t cfg = race_cfg(2, 0, 1);
  ASSERT_EQ(0, processor_config_validate(&cfg, 48000, NULL));
  cfg.parameters.race.delay = 2.0e8; // > 100M samples delay filter limit
  ASSERT_NE(0, processor_config_validate(&cfg, 48000, NULL));
  ASSERT_TRUE(dsp_processor_create("r", &cfg, 48000, 16, NULL) == NULL);
}

// Upstream race.rs returns early if either channel is an (empty) unused capture
// channel; cdsp marks those via used_channels.
TEST(audit_race_skips_unused_channel) {
  processor_config_t cfg = race_cfg(2, 0, 1);
  dsp_processor_t *race = dsp_processor_create("r", &cfg, 48000, 16, NULL);
  ASSERT_TRUE(race != NULL);

  const size_t frames = 16;
  audio_chunk_t *c = audio_chunk_create(frames, 2);
  double *a = audio_chunk_get_channel(c, 0);
  double *b = audio_chunk_get_channel(c, 1);
  for (size_t i = 0; i < frames; i++) {
    a[i] = (i == 0) ? 1.0 : 0.0;
    b[i] = 0.0;
  }
  audio_chunk_set_valid_frames(c, frames);
  bool used[2] = {true, false};
  audio_chunk_set_used_channels(c, used);

  dsp_processor_process(race, c);
  ASSERT_TRUE(a[0] == 1.0);
  for (size_t i = 1; i < frames; i++)
    ASSERT_TRUE(a[i] == 0.0);
  for (size_t i = 0; i < frames; i++)
    ASSERT_TRUE(b[i] == 0.0);

  // With both channels used, the cross-talk loop does run.
  bool all_used[2] = {true, true};
  audio_chunk_set_used_channels(c, all_used);
  dsp_processor_process(race, c);
  bool any_nonzero = false;
  for (size_t i = 0; i < frames; i++)
    if (b[i] != 0.0)
      any_nonzero = true;
  ASSERT_TRUE(any_nonzero);

  audio_chunk_free(c);
  dsp_processor_free(race);
}

// Report 05 §3.4: feedback/delay state must not migrate to a different
// channel pair on hot reload.
TEST(audit_race_transfer_channel_change_resets_state) {
  const size_t frames = 16;
  processor_config_t cfg_old = race_cfg(3, 0, 1);
  processor_config_t cfg_new = race_cfg(3, 0, 2);
  dsp_processor_t *old_r = dsp_processor_create("r", &cfg_old, 48000, 16, NULL);
  dsp_processor_t *new_r = dsp_processor_create("r", &cfg_new, 48000, 16, NULL);
  ASSERT_TRUE(old_r != NULL && new_r != NULL);

  audio_chunk_t *c = audio_chunk_create(frames, 3);
  for (size_t ch = 0; ch < 3; ch++) {
    double *buf = audio_chunk_get_channel(c, ch);
    for (size_t i = 0; i < frames; i++)
      buf[i] = 0.5;
  }
  audio_chunk_set_valid_frames(c, frames);
  dsp_processor_process(old_r, c);

  dsp_processor_transfer_state(new_r, old_r);
  for (size_t ch = 0; ch < 3; ch++)
    memset(audio_chunk_get_channel(c, ch), 0, frames * sizeof(double));
  dsp_processor_process(new_r, c);
  for (size_t ch = 0; ch < 3; ch++) {
    const double *buf = audio_chunk_get_channel(c, ch);
    for (size_t i = 0; i < frames; i++)
      ASSERT_TRUE(buf[i] == 0.0);
  }

  audio_chunk_free(c);
  dsp_processor_free(old_r);
  dsp_processor_free(new_r);
}

/* ------------------------------------------------------------------------- */
/* Mixer                                                                     */
/* ------------------------------------------------------------------------- */

TEST(audit_mixer_rejects_non_finite_gain) {
  mixer_source_t src = {.channel = 0, .has_gain = true, .gain = 0.0};
  mixer_mapping_t map = {.dest = 0, .sources = &src, .sources_count = 1};
  mixer_config_t cfg = {0};
  cfg.channels_in = 1;
  cfg.channels_out = 1;
  cfg.mapping = &map;
  cfg.mapping_count = 1;
  ASSERT_EQ(0, mixer_config_validate(&cfg, NULL));
  src.gain = NAN;
  ASSERT_NE(0, mixer_config_validate(&cfg, NULL));
  src.gain = -INFINITY;
  ASSERT_NE(0, mixer_config_validate(&cfg, NULL));
}

// Mixer mixes the full buffer (upstream mixes `input.frames`) and propagates
// valid_frames from the input.
TEST(audit_mixer_partial_chunk_valid_frames) {
  mixer_source_t src = {.channel = 0, .has_gain = true, .gain = 0.0};
  mixer_mapping_t map = {.dest = 0, .sources = &src, .sources_count = 1};
  mixer_config_t cfg = {0};
  cfg.channels_in = 1;
  cfg.channels_out = 1;
  cfg.mapping = &map;
  cfg.mapping_count = 1;
  mixer_t *m = mixer_create("m", &cfg, 8, NULL);
  ASSERT_TRUE(m != NULL);

  audio_chunk_t *in = audio_chunk_create(8, 1);
  audio_chunk_t *out = audio_chunk_create(8, 1);
  double *ib = audio_chunk_get_channel(in, 0);
  for (size_t i = 0; i < 8; i++)
    ib[i] = (i < 5) ? 1.0 : 0.0;
  audio_chunk_set_valid_frames(in, 5);
  ASSERT_EQ(MIXER_OK, mixer_process(m, in, out));
  ASSERT_EQ(5u, audio_chunk_get_valid_frames(out));
  const double *ob = audio_chunk_get_channel(out, 0);
  for (size_t i = 0; i < 8; i++)
    ASSERT_TRUE(ob[i] == ((i < 5) ? 1.0 : 0.0));

  audio_chunk_free(in);
  audio_chunk_free(out);
  mixer_free(m);
}

TEST_MAIN()
