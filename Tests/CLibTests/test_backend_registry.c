#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Backend/audio_backend_registry.h"
#include "Backend/backend_error.h"
#include "test_support.h"

TEST(BackendErrorInitializationAndDescription) {
  backend_error_t err;
  backend_error_init(&err, BACKEND_ERROR_DEVICE_NOT_FOUND,
                     "Device not found test");
  ASSERT_EQ(err.type, BACKEND_ERROR_DEVICE_NOT_FOUND);
  ASSERT_STR_EQ(err.message, "Device not found test");

  char buf[512];
  const char* desc = backend_error_description(&err, buf, sizeof(buf));
  ASSERT_TRUE(desc != NULL);
  ASSERT_TRUE(strstr(desc, "Device not found test") != NULL);

  device_error_t dev_err;
  device_error_init(&dev_err, DEVICE_ERROR_BUSY, "Device is busy");
  ASSERT_EQ(dev_err.type, DEVICE_ERROR_BUSY);
  ASSERT_TRUE(dev_err.is_error);

  device_error_clear(&dev_err);
  ASSERT_FALSE(dev_err.is_error);
}

TEST(AudioBackendRegistryProbing) {
  audio_device_t devices[16];
  memset(devices, 0, sizeof(devices));

  // Invalid backend name
  int count = audio_backend_registry_get_available_devices("invalid_backend",
                                                           true, devices, 16);
  ASSERT_TRUE(count <= 0);

  device_error_t dev_err;
  audio_device_descriptor_t* desc =
      audio_backend_registry_get_device_capabilities("invalid_backend", "dev",
                                                     true, &dev_err);
  ASSERT_TRUE(desc == NULL);

  // File/Generator backend probing if available
  count =
      audio_backend_registry_get_available_devices("file", true, devices, 16);
  // File backend may return 0 or list available virtual file endpoints
  ASSERT_TRUE(count >= 0);
}

TEST_MAIN()
