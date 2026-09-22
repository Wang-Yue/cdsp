#include "utils/device_buffer_estimator.h"

#include "utils/cdsp_time.h"

void device_buffer_estimator_init(device_buffer_estimator_t *est,
                                  double sample_rate) {
  if (!est)
    return;
  atomic_init(&est->update_time_ns, 0);
  atomic_init(&est->frames, 0);
  atomic_init(&est->sample_rate, sample_rate > 0.0 ? sample_rate : 0.0);
}

void device_buffer_estimator_reset(device_buffer_estimator_t *est) {
  if (!est)
    return;
  // Clear the timestamp first: a reader that samples between these two stores
  // sees "never published" and reports 0, rather than pairing a stale
  // timestamp with a cleared frame count.
  atomic_store_explicit(&est->update_time_ns, 0, memory_order_release);
  atomic_store_explicit(&est->frames, 0, memory_order_relaxed);
}

void device_buffer_estimator_set_rate(device_buffer_estimator_t *est,
                                      double sample_rate) {
  if (!est)
    return;
  atomic_store_explicit(&est->sample_rate,
                        sample_rate > 0.0 ? sample_rate : 0.0,
                        memory_order_relaxed);
  // The stored level was measured against the previous rate.
  device_buffer_estimator_reset(est);
}

void device_buffer_estimator_add(device_buffer_estimator_t *est,
                                 size_t frames) {
  if (!est)
    return;
  atomic_store_explicit(&est->frames, frames, memory_order_relaxed);
  // Published last, with release ordering, so a reader that observes this
  // timestamp also observes the frame count stored above.
  atomic_store_explicit(&est->update_time_ns, cdsp_time_now_ns(),
                        memory_order_release);
}

size_t device_buffer_estimator_estimate(const device_buffer_estimator_t *est) {
  if (!est)
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
