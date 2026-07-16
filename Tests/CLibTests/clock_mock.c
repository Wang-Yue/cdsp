#define _GNU_SOURCE
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>

#ifdef __APPLE__
#include <dlfcn.h>
#include <sys/time.h>
extern uint64_t clock_gettime_nsec_np(clockid_t clock_id);

static int real_clock_gettime(clockid_t clock_id, struct timespec* tp) {
  typedef int (*clock_gettime_t)(clockid_t, struct timespec*);
  static clock_gettime_t f = NULL;
  if (!f) f = (clock_gettime_t)dlsym(RTLD_NEXT, "clock_gettime");
  return f(clock_id, tp);
}

static int real_nanosleep(const struct timespec* req, struct timespec* rem) {
  typedef int (*nanosleep_t)(const struct timespec*, struct timespec*);
  static nanosleep_t f = NULL;
  if (!f) f = (nanosleep_t)dlsym(RTLD_NEXT, "nanosleep");
  return f(req, rem);
}

static uint64_t real_clock_gettime_nsec_np(clockid_t clock_id) {
  typedef uint64_t (*clock_gettime_nsec_np_t)(clockid_t);
  static clock_gettime_nsec_np_t f = NULL;
  if (!f)
    f = (clock_gettime_nsec_np_t)dlsym(RTLD_NEXT, "clock_gettime_nsec_np");
  return f(clock_id);
}
#else
extern int __real_clock_gettime(clockid_t clock_id, struct timespec* tp);
extern int __real_nanosleep(const struct timespec* req, struct timespec* rem);

static int real_clock_gettime(clockid_t clock_id, struct timespec* tp) {
  return __real_clock_gettime(clock_id, tp);
}

static int real_nanosleep(const struct timespec* req, struct timespec* rem) {
  return __real_nanosleep(req, rem);
}
#endif

static _Atomic uint64_t s_real_start_time_ns = 0;
static _Atomic uint64_t s_mock_start_time_ns = 0;

static void init_clock_mock(void) {
  if (atomic_load(&s_real_start_time_ns) != 0) return;

  struct timespec ts = {0};
  real_clock_gettime(CLOCK_MONOTONIC, &ts);
  uint64_t now_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;

  uint64_t expected = 0;
  if (atomic_compare_exchange_strong(&s_real_start_time_ns, &expected,
                                     now_ns)) {
    atomic_store(&s_mock_start_time_ns, now_ns);
  }
}

int cdsp_clock_gettime(clockid_t clock_id, struct timespec* tp) {
  if (atomic_load(&s_real_start_time_ns) == 0) {
    init_clock_mock();
  }

  struct timespec real_ts = {0};
  int ret = real_clock_gettime(clock_id, &real_ts);
  if (ret != 0) return ret;

  uint64_t real_now =
      (uint64_t)real_ts.tv_sec * 1000000000ULL + real_ts.tv_nsec;
  uint64_t elapsed = real_now - atomic_load(&s_real_start_time_ns);
  uint64_t fake_now = atomic_load(&s_mock_start_time_ns) + elapsed * 15;

  tp->tv_sec = (time_t)(fake_now / 1000000000ULL);
  tp->tv_nsec = (long)(fake_now % 1000000000ULL);
  return 0;
}

int cdsp_nanosleep(const struct timespec* req, struct timespec* rem) {
  uint64_t req_ns = (uint64_t)req->tv_sec * 1000000000ULL + req->tv_nsec;
  uint64_t scaled_ns = req_ns / 15;
  struct timespec scaled_req = {.tv_sec = (time_t)(scaled_ns / 1000000000ULL),
                                .tv_nsec = (long)(scaled_ns % 1000000000ULL)};
  return real_nanosleep(&scaled_req, rem);
}

#ifdef __APPLE__
uint64_t cdsp_clock_gettime_nsec_np(clockid_t clock_id) {
  if (atomic_load(&s_real_start_time_ns) == 0) {
    init_clock_mock();
  }
  uint64_t real_now = real_clock_gettime_nsec_np(clock_id);
  uint64_t elapsed = real_now - atomic_load(&s_real_start_time_ns);
  return atomic_load(&s_mock_start_time_ns) + elapsed * 15;
}

// macOS Symbol Interposition prototypes
int clock_gettime(clockid_t clock_id, struct timespec* tp) {
  return cdsp_clock_gettime(clock_id, tp);
}

int nanosleep(const struct timespec* req, struct timespec* rem) {
  return cdsp_nanosleep(req, rem);
}

uint64_t clock_gettime_nsec_np(clockid_t clock_id) {
  return cdsp_clock_gettime_nsec_np(clock_id);
}
#else
// Linux/Windows Linker Wrapping entry points
int __wrap_clock_gettime(clockid_t clock_id, struct timespec* tp) {
  return cdsp_clock_gettime(clock_id, tp);
}

int __wrap_nanosleep(const struct timespec* req, struct timespec* rem) {
  return cdsp_nanosleep(req, rem);
}
#endif
