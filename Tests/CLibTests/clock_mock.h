#ifndef CLIB_TESTS_CLOCK_MOCK_H
#define CLIB_TESTS_CLOCK_MOCK_H

#include <stdint.h>
#include <time.h>

#ifdef CDSP_TEST
// Undefine any potential macros first
#undef clock_gettime
#undef nanosleep

// Map to mock implementations
#define clock_gettime cdsp_clock_gettime

#ifdef __APPLE__
#undef clock_gettime_nsec_np
#define clock_gettime_nsec_np cdsp_clock_gettime_nsec_np
uint64_t cdsp_clock_gettime_nsec_np(clockid_t clock_id);
#endif

#ifdef CDSP_TEST_MOCK_NANOSLEEP
#undef nanosleep
#define nanosleep cdsp_nanosleep
#endif

int cdsp_clock_gettime(clockid_t clock_id, struct timespec* tp);
int cdsp_nanosleep(const struct timespec* req, struct timespec* rem);

#undef cdsp_time_now_ns
#define cdsp_time_now_ns mock_cdsp_time_now_ns
#undef cdsp_sleep_ms
#define cdsp_sleep_ms mock_cdsp_sleep_ms
#undef cdsp_sleep_us
#define cdsp_sleep_us mock_cdsp_sleep_us

uint64_t mock_cdsp_time_now_ns(void);
void mock_cdsp_sleep_ms(uint32_t ms);
void mock_cdsp_sleep_us(uint64_t us);
#endif

#endif  // CLIB_TESTS_CLOCK_MOCK_H
