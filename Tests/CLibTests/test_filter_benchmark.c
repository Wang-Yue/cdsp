#if defined(__linux__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "Filters/biquad.h"
#include "Filters/convolution.h"
#include "Filters/diffeq.h"
#include "test_support.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define CHUNK_SIZE 1024
#define SAMPLE_RATE 48000
#define NBR_FRAMES (16 * CHUNK_SIZE)

static double fetch_rust_filter_benchmark(const char* rust_name) {
  const char* home = getenv("HOME");
  if (!home) return NAN;
  char cmd[1024];
  snprintf(cmd, sizeof(cmd),
           "cd %s/camilladsp && RAYON_NUM_THREADS=1 cargo bench --bench "
           "filters -- %s --sample-size "
           "10 --warm-up-time 0.1 --measurement-time 0.2 2>&1",
           home, rust_name);
  FILE* fp = popen(cmd, "r");
  if (!fp) return NAN;
  char line[1024];
  double val_ns_per_frame = NAN;
  while (fgets(line, sizeof(line), fp)) {
    if (strstr(line, "time:")) {
      for (char* p = line; *p; p++) {
        if (*p == '[' || *p == ']') *p = ' ';
      }
      char name[128] = {0};
      char time_lbl[32] = {0};
      double val1 = 0, val2 = 0, val3 = 0;
      char unit[32] = {0};
      int count = sscanf(line, "%127s %31s %lf %31s %lf %31s %lf %31s", name,
                         time_lbl, &val1, unit, &val2, unit, &val3, unit);
      if (count >= 8 && strcmp(time_lbl, "time:") == 0) {
        double val_ns = val2;
        if (strcmp(unit, "µs") == 0 || strstr(unit, "u")) {
          val_ns = val2 * 1000.0;
        } else if (strcmp(unit, "ms") == 0) {
          val_ns = val2 * 1000000.0;
        }
        val_ns_per_frame = val_ns / 1024.0;
      }
    }
  }
  pclose(fp);
  return val_ns_per_frame;
}

static void run_filter_benchmark(const char* label, const char* rust_name,
                                 void* filter,
                                 void (*process_fn)(void*, double*, size_t)) {
  printf("Running %s_Benchmark...\n", label);
  fflush(stdout);

  double* buffer = (double*)calloc(CHUNK_SIZE, sizeof(double));

  // Warm-up
  for (int i = 0; i < 100; i++) {
    process_fn(filter, buffer, CHUNK_SIZE);
  }

  int iters = 5000;
  struct timespec start, end_time;
  clock_gettime(CLOCK_MONOTONIC, &start);
  for (int i = 0; i < iters; i++) {
    process_fn(filter, buffer, CHUNK_SIZE);
  }
  clock_gettime(CLOCK_MONOTONIC, &end_time);

  double elapsed_ns = (double)(end_time.tv_sec - start.tv_sec) * 1e9 +
                      (double)(end_time.tv_nsec - start.tv_nsec);
  double c_ns_per_frame = elapsed_ns / (double)(CHUNK_SIZE * iters);

  double cdsp_ns_per_frame = fetch_rust_filter_benchmark(rust_name);

  printf("\n==================================================\n");
  printf("Filter Benchmark: %s\n", label);
  printf("--------------------------------------------------\n");
  printf("Engine                   |        ns/frame\n");
  printf("--------------------------------------------------\n");
  printf("C %-22s | %15.1f\n", label, c_ns_per_frame);
  if (!isnan(cdsp_ns_per_frame)) {
    printf("CamillaDSP (Rust)        | %15.1f\n", cdsp_ns_per_frame);
  } else {
    printf("CamillaDSP (Rust)        |             N/A\n");
  }
  printf("--------------------------------------------------\n");
  if (!isnan(cdsp_ns_per_frame)) {
    printf("Relative Speedup        : %14.2fx\n",
           cdsp_ns_per_frame / c_ns_per_frame);
  }
  printf("==================================================\n\n");
  fflush(stdout);

  free(buffer);
}

static void process_conv(void* f, double* w, size_t n) {
  convolution_filter_process((convolution_filter_t*)f, w, n);
}

static void process_biquad(void* f, double* w, size_t n) {
  biquad_filter_process((biquad_filter_t*)f, w, n);
}

static void process_diffeq(void* f, double* w, size_t n) {
  diffeq_filter_process((diffeq_filter_t*)f, w, n);
}

TEST(Convolution_1024_Benchmark) {
  double* coeffs = (double*)calloc(1024, sizeof(double));
  convolution_config_t params = {
      .type = CONV_TYPE_VALUES, .values = coeffs, .values_count = 1024};
  convolution_filter_t* f =
      convolution_filter_create("conv-1024", &params, CHUNK_SIZE, NULL);
  run_filter_benchmark("FftConv_1024", "Conv/FftConv/1024", f, process_conv);
  convolution_filter_free(f);
  free(coeffs);
}

TEST(Convolution_4096_Benchmark) {
  double* coeffs = (double*)calloc(4096, sizeof(double));
  convolution_config_t params = {
      .type = CONV_TYPE_VALUES, .values = coeffs, .values_count = 4096};
  convolution_filter_t* f =
      convolution_filter_create("conv-4096", &params, CHUNK_SIZE, NULL);
  run_filter_benchmark("FftConv_4096", "Conv/FftConv/4096", f, process_conv);
  convolution_filter_free(f);
  free(coeffs);
}

TEST(Convolution_16384_Benchmark) {
  double* coeffs = (double*)calloc(16384, sizeof(double));
  convolution_config_t params = {
      .type = CONV_TYPE_VALUES, .values = coeffs, .values_count = 16384};
  convolution_filter_t* f =
      convolution_filter_create("conv-16384", &params, CHUNK_SIZE, NULL);
  run_filter_benchmark("FftConv_16384", "Conv/FftConv/16384", f, process_conv);
  convolution_filter_free(f);
  free(coeffs);
}

TEST(Biquad_Benchmark) {
  biquad_config_t params = {.type = BIQUAD_TYPE_FREE,
                                .b0 = 0.21476322779271284,
                                .b1 = 0.4295264555854257,
                                .b2 = 0.21476322779271284,
                                .a1 = -0.1462978543780541,
                                .a2 = 0.005350765548905586};
  biquad_filter_t* f = biquad_filter_create("biquad", &params, 44100, NULL);
  run_filter_benchmark("Biquad", "Biquad", f, process_biquad);
  biquad_filter_free(f);
}

TEST(DiffEq_Benchmark) {
  double a[] = {1.0, -0.1462978543780541, 0.005350765548905586};
  double b[] = {0.21476322779271284, 0.4295264555854257, 0.21476322779271284};
  diffeq_config_t params = {.a = a, .a_count = 3, .b = b, .b_count = 3};
  diffeq_filter_t* f = diffeq_filter_create("diffeq", &params, NULL);
  run_filter_benchmark("DiffEq", "DiffEq", f, process_diffeq);
  diffeq_filter_free(f);
}

TEST_MAIN()
