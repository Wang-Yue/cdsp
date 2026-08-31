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

#include "FFT/arbitrary_complex_fft.h"
#include "FFT/mixed_radix_fft.h"
#include "FFT/real_fft.h"
#include "test_support.h"

#if defined(ENABLE_ACCELERATE)
#include <Accelerate/Accelerate.h>

#include "FFT/complex_inner_real_fft.h"

typedef struct {
  vDSP_DFT_SetupD setup_forward;
  vDSP_DFT_SetupD setup_inverse;
} vdsp_bench_dft_t;

static vdsp_bench_dft_t* vdsp_bench_dft_create(size_t n) {
  vDSP_DFT_SetupD fwd =
      vDSP_DFT_zop_CreateSetupD(NULL, (vDSP_Length)n, vDSP_DFT_FORWARD);
  if (!fwd) return NULL;
  vDSP_DFT_SetupD inv =
      vDSP_DFT_zop_CreateSetupD(fwd, (vDSP_Length)n, vDSP_DFT_INVERSE);
  if (!inv) {
    vDSP_DFT_DestroySetupD(fwd);
    return NULL;
  }
  vdsp_bench_dft_t* dft =
      (vdsp_bench_dft_t*)calloc(1, sizeof(vdsp_bench_dft_t));
  if (!dft) {
    vDSP_DFT_DestroySetupD(fwd);
    vDSP_DFT_DestroySetupD(inv);
    return NULL;
  }
  dft->setup_forward = fwd;
  dft->setup_inverse = inv;
  return dft;
}

static void vdsp_bench_dft_execute(void* ctx, waveform_t real_in,
                                   waveform_t imag_in,
                                   mutable_waveform_t real_out,
                                   mutable_waveform_t imag_out, bool inverse) {
  vdsp_bench_dft_t* dft = (vdsp_bench_dft_t*)ctx;
  if (!dft) return;
  vDSP_DFT_SetupD setup = inverse ? dft->setup_inverse : dft->setup_forward;
  vDSP_DFT_ExecuteD(setup, real_in, imag_in, real_out, imag_out);
}

static void vdsp_bench_dft_free(void* ctx) {
  vdsp_bench_dft_t* dft = (vdsp_bench_dft_t*)ctx;
  if (!dft) return;
  if (dft->setup_inverse) vDSP_DFT_DestroySetupD(dft->setup_inverse);
  if (dft->setup_forward) vDSP_DFT_DestroySetupD(dft->setup_forward);
  free(dft);
}

static arbitrary_complex_fft_t* vdsp_bench_as_arbitrary(vdsp_bench_dft_t* dft) {
  if (!dft) return NULL;
  arbitrary_complex_fft_t* a =
      (arbitrary_complex_fft_t*)calloc(1, sizeof(arbitrary_complex_fft_t));
  if (!a) return NULL;
  a->ctx = dft;
  a->execute = vdsp_bench_dft_execute;
  a->free = vdsp_bench_dft_free;
  return a;
}
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define NUM_TRIALS 5

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

typedef struct {
  uint64_t state;
} splitmix64_t;

static uint64_t splitmix64_next(splitmix64_t* rng) {
  rng->state += 0x9E3779B97F4A7C15ULL;
  uint64_t z = rng->state;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

static double splitmix64_next_unit(splitmix64_t* rng) {
  return (double)(splitmix64_next(rng) >> 11) * (1.0 / 9007199254740992.0);
}

static void fill_random_complex(double* re, double* im, size_t n,
                                uint64_t seed) {
  splitmix64_t rng = {seed};
  for (size_t i = 0; i < n; i++) {
    re[i] = splitmix64_next_unit(&rng) * 2.0 - 1.0;
    im[i] = splitmix64_next_unit(&rng) * 2.0 - 1.0;
  }
}

static size_t calculate_iterations(size_t n) {
  double log2n = log2((double)(n > 1 ? n : 2));
  double target_ops =
      1.0e6;  // target ~1M equivalent points per trial (~1-2ms per size)
  size_t iters =
      (size_t)(target_ops / ((double)n * (log2n > 1.0 ? log2n : 1.0)));
  if (iters < 50) iters = 50;
  if (iters > 25000) iters = 25000;
  return iters;
}

static inline size_t gcd_u64(size_t a, size_t b) {
  while (b != 0) {
    size_t t = b;
    b = a % b;
    a = t;
  }
  return a;
}

static void run_rate_conversion_matrix_benchmark(void) {
  static const uint32_t rates[] = {8000,  12000, 16000, 24000,  32000,
                                   48000, 64000, 96000, 192000, 384000};
  static const char* labels[] = {"8k",  "12k", "16k", "24k",  "32k",
                                 "48k", "64k", "96k", "192k", "384k"};
  const size_t num_rates = sizeof(rates) / sizeof(rates[0]);

  double matrix_vs_vdsp[10][10];
  double matrix_mr_us[10][10];
  double matrix_vdsp_us[10][10];
  size_t matrix_nin[10][10];
  size_t matrix_nout[10][10];

  for (size_t r_in = 0; r_in < num_rates; r_in++) {
    for (size_t r_out = 0; r_out < num_rates; r_out++) {
      uint32_t in_rate = rates[r_in];
      uint32_t out_rate = rates[r_out];
      size_t g = gcd_u64(in_rate, out_rate);
      size_t min_in = in_rate / g;
      size_t min_out = out_rate / g;

      // Sizing with power-of-2 multiplier targeting ~1024 chunk size
      size_t k = 1;
      while (k * min_in < 1024) {
        k *= 2;
      }
      size_t n_in = k * min_in;
      size_t n_out = k * min_out;

      matrix_nin[r_in][r_out] = n_in;
      matrix_nout[r_in][r_out] = n_out;
      matrix_vs_vdsp[r_in][r_out] = -1.0;
      matrix_mr_us[r_in][r_out] = -1.0;
      matrix_vdsp_us[r_in][r_out] = -1.0;

      size_t len_in = 2 * n_in;
      size_t len_out = 2 * n_out;
      real_fft_t* rfft_in = real_fft_create(len_in, NULL);
      real_fft_t* rfft_out = real_fft_create(len_out, NULL);
      if (!rfft_in || !rfft_out) {
        if (rfft_in) real_fft_free(rfft_in);
        if (rfft_out) real_fft_free(rfft_out);
        continue;
      }

      size_t max_len = len_in > len_out ? len_in : len_out;
      size_t max_spec = (n_in > n_out ? n_in : n_out) + 1;
      double* in_real = (double*)malloc(max_len * sizeof(double));
      double* spec_re = (double*)malloc(max_spec * sizeof(double));
      double* spec_im = (double*)malloc(max_spec * sizeof(double));
      double* out_real = (double*)malloc(max_len * sizeof(double));

      splitmix64_t rng = {42};
      for (size_t i = 0; i < max_len; i++) {
        in_real[i] = splitmix64_next_unit(&rng) * 2.0 - 1.0;
      }

      size_t iters = calculate_iterations(n_in + n_out);

      // Warm-up RealFFT
      for (size_t i = 0; i < 20; i++) {
        real_fft_forward(rfft_in, in_real, spec_re, spec_im);
        real_fft_inverse(rfft_out, spec_re, spec_im, out_real);
      }

      // Time RealFFT
      double trials_r[NUM_TRIALS];
      for (int t = 0; t < NUM_TRIALS; t++) {
        uint64_t t0 = get_time_ns();
        for (size_t i = 0; i < iters; i++) {
          real_fft_forward(rfft_in, in_real, spec_re, spec_im);
          real_fft_inverse(rfft_out, spec_re, spec_im, out_real);
          do_not_optimize(out_real);
        }
        uint64_t t1 = get_time_ns();
        trials_r[t] = (double)(t1 - t0) / (double)iters;
      }
      qsort(trials_r, NUM_TRIALS, sizeof(double), compare_doubles);
      double r_ns = trials_r[NUM_TRIALS / 2];
      matrix_mr_us[r_in][r_out] = r_ns / 1000.0;

#if defined(ENABLE_ACCELERATE)
      vdsp_bench_dft_t* vd_in = vdsp_bench_dft_create(n_in);
      vdsp_bench_dft_t* vd_out = vdsp_bench_dft_create(n_out);
      if (vd_in && vd_out) {
        complex_inner_real_fft_t* ci_in = complex_inner_real_fft_create(
            len_in, vdsp_bench_as_arbitrary(vd_in));
        complex_inner_real_fft_t* ci_out = complex_inner_real_fft_create(
            len_out, vdsp_bench_as_arbitrary(vd_out));

        for (size_t i = 0; i < 20; i++) {
          complex_inner_real_fft_forward(ci_in, in_real, spec_re, spec_im);
          complex_inner_real_fft_inverse(ci_out, spec_re, spec_im, out_real);
        }
        double trials_vd[NUM_TRIALS];
        for (int t = 0; t < NUM_TRIALS; t++) {
          uint64_t t0 = get_time_ns();
          for (size_t i = 0; i < iters; i++) {
            complex_inner_real_fft_forward(ci_in, in_real, spec_re, spec_im);
            complex_inner_real_fft_inverse(ci_out, spec_re, spec_im, out_real);
            do_not_optimize(out_real);
          }
          uint64_t t1 = get_time_ns();
          trials_vd[t] = (double)(t1 - t0) / (double)iters;
        }
        qsort(trials_vd, NUM_TRIALS, sizeof(double), compare_doubles);
        double vd_ns = trials_vd[NUM_TRIALS / 2];
        matrix_vdsp_us[r_in][r_out] = vd_ns / 1000.0;
        matrix_vs_vdsp[r_in][r_out] = vd_ns / r_ns;  // RealFFT Perf / vDSP Perf

        complex_inner_real_fft_free(ci_in);
        complex_inner_real_fft_free(ci_out);
      } else {
        if (vd_in) vdsp_bench_dft_free(vd_in);
        if (vd_out) vdsp_bench_dft_free(vd_out);
      }
#endif

      real_fft_free(rfft_in);
      real_fft_free(rfft_out);
      free(in_real);
      free(spec_re);
      free(spec_im);
      free(out_real);
    }
  }

  printf(
      "\n======================================================================"
      "========================================================================"
      "=="
      "=====\n");
  printf(
      " 14. vDSP_DFT_zop Supported Audio & Telecom Rates Conversion Matrix "
      "(8k to 384k)\n");
  printf(
      "     Cell Value: Perf Ratio vs Apple vDSP (RealFFT Perf / vDSP Perf = "
      "vDSP time / RealFFT time; >1.0x = RealFFT faster).\n");
  printf(
      "========================================================================"
      "========================================================================"
      "=====\n");

  printf(" In \\ Out  |");
  for (size_t c = 0; c < num_rates; c++) {
    printf(" %-8s |", labels[c]);
  }
  printf("\n-----------+");
  for (size_t c = 0; c < num_rates; c++) {
    printf("----------+");
  }
  printf("\n");

  for (size_t r = 0; r < num_rates; r++) {
    printf(" %-9s |", labels[r]);
    for (size_t c = 0; c < num_rates; c++) {
      if (matrix_vs_vdsp[r][c] > 0.0) {
        printf("  %6.2fx  |", matrix_vs_vdsp[r][c]);
      } else {
        printf("    -     |");
      }
    }
    printf("\n");
  }

  printf(
      "-----------+----------+----------+----------+----------+----------+-----"
      "-----+----------+----------+----------+----------+\n");
}

static void run_441_48k_family_matrix_benchmark(void) {
  static const uint32_t rates[] = {44100,  48000,  88200,  96000,  176400,
                                   192000, 352800, 384000, 705600, 768000};
  static const char* labels[] = {"44.1k", "48k",    "88.2k", "96k",    "176.4k",
                                 "192k",  "352.8k", "384k",  "705.6k", "768k"};
  const size_t num_rates = sizeof(rates) / sizeof(rates[0]);

  double matrix_mpts[10][10];
  double matrix_us[10][10];

  for (size_t r_in = 0; r_in < num_rates; r_in++) {
    for (size_t r_out = 0; r_out < num_rates; r_out++) {
      uint32_t in_rate = rates[r_in];
      uint32_t out_rate = rates[r_out];
      size_t g = gcd_u64(in_rate, out_rate);
      size_t L = in_rate / g;
      size_t M = out_rate / g;

      size_t k = 1;
      if (L % 147 == 0 || M % 147 == 0) {
        k = 7;
        while (k * L < 512 && k * M < 512) {
          k *= 2;
        }
      } else {
        while (k * L < 1024) {
          k *= 2;
        }
      }
      size_t n_in = k * L;
      size_t n_out = k * M;

      matrix_mpts[r_in][r_out] = 0.0;
      matrix_us[r_in][r_out] = 0.0;

      mixed_radix_fft_t* mr_in = mixed_radix_fft_create(n_in);
      mixed_radix_fft_t* mr_out = mixed_radix_fft_create(n_out);
      if (!mr_in || !mr_out) {
        if (mr_in) mixed_radix_fft_free(mr_in);
        if (mr_out) mixed_radix_fft_free(mr_out);
        continue;
      }

      size_t max_n = n_in > n_out ? n_in : n_out;
      double* in_re = (double*)malloc(max_n * sizeof(double));
      double* in_im = (double*)malloc(max_n * sizeof(double));
      double* mid_re = (double*)malloc(max_n * sizeof(double));
      double* mid_im = (double*)malloc(max_n * sizeof(double));
      double* out_re = (double*)malloc(max_n * sizeof(double));
      double* out_im = (double*)malloc(max_n * sizeof(double));
      fill_random_complex(in_re, in_im, max_n, 42);

      size_t iters = calculate_iterations(n_in + n_out);

      // Warm-up
      for (size_t i = 0; i < 20; i++) {
        mixed_radix_fft_execute(mr_in, in_re, in_im, mid_re, mid_im, false);
        mixed_radix_fft_execute(mr_out, mid_re, mid_im, out_re, out_im, true);
      }

      // Time MR
      double trials[NUM_TRIALS];
      for (int t = 0; t < NUM_TRIALS; t++) {
        uint64_t t0 = get_time_ns();
        for (size_t i = 0; i < iters; i++) {
          mixed_radix_fft_execute(mr_in, in_re, in_im, mid_re, mid_im, false);
          mixed_radix_fft_execute(mr_out, mid_re, mid_im, out_re, out_im, true);
          do_not_optimize(out_re);
          do_not_optimize(out_im);
        }
        uint64_t t1 = get_time_ns();
        trials[t] = (double)(t1 - t0) / (double)iters;
      }
      qsort(trials, NUM_TRIALS, sizeof(double), compare_doubles);
      double mr_ns = trials[NUM_TRIALS / 2];
      double total_points = (double)(n_in + n_out);
      matrix_us[r_in][r_out] = mr_ns / 1000.0;
      matrix_mpts[r_in][r_out] = (total_points / mr_ns) * 1000.0;

      mixed_radix_fft_free(mr_in);
      mixed_radix_fft_free(mr_out);
      free(in_re);
      free(in_im);
      free(mid_re);
      free(mid_im);
      free(out_re);
      free(out_im);
    }
  }

  printf(
      "\n======================================================================"
      "===================================================================\n");
  printf(
      " 15. Complete 44.1k & 48k Family Cross-Rate Conversion Matrix (44.1 kHz "
      "to 768 kHz)\n");
  printf(
      "     Cell Value: Conversion Throughput in Mpt/s (Million Complex Points "
      "/ sec; higher is faster).\n");
  printf(
      "========================================================================"
      "=================================================================\n");

  printf(" In \\ Out   |");
  for (size_t c = 0; c < num_rates; c++) {
    printf(" %-8s |", labels[c]);
  }
  printf("\n------------+");
  for (size_t c = 0; c < num_rates; c++) {
    printf("----------+");
  }
  printf("\n");

  for (size_t r = 0; r < num_rates; r++) {
    printf(" %-10s |", labels[r]);
    for (size_t c = 0; c < num_rates; c++) {
      if (matrix_mpts[r][c] > 0.0) {
        printf("  %6.1f  |", matrix_mpts[r][c]);
      } else {
        printf("    -     |");
      }
    }
    printf("\n");
  }

  printf(
      "------------+----------+----------+----------+----------+----------+----"
      "------+----------+----------+----------+----------+\n");
}

static uint32_t gcd_u32(uint32_t a, uint32_t b) {
  while (b != 0) {
    uint32_t t = b;
    b = a % b;
    a = t;
  }
  return a;
}

typedef struct {
  size_t prime;
  size_t count;
} prime_freq_t;

static int compare_prime_freq(const void* a, const void* b) {
  const prime_freq_t* pa = (const prime_freq_t*)a;
  const prime_freq_t* pb = (const prime_freq_t*)b;
  if (pa->count != pb->count) {
    return (pb->count > pa->count) ? 1 : -1;
  }
  return (pa->prime > pb->prime) ? 1 : -1;
}

static void analyze_factors(size_t n, size_t* prime_counts,
                            size_t max_prime_tracked) {
  size_t rem = n;
  while (rem % 2 == 0) rem /= 2;
  while (rem % 3 == 0) rem /= 3;
  while (rem % 5 == 0) rem /= 5;
  while (rem % 7 == 0) rem /= 7;
  while (rem % 11 == 0) rem /= 11;
  while (rem % 13 == 0) rem /= 13;

  for (size_t d = 17; d * d <= rem; d += 2) {
    if (rem % d == 0) {
      if (d < max_prime_tracked) {
        prime_counts[d]++;
      }
      while (rem % d == 0) rem /= d;
    }
  }
  if (rem > 1 && rem < max_prime_tracked) {
    prime_counts[rem]++;
  }
}

static void print_missing_primes_table(const size_t* prime_counts,
                                       size_t max_prime_tracked,
                                       const char* survey_title) {
  prime_freq_t list[512];
  size_t num_unique = 0;
  for (size_t p = 14; p < max_prime_tracked; p++) {
    if (prime_counts[p] > 0) {
      list[num_unique].prime = p;
      list[num_unique].count = prime_counts[p];
      num_unique++;
      if (num_unique >= 512) break;
    }
  }

  printf(
      "\n--- Missing Prime Factors Required for 100%% Mixed-Radix Coverage "
      "(%s) ---\n",
      survey_title);
  if (num_unique == 0) {
    printf(
        " None! Current unrolled radixes (2, 3, 4, 5, 7, 8, 9, 11, 13) achieve "
        "100%% full coverage.\n\n");
    return;
  }

  qsort(list, num_unique, sizeof(prime_freq_t), compare_prime_freq);

  printf(
      " Prime Factor (p > 13) | Fallback Block Occurrences (N_in / N_out)\n");
  printf(
      "-----------------------+------------------------------------------\n");
  for (size_t i = 0; i < num_unique; i++) {
    printf("  Radix-%-14zu | %zu occurrences\n", list[i].prime, list[i].count);
  }
  printf(
      "-----------------------+------------------------------------------\n");
  printf(" Unique Primes Needed  : ");
  for (size_t i = 0; i < num_unique; i++) {
    printf("%zu%s", list[i].prime, (i + 1 < num_unique) ? ", " : "\n\n");
  }
}

static void run_exhaustive_rate_grid_survey(void) {
  printf(
      "\n======================================================================"
      "===================================================================\n");
  printf(
      "         EXHAUSTIVE RATE GRID BACKEND SURVEY (19x19 RATES x 9 CHUNK "
      "SIZES = 3,249 CASES)\n");
  printf(
      "========================================================================"
      "=================================================================\n");
  printf(
      " Cell format: 'MR' = 100%% Mixed-Radix (all 9 chunk sizes), or 'M/9' = "
      "M of 9 chunk sizes using Mixed-Radix.\n\n");

  static const uint32_t standard_rates[] = {
      8000,   11025,  12000,  16000,  22050, 24000,  32000,
      44100,  48000,  64000,  88200,  96000, 128000, 176400,
      192000, 352800, 384000, 705600, 768000};
  static const char* labels[] = {"8k",    "11k",  "12k",   "16k",  "22k",
                                 "24k",   "32k",  "44.1k", "48k",  "64k",
                                 "88.2k", "96k",  "128k",  "176k", "192k",
                                 "353k",  "384k", "706k",  "768k"};
  size_t num_rates = sizeof(standard_rates) / sizeof(standard_rates[0]);

  static const size_t sample_sizes[] = {64,   128,  256,  512,  1024,
                                        2048, 4096, 8192, 16384};
  size_t num_sample_sizes = sizeof(sample_sizes) / sizeof(sample_sizes[0]);

  int support_matrix[19][19];
  size_t chunk_mr_counts[9] = {0};
  size_t prime_counts[100000] = {0};
  size_t total_configs = 0;
  size_t total_mr = 0;
  size_t total_bluestein = 0;

  for (size_t r_in = 0; r_in < num_rates; r_in++) {
    for (size_t r_out = 0; r_out < num_rates; r_out++) {
      uint32_t in_rate = standard_rates[r_in];
      uint32_t out_rate = standard_rates[r_out];
      uint32_t g = gcd_u32(in_rate, out_rate);
      uint32_t M = in_rate / g;
      uint32_t L = out_rate / g;

      int mr_for_pair = 0;
      for (size_t s = 0; s < num_sample_sizes; s++) {
        size_t chunk_size = sample_sizes[s];
        size_t k = (chunk_size + M - 1) / M;
        if (k == 0) k = 1;
        size_t n_in = k * (size_t)M;
        size_t n_out = k * (size_t)L;

        total_configs++;

        mixed_radix_fft_t* plan_in = mixed_radix_fft_create(n_in);
        mixed_radix_fft_t* plan_out = mixed_radix_fft_create(n_out);

        if (plan_in && plan_out) {
          mr_for_pair++;
          chunk_mr_counts[s]++;
          total_mr++;
        } else {
          total_bluestein++;
          if (!plan_in) analyze_factors(n_in, prime_counts, 100000);
          if (!plan_out) analyze_factors(n_out, prime_counts, 100000);
        }

        if (plan_in) mixed_radix_fft_free(plan_in);
        if (plan_out) mixed_radix_fft_free(plan_out);
      }
      support_matrix[r_in][r_out] = mr_for_pair;
    }
  }

  // Print 19x19 Matrix Table
  printf(" In \\ Out  |");
  for (size_t c = 0; c < num_rates; c++) {
    printf(" %-5s |", labels[c]);
  }
  printf("\n-----------+");
  for (size_t c = 0; c < num_rates; c++) {
    printf("-------+");
  }
  printf("\n");

  for (size_t r = 0; r < num_rates; r++) {
    printf(" %-9s |", labels[r]);
    for (size_t c = 0; c < num_rates; c++) {
      int cnt = support_matrix[r][c];
      if (cnt == (int)num_sample_sizes) {
        printf("  MR   |");
      } else if (cnt == 0) {
        printf(" Blue  |");
      } else {
        printf("  %d/9  |", cnt);
      }
    }
    printf("\n");
  }
  printf("-----------+");
  for (size_t c = 0; c < num_rates; c++) {
    printf("-------+");
  }
  printf("\n\n");

  // Print Chunk-Size Breakdown Table
  printf("--- Coverage by Sample Size ---\n");
  printf(" Sample Size | Pairs Supported / Total | Coverage | Status\n");
  printf(
      "-------------+-------------------------+----------+---------------------"
      "------\n");
  for (size_t s = 0; s < num_sample_sizes; s++) {
    size_t mr = chunk_mr_counts[s];
    size_t total_pairs = num_rates * num_rates;
    double pct = 100.0 * (double)mr / (double)total_pairs;
    printf(" %-11zu | %4zu / %-4zu              | %5.1f%%   | %s\n",
           sample_sizes[s], mr, total_pairs, pct,
           mr == total_pairs ? "100% Zero-Bluestein"
                             : "Partial Bluestein fallback");
  }
  printf(
      "-------------+-------------------------+----------+---------------------"
      "------\n");

  printf("\n--- Overall Summary ---\n");
  printf(" Total Rate-Grid Configurations : %zu\n", total_configs);
  printf(" Mixed-Radix Engine (Fast Paths): %zu (%.1f%%)\n", total_mr,
         100.0 * (double)total_mr / (double)total_configs);
  printf(" Bluestein Fallback Cases       : %zu (%.1f%%)\n", total_bluestein,
         100.0 * (double)total_bluestein / (double)total_configs);

  print_missing_primes_table(prime_counts, 100000,
                             "19x19 Full Rate Grid (3,249 cases)");
}

static void run_441_48k_family_survey(void) {
  printf(
      "\n======================================================================"
      "===================================================================\n");
  printf(
      "         44.1k & 48k FAMILY BACKEND SURVEY (10x10 RATES x 4 CHUNK SIZES "
      "= 400 CASES)\n");
  printf("         Chunk Sizes: 1024, 2048, 4096, 8192\n");
  printf(
      "========================================================================"
      "=================================================================\n");
  printf(
      " Cell format: 'MR' = 100%% Mixed-Radix (all 4 chunk sizes), or 'M/4' = "
      "M of 4 chunk sizes using Mixed-Radix.\n\n");

  static const uint32_t rates[] = {44100,  48000,  88200,  96000,  176400,
                                   192000, 352800, 384000, 705600, 768000};
  static const char* labels[] = {"44.1k", "48k",    "88.2k", "96k",    "176.4k",
                                 "192k",  "352.8k", "384k",  "705.6k", "768k"};
  const size_t num_rates = sizeof(rates) / sizeof(rates[0]);

  static const size_t sample_sizes[] = {1024, 2048, 4096, 8192};
  const size_t num_sample_sizes =
      sizeof(sample_sizes) / sizeof(sample_sizes[0]);

  int support_matrix[10][10];
  size_t chunk_mr_counts[4] = {0};
  size_t prime_counts[100000] = {0};
  size_t total_configs = 0;
  size_t total_mr = 0;
  size_t total_bluestein = 0;

  for (size_t r_in = 0; r_in < num_rates; r_in++) {
    for (size_t r_out = 0; r_out < num_rates; r_out++) {
      uint32_t in_rate = rates[r_in];
      uint32_t out_rate = rates[r_out];
      uint32_t g = gcd_u32(in_rate, out_rate);
      uint32_t M = in_rate / g;
      uint32_t L = out_rate / g;

      int mr_for_pair = 0;
      for (size_t s = 0; s < num_sample_sizes; s++) {
        size_t chunk_size = sample_sizes[s];
        size_t k = (chunk_size + M - 1) / M;
        if (k == 0) k = 1;
        size_t n_in = k * (size_t)M;
        size_t n_out = k * (size_t)L;

        total_configs++;

        mixed_radix_fft_t* plan_in = mixed_radix_fft_create(n_in);
        mixed_radix_fft_t* plan_out = mixed_radix_fft_create(n_out);

        if (plan_in && plan_out) {
          mr_for_pair++;
          chunk_mr_counts[s]++;
          total_mr++;
        } else {
          total_bluestein++;
          if (!plan_in) analyze_factors(n_in, prime_counts, 100000);
          if (!plan_out) analyze_factors(n_out, prime_counts, 100000);
        }

        if (plan_in) mixed_radix_fft_free(plan_in);
        if (plan_out) mixed_radix_fft_free(plan_out);
      }
      support_matrix[r_in][r_out] = mr_for_pair;
    }
  }

  // Print 10x10 Matrix Table
  printf(" In \\ Out  |");
  for (size_t c = 0; c < num_rates; c++) {
    printf(" %-8s |", labels[c]);
  }
  printf("\n-----------+");
  for (size_t c = 0; c < num_rates; c++) {
    printf("----------+");
  }
  printf("\n");

  for (size_t r = 0; r < num_rates; r++) {
    printf(" %-9s |", labels[r]);
    for (size_t c = 0; c < num_rates; c++) {
      int cnt = support_matrix[r][c];
      if (cnt == (int)num_sample_sizes) {
        printf("    MR    |");
      } else if (cnt == 0) {
        printf("   Blue   |");
      } else {
        printf("   %d/4   |", cnt);
      }
    }
    printf("\n");
  }
  printf("-----------+");
  for (size_t c = 0; c < num_rates; c++) {
    printf("----------+");
  }
  printf("\n\n");

  // Print Chunk-Size Breakdown Table
  printf("--- Coverage by Sample Size (44.1k/48k Family) ---\n");
  printf(" Sample Size | Pairs Supported / Total | Coverage | Status\n");
  printf(
      "-------------+-------------------------+----------+---------------------"
      "------\n");
  for (size_t s = 0; s < num_sample_sizes; s++) {
    size_t mr = chunk_mr_counts[s];
    size_t total_pairs = num_rates * num_rates;
    double pct = 100.0 * (double)mr / (double)total_pairs;
    printf(" %-11zu | %4zu / %-4zu              | %5.1f%%   | %s\n",
           sample_sizes[s], mr, total_pairs, pct,
           mr == total_pairs ? "100% Zero-Bluestein"
                             : "Partial Bluestein fallback");
  }
  printf(
      "-------------+-------------------------+----------+---------------------"
      "------\n");

  printf("\n--- 44.1k & 48k Family Summary ---\n");
  printf(" Total Configurations Evaluated : %zu\n", total_configs);
  printf(" Mixed-Radix Engine (Fast Paths): %zu (%.1f%%)\n", total_mr,
         100.0 * (double)total_mr / (double)total_configs);
  printf(" Bluestein Fallback Cases       : %zu (%.1f%%)\n", total_bluestein,
         100.0 * (double)total_bluestein / (double)total_configs);

  print_missing_primes_table(prime_counts, 100000,
                             "44.1k & 48k Family (400 cases)");
}

TEST(MixedRadixFFT_AllFactors_Benchmark) {
  printf(
      "\n======================================================================"
      "===================================================================\n");
  printf(
      "                                       MIXED-RADIX FFT RESAMPLER "
      "CONVERSION BENCHMARK\n");
  printf(
      "========================================================================"
      "=================================================================\n");
  printf(
      " Methodology: High-resolution monotonic timer, 5-trial median, warm-up "
      "runs, anti-DCE memory barriers.\n");
  printf(
      " Operations: Forward FFT (size N_in) + Backward IFFT (size N_out) "
      "matching resampler spectral pipeline.\n");

  // 1. Exhaustive rate grid backend survey (3,249 configurations)
  run_exhaustive_rate_grid_survey();

  // 2. Focused 44.1k & 48k Family Survey (1024, 2048, 4096, 8192 - 400 cases)
  run_441_48k_family_survey();

  // 3. vDSP_DFT_zop Supported Audio & Telecom Rates Conversion Matrix (8k to
  // 384k)
  run_rate_conversion_matrix_benchmark();

  // 4. Complete 44.1k & 48k Family Cross-Rate Conversion Matrix (44.1 kHz to
  // 768 kHz)
  run_441_48k_family_matrix_benchmark();

  printf(
      "\n======================================================================"
      "===================================================================\n");
  printf(" Benchmark completed successfully.\n");
  printf(
      "========================================================================"
      "=================================================================\n\n");
}

TEST_MAIN()
