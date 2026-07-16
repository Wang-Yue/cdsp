#if defined(__linux__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif
#include <math.h>
#include <time.h>

#include "DoP/dop_decoder.h"
#include "DoP/dsd_encoder.h"
#include "test_support.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void run_encoder_benchmark(int channels, bool multithreaded) {
  size_t carrier_rate = 768000;
  dsd_encoder_t* encoder = dsd_encoder_create(channels, carrier_rate, DSD_MODE_DOP, 16,
                                              SDM_FILTER_SDM6, 20000.0, multithreaded);
  ASSERT_TRUE(encoder != NULL);
  ASSERT_TRUE(dsd_encoder_is_enabled(encoder));

  int frames = 1024;
  audio_chunk_t* pcm_source = audio_chunk_create(frames, channels);
  double amplitude = 0.5;
  for (int ch = 0; ch < channels; ch++) {
    for (int t = 0; t < frames; t++) {
      audio_chunk_get_channel(pcm_source, ch)[t] =
          amplitude *
          sin(2.0 * M_PI * 1000.0 * (double)t / (double)carrier_rate);
    }
  }

  audio_chunk_t* temp_chunk = audio_chunk_create(frames, channels);

  for (int i = 0; i < 100; i++) {
    for (int ch = 0; ch < channels; ch++) {
      memcpy(audio_chunk_get_channel(temp_chunk, ch),
             audio_chunk_get_channel(pcm_source, ch), frames * sizeof(double));
    }
    dsd_encoder_encode(encoder, temp_chunk);
  }

  int iters = 1000;
  struct timespec start, end;
  clock_gettime(CLOCK_MONOTONIC, &start);
  for (int i = 0; i < iters; i++) {
    for (int ch = 0; ch < channels; ch++) {
      memcpy(audio_chunk_get_channel(temp_chunk, ch),
             audio_chunk_get_channel(pcm_source, ch), frames * sizeof(double));
    }
    dsd_encoder_encode(encoder, temp_chunk);
  }
  clock_gettime(CLOCK_MONOTONIC, &end);
  double elapsed_ns = (double)(end.tv_sec - start.tv_sec) * 1e9 +
                      (double)(end.tv_nsec - start.tv_nsec);
  double ns_per_frame = elapsed_ns / (double)(frames * iters);
  double real_time_ratio = (1.0 / ((double)carrier_rate * 1e-9)) / ns_per_frame;

  printf("=== DSD Encoder Throughput (%d channels, multithreaded=%s) ===\n",
         channels, multithreaded ? "ON" : "OFF");
  printf("Throughput: %8.2f ns/frame\n", ns_per_frame);
  printf("Real-time ratio: %8.2fx\n", real_time_ratio);

  audio_chunk_free(temp_chunk);
  audio_chunk_free(pcm_source);
  dsd_encoder_free(encoder);
}

TEST(DSDEncoder_Benchmark) {
  // Benchmark stereo (always sequential optimized)
  run_encoder_benchmark(2, false);

  // Benchmark 8-channel (sequential vs parallel)
  run_encoder_benchmark(8, false);
  run_encoder_benchmark(8, true);
}

static void run_decoder_benchmark(int channels, bool multithreaded) {
  size_t carrier_rate = 768000;
  dsd_encoder_t* encoder = dsd_encoder_create(channels, carrier_rate, DSD_MODE_DOP, 16,
                                              SDM_FILTER_SDM6, 20000.0, false);
  dop_decoder_t* decoder =
      dop_decoder_create(channels, (double)carrier_rate, false, 20000.0, multithreaded);
  ASSERT_TRUE(encoder != NULL);
  ASSERT_TRUE(dsd_encoder_is_enabled(encoder));
  ASSERT_TRUE(decoder != NULL);

  int frames = 1024;
  audio_chunk_t* pcm_source = audio_chunk_create(frames, channels);
  double amplitude = 0.5;
  for (int ch = 0; ch < channels; ch++) {
    for (int t = 0; t < frames; t++) {
      audio_chunk_get_channel(pcm_source, ch)[t] =
          amplitude * sin(2.0 * M_PI * 1000.0 * (double)t / carrier_rate);
    }
  }

  audio_chunk_t* encoded_source = audio_chunk_create(frames, channels);
  for (int ch = 0; ch < channels; ch++) {
    memcpy(audio_chunk_get_channel(encoded_source, ch),
           audio_chunk_get_channel(pcm_source, ch), frames * sizeof(double));
  }
  dsd_encoder_encode(encoder, encoded_source);

  audio_chunk_t* temp_chunk = audio_chunk_create(frames, channels);

  for (int i = 0; i < 100; i++) {
    for (int ch = 0; ch < channels; ch++) {
      memcpy(audio_chunk_get_channel(temp_chunk, ch),
             audio_chunk_get_channel(encoded_source, ch),
             frames * sizeof(double));
    }
    bool processed = dop_decoder_detect_and_process(decoder, temp_chunk);
    ASSERT_TRUE(processed);
  }
  ASSERT_TRUE(dop_decoder_is_active(decoder));

  int iters = 1000;
  struct timespec start, end;
  clock_gettime(CLOCK_MONOTONIC, &start);
  for (int i = 0; i < iters; i++) {
    for (int ch = 0; ch < channels; ch++) {
      memcpy(audio_chunk_get_channel(temp_chunk, ch),
             audio_chunk_get_channel(encoded_source, ch),
             frames * sizeof(double));
    }
    dop_decoder_detect_and_process(decoder, temp_chunk);
  }
  clock_gettime(CLOCK_MONOTONIC, &end);
  double elapsed_ns = (double)(end.tv_sec - start.tv_sec) * 1e9 +
                      (double)(end.tv_nsec - start.tv_nsec);
  double ns_per_frame = elapsed_ns / (double)(frames * iters);
  double real_time_ratio = (1.0 / (carrier_rate * 1e-9)) / ns_per_frame;

  printf("=== DoP Decoder Throughput (%d channels, multithreaded=%s) ===\n",
         channels, multithreaded ? "ON" : "OFF");
  printf("Throughput: %8.2f ns/frame\n", ns_per_frame);
  printf("Real-time ratio: %8.2fx\n", real_time_ratio);

  audio_chunk_free(temp_chunk);
  audio_chunk_free(encoded_source);
  audio_chunk_free(pcm_source);
  dop_decoder_free(decoder);
  dsd_encoder_free(encoder);
}

TEST(DoPDecoder_Benchmark) {
  // Benchmark stereo (always sequential)
  run_decoder_benchmark(2, false);

  // Benchmark 8-channel (sequential vs parallel)
  run_decoder_benchmark(8, false);
  run_decoder_benchmark(8, true);
}

TEST_MAIN()
