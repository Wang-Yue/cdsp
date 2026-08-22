#include <string.h>

#include "Config/log_level.h"
#include "Logging/app_logger.h"
#include "test_support.h"

TEST(AppLoggerLevelFiltering) {
  app_logger_set_level(LOG_LEVEL_WARN);
  ASSERT_EQ(LOG_LEVEL_WARN, app_logger_get_level());

  app_logger_set_level(LOG_LEVEL_INFO);
  ASSERT_EQ(LOG_LEVEL_INFO, app_logger_get_level());
}

static int g_test_callback_count = 0;
static log_level_t g_last_level = LOG_LEVEL_OFF;
static char g_last_label[64] = {0};
static char g_last_message[256] = {0};

static void test_log_callback(log_level_t level, const char* label,
                              const char* message, void* user_data) {
  int* counter = (int*)user_data;
  if (counter) (*counter)++;
  g_last_level = level;
  if (label) {
    strncpy(g_last_label, label, sizeof(g_last_label) - 1);
  }
  if (message) {
    strncpy(g_last_message, message, sizeof(g_last_message) - 1);
  }
}

TEST(AppLoggerCallback) {
  g_test_callback_count = 0;
  g_last_level = LOG_LEVEL_OFF;
  memset(g_last_label, 0, sizeof(g_last_label));
  memset(g_last_message, 0, sizeof(g_last_message));

  app_logger_set_level(LOG_LEVEL_DEBUG);
  app_logger_set_callback(test_log_callback, &g_test_callback_count);

  logger_t log = logger_create("test.callback");
  logger_debug(&log, "Testing callback with value %d", 123);

  // Flush background logger and join worker thread to guarantee message
  // processed
  app_logger_flush_and_stop(app_logger_get_shared());

  ASSERT_TRUE(g_test_callback_count >= 1);
  ASSERT_EQ(LOG_LEVEL_DEBUG, g_last_level);
  ASSERT_TRUE(strstr(g_last_label, "test.callback") != NULL);
  ASSERT_TRUE(strstr(g_last_message, "Testing callback with value 123") !=
              NULL);

  // Reset callback
  app_logger_set_callback(NULL, NULL);
}

TEST_MAIN()
