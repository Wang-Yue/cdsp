#include "Utils/cdsp_time.h"

#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

static inline uint64_t cdsp_time_now_ns_internal(void) {
#if defined(__APPLE__)
  return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#elif defined(_WIN32)
  static LARGE_INTEGER freq;
  static BOOL has_freq = FALSE;
  if (!has_freq) {
    QueryPerformanceFrequency(&freq);
    has_freq = TRUE;
  }
  LARGE_INTEGER counter;
  QueryPerformanceCounter(&counter);
  uint64_t seconds = (uint64_t)(counter.QuadPart / freq.QuadPart);
  uint64_t fraction = (uint64_t)(counter.QuadPart % freq.QuadPart);
  return seconds * 1000000000ULL + (fraction * 1000000000ULL) / freq.QuadPart;
#else
  struct timespec ts = {0};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

static inline void cdsp_sleep_ms_internal(uint32_t ms) {
#if defined(_WIN32)
  Sleep(ms);
#else
  struct timespec ts = {0};
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (uint64_t)(ms % 1000) * 1000000ULL;
  nanosleep(&ts, NULL);
#endif
}

static inline void cdsp_sleep_us_internal(uint64_t us) {
#if defined(_WIN32)
  LARGE_INTEGER freq, start, now;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&start);
  uint64_t target_ticks = (us * freq.QuadPart) / 1000000ULL;
  if (us >= 1000) {
    Sleep((DWORD)(us / 1000));
  }
  do {
    QueryPerformanceCounter(&now);
  } while ((uint64_t)(now.QuadPart - start.QuadPart) < target_ticks);
#else
  struct timespec ts = {0};
  ts.tv_sec = us / 1000000ULL;
  ts.tv_nsec = (us % 1000000ULL) * 1000ULL;
  nanosleep(&ts, NULL);
#endif
}

#ifdef CDSP_TEST
#include <stdlib.h>

static uint64_t get_time_scale(void) {
  const char* env = getenv("CDSP_TIME_SCALE");
  if (env && env[0] != '\0') {
    int scale = atoi(env);
    if (scale > 0) return (uint64_t)scale;
  }
  return 15;
}
#endif

uint64_t cdsp_time_now_ns(void) {
#ifdef CDSP_TEST
  return cdsp_time_now_ns_internal() * get_time_scale();
#else
  return cdsp_time_now_ns_internal();
#endif
}

void cdsp_sleep_ms(uint32_t ms) {
#ifdef CDSP_TEST
  uint64_t scale = get_time_scale();
  uint32_t scaled_ms = (uint32_t)(ms / scale);
  if (scaled_ms == 0 && ms > 0) scaled_ms = 1;
  cdsp_sleep_ms_internal(scaled_ms);
#else
  cdsp_sleep_ms_internal(ms);
#endif
}

void cdsp_sleep_us(uint64_t us) {
#ifdef CDSP_TEST
  uint64_t scale = get_time_scale();
  uint64_t scaled_us = us / scale;
  if (scaled_us == 0 && us > 0) scaled_us = 1;
  cdsp_sleep_us_internal(scaled_us);
#else
  cdsp_sleep_us_internal(us);
#endif
}
