#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Logging/app_logger.h"
#include "Utils/double_helpers.h"
#include "test_support.h"

int g_test_count = 0;
int g_test_failures = 0;
test_entry_t* g_test_head = NULL;
test_entry_t** g_test_tail = &g_test_head;

#if defined(_WIN32)
FILE* __real_fopen(const char* filename, const char* mode);

FILE* __wrap_fopen(const char* filename, const char* mode) {
  if (filename && (strcmp(filename, "/dev/null") == 0 ||
                   strcmp(filename, "\\dev\\null") == 0)) {
    return __real_fopen("NUL", mode);
  }
  if (filename && (strncmp(filename, "/tmp/", 5) == 0 ||
                   strncmp(filename, "\\tmp\\", 5) == 0)) {
    char win_path[512];
    const char* temp_env = getenv("TEMP");
    if (!temp_env) temp_env = getenv("TMP");
    if (temp_env) {
      char clean_temp[512];
      strncpy(clean_temp, temp_env, sizeof(clean_temp) - 1);
      clean_temp[sizeof(clean_temp) - 1] = '\0';
      for (int i = 0; clean_temp[i] != '\0'; i++) {
        if (clean_temp[i] == '\\') clean_temp[i] = '/';
      }
      snprintf(win_path, sizeof(win_path), "%s/%s", clean_temp, filename + 5);
    } else {
      snprintf(win_path, sizeof(win_path), "./%s", filename + 5);
    }
    return __real_fopen(win_path, mode);
  }
  return __real_fopen(filename, mode);
}
#endif

int main(int argc, char* argv[]) {
#if defined(ENABLE_ACCELERATE)
  // Warm up Accelerate CBLAS thread-local dispatch state before running tests
  double dummy_d[4096] = {0};
  dsp_ops_scalar_multiply(dummy_d, 1.0, 4096);
  dsp_ops_multiply_add(dummy_d, 1.0, dummy_d, 4096);
#endif

  if (argc > 1 && strcmp(argv[1], "--list") == 0) {
    test_entry_t* curr = g_test_head;
    while (curr) {
      printf("%s\n", curr->name);
      curr = curr->next;
    }
    return 0;
  }
  const char* run_only = NULL;
  if (argc > 2 && strcmp(argv[1], "--run") == 0) {
    run_only = argv[2];
  } else if (argc > 1) {
    run_only = argv[1];
  }
  if (run_only) {
    test_entry_t* curr = g_test_head;
    while (curr) {
      if (strcmp(curr->name, run_only) == 0) {
        curr->func();
        app_logger_flush_and_stop(app_logger_get_shared());
        return g_test_failures > 0 ? 1 : 0;
      }
      curr = curr->next;
    }
    fprintf(stderr, "Test '%s' not found\n", run_only);
    return 2;
  }
  printf("\n=== Running Registered C Tests ===\n\n");
  test_entry_t* curr = g_test_head;
  while (curr) {
    printf("Running %s...\n", curr->name);
    g_test_count++;
    curr->func();
    test_entry_t* next = curr->next;
    free(curr);
    curr = next;
  }
  printf("\n=== Test Results: %d run, %d failures ===\n\n", g_test_count,
         g_test_failures);
  app_logger_flush_and_stop(app_logger_get_shared());
  return g_test_failures > 0 ? 1 : 0;
}
