#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>

#include "pipeline/state_file.h"
#include "test_support.h"

TEST(test_state_file_round_trip) {
  char test_file[256];
  snprintf(test_file, sizeof(test_file), "test_state_%d.yaml", getpid());

  dsp_state_t *original = dsp_state_create();
  ASSERT_TRUE(original != NULL);
  dsp_state_set_config_path(original, "/var/tmp/config.json");
  dsp_state_set_mute(original, 0, true);
  dsp_state_set_mute(original, 1, false);
  dsp_state_set_mute(original, 2, true);
  dsp_state_set_mute(original, 3, false);
  dsp_state_set_mute(original, 4, true);
  dsp_state_set_volume(original, 0, 0.0);
  dsp_state_set_volume(original, 1, -6.02);
  dsp_state_set_volume(original, 2, -12.0);
  dsp_state_set_volume(original, 3, -20.5);
  dsp_state_set_volume(original, 4, 3.14159);

  // Save state
  ASSERT_TRUE(dsp_state_save(test_file, original));

  // Load state
  dsp_state_t *loaded = dsp_state_create();
  ASSERT_TRUE(loaded != NULL);
  ASSERT_TRUE(dsp_state_load(test_file, loaded));

  // Check config path
  ASSERT_TRUE(dsp_state_has_config_path(loaded));
  ASSERT_STR_EQ(dsp_state_get_config_path(original),
                dsp_state_get_config_path(loaded));

  // Check mutes and volumes
  for (int i = 0; i < 5; i++) {
    ASSERT_TRUE(dsp_state_get_mute(original, i) ==
                dsp_state_get_mute(loaded, i));
    ASSERT_NEAR(dsp_state_get_volume(original, i),
                dsp_state_get_volume(loaded, i), 1e-6);
  }

  dsp_state_free(original);
  dsp_state_free(loaded);

  // Clean up
  unlink(test_file);
}

TEST(test_state_file_no_config_path) {
  char test_file[256];
  snprintf(test_file, sizeof(test_file), "test_state_no_path_%d.yaml",
           getpid());

  dsp_state_t *original = dsp_state_create();
  ASSERT_TRUE(original != NULL);
  dsp_state_set_has_config_path(original, false);
  dsp_state_set_mute(original, 0, false);
  dsp_state_set_mute(original, 1, true);
  dsp_state_set_mute(original, 2, false);
  dsp_state_set_mute(original, 3, true);
  dsp_state_set_mute(original, 4, false);
  dsp_state_set_volume(original, 0, -1.0);
  dsp_state_set_volume(original, 1, -2.0);
  dsp_state_set_volume(original, 2, -3.0);
  dsp_state_set_volume(original, 3, -4.0);
  dsp_state_set_volume(original, 4, -5.0);

  // Save state
  ASSERT_TRUE(dsp_state_save(test_file, original));

  // Load state
  dsp_state_t *loaded = dsp_state_create();
  ASSERT_TRUE(loaded != NULL);
  ASSERT_TRUE(dsp_state_load(test_file, loaded));

  // Check config path
  ASSERT_FALSE(dsp_state_has_config_path(loaded));

  // Check mutes and volumes
  for (int i = 0; i < 5; i++) {
    ASSERT_TRUE(dsp_state_get_mute(original, i) ==
                dsp_state_get_mute(loaded, i));
    ASSERT_NEAR(dsp_state_get_volume(original, i),
                dsp_state_get_volume(loaded, i), 1e-6);
  }

  dsp_state_free(original);
  dsp_state_free(loaded);

  // Clean up
  unlink(test_file);
}

// A statefile written by upstream CamillaDSP: its libyaml-based serializer
// emits block sequences at the same indentation as the mapping key.
TEST(test_state_file_load_upstream_format) {
  char test_file[256];
  snprintf(test_file, sizeof(test_file), "test_state_upstream_%d.yaml",
           getpid());

  FILE *fp = fopen(test_file, "w");
  ASSERT_TRUE(fp != NULL);
  fprintf(fp, "---\n"
              "config_path: /var/tmp/upstream.yml\n"
              "mute:\n"
              "- true\n"
              "- false\n"
              "- true\n"
              "- false\n"
              "- true\n"
              "volume:\n"
              "- -40.0\n"
              "- -12.5\n"
              "- 0.0\n"
              "- 3.5\n"
              "- -6.0\n");
  fclose(fp);

  dsp_state_t *loaded = dsp_state_create();
  ASSERT_TRUE(loaded != NULL);
  ASSERT_TRUE(dsp_state_load(test_file, loaded));

  ASSERT_TRUE(dsp_state_has_config_path(loaded));
  ASSERT_STR_EQ("/var/tmp/upstream.yml", dsp_state_get_config_path(loaded));

  const bool expected_mute[5] = {true, false, true, false, true};
  const double expected_volume[5] = {-40.0, -12.5, 0.0, 3.5, -6.0};
  for (int i = 0; i < 5; i++) {
    ASSERT_TRUE(expected_mute[i] == dsp_state_get_mute(loaded, i));
    ASSERT_NEAR(expected_volume[i], dsp_state_get_volume(loaded, i), 1e-6);
  }

  dsp_state_free(loaded);
  unlink(test_file);
}

// Upstream deserializes the whole file or nothing at all, so a short fader
// list must not be reported as a successful load.
TEST(test_state_file_rejects_incomplete) {
  char test_file[256];
  snprintf(test_file, sizeof(test_file), "test_state_short_%d.yaml", getpid());

  FILE *fp = fopen(test_file, "w");
  ASSERT_TRUE(fp != NULL);
  fprintf(fp, "---\n"
              "config_path: /var/tmp/short.yml\n"
              "mute:\n"
              "- true\n"
              "- false\n"
              "volume:\n"
              "- -40.0\n"
              "- -12.5\n");
  fclose(fp);

  dsp_state_t *loaded = dsp_state_create();
  ASSERT_TRUE(loaded != NULL);
  ASSERT_FALSE(dsp_state_load(test_file, loaded));
  // Nothing from an invalid file may be adopted, config_path included.
  ASSERT_FALSE(dsp_state_has_config_path(loaded));

  dsp_state_free(loaded);
  unlink(test_file);
}

// Upstream uses `deny_unknown_fields`.
TEST(test_state_file_rejects_unknown_key) {
  char test_file[256];
  snprintf(test_file, sizeof(test_file), "test_state_unknown_%d.yaml",
           getpid());

  FILE *fp = fopen(test_file, "w");
  ASSERT_TRUE(fp != NULL);
  fprintf(fp, "---\n"
              "config_path: null\n"
              "mute:\n"
              "- false\n"
              "- false\n"
              "- false\n"
              "- false\n"
              "- false\n"
              "volume:\n"
              "- 0.0\n"
              "- 0.0\n"
              "- 0.0\n"
              "- 0.0\n"
              "- 0.0\n"
              "bogus: 1\n");
  fclose(fp);

  dsp_state_t *loaded = dsp_state_create();
  ASSERT_TRUE(loaded != NULL);
  ASSERT_FALSE(dsp_state_load(test_file, loaded));

  dsp_state_free(loaded);
  unlink(test_file);
}

TEST_MAIN()
