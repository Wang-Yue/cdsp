#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Config/log_level.h"
#include "test_support.h"

TEST(LogLevelStringConversions) {
  ASSERT_STR_EQ(log_level_to_string(LOG_LEVEL_OFF), "Off");
  ASSERT_STR_EQ(log_level_to_string(LOG_LEVEL_ERROR), "Error");
  ASSERT_STR_EQ(log_level_to_string(LOG_LEVEL_WARN), "Warn");
  ASSERT_STR_EQ(log_level_to_string(LOG_LEVEL_INFO), "Info");
  ASSERT_STR_EQ(log_level_to_string(LOG_LEVEL_DEBUG), "Debug");
  ASSERT_STR_EQ(log_level_to_string(LOG_LEVEL_TRACE), "Trace");

  ASSERT_EQ(log_level_from_string("off"), LOG_LEVEL_OFF);
  ASSERT_EQ(log_level_from_string("Error"), LOG_LEVEL_ERROR);
  ASSERT_EQ(log_level_from_string("WARN"), LOG_LEVEL_WARN);
  ASSERT_EQ(log_level_from_string("info"), LOG_LEVEL_INFO);
  ASSERT_EQ(log_level_from_string("Debug"), LOG_LEVEL_DEBUG);
  ASSERT_EQ(log_level_from_string("TRACE"), LOG_LEVEL_TRACE);
  ASSERT_EQ(log_level_from_string("invalid_str"), LOG_LEVEL_INFO);
  ASSERT_EQ(log_level_from_string(NULL), LOG_LEVEL_INFO);
}

TEST(LogLevelRawByteEncoding) {
  log_level_t levels[] = {LOG_LEVEL_OFF,  LOG_LEVEL_ERROR, LOG_LEVEL_WARN,
                          LOG_LEVEL_INFO, LOG_LEVEL_DEBUG, LOG_LEVEL_TRACE};

  for (size_t i = 0; i < 6; i++) {
    uint8_t byte = log_level_to_raw_byte(levels[i]);
    log_level_t decoded = log_level_from_raw_byte(byte);
    ASSERT_EQ(decoded, levels[i]);
  }
}

TEST_MAIN()
