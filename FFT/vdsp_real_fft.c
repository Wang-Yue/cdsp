#if defined(ENABLE_ACCELERATE)
// vDSP `fft_zrip` backend for power-of-two real-FFT lengths.
//
// Selected by `RealFFT.init` when `length` is a power of two
// `≥ 8`. vDSP's hand-tuned NEON/SSE radix-2 split-complex real FFT is
// the fastest path on Apple Silicon — for our resampler matrix it
// roughly doubles the throughput of the "complex-FFT-via-half-N" path
// for sizes like 1024/2048/4096.

#include "FFT/vdsp_real_fft.h"

#include <Accelerate/Accelerate.h>
#include <stdlib.h>
#include <string.h>

struct vdsp_real_fft {
  real_fft_backend_t base;
  size_t half_n;
  vDSP_Length log2n;
  FFTSetupD setup;
  double* work_re;
  double* work_im;
};

/**
 * @brief Wrapper for vdsp_real_fft_forward to match the real_fft_backend_t
 * interface.
 *
 * @param ctx Pointer to the vdsp_real_fft_t context.
 * @param real_in Input waveform containing real samples.
 * @param spec_re Output waveform for the real part of the spectrum.
 * @param spec_im Output waveform for the imaginary part of the spectrum.
 */
static void vdsp_real_fft_forward_wrapper(void* ctx, waveform_t real_in,
                                          mutable_waveform_t spec_re,
                                          mutable_waveform_t spec_im) {
  vdsp_real_fft_forward((vdsp_real_fft_t*)ctx, real_in, spec_re, spec_im);
}

/**
 * @brief Wrapper for vdsp_real_fft_inverse to match the real_fft_backend_t
 * interface.
 *
 * @param ctx Pointer to the vdsp_real_fft_t context.
 * @param spec_re Input waveform for the real part of the spectrum.
 * @param spec_im Input waveform for the imaginary part of the spectrum.
 * @param real_out Output waveform for the reconstructed real samples.
 */
static void vdsp_real_fft_inverse_wrapper(void* ctx, waveform_t spec_re,
                                          waveform_t spec_im,
                                          mutable_waveform_t real_out) {
  vdsp_real_fft_inverse((vdsp_real_fft_t*)ctx, spec_re, spec_im, real_out);
}

/**
 * @brief Wrapper for vdsp_real_fft_free to match the real_fft_backend_t
 * interface.
 *
 * @param ctx Pointer to the vdsp_real_fft_t context.
 */
static void vdsp_real_fft_free_wrapper(void* ctx) {
  vdsp_real_fft_free((vdsp_real_fft_t*)ctx);
}

vdsp_real_fft_t* vdsp_real_fft_create(size_t length) {
  // vDSP FFT requires length to be a power of 2 and >= 8.
  if (length < 8 || (length & (length - 1)) != 0) return NULL;

  // Calculate log2(length) as required by vDSP setup.
  vDSP_Length log2n = 0;
  size_t temp = length;
  while (temp > 1) {
    log2n++;
    temp >>= 1;
  }
  FFTSetupD setup = vDSP_create_fftsetupD(log2n, kFFTRadix2);
  if (!setup) return NULL;

  vdsp_real_fft_t* fft = (vdsp_real_fft_t*)calloc(1, sizeof(vdsp_real_fft_t));
  if (!fft) {
    vDSP_destroy_fftsetupD(setup);
    return NULL;
  }
  fft->setup = setup;
  size_t half_n = length / 2;
  fft->work_re = (double*)calloc(half_n, sizeof(double));
  fft->work_im = (double*)calloc(half_n, sizeof(double));
  if (!fft->work_re || !fft->work_im) {
    vdsp_real_fft_free(fft);
    return NULL;
  }

  fft->base.ctx = fft;
  fft->base.forward = vdsp_real_fft_forward_wrapper;
  fft->base.inverse = vdsp_real_fft_inverse_wrapper;
  fft->base.free = vdsp_real_fft_free_wrapper;
  fft->half_n = half_n;
  fft->log2n = log2n;
  return fft;
}

void vdsp_real_fft_forward(vdsp_real_fft_t* fft, waveform_t real_in,
                           mutable_waveform_t spec_re,
                           mutable_waveform_t spec_im) {
  if (!fft) return;
  size_t n = fft->half_n;
  // Deinterleave 2N real samples directly into spec_re and spec_im:
  // spec_re[k] = realIn[2k], spec_im[k] = realIn[2k+1].
  DSPDoubleSplitComplex split = {spec_re, spec_im};
  vDSP_ctozD((const DSPDoubleComplex*)real_in, 2, &split, 1, (vDSP_Length)n);
  // In-place real-to-complex forward FFT. vDSP scales by 2.
  vDSP_fft_zripD(fft->setup, &split, 1, fft->log2n, FFT_FORWARD);

  // Unpack vDSP's format (Nyquist in imagp[0]) and scale by 0.5 in-place.
  // Layout mapping into flat (N+1)-bin spectrum layout:
  //   specRe[0]   = vDSP_DC / 2 = unscaled DC
  //   specIm[0]   = 0
  //   specRe[k]   = vDSP_Re[k] / 2   for k = 1..N-1
  //   specIm[k]   = vDSP_Im[k] / 2   for k = 1..N-1
  //   specRe[N]   = vDSP_Im[0] / 2   (Nyquist was packed in imagp[0])
  //   specIm[N]   = 0
  double nyquist = spec_im[0] * 0.5;
  double half = 0.5;
  vDSP_vsmulD(spec_re, 1, &half, spec_re, 1, (vDSP_Length)n);
  if (n > 1) {
    vDSP_vsmulD(spec_im + 1, 1, &half, spec_im + 1, 1, (vDSP_Length)(n - 1));
  }
  spec_im[0] = 0.0;
  spec_re[n] = nyquist;
  spec_im[n] = 0.0;
}

void vdsp_real_fft_inverse(vdsp_real_fft_t* fft, waveform_t spec_re,
                           waveform_t spec_im, mutable_waveform_t real_out) {
  if (!fft || !fft->work_re || !fft->work_im) return;
  size_t n = fft->half_n;
  memcpy(fft->work_re, spec_re, n * sizeof(double));
  if (n > 1) {
    memcpy(fft->work_im + 1, spec_im + 1, (n - 1) * sizeof(double));
  }
  fft->work_im[0] = spec_re[n];

  DSPDoubleSplitComplex split = {fft->work_re, fft->work_im};
  vDSP_fft_zripD(fft->setup, &split, 1, fft->log2n, FFT_INVERSE);

  // Asymmetric vDSP scaling: forward applies a `2×` factor, inverse does not.
  // Feeding unscaled bins (we already halved the forward output) directly
  // produces the unnormalised IDFT result — `length · signal` — which is
  // exactly the RealFFT convention. No extra scaling needed here.
  //
  // Re-interleave split-complex in-place back to 2N reals:
  // realOut[2k] = split.real[k], realOut[2k+1] = split.imag[k].
  vDSP_ztocD(&split, 1, (DSPDoubleComplex*)real_out, 2, (vDSP_Length)n);
}

void vdsp_real_fft_free(vdsp_real_fft_t* fft) {
  if (!fft) return;
  if (fft->setup) vDSP_destroy_fftsetupD(fft->setup);
  if (fft->work_re) free(fft->work_re);
  if (fft->work_im) free(fft->work_im);
  free(fft);
}

// Single-precision (float) vDSP FFT implementation
struct vdsp_real_fftf {
  real_fftf_backend_t base;
  size_t half_n;
  vDSP_Length log2n;
  FFTSetup setup;
  float* work_re;
  float* work_im;
};

static void vdsp_real_fftf_forward_wrapper(void* ctx, const float* real_in,
                                           float* spec_re, float* spec_im) {
  vdsp_real_fftf_forward((vdsp_real_fftf_t*)ctx, real_in, spec_re, spec_im);
}

static void vdsp_real_fftf_inverse_wrapper(void* ctx, const float* spec_re,
                                           const float* spec_im,
                                           float* real_out) {
  vdsp_real_fftf_inverse((vdsp_real_fftf_t*)ctx, spec_re, spec_im, real_out);
}

static void vdsp_real_fftf_free_wrapper(void* ctx) {
  vdsp_real_fftf_free((vdsp_real_fftf_t*)ctx);
}

vdsp_real_fftf_t* vdsp_real_fftf_create(size_t length) {
  if (length < 8 || (length & (length - 1)) != 0) return NULL;

  vDSP_Length log2n = 0;
  size_t temp = length;
  while (temp > 1) {
    log2n++;
    temp >>= 1;
  }
  FFTSetup setup = vDSP_create_fftsetup(log2n, kFFTRadix2);
  if (!setup) return NULL;

  vdsp_real_fftf_t* fft =
      (vdsp_real_fftf_t*)calloc(1, sizeof(vdsp_real_fftf_t));
  if (!fft) {
    vDSP_destroy_fftsetup(setup);
    return NULL;
  }
  fft->setup = setup;
  size_t half_n = length / 2;
  fft->work_re = (float*)calloc(half_n, sizeof(float));
  fft->work_im = (float*)calloc(half_n, sizeof(float));
  if (!fft->work_re || !fft->work_im) {
    vdsp_real_fftf_free(fft);
    return NULL;
  }

  fft->base.ctx = fft;
  fft->base.forward = vdsp_real_fftf_forward_wrapper;
  fft->base.inverse = vdsp_real_fftf_inverse_wrapper;
  fft->base.free = vdsp_real_fftf_free_wrapper;
  fft->half_n = half_n;
  fft->log2n = log2n;
  fft->setup = setup;
  return fft;
}

void vdsp_real_fftf_forward(vdsp_real_fftf_t* fft, const float* real_in,
                            float* spec_re, float* spec_im) {
  if (!fft) return;
  size_t n = fft->half_n;
  DSPSplitComplex split = {spec_re, spec_im};
  vDSP_ctoz((const DSPComplex*)real_in, 2, &split, 1, (vDSP_Length)n);
  vDSP_fft_zrip(fft->setup, &split, 1, fft->log2n, FFT_FORWARD);

  // Unpack vDSP's format (Nyquist in imagp[0]) and scale by 0.5 in-place.
  // Layout mapping into flat (N+1)-bin spectrum layout:
  //   specRe[0]   = vDSP_DC / 2 = unscaled DC
  //   specIm[0]   = 0
  //   specRe[k]   = vDSP_Re[k] / 2   for k = 1..N-1
  //   specIm[k]   = vDSP_Im[k] / 2   for k = 1..N-1
  //   specRe[N]   = vDSP_Im[0] / 2   (Nyquist was packed in imagp[0])
  //   specIm[N]   = 0
  float nyquist = spec_im[0] * 0.5f;
  float half = 0.5f;
  vDSP_vsmul(spec_re, 1, &half, spec_re, 1, (vDSP_Length)n);
  if (n > 1) {
    vDSP_vsmul(spec_im + 1, 1, &half, spec_im + 1, 1, (vDSP_Length)(n - 1));
  }
  spec_im[0] = 0.0f;
  spec_re[n] = nyquist;
  spec_im[n] = 0.0f;
}

void vdsp_real_fftf_inverse(vdsp_real_fftf_t* fft, const float* spec_re,
                            const float* spec_im, float* real_out) {
  if (!fft || !fft->work_re || !fft->work_im) return;
  size_t n = fft->half_n;
  memcpy(fft->work_re, spec_re, n * sizeof(float));
  if (n > 1) {
    memcpy(fft->work_im + 1, spec_im + 1, (n - 1) * sizeof(float));
  }
  fft->work_im[0] = spec_re[n];

  DSPSplitComplex split = {fft->work_re, fft->work_im};
  vDSP_fft_zrip(fft->setup, &split, 1, fft->log2n, FFT_INVERSE);

  // Asymmetric vDSP scaling: forward applies a `2×` factor, inverse does not.
  // Feeding unscaled bins (we already halved the forward output) directly
  // produces the unnormalised IDFT result — `length · signal` — which is
  // exactly the RealFFT convention. No extra scaling needed here.
  //
  // Re-interleave split-complex in-place back to 2N reals:
  // realOut[2k] = split.real[k], realOut[2k+1] = split.imag[k].
  vDSP_ztoc(&split, 1, (DSPComplex*)real_out, 2, (vDSP_Length)n);
}

void vdsp_real_fftf_free(vdsp_real_fftf_t* fft) {
  if (!fft) return;
  if (fft->setup) vDSP_destroy_fftsetup(fft->setup);
  if (fft->work_re) free(fft->work_re);
  if (fft->work_im) free(fft->work_im);
  free(fft);
}

#endif  // ENABLE_ACCELERATE
