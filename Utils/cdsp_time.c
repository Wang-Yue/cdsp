#include "Utils/cdsp_time.h"

#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif
#endif

uint64_t cdsp_time_now_ns(void) {
#if defined(__APPLE__)
#if defined(CLOCK_UPTIME_RAW)
  return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
  static mach_timebase_info_data_t s_timebase_info;
  if (s_timebase_info.denom == 0) {
    mach_timebase_info(&s_timebase_info);
  }
  uint64_t mach_now = mach_absolute_time();
  return mach_now * s_timebase_info.numer / s_timebase_info.denom;
#endif
#elif defined(_WIN32)
  static LARGE_INTEGER freq;
  static BOOL has_freq = FALSE;
  if (!has_freq) {
    QueryPerformanceFrequency(&freq);
    has_freq = TRUE;
  }
  LARGE_INTEGER counter;
  QueryPerformanceCounter(&counter);
  return (uint64_t)((counter.QuadPart * 1000000000ULL) / freq.QuadPart);
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

void cdsp_sleep_ms(uint32_t ms) {
#if defined(_WIN32)
  Sleep(ms);
#else
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (uint64_t)(ms % 1000) * 1000000ULL;
  nanosleep(&ts, NULL);
#endif
}

void cdsp_sleep_us(uint64_t us) {
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
  struct timespec ts;
  ts.tv_sec = us / 1000000ULL;
  ts.tv_nsec = (us % 1000000ULL) * 1000ULL;
  nanosleep(&ts, NULL);
#endif
}
