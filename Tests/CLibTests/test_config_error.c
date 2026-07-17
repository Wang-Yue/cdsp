#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Config/config_error.h"
#include "test_support.h"

TEST(ConfigErrorInitializationAndFormatting) {
  config_error_t err;
  config_error_init(&err);
  ASSERT_EQ(err.type, CONFIG_ERR_NONE);
  ASSERT_EQ(strlen(err.message), 0);

  config_error_set(&err, CONFIG_ERR_INVALID_FILTER,
                   "Filter frequency out of range");
  ASSERT_EQ(err.type, CONFIG_ERR_INVALID_FILTER);
  ASSERT_STR_EQ(err.message, "Filter frequency out of range");

  char buf[512];
  config_error_description(&err, buf, sizeof(buf));
  ASSERT_STR_EQ(buf, "Invalid filter: Filter frequency out of range");

  config_error_init(&err);
  ASSERT_EQ(err.type, CONFIG_ERR_NONE);
  config_error_description(&err, buf, sizeof(buf));
  ASSERT_STR_EQ(buf, "");
}

TEST_MAIN()
