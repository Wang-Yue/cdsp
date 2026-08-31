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

#include "FFT/pure_real_fft.h"
#include "FFT/real_fft_backend.h"
#include "Filters/convolution.h"
#include "Filters/filter.h"
#include "test_support.h"

#if defined(ENABLE_ACCELERATE)
#include <Accelerate/Accelerate.h>

#include "FFT/vdsp_real_fft.h"
#endif

#if defined(HAS_FFTW) || defined(__has_include)
#if __has_include(<fftw3.h>)
#define HAS_FFTW_HEADER 1
#include <complex.h>
#include <fftw3.h>
#endif
#endif

// Vector type for SIMD complex MAC
typedef double double4 __attribute__((vector_size(32), aligned(8)));

// ----------------------------------------------------------------------------
// FFTW3 Real FFT Backend Implementation
// ----------------------------------------------------------------------------
#if defined(HAS_FFTW_HEADER)
typedef struct {
  real_fft_backend_t base;
  size_t length;
  size_t spectrum_length;
  double* in_real;
  fftw_complex* out_complex;
  fftw_plan plan_forward;
  fftw_plan plan_inverse;
} fftw_bench_ctx_t;

static void fftw_bench_forward(void* ctx, waveform_t real_in,
                               mutable_waveform_t spec_re,
                               mutable_waveform_t spec_im) {
  fftw_bench_ctx_t* fft = (fftw_bench_ctx_t*)ctx;
  memcpy(fft->in_real, real_in, fft->length * sizeof(double));
  fftw_execute(fft->plan_forward);
  for (size_t i = 0; i < fft->spectrum_length; i++) {
    spec_re[i] = __real__(fft->out_complex[i]);
    spec_im[i] = __imag__(fft->out_complex[i]);
  }
}

static void fftw_bench_inverse(void* ctx, waveform_t spec_re,
                               waveform_t spec_im,
                               mutable_waveform_t real_out) {
  fftw_bench_ctx_t* fft = (fftw_bench_ctx_t*)ctx;
  for (size_t i = 0; i < fft->spectrum_length; i++) {
    __real__(fft->out_complex[i]) = spec_re[i];
    __imag__(fft->out_complex[i]) = spec_im[i];
  }
  fftw_execute(fft->plan_inverse);
  memcpy(real_out, fft->in_real, fft->length * sizeof(double));
}

static void fftw_bench_free(void* ctx) {
  fftw_bench_ctx_t* fft = (fftw_bench_ctx_t*)ctx;
  if (!fft) return;
  if (fft->plan_forward) fftw_destroy_plan(fft->plan_forward);
  if (fft->plan_inverse) fftw_destroy_plan(fft->plan_inverse);
  if (fft->in_real) fftw_free(fft->in_real);
  if (fft->out_complex) fftw_free(fft->out_complex);
  free(fft);
}

static real_fft_backend_t* fftw_backend_create(size_t length, unsigned flags) {
  if (length == 0 || length % 2 != 0) return NULL;
  fftw_bench_ctx_t* ctx =
      (fftw_bench_ctx_t*)calloc(1, sizeof(fftw_bench_ctx_t));
  if (!ctx) return NULL;
  ctx->length = length;
  ctx->spectrum_length = length / 2 + 1;
  ctx->in_real = (double*)fftw_malloc(length * sizeof(double));
  ctx->out_complex =
      (fftw_complex*)fftw_malloc(ctx->spectrum_length * sizeof(fftw_complex));
  if (!ctx->in_real || !ctx->out_complex) {
    fftw_bench_free(ctx);
    return NULL;
  }
  ctx->plan_forward =
      fftw_plan_dft_r2c_1d((int)length, ctx->in_real, ctx->out_complex, flags);
  ctx->plan_inverse =
      fftw_plan_dft_c2r_1d((int)length, ctx->out_complex, ctx->in_real, flags);
  if (!ctx->plan_forward || !ctx->plan_inverse) {
    fftw_bench_free(ctx);
    return NULL;
  }
  ctx->base.ctx = ctx;
  ctx->base.forward = fftw_bench_forward;
  ctx->base.inverse = fftw_bench_inverse;
  ctx->base.free = fftw_bench_free;
  return &ctx->base;
}
#endif

typedef struct {
  size_t chunk_size;
  size_t fft_len;
  size_t spec_len;
  size_t num_segments;
  real_fft_backend_t* backend;
  double** spec_re;
  double** spec_im;
  double** hist_re;
  double** hist_im;
  size_t write_idx;
  double* overlap_buffer;
  double* time_buf;
  double* spec_accum_re;
  double* spec_accum_im;
} bench_conv_engine_t;

static void bench_conv_free(bench_conv_engine_t* engine) {
  if (!engine) return;
  if (engine->spec_re) {
    for (size_t s = 0; s < engine->num_segments; s++) {
      if (engine->spec_re[s]) free(engine->spec_re[s]);
    }
    free(engine->spec_re);
  }
  if (engine->spec_im) {
    for (size_t s = 0; s < engine->num_segments; s++) {
      if (engine->spec_im[s]) free(engine->spec_im[s]);
    }
    free(engine->spec_im);
  }
  if (engine->hist_re) {
    for (size_t s = 0; s < engine->num_segments; s++) {
      if (engine->hist_re[s]) free(engine->hist_re[s]);
    }
    free(engine->hist_re);
  }
  if (engine->hist_im) {
    for (size_t s = 0; s < engine->num_segments; s++) {
      if (engine->hist_im[s]) free(engine->hist_im[s]);
    }
    free(engine->hist_im);
  }
  if (engine->overlap_buffer) free(engine->overlap_buffer);
  if (engine->time_buf) free(engine->time_buf);
  if (engine->spec_accum_re) free(engine->spec_accum_re);
  if (engine->spec_accum_im) free(engine->spec_accum_im);
  free(engine);
}

static bench_conv_engine_t* bench_conv_create(real_fft_backend_t* backend,
                                              size_t chunk_size,
                                              const double* coeffs,
                                              size_t coeffs_count) {
  if (!backend || chunk_size == 0 || !coeffs || coeffs_count == 0) return NULL;
  bench_conv_engine_t* engine =
      (bench_conv_engine_t*)calloc(1, sizeof(bench_conv_engine_t));
  if (!engine) return NULL;

  engine->backend = backend;
  engine->chunk_size = chunk_size;
  engine->fft_len = 2 * chunk_size;
  engine->spec_len = chunk_size + 1;
  engine->num_segments = (coeffs_count + chunk_size - 1) / chunk_size;

  size_t num_seg = engine->num_segments;
  size_t spec_len = engine->spec_len;
  size_t fft_len = engine->fft_len;

  engine->spec_re = (double**)calloc(num_seg, sizeof(double*));
  engine->spec_im = (double**)calloc(num_seg, sizeof(double*));
  engine->hist_re = (double**)calloc(num_seg, sizeof(double*));
  engine->hist_im = (double**)calloc(num_seg, sizeof(double*));

  if (!engine->spec_re || !engine->spec_im || !engine->hist_re ||
      !engine->hist_im) {
    bench_conv_free(engine);
    return NULL;
  }

  double* scratch = (double*)calloc(fft_len, sizeof(double));
  if (!scratch) {
    bench_conv_free(engine);
    return NULL;
  }
  double inv_scale = 1.0 / (double)fft_len;

  for (size_t s = 0; s < num_seg; s++) {
    engine->spec_re[s] = (double*)calloc(spec_len, sizeof(double));
    engine->spec_im[s] = (double*)calloc(spec_len, sizeof(double));
    engine->hist_re[s] = (double*)calloc(spec_len, sizeof(double));
    engine->hist_im[s] = (double*)calloc(spec_len, sizeof(double));

    if (!engine->spec_re[s] || !engine->spec_im[s] || !engine->hist_re[s] ||
        !engine->hist_im[s]) {
      free(scratch);
      bench_conv_free(engine);
      return NULL;
    }

    memset(scratch, 0, fft_len * sizeof(double));
    size_t offset = s * chunk_size;
    size_t copy_len = (coeffs_count > offset) ? (coeffs_count - offset) : 0;
    if (copy_len > chunk_size) copy_len = chunk_size;
    for (size_t k = 0; k < copy_len; k++) {
      scratch[k] = coeffs[offset + k] * inv_scale;
    }
    backend->forward(backend->ctx, scratch, engine->spec_re[s],
                     engine->spec_im[s]);
  }
  free(scratch);

  engine->write_idx = 0;
  engine->overlap_buffer = (double*)calloc(chunk_size, sizeof(double));
  engine->time_buf = (double*)calloc(fft_len, sizeof(double));
  engine->spec_accum_re = (double*)calloc(spec_len, sizeof(double));
  engine->spec_accum_im = (double*)calloc(spec_len, sizeof(double));

  if (!engine->overlap_buffer || !engine->time_buf || !engine->spec_accum_re ||
      !engine->spec_accum_im) {
    bench_conv_free(engine);
    return NULL;
  }

  return engine;
}

static void bench_conv_process(bench_conv_engine_t* engine, double* waveform) {
  if (!engine || engine->num_segments == 0) return;
  size_t cs = engine->chunk_size;
  size_t spec_len = engine->spec_len;
  size_t num_seg = engine->num_segments;
  size_t widx = engine->write_idx;
  real_fft_backend_t* backend = engine->backend;

  // 1. Stage new block + zero-pad
  memcpy(engine->time_buf, waveform, cs * sizeof(double));
  memset(engine->time_buf + cs, 0, cs * sizeof(double));

  // 2. Forward FFT directly into history buffers
  backend->forward(backend->ctx, engine->time_buf, engine->hist_re[widx],
                   engine->hist_im[widx]);

  // 3. Spectrum-domain multiply-accumulate across segment history
  memset(engine->spec_accum_re, 0, spec_len * sizeof(double));
  memset(engine->spec_accum_im, 0, spec_len * sizeof(double));

  for (size_t s = 0; s < num_seg; s++) {
    size_t hidx = (widx + num_seg - s) % num_seg;
    const double* hre = engine->hist_re[hidx];
    const double* him = engine->hist_im[hidx];
    const double* sre = engine->spec_re[s];
    const double* sim = engine->spec_im[s];
    double* acc_re = engine->spec_accum_re;
    double* acc_im = engine->spec_accum_im;

    size_t vec_len = (spec_len / 16) * 16;
    for (size_t k = 0; k < vec_len; k += 16) {
      double4 h_re0 = *(const double4*)&hre[k];
      double4 h_im0 = *(const double4*)&him[k];
      double4 s_re0 = *(const double4*)&sre[k];
      double4 s_im0 = *(const double4*)&sim[k];
      double4 a_re0 = *(const double4*)&acc_re[k];
      double4 a_im0 = *(const double4*)&acc_im[k];

      double4 h_re1 = *(const double4*)&hre[k + 4];
      double4 h_im1 = *(const double4*)&him[k + 4];
      double4 s_re1 = *(const double4*)&sre[k + 4];
      double4 s_im1 = *(const double4*)&sim[k + 4];
      double4 a_re1 = *(const double4*)&acc_re[k + 4];
      double4 a_im1 = *(const double4*)&acc_im[k + 4];

      double4 h_re2 = *(const double4*)&hre[k + 8];
      double4 h_im2 = *(const double4*)&him[k + 8];
      double4 s_re2 = *(const double4*)&sre[k + 8];
      double4 s_im2 = *(const double4*)&sim[k + 8];
      double4 a_re2 = *(const double4*)&acc_re[k + 8];
      double4 a_im2 = *(const double4*)&acc_im[k + 8];

      double4 h_re3 = *(const double4*)&hre[k + 12];
      double4 h_im3 = *(const double4*)&him[k + 12];
      double4 s_re3 = *(const double4*)&sre[k + 12];
      double4 s_im3 = *(const double4*)&sim[k + 12];
      double4 a_re3 = *(const double4*)&acc_re[k + 12];
      double4 a_im3 = *(const double4*)&acc_im[k + 12];

      a_re0 += h_re0 * s_re0 - h_im0 * s_im0;
      a_im0 += h_re0 * s_im0 + h_im0 * s_re0;

      a_re1 += h_re1 * s_re1 - h_im1 * s_im1;
      a_im1 += h_re1 * s_im1 + h_im1 * s_re1;

      a_re2 += h_re2 * s_re2 - h_im2 * s_im2;
      a_im2 += h_re2 * s_im2 + h_im2 * s_re2;

      a_re3 += h_re3 * s_re3 - h_im3 * s_im3;
      a_im3 += h_re3 * s_im3 + h_im3 * s_re3;

      *(double4*)&acc_re[k] = a_re0;
      *(double4*)&acc_im[k] = a_im0;
      *(double4*)&acc_re[k + 4] = a_re1;
      *(double4*)&acc_im[k + 4] = a_im1;
      *(double4*)&acc_re[k + 8] = a_re2;
      *(double4*)&acc_im[k + 8] = a_im2;
      *(double4*)&acc_re[k + 12] = a_re3;
      *(double4*)&acc_im[k + 12] = a_im3;
    }
    for (size_t k = vec_len; k < spec_len; k++) {
      acc_re[k] += hre[k] * sre[k] - him[k] * sim[k];
      acc_im[k] += hre[k] * sim[k] + him[k] * sre[k];
    }
  }

  // 4. Inverse FFT
  backend->inverse(backend->ctx, engine->spec_accum_re, engine->spec_accum_im,
                   engine->time_buf);

  // 5. Overlap-add output
  for (size_t i = 0; i < cs; i++) {
    waveform[i] = engine->time_buf[i] + engine->overlap_buffer[i];
  }
  memcpy(engine->overlap_buffer, engine->time_buf + cs, cs * sizeof(double));
  engine->write_idx = (widx + 1) % num_seg;
}

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

static double measure_conv_engine_ns_per_frame(bench_conv_engine_t* engine,
                                               size_t chunk_size,
                                               size_t iters) {
  double* buf = (double*)calloc(chunk_size, sizeof(double));
  for (size_t i = 0; i < chunk_size; i++) {
    buf[i] = sin(2.0 * M_PI * 1000.0 * (double)i / 48000.0);
  }

  // Warm-up
  for (size_t i = 0; i < 50; i++) {
    bench_conv_process(engine, buf);
  }

  double trials[NUM_TRIALS];
  for (int t = 0; t < NUM_TRIALS; t++) {
    uint64_t t0 = get_time_ns();
    for (size_t i = 0; i < iters; i++) {
      bench_conv_process(engine, buf);
      do_not_optimize(buf);
    }
    uint64_t t1 = get_time_ns();
    trials[t] = (double)(t1 - t0) / (double)(chunk_size * iters);
  }
  free(buf);
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
TEST(Convolution_Backend_Comparison_Taps_Sweep) {
  printf(
      "\n======================================================================"
      "===================================================\n");
  printf(
      " CONVOLUTION PERFORMANCE BENCHMARK: vDSP vs FFTW3 vs Rust vs Pure C\n");
  printf(
      " Fixed Block Size (Chunk Size): 1024 frames (Double Precision f64)\n");
  printf(
      " Monotonic Timer: 5-trial median with warm-up runs and anti-DCE memory "
      "barriers\n");
  printf(
      "========================================================================"
      "=================================================\n");
  printf(
      "   Taps | Segments |    vDSP (Apple)   |  FFTW3 (Measure)  |    Rust "
      "(RustFFT) |      Pure C       | vDSP Speedup vs C \n");
  printf(
      "        |          | ns/frame (us/blk) | ns/frame (us/blk) | ns/frame "
      "(us/blk) | ns/frame (us/blk) |     (ratio)       \n");
  printf(
      "--------+----------+-------------------+-------------------+------------"
      "-------+-------------------+-------------------\n");
  fflush(stdout);

  size_t taps_list[] = {512,   1024,  2048,  4096,  8192,
                        16384, 32768, 65536, 131072};
  size_t num_taps = sizeof(taps_list) / sizeof(taps_list[0]);
  size_t chunk_size = 1024;
  size_t fft_len = 2 * chunk_size;

  for (size_t i = 0; i < num_taps; i++) {
    size_t taps = taps_list[i];
    size_t segments = (taps + chunk_size - 1) / chunk_size;
    size_t iters = (taps <= 4096) ? 3000 : ((taps <= 32768) ? 1000 : 300);

    double* coeffs = (double*)calloc(taps, sizeof(double));
    for (size_t k = 0; k < taps; k++) {
      coeffs[k] = exp(-3.0 * (double)k / (double)taps) *
                  sin(2.0 * M_PI * 440.0 * (double)k / 48000.0);
    }

    // 1. Accelerate (vDSP) backend
    double vdsp_ns = NAN;
#if defined(ENABLE_ACCELERATE)
    vdsp_real_fft_t* vdsp_raw = vdsp_real_fft_create(fft_len);
    if (vdsp_raw) {
      real_fft_backend_t* vdsp_be = vdsp_real_fft_as_backend(vdsp_raw);
      bench_conv_engine_t* conv_vdsp =
          bench_conv_create(vdsp_be, chunk_size, coeffs, taps);
      if (conv_vdsp) {
        vdsp_ns =
            measure_conv_engine_ns_per_frame(conv_vdsp, chunk_size, iters);
        bench_conv_free(conv_vdsp);
      }
      vdsp_real_fft_free(vdsp_raw);
    }
#endif

    // 2. FFTW Measure backend
    double fftw_meas_ns = NAN;
#if defined(HAS_FFTW_HEADER)
    real_fft_backend_t* fftw_meas_be =
        fftw_backend_create(fft_len, FFTW_MEASURE);
    if (fftw_meas_be) {
      bench_conv_engine_t* conv_fftw_meas =
          bench_conv_create(fftw_meas_be, chunk_size, coeffs, taps);
      if (conv_fftw_meas) {
        fftw_meas_ns =
            measure_conv_engine_ns_per_frame(conv_fftw_meas, chunk_size, iters);
        bench_conv_free(conv_fftw_meas);
      }
      fftw_meas_be->free(fftw_meas_be->ctx);
    }
#endif

    // 3. Rust (RustFFT / CamillaDSP FftConv)
    double rust_ns = fetch_rust_conv_ns_per_frame(taps, chunk_size, iters);

    // 4. Pure C Real FFT backend
    double pure_ns = NAN;
    pure_real_fft_t* pure_raw = pure_real_fft_create(fft_len);
    if (pure_raw) {
      real_fft_backend_t* pure_be = pure_real_fft_as_backend(pure_raw);
      bench_conv_engine_t* conv_pure =
          bench_conv_create(pure_be, chunk_size, coeffs, taps);
      if (conv_pure) {
        pure_ns =
            measure_conv_engine_ns_per_frame(conv_pure, chunk_size, iters);
        bench_conv_free(conv_pure);
      }
      pure_real_fft_free(pure_raw);
    }

    // Print row
    char vdsp_str[32], fftw_meas_str[32], rust_str[32], pure_str[32],
        speedup_str[32];
    if (!isnan(vdsp_ns)) {
      snprintf(vdsp_str, sizeof(vdsp_str), "%5.1f (%5.1f us)", vdsp_ns,
               vdsp_ns * chunk_size / 1000.0);
    } else {
      strcpy(vdsp_str, "        N/A    ");
    }

    if (!isnan(fftw_meas_ns)) {
      snprintf(fftw_meas_str, sizeof(fftw_meas_str), "%5.1f (%5.1f us)",
               fftw_meas_ns, fftw_meas_ns * chunk_size / 1000.0);
    } else {
      strcpy(fftw_meas_str, "        N/A    ");
    }

    if (!isnan(rust_ns)) {
      snprintf(rust_str, sizeof(rust_str), "%5.1f (%5.1f us)", rust_ns,
               rust_ns * chunk_size / 1000.0);
    } else {
      strcpy(rust_str, "        N/A    ");
    }

    if (!isnan(pure_ns)) {
      snprintf(pure_str, sizeof(pure_str), "%5.1f (%5.1f us)", pure_ns,
               pure_ns * chunk_size / 1000.0);
    } else {
      strcpy(pure_str, "        N/A    ");
    }

    if (!isnan(vdsp_ns) && !isnan(pure_ns)) {
      snprintf(speedup_str, sizeof(speedup_str), "      %5.2fx       ",
               pure_ns / vdsp_ns);
    } else {
      strcpy(speedup_str, "        N/A        ");
    }

    printf(" %6zu | %8zu | %17s | %17s | %17s | %17s | %s\n", taps, segments,
           vdsp_str, fftw_meas_str, rust_str, pure_str, speedup_str);
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
TEST(Convolution_Backend_Comparison_Chunk_Size_Sweep) {
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
      " Chunk | Latency @48k |    vDSP (Apple)   |  FFTW3 (Measure)  |    Rust "
      "(RustFFT) |      Pure C       | Peak Throughput (vDSP)\n");
  printf(
      " Size  |     (ms)     | ns/frame (us/blk) | ns/frame (us/blk) | "
      "ns/frame (us/blk) | ns/frame (us/blk) |    (MSamples/sec)     \n");
  printf(
      "-------+--------------+-------------------+-------------------+---------"
      "----------+-------------------+-----------------------\n");
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
    size_t fft_len = 2 * cs;
    size_t iters = (cs <= 256) ? 8000 : ((cs <= 1024) ? 3000 : 1000);
    double latency_ms = (double)cs / 48000.0 * 1000.0;

    // 1. Accelerate (vDSP)
    double vdsp_ns = NAN;
#if defined(ENABLE_ACCELERATE)
    vdsp_real_fft_t* vdsp_raw = vdsp_real_fft_create(fft_len);
    if (vdsp_raw) {
      real_fft_backend_t* vdsp_be = vdsp_real_fft_as_backend(vdsp_raw);
      bench_conv_engine_t* conv_vdsp =
          bench_conv_create(vdsp_be, cs, coeffs, taps);
      if (conv_vdsp) {
        vdsp_ns = measure_conv_engine_ns_per_frame(conv_vdsp, cs, iters);
        bench_conv_free(conv_vdsp);
      }
      vdsp_real_fft_free(vdsp_raw);
    }
#endif

    // 2. FFTW Measure
    double fftw_meas_ns = NAN;
#if defined(HAS_FFTW_HEADER)
    real_fft_backend_t* fftw_meas_be =
        fftw_backend_create(fft_len, FFTW_MEASURE);
    if (fftw_meas_be) {
      bench_conv_engine_t* conv_fftw_meas =
          bench_conv_create(fftw_meas_be, cs, coeffs, taps);
      if (conv_fftw_meas) {
        fftw_meas_ns =
            measure_conv_engine_ns_per_frame(conv_fftw_meas, cs, iters);
        bench_conv_free(conv_fftw_meas);
      }
      fftw_meas_be->free(fftw_meas_be->ctx);
    }
#endif

    // 3. Rust (RustFFT)
    double rust_ns = fetch_rust_conv_ns_per_frame(taps, cs, iters);

    // 4. Pure C
    double pure_ns = NAN;
    pure_real_fft_t* pure_raw = pure_real_fft_create(fft_len);
    if (pure_raw) {
      real_fft_backend_t* pure_be = pure_real_fft_as_backend(pure_raw);
      bench_conv_engine_t* conv_pure =
          bench_conv_create(pure_be, cs, coeffs, taps);
      if (conv_pure) {
        pure_ns = measure_conv_engine_ns_per_frame(conv_pure, cs, iters);
        bench_conv_free(conv_pure);
      }
      pure_real_fft_free(pure_raw);
    }

    char vdsp_str[32], fftw_meas_str[32], rust_str[32], pure_str[32],
        tp_str[32];
    if (!isnan(vdsp_ns)) {
      snprintf(vdsp_str, sizeof(vdsp_str), "%5.1f (%5.1f us)", vdsp_ns,
               vdsp_ns * cs / 1000.0);
      double mps = 1000.0 / vdsp_ns;
      snprintf(tp_str, sizeof(tp_str), "%6.1f Mpts/s", mps);
    } else {
      strcpy(vdsp_str, "       N/A     ");
      strcpy(tp_str, "      N/A     ");
    }

    if (!isnan(fftw_meas_ns)) {
      snprintf(fftw_meas_str, sizeof(fftw_meas_str), "%5.1f (%5.1f us)",
               fftw_meas_ns, fftw_meas_ns * cs / 1000.0);
    } else {
      strcpy(fftw_meas_str, "       N/A     ");
    }

    if (!isnan(rust_ns)) {
      snprintf(rust_str, sizeof(rust_str), "%5.1f (%5.1f us)", rust_ns,
               rust_ns * cs / 1000.0);
    } else {
      strcpy(rust_str, "       N/A     ");
    }

    if (!isnan(pure_ns)) {
      snprintf(pure_str, sizeof(pure_str), "%5.1f (%5.1f us)", pure_ns,
               pure_ns * cs / 1000.0);
    } else {
      strcpy(pure_str, "       N/A     ");
    }

    printf(" %5zu |    %5.2f ms   | %17s | %17s | %17s | %17s | %21s \n", cs,
           latency_ms, vdsp_str, fftw_meas_str, rust_str, pure_str, tp_str);
    fflush(stdout);
  }
  free(coeffs);
  printf(
      "------------------------------------------------------------------------"
      "-------------------------------------------------\n\n");
}

TEST_MAIN()
