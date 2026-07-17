#ifndef CTESTS_TEST_SUPPORT_H
#define CTESTS_TEST_SUPPORT_H

#include <complex.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include <stdarg.h>

static inline int custom_snprintf(char* str, size_t size, const char* format,
                                  ...) {
  va_list args;
  va_start(args, format);
  int ret;
  if (strncmp(format, "/tmp/", 5) == 0) {
    char temp[512];
    ret = vsnprintf(temp, sizeof(temp), format, args);
    if (ret >= 0 && ret < (int)sizeof(temp)) {
      char* dot = strrchr(temp, '.');
      if (dot && strcmp(dot, ".raw") == 0) {
        *dot = '\0';
        ret = (snprintf)(str, size, "%s_%d.raw", temp, getpid());
      } else if (dot && strcmp(dot, ".yaml") == 0) {
        *dot = '\0';
        ret = (snprintf)(str, size, "%s_%d.yaml", temp, getpid());
      } else {
        ret = (snprintf)(str, size, "%s_%d", temp, getpid());
      }
    }
  } else {
    ret = vsnprintf(str, size, format, args);
  }
  va_end(args);
  return ret;
}

#ifdef snprintf
#undef snprintf
#endif
#define snprintf(buf, size, fmt, ...) \
  custom_snprintf(buf, size, fmt, ##__VA_ARGS__)

typedef void (*test_func_t)(void);
typedef struct test_entry {
  const char* name;
  test_func_t func;
  struct test_entry* next;
} test_entry_t;

#ifndef CDSP_COMBINED_TEST_SUITE
static int g_test_count = 0;
static int g_test_failures = 0;
static test_entry_t* g_test_head = NULL;
static test_entry_t** g_test_tail = &g_test_head;
#else
extern int g_test_count;
extern int g_test_failures;
extern test_entry_t* g_test_head;
extern test_entry_t** g_test_tail;
#endif

static inline void register_test(const char* name, test_func_t func) {
  test_entry_t* entry = (test_entry_t*)malloc(sizeof(test_entry_t));
  if (!entry) return;
  entry->name = name;
  entry->func = func;
  entry->next = NULL;
  *g_test_tail = entry;
  g_test_tail = &entry->next;
}

#define TEST(name)                                                       \
  static void test_##name(void);                                         \
  __attribute__((constructor, used)) static void register_##name(void) { \
    register_test(#name, test_##name);                                   \
  }                                                                      \
  static void test_##name(void)

#define RUN_TEST(name)                \
  do {                                \
    printf("Running %s...\n", #name); \
    g_test_count++;                   \
    test_##name();                    \
  } while (0)

#define ASSERT_TRUE(cond)                                                    \
  do {                                                                       \
    if (!(cond)) {                                                           \
      printf("  [FAIL] %s:%d: Assertion '%s' failed.\n", __FILE__, __LINE__, \
             #cond);                                                         \
      g_test_failures++;                                                     \
      return;                                                                \
    }                                                                        \
  } while (0)

#define ASSERT_FALSE(cond)                                                    \
  do {                                                                        \
    if (cond) {                                                               \
      printf("  [FAIL] %s:%d: Assertion '!%s' failed.\n", __FILE__, __LINE__, \
             #cond);                                                          \
      g_test_failures++;                                                      \
      return;                                                                 \
    }                                                                         \
  } while (0)

#define ASSERT_EQ(exp, act)                                             \
  do {                                                                  \
    if ((exp) != (act)) {                                               \
      printf("  [FAIL] %s:%d: Expected %s == %s (got %lld vs %lld).\n", \
             __FILE__, __LINE__, #exp, #act, (long long)(exp),          \
             (long long)(act));                                         \
      g_test_failures++;                                                \
      return;                                                           \
    }                                                                   \
  } while (0)

#define ASSERT_NE(exp, act)                                                    \
  do {                                                                         \
    if ((exp) == (act)) {                                                      \
      printf("  [FAIL] %s:%d: Expected %s != %s.\n", __FILE__, __LINE__, #exp, \
             #act);                                                            \
      g_test_failures++;                                                       \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define ASSERT_NEAR(exp, act, eps)                                           \
  do {                                                                       \
    double _e = (double)(exp);                                               \
    double _a = (double)(act);                                               \
    double _diff = fabs(_e - _a);                                            \
    if (_diff > (double)(eps)) {                                             \
      printf(                                                                \
          "  [FAIL] %s:%d: Expected %s (~%g) and %s (~%g) to be within %g, " \
          "diff is %g.\n",                                                   \
          __FILE__, __LINE__, #exp, _e, #act, _a, (double)(eps), _diff);     \
      g_test_failures++;                                                     \
      return;                                                                \
    }                                                                        \
  } while (0)

#define ASSERT_FLOAT_EQ(exp, act) ASSERT_NEAR(exp, act, 1e-5)
#define ASSERT_DOUBLE_EQ(exp, act) ASSERT_NEAR(exp, act, 1e-9)

#define ASSERT_STR_EQ(exp, act)                                           \
  do {                                                                    \
    const char* _e = (exp);                                               \
    const char* _a = (act);                                               \
    if (_e == NULL || _a == NULL || strcmp(_e, _a) != 0) {                \
      printf("  [FAIL] %s:%d: Expected string '%s' == '%s'.\n", __FILE__, \
             __LINE__, _e ? _e : "NULL", _a ? _a : "NULL");               \
      g_test_failures++;                                                  \
      return;                                                             \
    }                                                                     \
  } while (0)

#ifndef CDSP_COMBINED_TEST_SUITE
#define TEST_MAIN()                                                         \
  int main(int argc, char* argv[]) {                                        \
    if (argc > 1 && strcmp(argv[1], "--list") == 0) {                       \
      test_entry_t* curr = g_test_head;                                     \
      while (curr) {                                                        \
        printf("%s\n", curr->name);                                         \
        curr = curr->next;                                                  \
      }                                                                     \
      return 0;                                                             \
    }                                                                       \
    const char* run_only = NULL;                                            \
    if (argc > 2 && strcmp(argv[1], "--run") == 0) {                        \
      run_only = argv[2];                                                   \
    } else if (argc > 1) {                                                  \
      run_only = argv[1];                                                   \
    }                                                                       \
    if (run_only) {                                                         \
      test_entry_t* curr = g_test_head;                                     \
      while (curr) {                                                        \
        if (strcmp(curr->name, run_only) == 0) {                            \
          curr->func();                                                     \
          return g_test_failures > 0 ? 1 : 0;                               \
        }                                                                   \
        curr = curr->next;                                                  \
      }                                                                     \
      fprintf(stderr, "Test '%s' not found\n", run_only);                   \
      return 2;                                                             \
    }                                                                       \
    printf("\n=== Running Registered C Tests ===\n\n");                     \
    test_entry_t* curr = g_test_head;                                       \
    while (curr) {                                                          \
      printf("Running %s...\n", curr->name);                                \
      g_test_count++;                                                       \
      curr->func();                                                         \
      test_entry_t* next = curr->next;                                      \
      free(curr);                                                           \
      curr = next;                                                          \
    }                                                                       \
    printf("\n=== Test Results: %d run, %d failures ===\n\n", g_test_count, \
           g_test_failures);                                                \
    return g_test_failures > 0 ? 1 : 0;                                     \
  }
#else
#define TEST_MAIN()
#endif

#endif  // CTESTS_TEST_SUPPORT_H
