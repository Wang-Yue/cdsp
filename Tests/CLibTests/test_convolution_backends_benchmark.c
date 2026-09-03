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

#include "Config/filter_config_types.h"
#include "FFT/real_fft.h"
#include "Filters/convolution.h"
#include "Filters/filter.h"
#include "test_support.h"

// ----------------------------------------------------------------------------
// Benchmark Helpers & Timers
// ----------------------------------------------------------------------------
static inline void do_not_optimize(void* p) {
#if defined(__GNUC__) || defined(__clang__)
  __asm__ __volatile__("" : : "g"(p) : "memory");
#else
  volatile uint8_t* vp = (volatile uint8_t*)p;
  (void)vp;
#endif
}

static uint64_t get_time_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int compare_doubles(const void* a, const void* b) {
  double da = *(const double*)a;
  double db = *(const double*)b;
  return (da > db) - (da < db);
}

#define NUM_TRIALS 5

static double measure_convolution_ns_per_frame(size_t chunk_size,
                                               const double* coeffs,
                                               size_t taps, size_t iters) {
  convolution_config_t params = {.type = CONV_TYPE_VALUES,
                                 .values = (double*)coeffs,
                                 .values_count = taps};
  filter_config_t cfg = {.type = FILTER_TYPE_CONV, .parameters.conv = params};
  void* filter =
      g_convolution_vtable.create("conv", &cfg, 48000, chunk_size, NULL, NULL);
  if (!filter) return NAN;

  double* buf = (double*)calloc(chunk_size, sizeof(double));
  for (size_t i = 0; i < chunk_size; i++) {
    buf[i] = sin(2.0 * M_PI * 1000.0 * (double)i / 48000.0);
  }

  // Warm-up
  for (size_t i = 0; i < 50; i++) {
    g_convolution_vtable.process(filter, buf, chunk_size);
  }

  double trials[NUM_TRIALS];
  for (int t = 0; t < NUM_TRIALS; t++) {
    uint64_t t0 = get_time_ns();
    for (size_t i = 0; i < iters; i++) {
      g_convolution_vtable.process(filter, buf, chunk_size);
      do_not_optimize(buf);
    }
    uint64_t t1 = get_time_ns();
    trials[t] = (double)(t1 - t0) / (double)(chunk_size * iters);
  }
  free(buf);
  g_convolution_vtable.free(filter);
  qsort(trials, NUM_TRIALS, sizeof(double), compare_doubles);
  return trials[NUM_TRIALS / 2];
}

static double fetch_rust_conv_ns_per_frame(size_t taps, size_t chunk_size,
                                           size_t iters) {
  char args[256];
  snprintf(args, sizeof(args), "bench conv %zu %zu %zu", taps, chunk_size,
           iters);
  return test_run_rust_harness_bench("cdsp_filter_compare", args);
}

// ----------------------------------------------------------------------------
// Test 1: Impulse Response Length (Taps) Sweep Benchmark
// ----------------------------------------------------------------------------
TEST(Convolution_Performance_Taps_Sweep) {
  printf(
      "\n======================================================================"
      "===================================================\n");
  printf(
      " CONVOLUTION PERFORMANCE BENCHMARK: CDSP (FFTW3) vs Rust (RustFFT)\n");
  printf(
      " Fixed Block Size (Chunk Size): 1024 frames (Double Precision f64)\n");
  printf(
      " Monotonic Timer: 5-trial median with warm-up runs and anti-DCE memory "
      "barriers\n");
  printf(
      "========================================================================"
      "=================================================\n");
  printf(
      "   Taps | Segments |   CDSP FFTW3 (f64)  |    Rust (RustFFT) |  CDSP vs "
      "Rust Speedup \n");
  printf(
      "        |          | ns/frame (us/blk)   | ns/frame (us/blk) |        "
      "(ratio)        \n");
  printf(
      "--------+----------+---------------------+-------------------+----------"
      "-------------\n");
  fflush(stdout);

  size_t taps_list[] = {512,   1024,  2048,  4096,  8192,
                        16384, 32768, 65536, 131072};
  size_t num_taps = sizeof(taps_list) / sizeof(taps_list[0]);
  size_t chunk_size = 1024;

  for (size_t i = 0; i < num_taps; i++) {
    size_t taps = taps_list[i];
    size_t segments = (taps + chunk_size - 1) / chunk_size;
    size_t iters = (taps <= 4096) ? 3000 : ((taps <= 32768) ? 1000 : 300);

    double* coeffs = (double*)calloc(taps, sizeof(double));
    for (size_t k = 0; k < taps; k++) {
      coeffs[k] = exp(-3.0 * (double)k / (double)taps) *
                  sin(2.0 * M_PI * 440.0 * (double)k / 48000.0);
    }

    // 1. CDSP FFTW3 backend
    double cdsp_ns =
        measure_convolution_ns_per_frame(chunk_size, coeffs, taps, iters);

    // 2. Rust (RustFFT / CamillaDSP FftConv)
    double rust_ns = fetch_rust_conv_ns_per_frame(taps, chunk_size, iters);

    // Print row
    char cdsp_str[32], rust_str[32], speedup_str[32];
    if (!isnan(cdsp_ns)) {
      snprintf(cdsp_str, sizeof(cdsp_str), "%5.1f (%5.1f us)", cdsp_ns,
               cdsp_ns * chunk_size / 1000.0);
    } else {
      strcpy(cdsp_str, "        N/A    ");
    }

    if (!isnan(rust_ns)) {
      snprintf(rust_str, sizeof(rust_str), "%5.1f (%5.1f us)", rust_ns,
               rust_ns * chunk_size / 1000.0);
    } else {
      strcpy(rust_str, "        N/A    ");
    }

    if (!isnan(cdsp_ns) && !isnan(rust_ns)) {
      snprintf(speedup_str, sizeof(speedup_str), "        %5.2fx        ",
               rust_ns / cdsp_ns);
    } else {
      strcpy(speedup_str, "        N/A          ");
    }

    printf(" %6zu | %8zu | %19s | %17s | %s\n", taps, segments, cdsp_str,
           rust_str, speedup_str);
    fflush(stdout);

    free(coeffs);
  }
  printf(
      "------------------------------------------------------------------------"
      "-------------------------------------------------\n\n");
}

// ----------------------------------------------------------------------------
// Test 2: Chunk / Buffer Size Sweep Benchmark
// ----------------------------------------------------------------------------
TEST(Convolution_Performance_Chunk_Size_Sweep) {
  printf(
      "\n======================================================================"
      "===================================================\n");
  printf(
      " CONVOLUTION BUFFER SIZE / LATENCY SWEEP (Fixed 4,096 Taps IR, Double "
      "Precision f64)\n");
  printf(
      " Compares throughput and per-chunk latency across different block "
      "sizes\n");
  printf(
      "========================================================================"
      "=================================================\n");
  printf(
      " Chunk | Latency @48k |   CDSP FFTW3 (f64)  |    Rust (RustFFT) |  CDSP "
      "vs Rust Speedup \n");
  printf(
      " Size  |     (ms)     | ns/frame (us/blk)   | ns/frame (us/blk) |       "
      " (ratio)        \n");
  printf(
      "-------+--------------+---------------------+-------------------+-------"
      "----------------\n");
  fflush(stdout);

  size_t chunk_sizes[] = {128, 256, 512, 1024, 2048, 4096};
  size_t num_chunks = sizeof(chunk_sizes) / sizeof(chunk_sizes[0]);
  size_t taps = 4096;

  double* coeffs = (double*)calloc(taps, sizeof(double));
  for (size_t k = 0; k < taps; k++) {
    coeffs[k] = exp(-3.0 * (double)k / (double)taps) *
                sin(2.0 * M_PI * 440.0 * (double)k / 48000.0);
  }

  for (size_t i = 0; i < num_chunks; i++) {
    size_t cs = chunk_sizes[i];
    size_t iters = (cs <= 256) ? 8000 : ((cs <= 1024) ? 3000 : 1000);
    double latency_ms = (double)cs / 48000.0 * 1000.0;

    // 1. CDSP FFTW3
    double cdsp_ns = measure_convolution_ns_per_frame(cs, coeffs, taps, iters);

    // 2. Rust (RustFFT)
    double rust_ns = fetch_rust_conv_ns_per_frame(taps, cs, iters);

    char cdsp_str[32], rust_str[32], speedup_str[32];
    if (!isnan(cdsp_ns)) {
      snprintf(cdsp_str, sizeof(cdsp_str), "%5.1f (%5.1f us)", cdsp_ns,
               cdsp_ns * cs / 1000.0);
    } else {
      strcpy(cdsp_str, "        N/A    ");
    }

    if (!isnan(rust_ns)) {
      snprintf(rust_str, sizeof(rust_str), "%5.1f (%5.1f us)", rust_ns,
               rust_ns * cs / 1000.0);
    } else {
      strcpy(rust_str, "        N/A    ");
    }

    if (!isnan(cdsp_ns) && !isnan(rust_ns)) {
      snprintf(speedup_str, sizeof(speedup_str), "        %5.2fx        ",
               rust_ns / cdsp_ns);
    } else {
      strcpy(speedup_str, "        N/A          ");
    }

    printf(" %5zu |    %5.2f ms   | %19s | %17s | %s\n", cs, latency_ms,
           cdsp_str, rust_str, speedup_str);
    fflush(stdout);
  }
  free(coeffs);
  printf(
      "------------------------------------------------------------------------"
      "-------------------------------------------------\n\n");
}

// ----------------------------------------------------------------------------
// Test 3: Pure Real FFT (R2C + C2R Roundtrip) Performance Sweep
// ----------------------------------------------------------------------------
static double measure_realfft_roundtrip_ns(size_t length, size_t iters) {
  real_fft_t* fft = real_fft_create(length, NULL);
  if (!fft) return NAN;

  size_t spec_len = real_fft_get_spectrum_length(fft);
  double* in_real = (double*)calloc(length, sizeof(double));
  complex_t* spec = (complex_t*)calloc(spec_len, sizeof(complex_t));
  double* out_real = (double*)calloc(length, sizeof(double));
  if (!in_real || !spec || !out_real) {
    if (in_real) free(in_real);
    if (spec) free(spec);
    if (out_real) free(out_real);
    real_fft_free(fft);
    return NAN;
  }

  for (size_t i = 0; i < length; i++) {
    in_real[i] = sin(2.0 * M_PI * 1000.0 * (double)i / 48000.0);
  }

  // Warm-up
  for (size_t i = 0; i < 50; i++) {
    real_fft_forward(fft, in_real, spec);
    real_fft_inverse(fft, spec, out_real);
  }

  double trials[NUM_TRIALS];
  for (int t = 0; t < NUM_TRIALS; t++) {
    uint64_t t0 = get_time_ns();
    for (size_t i = 0; i < iters; i++) {
      real_fft_forward(fft, in_real, spec);
      real_fft_inverse(fft, spec, out_real);
      do_not_optimize(out_real);
    }
    uint64_t t1 = get_time_ns();
    trials[t] = (double)(t1 - t0) / (double)iters;
  }

  free(in_real);
  free(spec);
  free(out_real);
  real_fft_free(fft);

  qsort(trials, NUM_TRIALS, sizeof(double), compare_doubles);
  return trials[NUM_TRIALS / 2];
}

static double fetch_rust_realfft_ns(size_t length, size_t iters) {
  char args[256];
  snprintf(args, sizeof(args), "bench realfft %zu %zu", length, iters);
  return test_run_rust_harness_bench("cdsp_filter_compare", args);
}

TEST(RealFFT_PowerOfTwo_Performance_Sweep) {
  printf(
      "\n======================================================================"
      "===================================================\n");
  printf(
      " REAL FFT (POWER-OF-TWO) ROUNDTRIP BENCHMARK: CDSP (FFTW3) vs Rust "
      "(realfft/RustFFT)\n");
  printf(
      " R2C Forward FFT + C2R Inverse FFT Roundtrip (Double Precision f64)\n");
  printf(
      " Monotonic Timer: 5-trial median with warm-up runs and anti-DCE memory "
      "barriers\n");
  printf(
      "========================================================================"
      "=================================================\n");
  printf(
      " Length |   CDSP FFTW3 (f64)  |    Rust (realfft)   |  CDSP vs Rust "
      "Speedup \n");
  printf(
      " (pts)  |  us/roundtrip (ns)  |  us/roundtrip (ns)  |        (ratio)    "
      "    \n");
  printf(
      "--------+---------------------+---------------------+-------------------"
      "----\n");
  fflush(stdout);

  size_t lengths[] = {128,  256,   512,   1024,  2048,  4096,
                      8192, 16384, 32768, 65536, 131072};
  size_t num_lengths = sizeof(lengths) / sizeof(lengths[0]);

  for (size_t i = 0; i < num_lengths; i++) {
    size_t len = lengths[i];
    size_t iters = (len <= 1024)
                       ? 8000
                       : ((len <= 8192) ? 3000 : ((len <= 32768) ? 1000 : 300));

    // 1. CDSP FFTW3
    double cdsp_ns = measure_realfft_roundtrip_ns(len, iters);

    // 2. Rust realfft / RustFFT
    double rust_ns = fetch_rust_realfft_ns(len, iters);

    char cdsp_str[32], rust_str[32], speedup_str[32];
    if (!isnan(cdsp_ns)) {
      snprintf(cdsp_str, sizeof(cdsp_str), "%6.2f us (%5.0f ns)",
               cdsp_ns / 1000.0, cdsp_ns);
    } else {
      strcpy(cdsp_str, "        N/A    ");
    }

    if (!isnan(rust_ns)) {
      snprintf(rust_str, sizeof(rust_str), "%6.2f us (%5.0f ns)",
               rust_ns / 1000.0, rust_ns);
    } else {
      strcpy(rust_str, "        N/A    ");
    }

    if (!isnan(cdsp_ns) && !isnan(rust_ns)) {
      snprintf(speedup_str, sizeof(speedup_str), "        %5.2fx        ",
               rust_ns / cdsp_ns);
    } else {
      strcpy(speedup_str, "        N/A          ");
    }

    printf(" %6zu | %19s | %19s | %s\n", len, cdsp_str, rust_str, speedup_str);
    fflush(stdout);
  }
  printf(
      "------------------------------------------------------------------------"
      "-------------------------------------------------\n\n");
}

// ----------------------------------------------------------------------------
// Test 4: Synchronous Resampler Non-Power-of-Two Real FFT Sweep
// ----------------------------------------------------------------------------
typedef struct {
  size_t length;
  const char* context;
} resampler_fft_entry_t;

TEST(RealFFT_Resampler_NonPowerOfTwo_Sweep) {
  printf(
      "\n======================================================================"
      "===================================================\n");
  printf(
      " REAL FFT (NON-POWER-OF-TWO RESAMPLER) BENCHMARK: CDSP (FFTW3) vs Rust "
      "(realfft/RustFFT)\n");
  printf(
      " Critical composite/prime-factored FFT lengths used in Synchronous "
      "Resampling (f64)\n");
  printf(
      " Monotonic Timer: 5-trial median with warm-up runs and anti-DCE memory "
      "barriers\n");
  printf(
      "========================================================================"
      "=================================================\n");
  printf(
      " Length | Resampler Context / Sample Rates |  CDSP FFTW3 (f64)   |   "
      "Rust (realfft)    | Speedup (ratio) \n");
  printf(
      "--------+----------------------------------+---------------------+------"
      "-"
      "--------------+-----------------\n");
  fflush(stdout);

  resampler_fft_entry_t entries[] = {
      {294, "44.1k <-> 48k/96k  (2x147 base in) "},
      {320, "44.1k <-> 48k      (2x160 base out)"},
      {588, "44.1k <-> 48k      (4x147 subchunk)"},
      {640, "44.1k <-> 96k/192k (4x160 base out)"},
      {882, "32k   <-> 44.1k    (2x441 base out)"},
      {960, "44.1k <-> 48k      (6x160 subchunk)"},
      {1176, "44.1k <-> 48k      (8x147 subchunk)"},
      {1280, "44.1k <-> 48k      (8x160 subchunk)"},
      {1764, "32k   <-> 44.1k    (4x441 subchunk)"},
      {2058, "44.1k <-> 48k (1024-blk in: 2x1029)"},
      {2240, "44.1k <-> 48k (1024-blk out:2x1120)"},
      {3528, "32k   <-> 44.1k    (8x441 subchunk)"},
      {4116, "44.1k <-> 48k (2048-blk in: 2x2058)"},
      {4480, "44.1k <-> 48k (2048-blk out:2x2240)"},
  };
  size_t num_entries = sizeof(entries) / sizeof(entries[0]);

  for (size_t i = 0; i < num_entries; i++) {
    size_t len = entries[i].length;
    size_t iters = (len <= 1024) ? 5000 : 2000;

    // 1. CDSP FFTW3
    double cdsp_ns = measure_realfft_roundtrip_ns(len, iters);

    // 2. Rust realfft / RustFFT
    double rust_ns = fetch_rust_realfft_ns(len, iters);

    char cdsp_str[32], rust_str[32], speedup_str[32];
    if (!isnan(cdsp_ns)) {
      snprintf(cdsp_str, sizeof(cdsp_str), "%6.2f us (%5.0f ns)",
               cdsp_ns / 1000.0, cdsp_ns);
    } else {
      strcpy(cdsp_str, "        N/A    ");
    }

    if (!isnan(rust_ns)) {
      snprintf(rust_str, sizeof(rust_str), "%6.2f us (%5.0f ns)",
               rust_ns / 1000.0, rust_ns);
    } else {
      strcpy(rust_str, "        N/A    ");
    }

    if (!isnan(cdsp_ns) && !isnan(rust_ns)) {
      snprintf(speedup_str, sizeof(speedup_str), "     %5.2fx     ",
               rust_ns / cdsp_ns);
    } else {
      strcpy(speedup_str, "      N/A       ");
    }

    printf(" %6zu | %-32s | %19s | %19s | %s\n", len, entries[i].context,
           cdsp_str, rust_str, speedup_str);
    fflush(stdout);
  }
  printf(
      "------------------------------------------------------------------------"
      "-------------------------------------------------\n\n");
}

TEST_MAIN()
