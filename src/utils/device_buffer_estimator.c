#include "utils/device_buffer_estimator.h"

#include "utils/cdsp_time.h"

#define MIN_MAX_AGE_FLOOR_NS 20000000ULL

static uint64_t compute_min_max_age_ns(double sample_rate, size_t chunk_size) {
  uint64_t age_ns = MIN_MAX_AGE_FLOOR_NS;
  if (sample_rate > 0.0 && chunk_size > 0) {
    double two_chunks_ns = (2.0 * (double)chunk_size / sample_rate) * 1e9;
    if (two_chunks_ns > (double)age_ns) {
      age_ns = (uint64_t)two_chunks_ns;
    }
  }
  return age_ns;
}

void device_buffer_estimator_init(device_buffer_estimator_t *est,
                                  double sample_rate) {
  if (!est)
    return;
  double rate = sample_rate > 0.0 ? sample_rate : 0.0;
  atomic_init(&est->frames, 0);
  atomic_init(&est->sample_rate, rate);
  atomic_init(&est->chunk_size, 0);
  atomic_init(&est->min_max_age_ns, compute_min_max_age_ns(rate, 0));
  atomic_init(&est->valid, true);
  for (size_t i = 0; i < DEVICE_BUFFER_ESTIMATOR_INTERVALS; i++) {
    atomic_init(&est->intervals_ns[i], 0);
  }
  est->interval_index = 0;
  atomic_init(&est->update_time_ns, cdsp_time_now_ns());
}

void device_buffer_estimator_reset(device_buffer_estimator_t *est) {
  if (!est)
    return;
  atomic_store_explicit(&est->frames, 0, memory_order_relaxed);
  for (size_t i = 0; i < DEVICE_BUFFER_ESTIMATOR_INTERVALS; i++) {
    atomic_store_explicit(&est->intervals_ns[i], 0, memory_order_relaxed);
  }
  est->interval_index = 0;
  atomic_store_explicit(&est->valid, true, memory_order_relaxed);
  atomic_store_explicit(&est->update_time_ns, cdsp_time_now_ns(),
                        memory_order_release);
}

void device_buffer_estimator_set_rate(device_buffer_estimator_t *est,
                                      double sample_rate) {
  if (!est)
    return;
  double rate = sample_rate > 0.0 ? sample_rate : 0.0;
  atomic_store_explicit(&est->sample_rate, rate, memory_order_relaxed);
  size_t cs = atomic_load_explicit(&est->chunk_size, memory_order_relaxed);
  atomic_store_explicit(&est->min_max_age_ns, compute_min_max_age_ns(rate, cs),
                        memory_order_relaxed);
  // The stored level was measured against the previous rate.
  device_buffer_estimator_reset(est);
}

void device_buffer_estimator_set_chunk_size(device_buffer_estimator_t *est,
                                            size_t chunk_size) {
  if (!est)
    return;
  atomic_store_explicit(&est->chunk_size, chunk_size, memory_order_relaxed);
  double rate = atomic_load_explicit(&est->sample_rate, memory_order_relaxed);
  atomic_store_explicit(&est->min_max_age_ns,
                        compute_min_max_age_ns(rate, chunk_size),
                        memory_order_relaxed);
}

void device_buffer_estimator_invalidate(device_buffer_estimator_t *est) {
  if (!est)
    return;
  atomic_store_explicit(&est->valid, false, memory_order_release);
}

void device_buffer_estimator_add(device_buffer_estimator_t *est,
                                 size_t frames) {
  if (!est)
    return;
  uint64_t now_ns = cdsp_time_now_ns();
  uint64_t prev_ns =
      atomic_load_explicit(&est->update_time_ns, memory_order_relaxed);
  uint64_t dt_ns = (now_ns > prev_ns && prev_ns > 0) ? (now_ns - prev_ns) : 0;
  size_t idx = est->interval_index % DEVICE_BUFFER_ESTIMATOR_INTERVALS;
  atomic_store_explicit(&est->intervals_ns[idx], dt_ns, memory_order_relaxed);
  est->interval_index = (idx + 1) % DEVICE_BUFFER_ESTIMATOR_INTERVALS;
  atomic_store_explicit(&est->frames, frames, memory_order_relaxed);
  atomic_store_explicit(&est->valid, true, memory_order_relaxed);
  // Published last, with release ordering, so a reader that observes this
  // timestamp also observes the frame count and validity stored above.
  atomic_store_explicit(&est->update_time_ns, now_ns, memory_order_release);
}

static uint64_t
device_buffer_estimator_max_age_ns(const device_buffer_estimator_t *est) {
  uint64_t sorted[DEVICE_BUFFER_ESTIMATOR_INTERVALS];
  for (size_t i = 0; i < DEVICE_BUFFER_ESTIMATOR_INTERVALS; i++) {
    sorted[i] =
        atomic_load_explicit(&est->intervals_ns[i], memory_order_relaxed);
    uint64_t key = sorted[i];
    size_t j = i;
    while (j > 0 && sorted[j - 1] > key) {
      sorted[j] = sorted[j - 1];
      j--;
    }
    sorted[j] = key;
  }
  uint64_t two_median = 2 * sorted[DEVICE_BUFFER_ESTIMATOR_INTERVALS / 2];
  uint64_t min_max =
      atomic_load_explicit(&est->min_max_age_ns, memory_order_relaxed);
  return two_median > min_max ? two_median : min_max;
}

device_buffer_estimate_status_t
device_buffer_estimator_checked_estimate(const device_buffer_estimator_t *est,
                                         size_t *out_frames) {
  if (out_frames)
    *out_frames = 0;
  if (!est)
    return DEVICE_BUFFER_ESTIMATE_INVALID;
  if (!atomic_load_explicit(&est->valid, memory_order_acquire))
    return DEVICE_BUFFER_ESTIMATE_INVALID;

  uint64_t updated_at_ns =
      atomic_load_explicit(&est->update_time_ns, memory_order_acquire);
  uint64_t now_ns = cdsp_time_now_ns();
  uint64_t elapsed_ns =
      (now_ns > updated_at_ns) ? (now_ns - updated_at_ns) : 0;
  if (out_frames)
    *out_frames = device_buffer_estimator_estimate(est);
  if (elapsed_ns > device_buffer_estimator_max_age_ns(est))
    return DEVICE_BUFFER_ESTIMATE_STALE;
  return DEVICE_BUFFER_ESTIMATE_OK;
}

size_t device_buffer_estimator_estimate(const device_buffer_estimator_t *est) {
  if (!est)
    return 0;
  if (!atomic_load_explicit(&est->valid, memory_order_acquire))
    return 0;

  uint64_t updated_at_ns =
      atomic_load_explicit(&est->update_time_ns, memory_order_acquire);
  if (updated_at_ns == 0)
    return 0;

  size_t frames = atomic_load_explicit(&est->frames, memory_order_relaxed);
  if (frames == 0)
    return 0;

  double sample_rate =
      atomic_load_explicit(&est->sample_rate, memory_order_relaxed);
  if (sample_rate <= 0.0)
    return frames;

  uint64_t now_ns = cdsp_time_now_ns();
  if (now_ns <= updated_at_ns)
    return frames;

  double elapsed_s = (double)(now_ns - updated_at_ns) * 1e-9;
  double frames_consumed = sample_rate * elapsed_s;
  if (frames_consumed >= (double)frames)
    return 0;

  return frames - (size_t)frames_consumed;
}
