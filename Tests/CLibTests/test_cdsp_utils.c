#include <stdint.h>
#include <stdlib.h>

#include "Utils/cdsp_time.h"
#include "Utils/double_helpers.h"
#include "Utils/float_helpers.h"
#include "test_support.h"

TEST(CdspTimeMonotonicProgression) {
  uint64_t t1 = cdsp_time_now_ns();
  cdsp_sleep_ms(5);
  uint64_t t2 = cdsp_time_now_ns();

  ASSERT_TRUE(t2 > t1);
  uint64_t diff_ns = t2 - t1;
  // Sleep of 5ms should be at least 4ms (4,000,000 ns)
  ASSERT_TRUE(diff_ns >= 4000000ULL);

  cdsp_sleep_us(500);  // 500 microseconds
  uint64_t t3 = cdsp_time_now_ns();
  ASSERT_TRUE(t3 > t2);
}

TEST(DoubleHelpersDecibelsAndEnvelope) {
  double gain_0db = double_from_db(0.0);
  ASSERT_NEAR(gain_0db, 1.0, 1e-6);

  double gain_m6db = double_from_db(-6.0205999);
  ASSERT_NEAR(gain_m6db, 0.5, 1e-4);

  float db_unity = float_to_db(1.0f);
  ASSERT_NEAR(db_unity, 0.0f, 1e-6);

  float db_zero = float_to_db(0.0f);
  ASSERT_EQ(db_zero, -1000.0f);

  float db_neg = float_to_db(-5.0f);
  ASSERT_EQ(db_neg, -1000.0f);

  // Attack / Release envelope smoothing
  double smoothed_attack = double_smooth_envelope(1.0, 0.0, 0.1, 0.01);
  ASSERT_TRUE(smoothed_attack > 0.0);
  ASSERT_TRUE(smoothed_attack <= 1.0);

  double smoothed_release = double_smooth_envelope(0.0, 1.0, 0.1, 0.01);
  ASSERT_TRUE(smoothed_release < 1.0);
  ASSERT_TRUE(smoothed_release >= 0.0);
}

#include "Utils/cdsp_path.h"

TEST(PathTildeExpansion) {
  char buf[512];
  cdsp_expand_path("~/music.wav", buf, sizeof(buf));

  const char* home = getenv("HOME");
#if defined(_WIN32)
  if (!home) home = getenv("USERPROFILE");
#endif
  if (home && home[0] != '\0') {
    char expected[512];
    snprintf(expected, sizeof(expected), "%s/music.wav", home);
    ASSERT_STR_EQ(expected, buf);
  } else {
    ASSERT_STR_EQ("~/music.wav", buf);
  }

  cdsp_expand_path("/absolute/path.wav", buf, sizeof(buf));
  ASSERT_STR_EQ("/absolute/path.wav", buf);
}

TEST_MAIN()
