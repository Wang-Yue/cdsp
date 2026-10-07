// Regression tests for audit report 05 §5 (state file parsing/serialization).
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "pipeline/state_file.h"
#include "test_support.h"

static void write_text(const char *path, const char *text) {
  FILE *fp = fopen(path, "w");
  if (fp) {
    fputs(text, fp);
    fclose(fp);
  }
}

static bool load_text(const char *text) {
  char path[256];
  snprintf(path, sizeof(path), "audit_state_%d.yaml", getpid());
  write_text(path, text);
  dsp_state_t *st = dsp_state_create();
  bool ok = dsp_state_load(path, st);
  dsp_state_free(st);
  unlink(path);
  return ok;
}

// §5.1: a scalar after "mute:" / "volume:" must not be silently ignored.
TEST(StateFileRejectsScalarGarbageOnMuteKey) {
  ASSERT_FALSE(load_text("config_path: null\n"
                         "mute: invalid_garbage\n"
                         "- false\n- false\n- false\n- false\n- false\n"
                         "volume: [0, 0, 0, 0, 0]\n"));
}

TEST(StateFileRejectsScalarGarbageOnVolumeKey) {
  ASSERT_FALSE(load_text("config_path: null\n"
                         "mute: [false, false, false, false, false]\n"
                         "volume: garbage\n"
                         "- 0\n- 0\n- 0\n- 0\n- 0\n"));
}

TEST(StateFileAcceptsBlockSequences) {
  ASSERT_TRUE(load_text("config_path: null\n"
                        "mute:\n- false\n- true\n- false\n- false\n- false\n"
                        "volume:\n- 0\n- -3\n- 0\n- 0\n- 0\n"));
}

// §5.2: trailing garbage after ']' and empty items must be rejected.
TEST(StateFileRejectsTrailingGarbageAfterFlowSequence) {
  ASSERT_FALSE(load_text("config_path: null\n"
                         "mute: [false, false, false, false, false] junk\n"
                         "volume: [0, 0, 0, 0, 0]\n"));
}

TEST(StateFileRejectsEmptyFlowItems) {
  ASSERT_FALSE(load_text("config_path: null\n"
                         "mute: [false, false, false, false, false, ,,,]\n"
                         "volume: [0, 0, 0, 0, 0]\n"));
  ASSERT_FALSE(load_text("config_path: null\n"
                         "mute: [false, false,, false, false, false]\n"
                         "volume: [0, 0, 0, 0, 0]\n"));
}

TEST(StateFileAcceptsFlowSequencesAndSingleTrailingComma) {
  ASSERT_TRUE(load_text("config_path: null\n"
                        "mute: [false, false, false, false, false]\n"
                        "volume: [0, -1.5, 0, 0, 0,]\n"));
}

// §5.3: control characters in config_path survive a save/load round trip.
TEST(StateFileRoundTripsControlCharactersInConfigPath) {
  char path[256];
  snprintf(path, sizeof(path), "audit_state_ctl_%d.yaml", getpid());
  const char *weird = "/tmp/a\nb\rc\td\"e\\f\x01g.yml";
  dsp_state_t *st = dsp_state_create();
  dsp_state_set_config_path(st, weird);
  for (int i = 0; i < 5; i++) {
    dsp_state_set_mute(st, i, i % 2 == 0);
    dsp_state_set_volume(st, i, -1.0 * i);
  }
  ASSERT_TRUE(dsp_state_save(path, st));

  dsp_state_t *loaded = dsp_state_create();
  ASSERT_TRUE(dsp_state_load(path, loaded));
  ASSERT_TRUE(dsp_state_has_config_path(loaded));
  ASSERT_STR_EQ(weird, dsp_state_get_config_path(loaded));
  for (int i = 0; i < 5; i++) {
    ASSERT_TRUE(dsp_state_get_mute(loaded, i) == (i % 2 == 0));
    ASSERT_NEAR(-1.0 * i, dsp_state_get_volume(loaded, i), 1e-9);
  }
  dsp_state_free(st);
  dsp_state_free(loaded);
  unlink(path);
}

TEST_MAIN()
