#include "backend/playback_buffer.h"

void playback_buffer_init(playback_buffer_t *pb, double sample_rate) {
  if (!pb)
    return;
  device_buffer_estimator_init(&pb->device, sample_rate);
}

void playback_buffer_set_rate(playback_buffer_t *pb, double sample_rate) {
  if (!pb)
    return;
  device_buffer_estimator_set_rate(&pb->device, sample_rate);
}

void playback_buffer_reset(playback_buffer_t *pb) {
  if (!pb)
    return;
  device_buffer_estimator_reset(&pb->device);
}

void playback_buffer_publish(playback_buffer_t *pb, size_t device_frames) {
  if (!pb)
    return;
  device_buffer_estimator_add(&pb->device, device_frames);
}

size_t playback_buffer_level(const playback_buffer_t *pb,
                             const spsc_byte_ring_buffer_t *ring,
                             size_t blockalign) {
  size_t ring_frames = 0;
  if (ring && blockalign > 0) {
    ring_frames =
        spsc_byte_ring_buffer_get_available_to_read(ring) / blockalign;
  }
  return ring_frames +
         device_buffer_estimator_estimate(pb ? &pb->device : NULL);
}

size_t playback_buffer_planar_level(const playback_buffer_t *pb,
                                    const spsc_planar_ring_buffer_t *ring) {
  size_t ring_frames = 0;
  if (ring) {
    ring_frames = spsc_planar_ring_buffer_get_available_to_read(ring);
  }
  return ring_frames +
         device_buffer_estimator_estimate(pb ? &pb->device : NULL);
}
