#include "FFT/real_fft.h"

#include <stdlib.h>

#include "Config/config_error.h"
#include "FFT/real_fft_backend.h"
#include "Logging/app_logger.h"
#include "Utils/double_helpers.h"
#include "Utils/msan_compat.h"

__attribute__((unused)) static const logger_t g_logger = {"dsp.fft"};

// MARK: - Core RealFFT Context Structures

struct real_fft {
  size_t length; /**< Time-domain length (must be positive and even). */
  size_t
      spectrum_length; /**< Number of unique complex bins (= length / 2 + 1). */
  real_fft_backend_t* backend; /**< Dispatched backend implementation. */
};

struct real_fftf {
  size_t length; /**< Time-domain length (must be positive and even). */
  size_t
      spectrum_length; /**< Number of unique complex bins (= length / 2 + 1). */
  real_fftf_backend_t* backend; /**< Dispatched backend implementation. */
};

// MARK: - Public API Dispatch (Double-Precision)

size_t real_fft_get_length(const real_fft_t* fft) {
  return fft ? fft->length : 0;
}

size_t real_fft_get_spectrum_length(const real_fft_t* fft) {
  return fft ? fft->spectrum_length : 0;
}

void real_fft_forward(real_fft_t* fft, waveform_t real_in,
                      mutable_waveform_t spec_re, mutable_waveform_t spec_im) {
  if (fft && fft->backend && fft->backend->forward) {
    fft->backend->forward(fft->backend->ctx, real_in, spec_re, spec_im);
  }
}

void real_fft_inverse(real_fft_t* fft, waveform_t spec_re, waveform_t spec_im,
                      mutable_waveform_t real_out) {
  if (fft && fft->backend && fft->backend->inverse) {
    fft->backend->inverse(fft->backend->ctx, spec_re, spec_im, real_out);
  }
}

void real_fft_free(real_fft_t* fft) {
  if (fft) {
    if (fft->backend && fft->backend->free) {
      fft->backend->free(fft->backend->ctx);
    }
    free(fft);
  }
}

// MARK: - Public API Dispatch (Single-Precision Float)

size_t real_fftf_get_length(const real_fftf_t* fft) {
  return fft ? fft->length : 0;
}

size_t real_fftf_get_spectrum_length(const real_fftf_t* fft) {
  return fft ? fft->spectrum_length : 0;
}

void real_fftf_forward(real_fftf_t* fft, const float* real_in, float* spec_re,
                       float* spec_im) {
  if (fft && fft->backend && fft->backend->forward) {
    fft->backend->forward(fft->backend->ctx, real_in, spec_re, spec_im);
  }
}

void real_fftf_inverse(real_fftf_t* fft, const float* spec_re,
                       const float* spec_im, float* real_out) {
  if (fft && fft->backend && fft->backend->inverse) {
    fft->backend->inverse(fft->backend->ctx, spec_re, spec_im, real_out);
  }
}

void real_fftf_free(real_fftf_t* fft) {
  if (fft) {
    if (fft->backend && fft->backend->free) {
      fft->backend->free(fft->backend->ctx);
    }
    free(fft);
  }
}

// MARK: - Backend Implementations & Factories

#if defined(ENABLE_FFTW)

// ============================================================================
// FFTW3 Backend
// ============================================================================

#include <complex.h>  // IWYU pragma: keep
#include <fftw3.h>
#include <string.h>

struct fftw_real_fft_ctx {
  real_fft_backend_t base;
  size_t length;
  size_t spectrum_length;
  double* in_real;
  fftw_complex* out_complex;
  fftw_plan plan_forward;
  fftw_plan plan_inverse;
};

/**
 * @brief Forward FFT implementation using FFTW.
 *
 * Copies input to FFTW input buffer, executes plan, and copies results to
 * output.
 *
 * @param ctx Pointer to the fftw_real_fft_ctx.
 * @param real_in Input real waveform.
 * @param spec_re Output real part of the spectrum.
 * @param spec_im Output imaginary part of the spectrum.
 */
static void fftw_real_fft_forward(void* ctx, waveform_t real_in,
                                  mutable_waveform_t spec_re,
                                  mutable_waveform_t spec_im) {
  struct fftw_real_fft_ctx* fft = (struct fftw_real_fft_ctx*)ctx;
  memcpy(fft->in_real, real_in, fft->length * sizeof(double));
  fftw_execute(fft->plan_forward);
  CDSP_MSAN_UNPOISON(fft->out_complex,
                     fft->spectrum_length * sizeof(fftw_complex));
  for (size_t i = 0; i < fft->spectrum_length; i++) {
    spec_re[i] = __real__(fft->out_complex[i]);
    spec_im[i] = __imag__(fft->out_complex[i]);
  }
}

/**
 * @brief Inverse FFT implementation using FFTW.
 *
 * Copies input spectrum to FFTW complex buffer, executes plan, and copies
 * results to output.
 *
 * @param ctx Pointer to the fftw_real_fft_ctx.
 * @param spec_re Input real part of the spectrum.
 * @param spec_im Input imaginary part of the spectrum.
 * @param real_out Output real waveform.
 */
static void fftw_real_fft_inverse(void* ctx, waveform_t spec_re,
                                  waveform_t spec_im,
                                  mutable_waveform_t real_out) {
  struct fftw_real_fft_ctx* fft = (struct fftw_real_fft_ctx*)ctx;
  for (size_t i = 0; i < fft->spectrum_length; i++) {
    __real__(fft->out_complex[i]) = spec_re[i];
    __imag__(fft->out_complex[i]) = spec_im[i];
  }
  fftw_execute(fft->plan_inverse);
  CDSP_MSAN_UNPOISON(fft->in_real, fft->length * sizeof(double));
  memcpy(real_out, fft->in_real, fft->length * sizeof(double));
}

/**
 * @brief Free FFTW resources.
 *
 * Destroys plans and frees allocated buffers.
 *
 * @param ctx Pointer to the fftw_real_fft_ctx.
 */
static void fftw_real_fft_free(void* ctx) {
  struct fftw_real_fft_ctx* fft = (struct fftw_real_fft_ctx*)ctx;
  if (!fft) return;
  if (fft->plan_forward) fftw_destroy_plan(fft->plan_forward);
  if (fft->plan_inverse) fftw_destroy_plan(fft->plan_inverse);
  if (fft->in_real) fftw_free(fft->in_real);
  if (fft->out_complex) fftw_free(fft->out_complex);
  free(fft);
}

real_fft_t* real_fft_create(size_t length, config_error_t* err) {
  if (length == 0) {
    config_error_set(err, CONFIG_ERR_PARSE, "RealFFT: length must be positive");
    return NULL;
  }
  if (length % 2 != 0) {
    config_error_set(err, CONFIG_ERR_PARSE,
                     "RealFFT: length must be even, got %zu", length);
    return NULL;
  }
  real_fft_t* fft = (real_fft_t*)calloc(1, sizeof(real_fft_t));
  if (!fft) {
    config_error_set(err, CONFIG_ERR_PARSE, "Failed to allocate RealFFT");
    return NULL;
  }
  fft->length = length;
  fft->spectrum_length = length / 2 + 1;

  struct fftw_real_fft_ctx* ctx =
      (struct fftw_real_fft_ctx*)calloc(1, sizeof(struct fftw_real_fft_ctx));
  if (!ctx) {
    config_error_set(err, CONFIG_ERR_PARSE, "Failed to allocate FFTW context");
    free(fft);
    return NULL;
  }
  ctx->length = length;
  ctx->spectrum_length = length / 2 + 1;
  ctx->in_real = (double*)fftw_malloc(length * sizeof(double));
  ctx->out_complex =
      (fftw_complex*)fftw_malloc(ctx->spectrum_length * sizeof(fftw_complex));
  if (!ctx->in_real || !ctx->out_complex) {
    config_error_set(err, CONFIG_ERR_PARSE, "Failed to allocate FFTW buffers");
    fftw_real_fft_free(ctx);
    free(fft);
    return NULL;
  }
  ctx->plan_forward = fftw_plan_dft_r2c_1d((int)length, ctx->in_real,
                                           ctx->out_complex, FFTW_ESTIMATE);
  ctx->plan_inverse = fftw_plan_dft_c2r_1d((int)length, ctx->out_complex,
                                           ctx->in_real, FFTW_ESTIMATE);
  if (!ctx->plan_forward || !ctx->plan_inverse) {
    config_error_set(err, CONFIG_ERR_PARSE, "Failed to create FFTW plan");
    fftw_real_fft_free(ctx);
    free(fft);
    return NULL;
  }
  ctx->base.ctx = ctx;
  ctx->base.forward = fftw_real_fft_forward;
  ctx->base.inverse = fftw_real_fft_inverse;
  ctx->base.free = fftw_real_fft_free;

  fft->backend = &ctx->base;
  return fft;
}

struct fftwf_real_fft_ctx {
  real_fftf_backend_t base;
  size_t length;
  size_t spectrum_length;
  float* in_real;
  fftwf_complex* out_complex;
  fftwf_plan plan_forward;
  fftwf_plan plan_inverse;
};

static void fftwf_real_fft_forward(void* ctx, const float* real_in,
                                   float* spec_re, float* spec_im) {
  struct fftwf_real_fft_ctx* fft = (struct fftwf_real_fft_ctx*)ctx;
  memcpy(fft->in_real, real_in, fft->length * sizeof(float));
  fftwf_execute(fft->plan_forward);
  CDSP_MSAN_UNPOISON(fft->out_complex,
                     fft->spectrum_length * sizeof(fftwf_complex));
  for (size_t i = 0; i < fft->spectrum_length; i++) {
    spec_re[i] = __real__(fft->out_complex[i]);
    spec_im[i] = __imag__(fft->out_complex[i]);
  }
}

static void fftwf_real_fft_inverse(void* ctx, const float* spec_re,
                                   const float* spec_im, float* real_out) {
  struct fftwf_real_fft_ctx* fft = (struct fftwf_real_fft_ctx*)ctx;
  for (size_t i = 0; i < fft->spectrum_length; i++) {
    __real__(fft->out_complex[i]) = spec_re[i];
    __imag__(fft->out_complex[i]) = spec_im[i];
  }
  fftwf_execute(fft->plan_inverse);
  CDSP_MSAN_UNPOISON(fft->in_real, fft->length * sizeof(float));
  memcpy(real_out, fft->in_real, fft->length * sizeof(float));
}

static void fftwf_real_fft_free(void* ctx) {
  struct fftwf_real_fft_ctx* fft = (struct fftwf_real_fft_ctx*)ctx;
  if (!fft) return;
  if (fft->plan_forward) fftwf_destroy_plan(fft->plan_forward);
  if (fft->plan_inverse) fftwf_destroy_plan(fft->plan_inverse);
  if (fft->in_real) fftwf_free(fft->in_real);
  if (fft->out_complex) fftwf_free(fft->out_complex);
  free(fft);
}

real_fftf_t* real_fftf_create(size_t length) {
  if (length == 0 || length % 2 != 0) return NULL;
  real_fftf_t* fft = (real_fftf_t*)calloc(1, sizeof(real_fftf_t));
  if (!fft) return NULL;
  fft->length = length;
  fft->spectrum_length = length / 2 + 1;

  struct fftwf_real_fft_ctx* ctx =
      (struct fftwf_real_fft_ctx*)calloc(1, sizeof(struct fftwf_real_fft_ctx));
  if (!ctx) {
    free(fft);
    return NULL;
  }
  ctx->length = length;
  ctx->spectrum_length = length / 2 + 1;
  ctx->in_real = (float*)fftwf_malloc(length * sizeof(float));
  ctx->out_complex = (fftwf_complex*)fftwf_malloc(ctx->spectrum_length *
                                                  sizeof(fftwf_complex));
  if (!ctx->in_real || !ctx->out_complex) {
    fftwf_real_fft_free(ctx);
    free(fft);
    return NULL;
  }
  ctx->plan_forward = fftwf_plan_dft_r2c_1d((int)length, ctx->in_real,
                                            ctx->out_complex, FFTW_ESTIMATE);
  ctx->plan_inverse = fftwf_plan_dft_c2r_1d((int)length, ctx->out_complex,
                                            ctx->in_real, FFTW_ESTIMATE);
  if (!ctx->plan_forward || !ctx->plan_inverse) {
    fftwf_real_fft_free(ctx);
    free(fft);
    return NULL;
  }
  ctx->base.ctx = ctx;
  ctx->base.forward = fftwf_real_fft_forward;
  ctx->base.inverse = fftwf_real_fft_inverse;
  ctx->base.free = fftwf_real_fft_free;

  fft->backend = (real_fftf_backend_t*)&ctx->base;
  return fft;
}

#else

// ============================================================================
// Native High-Performance Cross-Platform Backend:
// 1. Power-of-2 (>= 8): vDSP (Apple Accelerate) or Pure-C Real FFT
// 2. Arbitrary Even (Non-Power-of-2): ComplexInner + Mixed-Radix / Bluestein
// ============================================================================

#include "FFT/arbitrary_complex_fft.h"
#include "FFT/bluestein_fft.h"
#include "FFT/complex_inner_real_fft.h"
#include "FFT/mixed_radix_fft.h"
#include "FFT/pure_real_fft.h"

#if defined(ENABLE_ACCELERATE)
#include "FFT/vdsp_real_fft.h"
#endif

real_fft_t* real_fft_create(size_t length, config_error_t* err) {
  if (length == 0) {
    config_error_set(err, CONFIG_ERR_PARSE, "RealFFT: length must be positive");
    logger_error(&g_logger, "RealFFT: length must be positive");
    return NULL;
  }
  if (length % 2 != 0) {
    config_error_set(err, CONFIG_ERR_PARSE,
                     "RealFFT: length must be even, got %zu", length);
    logger_error(&g_logger, "RealFFT: length must be even, got %zu", length);
    return NULL;
  }
  real_fft_t* fft = (real_fft_t*)calloc(1, sizeof(real_fft_t));
  if (!fft) {
    config_error_set(err, CONFIG_ERR_PARSE, "Failed to allocate RealFFT");
    logger_error(&g_logger, "Failed to allocate RealFFT");
    return NULL;
  }
  fft->length = length;
  fft->spectrum_length = length / 2 + 1;

  // Branch 1: Power-of-2 (>= 8)
  if ((length & (length - 1)) == 0 && length >= 8) {
#if defined(ENABLE_ACCELERATE)
    vdsp_real_fft_t* vdsp = vdsp_real_fft_create(length);
    if (vdsp) {
      fft->backend = vdsp_real_fft_as_backend(vdsp);
      logger_debug(&g_logger,
                   "RealFFT created using vDSP Real FFT backend (length=%zu)",
                   length);
      return fft;
    }
#else
    pure_real_fft_t* pure = pure_real_fft_create(length);
    if (pure) {
      fft->backend = pure_real_fft_as_backend(pure);
      logger_debug(&g_logger,
                   "RealFFT created using Pure-C Real FFT backend (length=%zu)",
                   length);
      return fft;
    }
#endif
  }

  // Branch 2: Arbitrary Even (Non-Power-of-2 or pow2 < 8)
  // Build 2N-point real FFT from an N-point complex FFT via ComplexInnerRealFFT.
  size_t half_n = length / 2;
  arbitrary_complex_fft_t* inner = NULL;
  const char* backend_name = "unknown";

  mixed_radix_fft_t* mr = mixed_radix_fft_create(half_n);
  if (mr) {
    inner = mixed_radix_fft_as_arbitrary(mr);
    backend_name = "Mixed-Radix FFT";
  } else {
    bluestein_fft_t* bs = bluestein_fft_create(half_n, err);
    if (bs) {
      inner = bluestein_fft_as_arbitrary(bs);
      backend_name = "Bluestein FFT";
    }
  }

  if (!inner) {
    logger_error(&g_logger,
                 "Failed to initialize any complex FFT backend for half_n=%zu",
                 half_n);
    free(fft);
    return NULL;
  }

  complex_inner_real_fft_t* complex_inner =
      complex_inner_real_fft_create(length, inner);
  if (!complex_inner) {
    config_error_set(err, CONFIG_ERR_PARSE,
                     "Failed to allocate ComplexInnerRealFFT");
    logger_error(&g_logger, "Failed to allocate ComplexInnerRealFFT");
    arbitrary_complex_fft_free(inner);
    free(fft);
    return NULL;
  }

  fft->backend = complex_inner_real_fft_as_backend(complex_inner);
  logger_debug(&g_logger,
               "RealFFT created using ComplexInner + %s backend (length=%zu)",
               backend_name, length);
  return fft;
}

real_fftf_t* real_fftf_create(size_t length) {
  if (length == 0 || length % 2 != 0) return NULL;
  real_fftf_t* fft = (real_fftf_t*)calloc(1, sizeof(real_fftf_t));
  if (!fft) return NULL;
  fft->length = length;
  fft->spectrum_length = length / 2 + 1;

  if ((length & (length - 1)) == 0 && length >= 8) {
#if defined(ENABLE_ACCELERATE)
    vdsp_real_fftf_t* vdsp = vdsp_real_fftf_create(length);
    if (vdsp) {
      fft->backend = vdsp_real_fftf_as_backend(vdsp);
      return fft;
    }
#else
    pure_real_fftf_t* pure = pure_real_fftf_create(length);
    if (pure) {
      fft->backend = pure_real_fftf_as_backend(pure);
      return fft;
    }
#endif
  }

  free(fft);
  return NULL;
}

#endif
