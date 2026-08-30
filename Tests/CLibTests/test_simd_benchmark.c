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

#if defined(__APPLE__)
#include <Accelerate/Accelerate.h>
#define HAS_VDSP 1
#define HAS_BLAS 1
#elif defined(HAS_CBLAS)
#include <cblas.h>
#define HAS_VDSP 0
#define HAS_BLAS 1
#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak)) void openblas_set_num_threads(int num_threads);
#endif
#else
#define HAS_VDSP 0
#define HAS_BLAS 0
#endif

#include "Utils/double_helpers.h"
#include "Utils/float_helpers.h"
#include "test_support.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#if defined(__linux__)
#include <sched.h>
#include <unistd.h>
#endif

static inline void do_not_optimize(void* p) {
  __asm__ __volatile__("" : : "g"(p) : "memory");
}

static void* alloc_aligned(size_t bytes) {
  void* ptr = NULL;
#if defined(_WIN32)
  return _aligned_malloc(bytes, 64);
#elif defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200112L
  if (posix_memalign(&ptr, 64, bytes) != 0) {
    return malloc(bytes);
  }
  return ptr;
#else
  return malloc(bytes);
#endif
}

static void free_aligned(void* ptr) {
#if defined(_WIN32)
  _aligned_free(ptr);
#else
  free(ptr);
#endif
}

static void pin_cpu_if_needed(void) {
#if defined(__linux__)
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  if (sched_getaffinity(0, sizeof(cpuset), &cpuset) == 0) {
    int count = CPU_COUNT(&cpuset);
    if (count > 1) {
      int target_cpu = 1;
      if (!CPU_ISSET(target_cpu, &cpuset)) {
        for (int i = 0; i < CPU_SETSIZE; i++) {
          if (CPU_ISSET(i, &cpuset)) {
            target_cpu = i;
            break;
          }
        }
      }
      CPU_ZERO(&cpuset);
      CPU_SET(target_cpu, &cpuset);
      sched_setaffinity(0, sizeof(cpuset), &cpuset);
    }
  }
#endif
}

static void cpu_frequency_warmup(void) {
  volatile double dummy = 1.0;
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  uint64_t start = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
  while (1) {
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    if (now - start >= 50000000ULL) break;  // 50ms spin
    dummy = dummy * 1.0000001 + 0.0000001;
  }
}

static uint64_t get_time_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

#define NUM_TRIALS 5

static int compare_doubles(const void* a, const void* b) {
  double da = *(const double*)a;
  double db = *(const double*)b;
  return (da > db) - (da < db);
}

typedef enum {
  OP_DBL_SCALAR_MUL = 0,
  OP_DBL_VEC_ADD,
  OP_DBL_ADD_INPLACE,
  OP_DBL_MUL,
  OP_DBL_MUL_ADD,
  OP_DBL_DOT_PROD,
  OP_DBL_CLIP,
  OP_DBL_COMPLEX_MUL,
  OP_FLT_PEAK_ABS,
  OP_FLT_RMS,
  OP_DBL_TO_FLT,
  OP_FLT_ADD,
  OP_FLT_MUL,
  OP_FLT_SCALAR_MUL,
  OP_FLT_HANN,
  OP_FLT_MAX,
  OP_FLT_ZVABS,
  OP_FLT_VDBCON,
  OP_COUNT
} bench_op_id_t;

typedef struct {
  bench_op_id_t id;
  const char* name;
  bool is_float;
  bool has_blas;
  bool has_vdsp;
} bench_op_info_t;

static const bench_op_info_t OP_INFOS[OP_COUNT] = {
    {OP_DBL_SCALAR_MUL, "dsp_ops_scalar_multiply", false, true, true},
    {OP_DBL_VEC_ADD, "dsp_ops_vector_add", false, true, true},
    {OP_DBL_ADD_INPLACE, "dsp_ops_add (in-place)", false, true, true},
    {OP_DBL_MUL, "dsp_ops_multiply", false, false, true},
    {OP_DBL_MUL_ADD, "dsp_ops_multiply_add", false, true, true},
    {OP_DBL_DOT_PROD, "sinc_dot_product", false, true, true},
    {OP_DBL_CLIP, "dsp_ops_clip", false, false, true},
    {OP_DBL_COMPLEX_MUL, "dsp_ops_complex_multiply", false, false, true},
    {OP_FLT_PEAK_ABS, "dsp_ops_peak_absolute", false, true, true},
    {OP_FLT_RMS, "dsp_ops_rms", false, true, true},
    {OP_DBL_TO_FLT, "dsp_ops_double_to_float", false, false, true},
    {OP_FLT_ADD, "dsp_ops_float_add", true, true, true},
    {OP_FLT_MUL, "dsp_ops_float_multiply", true, false, true},
    {OP_FLT_SCALAR_MUL, "dsp_ops_float_scalar_multiply", true, true, true},
    {OP_FLT_HANN, "dsp_ops_float_hann_window", true, false, true},
    {OP_FLT_MAX, "dsp_ops_float_max", true, false, true},
    {OP_FLT_ZVABS, "dsp_ops_float_zvabs", true, false, true},
    {OP_FLT_VDBCON, "dsp_ops_float_vdbcon", true, false, true}};

#define NUM_BENCH_SIZES 6
static const size_t BENCH_SIZES[NUM_BENCH_SIZES] = {64,   256,   1024,
                                                    4096, 16384, 65536};

// Results storage: [op_index][size_index]
static double g_speedup_vdsp[OP_COUNT][NUM_BENCH_SIZES];
static double g_speedup_blas[OP_COUNT][NUM_BENCH_SIZES];

// -----------------------------------------------------------------------------
// Benchmark Execution
// -----------------------------------------------------------------------------

static void benchmark_op_at_size(bench_op_id_t op_id, size_t size_idx) {
  size_t count = BENCH_SIZES[size_idx];
  bench_op_info_t info = OP_INFOS[op_id];

  // Allocate aligned test buffers
  double* da1 = (double*)alloc_aligned(count * sizeof(double));
  double* da2 = (double*)alloc_aligned(count * sizeof(double));
  double* da3 = (double*)alloc_aligned(count * sizeof(double));
  double* da4 = (double*)alloc_aligned(count * sizeof(double));
  double* da_out_c = (double*)alloc_aligned(count * sizeof(double));
  double* da_out_c2 = (double*)alloc_aligned(count * sizeof(double));
  double* da_out_lib = (double*)alloc_aligned(count * sizeof(double));
  double* da_out_lib2 = (double*)alloc_aligned(count * sizeof(double));

  float* fa1 = (float*)alloc_aligned(count * sizeof(float));
  float* fa2 = (float*)alloc_aligned(count * sizeof(float));
  float* fa_out_c = (float*)alloc_aligned(count * sizeof(float));
  float* fa_out_lib = (float*)alloc_aligned(count * sizeof(float));

  double scalar_d = 1.0000001;
  double low_d = -0.5, high_d = 0.5;
  float scalar_f = 1.0000001f;
  float threshold_f = 0.1f;
  float ref_f = 1.0f;
  do_not_optimize(&scalar_d);
  do_not_optimize(&scalar_f);
  do_not_optimize(&threshold_f);
  do_not_optimize(&ref_f);

  for (size_t i = 0; i < count; i++) {
    da1[i] = sin((double)i * 0.05) + 0.1;
    da2[i] = cos((double)i * 0.05) - 0.2;
    da3[i] = sin((double)i * 0.02 + 0.5) * 0.8;
    da4[i] = cos((double)i * 0.02 + 0.5) * 0.8;
    da_out_c[i] = da2[i];
    da_out_c2[i] = 0.0;
    da_out_lib[i] = da2[i];
    da_out_lib2[i] = 0.0;

    fa1[i] = (float)fabs(sin((double)i * 0.05)) + 0.01f;
    fa2[i] = (float)fabs(cos((double)i * 0.05)) + 0.02f;
    fa_out_c[i] = fa2[i];
    fa_out_lib[i] = fa2[i];
  }

  size_t iters = 2000000 / (count < 64 ? 64 : count);
  if (iters < 5000) iters = 5000;
  if (iters > 2000000) iters = 2000000;

  volatile float scalar_res_f = 0.0f;
  volatile double scalar_res_d = 0.0;

  // 1. Measure Optimized C
  double trials_c[NUM_TRIALS];
  for (int t = 0; t < NUM_TRIALS; t++) {
    memcpy(da_out_c, da2, count * sizeof(double));
    memcpy(fa_out_c, fa2, count * sizeof(float));
    uint64_t start = get_time_ns();
    for (size_t iter = 0; iter < iters; iter++) {
      switch (op_id) {
        case OP_DBL_SCALAR_MUL:
          dsp_ops_scalar_multiply(da_out_c, scalar_d, count);
          break;
        case OP_DBL_VEC_ADD:
          dsp_ops_vector_add(da1, da2, da_out_c, count);
          break;
        case OP_DBL_ADD_INPLACE:
          dsp_ops_add(da1, da_out_c, count);
          break;
        case OP_DBL_MUL:
          dsp_ops_multiply(da1, da_out_c, count);
          break;
        case OP_DBL_MUL_ADD:
          dsp_ops_multiply_add(da1, scalar_d, da_out_c, count);
          break;
        case OP_DBL_DOT_PROD:
          scalar_res_d += sinc_dot_product(da1, da2, count);
          break;
        case OP_DBL_CLIP:
          dsp_ops_clip(da_out_c, low_d, high_d, count);
          break;
        case OP_DBL_COMPLEX_MUL:
          dsp_ops_complex_multiply(da1, da2, da3, da4, da_out_c, da_out_c2,
                                   count);
          break;
        case OP_FLT_PEAK_ABS:
          scalar_res_f += dsp_ops_peak_absolute(da1, count);
          break;
        case OP_FLT_RMS:
          scalar_res_f += dsp_ops_rms(da1, count);
          break;
        case OP_DBL_TO_FLT:
          dsp_ops_double_to_float(da1, fa_out_c, count);
          break;
        case OP_FLT_ADD:
          dsp_ops_float_add(fa1, fa_out_c, count);
          break;
        case OP_FLT_MUL:
          dsp_ops_float_multiply(fa1, fa2, fa_out_c, count);
          break;
        case OP_FLT_SCALAR_MUL:
          dsp_ops_float_scalar_multiply(fa_out_c, scalar_f, count);
          break;
        case OP_FLT_HANN:
          dsp_ops_float_hann_window(fa_out_c, count);
          break;
        case OP_FLT_MAX:
          scalar_res_f += dsp_ops_float_max(fa1, count);
          break;
        case OP_FLT_ZVABS:
          dsp_ops_float_zvabs(fa1, fa2, fa_out_c, count);
          break;
        case OP_FLT_VDBCON:
          dsp_ops_float_vdbcon(fa1, ref_f, fa_out_c, count);
          break;
        default:
          break;
      }
    }
    uint64_t end = get_time_ns();
    do_not_optimize(da_out_c);
    do_not_optimize(fa_out_c);
    trials_c[t] = (double)(end - start) / (double)iters;
  }
  qsort(trials_c, NUM_TRIALS, sizeof(double), compare_doubles);
  double time_c_ns = trials_c[NUM_TRIALS / 2];

  // 2. Measure vDSP (if platform supports it and op has vDSP implementation)
#if HAS_VDSP
  if (info.has_vdsp) {
    double trials_vdsp[NUM_TRIALS];
    for (int t = 0; t < NUM_TRIALS; t++) {
      memcpy(da_out_lib, da2, count * sizeof(double));
      memcpy(fa_out_lib, fa2, count * sizeof(float));
      uint64_t start = get_time_ns();
      for (size_t iter = 0; iter < iters; iter++) {
        switch (op_id) {
          case OP_DBL_SCALAR_MUL:
            vDSP_vsmulD(da_out_lib, 1, &scalar_d, da_out_lib, 1, count);
            break;
          case OP_DBL_VEC_ADD:
            vDSP_vaddD(da1, 1, da2, 1, da_out_lib, 1, count);
            break;
          case OP_DBL_ADD_INPLACE:
            vDSP_vaddD(da1, 1, da_out_lib, 1, da_out_lib, 1, count);
            break;
          case OP_DBL_MUL:
            vDSP_vmulD(da1, 1, da_out_lib, 1, da_out_lib, 1, count);
            break;
          case OP_DBL_MUL_ADD:
            vDSP_vsmaD(da1, 1, &scalar_d, da_out_lib, 1, da_out_lib, 1, count);
            break;
          case OP_DBL_DOT_PROD: {
            double dp = 0.0;
            vDSP_dotprD(da1, 1, da2, 1, &dp, count);
            scalar_res_d += dp;
            break;
          }
          case OP_DBL_CLIP:
            vDSP_vclipD(da_out_lib, 1, &low_d, &high_d, da_out_lib, 1, count);
            break;
          case OP_DBL_COMPLEX_MUL: {
            DSPDoubleSplitComplex a = {da1, da2};
            DSPDoubleSplitComplex b = {da3, da4};
            DSPDoubleSplitComplex out = {da_out_lib, da_out_lib2};
            vDSP_zvmulD(&a, 1, &b, 1, &out, 1, count, 1);
            break;
          }
          case OP_FLT_PEAK_ABS: {
            double peak_d = 0.0;
            vDSP_maxmgvD(da1, 1, &peak_d, count);
            scalar_res_f += (float)peak_d;
            break;
          }
          case OP_FLT_RMS: {
            double rms_d = 0.0;
            vDSP_rmsqvD(da1, 1, &rms_d, count);
            scalar_res_f += (float)rms_d;
            break;
          }
          case OP_DBL_TO_FLT:
            vDSP_vdpsp(da1, 1, fa_out_lib, 1, count);
            break;
          case OP_FLT_ADD:
            vDSP_vadd(fa1, 1, fa_out_lib, 1, fa_out_lib, 1, count);
            break;
          case OP_FLT_MUL:
            vDSP_vmul(fa1, 1, fa2, 1, fa_out_lib, 1, count);
            break;
          case OP_FLT_SCALAR_MUL:
            vDSP_vsmul(fa_out_lib, 1, &scalar_f, fa_out_lib, 1, count);
            break;
          case OP_FLT_HANN:
            vDSP_hann_window(fa_out_lib, (vDSP_Length)count, 0);
            break;
          case OP_FLT_MAX: {
            float max_v = 0.0f;
            vDSP_maxv(fa1, 1, &max_v, count);
            scalar_res_f += max_v;
            break;
          }
          case OP_FLT_ZVABS: {
            DSPSplitComplex sc = {fa1, fa2};
            vDSP_zvabs(&sc, 1, fa_out_lib, 1, count);
            break;
          }
          case OP_FLT_VDBCON:
            vDSP_vdbcon(fa1, 1, &ref_f, fa_out_lib, 1, count, 1);
            break;
          default:
            break;
        }
      }
      uint64_t end = get_time_ns();
      do_not_optimize(da_out_lib);
      do_not_optimize(fa_out_lib);
      trials_vdsp[t] = (double)(end - start) / (double)iters;
    }
    qsort(trials_vdsp, NUM_TRIALS, sizeof(double), compare_doubles);
    double time_vdsp_ns = trials_vdsp[NUM_TRIALS / 2];
    g_speedup_vdsp[op_id][size_idx] = time_c_ns / time_vdsp_ns;
  } else {
    g_speedup_vdsp[op_id][size_idx] = -1.0;  // null
  }
#else
  g_speedup_vdsp[op_id][size_idx] = -1.0;  // null on non-Apple
#endif

  // 3. Measure BLAS (if platform supports it and op has BLAS counterpart)
#if HAS_BLAS
  if (info.has_blas) {
    double trials_blas[NUM_TRIALS];
    for (int t = 0; t < NUM_TRIALS; t++) {
      memcpy(da_out_lib, da2, count * sizeof(double));
      memcpy(fa_out_lib, fa2, count * sizeof(float));
      uint64_t start = get_time_ns();
      for (size_t iter = 0; iter < iters; iter++) {
        switch (op_id) {
          case OP_DBL_SCALAR_MUL:
            cblas_dscal((int)count, scalar_d, da_out_lib, 1);
            break;
          case OP_DBL_VEC_ADD:
            cblas_dcopy((int)count, da1, 1, da_out_lib, 1);
            cblas_daxpy((int)count, 1.0, da2, 1, da_out_lib, 1);
            break;
          case OP_DBL_ADD_INPLACE:
            cblas_daxpy((int)count, 1.0, da1, 1, da_out_lib, 1);
            break;
          case OP_DBL_MUL_ADD:
            cblas_daxpy((int)count, scalar_d, da1, 1, da_out_lib, 1);
            break;
          case OP_DBL_DOT_PROD:
            scalar_res_d += cblas_ddot((int)count, da1, 1, da2, 1);
            break;
          case OP_FLT_PEAK_ABS: {
            int idx = (int)cblas_idamax((int)count, da1, 1);
            scalar_res_f += (float)fabs(da1[idx]);
            break;
          }
          case OP_FLT_RMS: {
            double norm = cblas_dnrm2((int)count, da1, 1);
            scalar_res_f += (float)(norm / sqrt((double)count));
            break;
          }
          case OP_FLT_ADD:
            cblas_saxpy((int)count, 1.0f, fa1, 1, fa_out_lib, 1);
            break;
          case OP_FLT_SCALAR_MUL:
            cblas_sscal((int)count, scalar_f, fa_out_lib, 1);
            break;
          default:
            break;
        }
      }
      uint64_t end = get_time_ns();
      do_not_optimize(da_out_lib);
      do_not_optimize(fa_out_lib);
      trials_blas[t] = (double)(end - start) / (double)iters;
    }
    qsort(trials_blas, NUM_TRIALS, sizeof(double), compare_doubles);
    double time_blas_ns = trials_blas[NUM_TRIALS / 2];
    g_speedup_blas[op_id][size_idx] = time_c_ns / time_blas_ns;
  } else {
    g_speedup_blas[op_id][size_idx] = -1.0;  // null
  }
#else
  g_speedup_blas[op_id][size_idx] = -1.0;  // null
#endif

  free_aligned(da1);
  free_aligned(da2);
  free_aligned(da3);
  free_aligned(da4);
  free_aligned(da_out_c);
  free_aligned(da_out_c2);
  free_aligned(da_out_lib);
  free_aligned(da_out_lib2);
  free_aligned(fa1);
  free_aligned(fa2);
  free_aligned(fa_out_c);
  free_aligned(fa_out_lib);
}

// Format speedup entry "vDSP / BLAS"
static void format_entry(char* buf, size_t buf_len, double sp_vdsp,
                         double sp_blas) {
  char vdsp_str[16];
  char blas_str[16];

  if (sp_vdsp < 0.0) {
    snprintf(vdsp_str, sizeof(vdsp_str), "null");
  } else {
    snprintf(vdsp_str, sizeof(vdsp_str), "%.2fx", sp_vdsp);
  }

  if (sp_blas < 0.0) {
    snprintf(blas_str, sizeof(blas_str), "null");
  } else {
    snprintf(blas_str, sizeof(blas_str), "%.2fx", sp_blas);
  }

  snprintf(buf, buf_len, "%s / %s", vdsp_str, blas_str);
}

TEST(SIMD_Benchmark) {
#if defined(_WIN32)
  _putenv("VECLIB_MAXIMUM_THREADS=1");
#else
  setenv("VECLIB_MAXIMUM_THREADS", "1", 1);
#endif
#if defined(HAS_CBLAS) && !defined(__APPLE__)
  if (openblas_set_num_threads) {
    openblas_set_num_threads(1);
  }
#endif

  pin_cpu_if_needed();
  cpu_frequency_warmup();

  printf(
      "========================================================================"
      "=========================================================\n");
  printf(
      " CDSP SIMD Benchmark: Speedup over Compiler-Optimized C (vDSP / "
      "BLAS)\n");
  printf(" Platform: %s | vDSP: %s | BLAS: %s\n",
#if defined(__APPLE__)
         "macOS (Apple Silicon / Intel)",
#elif defined(__linux__)
         "Linux",
#elif defined(_WIN32)
         "Windows",
#else
         "Generic",
#endif
         HAS_VDSP ? "Available" : "null (Not Supported)",
         HAS_BLAS ? "Available" : "null (Not Configured)");
  printf(
      "========================================================================"
      "=========================================================\n");

  printf("Running benchmarks across %d operations and %d buffer sizes...\n",
         OP_COUNT, NUM_BENCH_SIZES);

  for (int op = 0; op < OP_COUNT; op++) {
    for (int s = 0; s < NUM_BENCH_SIZES; s++) {
      benchmark_op_at_size((bench_op_id_t)op, s);
    }
  }

  printf("\n");
  printf("%-30s | %-14s | %-14s | %-14s | %-14s | %-14s | %-14s\n",
         "Function (Helper)", "N = 64", "N = 256", "N = 1024", "N = 4096",
         "N = 16384", "N = 65536");
  printf("%-30s | %-14s | %-14s | %-14s | %-14s | %-14s | %-14s\n", "",
         "(vDSP / BLAS)", "(vDSP / BLAS)", "(vDSP / BLAS)", "(vDSP / BLAS)",
         "(vDSP / BLAS)", "(vDSP / BLAS)");
  printf(
      "-------------------------------+----------------+----------------+------"
      "----------+----------------+----------------+---------------\n");

  for (int op = 0; op < OP_COUNT; op++) {
    char col[NUM_BENCH_SIZES][32];
    for (int s = 0; s < NUM_BENCH_SIZES; s++) {
      format_entry(col[s], sizeof(col[s]), g_speedup_vdsp[op][s],
                   g_speedup_blas[op][s]);
    }
    printf("%-30s | %-14s | %-14s | %-14s | %-14s | %-14s | %-14s\n",
           OP_INFOS[op].name, col[0], col[1], col[2], col[3], col[4], col[5]);
  }
  printf(
      "-------------------------------+----------------+----------------+------"
      "----------+----------------+----------------+---------------\n");
  printf(
      "Note: Speedup > 1.00x means library out-performed pure C. 'null' "
      "indicates not applicable or unsupported on current platform.\n\n");
}

TEST_MAIN()
