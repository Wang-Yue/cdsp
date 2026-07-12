#include "Logging/app_logger.h"
#include "test_support.h"

TEST(AppLoggerLevelFiltering) {
  app_logger_set_level(LOG_LEVEL_WARN);
  ASSERT_EQ(LOG_LEVEL_WARN, app_logger_get_level());

  app_logger_set_level(LOG_LEVEL_INFO);
  ASSERT_EQ(LOG_LEVEL_INFO, app_logger_get_level());
}

TEST(AppLoggerBasicLogging) {
  logger_t log = logger_create("test.logger");
  logger_info(&log, "Hello %s %d %.1f", "world", 42, 3.14);
  logger_warn(&log, "Warning message");
  app_logger_flush_and_stop(app_logger_get_shared());
  ASSERT_TRUE(true);
}

TEST_MAIN()
