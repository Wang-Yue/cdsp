#ifndef CLIB_ENGINE_CDSP_SEM_H
#define CLIB_ENGINE_CDSP_SEM_H

/**
 * @file cdsp_sem.h
 * @brief Cross-platform, low-overhead OS kernel semaphore abstraction
 * (cdsp_sem_t).
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __APPLE__
#include <dispatch/dispatch.h>

/**
 * @brief Platform-specific semaphore handle wrapper (macOS Grand Central
 * Dispatch).
 */
typedef dispatch_semaphore_t cdsp_sem_t;

/**
 * @brief Creates a platform-specific semaphore wrapper initialized to value 0.
 * @return The initialized semaphore handle, or NULL on failure.
 */
static inline cdsp_sem_t cdsp_sem_create(void) {
  return dispatch_semaphore_create(0);
}

/**
 * @brief Destroys the semaphore.
 * @param sem The semaphore handle to destroy.
 */
static inline void cdsp_sem_destroy(cdsp_sem_t sem) {
  if (sem) {
    while (dispatch_semaphore_wait(sem, DISPATCH_TIME_NOW) == 0);
    dispatch_release(sem);
  }
}

/**
 * @brief Signals the semaphore to wake waiting threads.
 * @param sem The semaphore handle.
 */
static inline void cdsp_sem_signal(cdsp_sem_t sem) {
  if (sem) dispatch_semaphore_signal(sem);
}

/**
 * @brief Waits indefinitely on the semaphore.
 * @param sem The semaphore handle.
 */
static inline void cdsp_sem_wait(cdsp_sem_t sem) {
  if (sem) dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
}

/**
 * @brief Waits on the semaphore up to a specified timeout in milliseconds.
 * @param sem The semaphore handle.
 * @param timeout_ms Timeout duration in milliseconds.
 * @return true if signaled before timeout, false on timeout or error.
 */
static inline bool cdsp_sem_timedwait(cdsp_sem_t sem, uint32_t timeout_ms) {
  if (!sem) return false;
  dispatch_time_t timeout =
      dispatch_time(DISPATCH_TIME_NOW, (int64_t)timeout_ms * 1000000LL);
  return dispatch_semaphore_wait(sem, timeout) == 0;
}

#elif defined(__linux__)
#include <semaphore.h>
#include <stdlib.h>
#include <time.h>

typedef sem_t* cdsp_sem_t;

static inline cdsp_sem_t cdsp_sem_create(void) {
  sem_t* sem = (sem_t*)calloc(1, sizeof(sem_t));
  if (!sem) return NULL;
  if (sem_init(sem, 0, 0) != 0) {
    free(sem);
    return NULL;
  }
  return sem;
}

static inline void cdsp_sem_destroy(cdsp_sem_t sem) {
  if (sem) {
    sem_destroy(sem);
    free(sem);
  }
}

#include <errno.h>

static inline void cdsp_sem_signal(cdsp_sem_t sem) {
  if (sem) sem_post(sem);
}

static inline void cdsp_sem_wait(cdsp_sem_t sem) {
  if (!sem) return;
  int res;
  do {
    res = sem_wait(sem);
  } while (res == -1 && errno == EINTR);
}

static inline bool cdsp_sem_timedwait(cdsp_sem_t sem, uint32_t timeout_ms) {
  if (!sem) return false;
  struct timespec ts = {0};
  if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return false;
  uint64_t nsec = (uint64_t)ts.tv_nsec + (uint64_t)timeout_ms * 1000000ULL;
  ts.tv_sec += nsec / 1000000000ULL;
  ts.tv_nsec = nsec % 1000000000ULL;
  int res;
  do {
    res = sem_timedwait(sem, &ts);
  } while (res == -1 && errno == EINTR);
  return res == 0;
}

#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef HANDLE cdsp_sem_t;

static inline cdsp_sem_t cdsp_sem_create(void) {
  return CreateSemaphore(NULL, 0, 2147483647L, NULL);
}

static inline void cdsp_sem_destroy(cdsp_sem_t sem) {
  if (sem) CloseHandle(sem);
}

static inline void cdsp_sem_signal(cdsp_sem_t sem) {
  if (sem) ReleaseSemaphore(sem, 1, NULL);
}

static inline void cdsp_sem_wait(cdsp_sem_t sem) {
  if (sem) WaitForSingleObject(sem, INFINITE);
}

static inline bool cdsp_sem_timedwait(cdsp_sem_t sem, uint32_t timeout_ms) {
  if (!sem) return false;
  return WaitForSingleObject(sem, (DWORD)timeout_ms) == WAIT_OBJECT_0;
}

#endif

#endif  // CLIB_ENGINE_CDSP_SEM_H
