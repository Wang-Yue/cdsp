#include "backend/playback_buffer.h"

void playback_buffer_init(playback_buffer_t *pb, double sample_rate) {
  if (!pb)
    return;
  device_buffer_estimator_init(&pb->device, sample_rate);
  atomic_init(&pb->target_level, 0);
  atomic_init(&pb->silence_to_insert, 0);
  atomic_init(&pb->is_running, true);
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

void playback_buffer_set_target_level(playback_buffer_t *pb,
                                      size_t target_level) {
  if (!pb)
    return;
  atomic_store_explicit(&pb->target_level, target_level, memory_order_release);
}

void playback_buffer_prefill_planar(playback_buffer_t *pb,
                                    spsc_planar_ring_buffer_t *ring,
                                    size_t frames) {
  if (!pb)
    return;
  atomic_store_explicit(&pb->target_level, frames, memory_order_release);
  atomic_store_explicit(&pb->silence_to_insert, 0, memory_order_release);
  atomic_store_explicit(&pb->is_running, true, memory_order_release);
  if (ring && frames > 0) {
    spsc_planar_ring_buffer_write_silence(ring, frames);
  }
}

void playback_buffer_prefill_byte(playback_buffer_t *pb,
                                  spsc_byte_ring_buffer_t *ring, size_t frames,
                                  size_t blockalign, uint8_t silence_byte) {
  if (!pb)
    return;
  atomic_store_explicit(&pb->target_level, frames, memory_order_release);
  atomic_store_explicit(&pb->silence_to_insert, 0, memory_order_release);
  atomic_store_explicit(&pb->is_running, true, memory_order_release);
  if (ring && frames > 0 && blockalign > 0) {
    spsc_byte_ring_buffer_write_silence(ring, frames * blockalign,
                                        silence_byte);
  }
}

size_t playback_buffer_render_planar(playback_buffer_t *pb,
                                     spsc_planar_ring_buffer_t *ring,
                                     void *const *dst_channels, size_t frames,
                                     uint8_t silence_byte) {
  if (!pb || !ring || !dst_channels || frames == 0)
    return 0;

  if (!atomic_load_explicit(&pb->is_running, memory_order_relaxed)) {
    size_t avail = spsc_planar_ring_buffer_get_available_to_read(ring);
    if (avail > 0) {
      atomic_store_explicit(&pb->is_running, true, memory_order_relaxed);
      atomic_store_explicit(
          &pb->silence_to_insert,
          atomic_load_explicit(&pb->target_level, memory_order_relaxed),
          memory_order_relaxed);
    }
  }

  size_t consumed = spsc_planar_ring_buffer_read_with_silence(
      ring, dst_channels, frames, silence_byte, &pb->silence_to_insert,
      &pb->is_running);

  playback_buffer_publish(
      pb, atomic_load_explicit(&pb->silence_to_insert, memory_order_relaxed));
  return consumed;
}

size_t playback_buffer_render_byte(playback_buffer_t *pb,
                                   spsc_byte_ring_buffer_t *ring, void *dst,
                                   size_t frames, size_t blockalign,
                                   uint8_t silence_byte) {
  if (!pb || !ring || !dst || frames == 0 || blockalign == 0)
    return 0;

  if (!atomic_load_explicit(&pb->is_running, memory_order_relaxed)) {
    size_t avail_bytes = spsc_byte_ring_buffer_get_available_to_read(ring);
    size_t avail_frames = avail_bytes / blockalign;
    if (avail_frames > 0) {
      atomic_store_explicit(&pb->is_running, true, memory_order_relaxed);
      atomic_store_explicit(
          &pb->silence_to_insert,
          atomic_load_explicit(&pb->target_level, memory_order_relaxed),
          memory_order_relaxed);
    }
  }

  size_t consumed = spsc_byte_ring_buffer_read_with_silence(
      ring, dst, frames, blockalign, silence_byte, &pb->silence_to_insert,
      &pb->is_running);

  playback_buffer_publish(
      pb, atomic_load_explicit(&pb->silence_to_insert, memory_order_relaxed));
  return consumed;
}
