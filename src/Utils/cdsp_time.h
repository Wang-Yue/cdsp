#ifndef CDSP_TIME_H
#define CDSP_TIME_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Get high-resolution monotonic timestamp in nanoseconds.
 *
 * Monotonic, non-decreasing clock representation.
 * - Under macOS/Darwin: Uses `clock_gettime_nsec_np(CLOCK_UPTIME_RAW)` or
 * `mach_absolute_time()`.
 * - Under Linux/POSIX: Uses `clock_gettime(CLOCK_MONOTONIC, &ts)` converted to
 * nanoseconds.
 * - Under Windows: Uses `QueryPerformanceCounter`.
 *
 * @return Current timestamp in nanoseconds.
 */
uint64_t cdsp_time_now_ns(void);

/**
 * @brief Sleep for specified number of milliseconds.
 *
 * Cross-platform helper unifying `Sleep` (Windows) and `nanosleep` / `usleep`
 * (POSIX/macOS).
 *
 * @param ms Duration in milliseconds.
 */
void cdsp_sleep_ms(uint32_t ms);

/**
 * @brief Sleep for specified number of microseconds.
 *
 * Cross-platform helper unifying sub-millisecond sleep implementation across
 * OSes.
 *
 * @param us Duration in microseconds.
 */
void cdsp_sleep_us(uint64_t us);

#ifdef __cplusplus
}
#endif

#endif  // CDSP_TIME_H
