#include "test_support.h"

int g_test_count = 0;
int g_test_failures = 0;
test_entry_t* g_test_head = NULL;
test_entry_t** g_test_tail = &g_test_head;

int main(int argc, char* argv[]) {
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
  return g_test_failures > 0 ? 1 : 0;
}
