#include <stdint.h>
#include <time.h>

#ifdef __APPLE__
#include <sys/time.h>
extern uint64_t clock_gettime_nsec_np(clockid_t clock_id);
#endif

static uint64_t s_real_start_time_ns = 0;
static uint64_t s_mock_start_time_ns = 0;

static void init_clock_mock(void) {
  if (s_real_start_time_ns != 0) return;

  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  s_real_start_time_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
  s_mock_start_time_ns = s_real_start_time_ns;
}

int cdsp_clock_gettime(clockid_t clock_id, struct timespec* tp) {
  if (s_real_start_time_ns == 0) {
    init_clock_mock();
  }

  struct timespec real_ts;
  int ret = clock_gettime(clock_id, &real_ts);
  if (ret != 0) return ret;

  uint64_t real_now =
      (uint64_t)real_ts.tv_sec * 1000000000ULL + real_ts.tv_nsec;
  uint64_t elapsed = real_now - s_real_start_time_ns;
  uint64_t fake_now = s_mock_start_time_ns + elapsed * 15;

  tp->tv_sec = (time_t)(fake_now / 1000000000ULL);
  tp->tv_nsec = (long)(fake_now % 1000000000ULL);
  return 0;
}

uint64_t cdsp_clock_gettime_nsec_np(clockid_t clock_id) {
#ifdef __APPLE__
  if (s_real_start_time_ns == 0) {
    init_clock_mock();
  }
  uint64_t real_now = clock_gettime_nsec_np(clock_id);
  uint64_t elapsed = real_now - s_real_start_time_ns;
  return s_mock_start_time_ns + elapsed * 15;
#else
  (void)clock_id;
  struct timespec ts;
  cdsp_clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
#endif
}

int cdsp_nanosleep(const struct timespec* req, struct timespec* rem) {
  uint64_t req_ns = (uint64_t)req->tv_sec * 1000000000ULL + req->tv_nsec;
  uint64_t scaled_ns = req_ns / 15;
  struct timespec scaled_req = {.tv_sec = (time_t)(scaled_ns / 1000000000ULL),
                                .tv_nsec = (long)(scaled_ns % 1000000000ULL)};
  return nanosleep(&scaled_req, rem);
}
