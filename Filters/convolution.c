#include "Filters/convolution.h"

#include <complex.h>
#include <fftw3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Audio/processing_parameters.h"
#include "Audio/sample_conversion.h"
#include "Config/config_error.h"
#include "Config/engine_config_types.h"
#include "Config/filter_config_types.h"
#include "Filters/filter.h"
#include "Utils/cdsp_path.h"
#include "Utils/double_helpers.h"

struct convolution_filter {
  char name[64];
  size_t chunk_size;
  size_t num_segments;
  size_t spec_len;
  fftw_plan plan_forward;
  fftw_plan plan_inverse;
  fftw_complex** coeffs_f;
  fftw_complex** hist_f;
  fftw_complex* temp_buf;
  size_t write_idx;
  double* overlap_buffer;
  double* time_buf;
  double* out_buf;
  double* input_buffer;
  double* output_buffer;
  size_t buf_pos;
};

typedef struct convolution_filter convolution_filter_t;

#if defined(__aarch64__) || defined(__ARM_NEON)
#include <arm_neon.h>

static inline void complex_mul_simd(fftw_complex* restrict result,
                                    const fftw_complex* restrict slice_a,
                                    const fftw_complex* restrict slice_b,
                                    size_t len) {
  float64x2_t sign = {-1.0, 1.0};
  size_t chunks_4 = len / 4;
  double* r_ptr = (double*)result;
  const double* a_ptr = (const double*)slice_a;
  const double* b_ptr = (const double*)slice_b;

  for (size_t i = 0; i < chunks_4; i++) {
    size_t off = i * 8;
    float64x2_t a0 = vld1q_f64(a_ptr + off);
    float64x2_t b0 = vld1q_f64(b_ptr + off);
    float64x2_t a1 = vld1q_f64(a_ptr + off + 2);
    float64x2_t b1 = vld1q_f64(b_ptr + off + 2);
    float64x2_t a2 = vld1q_f64(a_ptr + off + 4);
    float64x2_t b2 = vld1q_f64(b_ptr + off + 4);
    float64x2_t a3 = vld1q_f64(a_ptr + off + 6);
    float64x2_t b3 = vld1q_f64(b_ptr + off + 6);

    float64x2_t a_re0 = vdupq_laneq_f64(a0, 0);
    float64x2_t a_im0 = vdupq_laneq_f64(a0, 1);
    float64x2_t a_re1 = vdupq_laneq_f64(a1, 0);
    float64x2_t a_im1 = vdupq_laneq_f64(a1, 1);
    float64x2_t a_re2 = vdupq_laneq_f64(a2, 0);
    float64x2_t a_im2 = vdupq_laneq_f64(a2, 1);
    float64x2_t a_re3 = vdupq_laneq_f64(a3, 0);
    float64x2_t a_im3 = vdupq_laneq_f64(a3, 1);

    float64x2_t b_sw0 = vmulq_f64(vextq_f64(b0, b0, 1), sign);
    float64x2_t b_sw1 = vmulq_f64(vextq_f64(b1, b1, 1), sign);
    float64x2_t b_sw2 = vmulq_f64(vextq_f64(b2, b2, 1), sign);
    float64x2_t b_sw3 = vmulq_f64(vextq_f64(b3, b3, 1), sign);

    vst1q_f64(r_ptr + off, vfmaq_f64(vmulq_f64(a_re0, b0), a_im0, b_sw0));
    vst1q_f64(r_ptr + off + 2, vfmaq_f64(vmulq_f64(a_re1, b1), a_im1, b_sw1));
    vst1q_f64(r_ptr + off + 4, vfmaq_f64(vmulq_f64(a_re2, b2), a_im2, b_sw2));
    vst1q_f64(r_ptr + off + 6, vfmaq_f64(vmulq_f64(a_re3, b3), a_im3, b_sw3));
  }
  for (size_t j = chunks_4 * 4; j < len; j++) {
    size_t off = j * 2;
    float64x2_t a0 = vld1q_f64(a_ptr + off);
    float64x2_t b0 = vld1q_f64(b_ptr + off);
    float64x2_t a_re0 = vdupq_laneq_f64(a0, 0);
    float64x2_t a_im0 = vdupq_laneq_f64(a0, 1);
    float64x2_t b_sw0 = vmulq_f64(vextq_f64(b0, b0, 1), sign);
    vst1q_f64(r_ptr + off, vfmaq_f64(vmulq_f64(a_re0, b0), a_im0, b_sw0));
  }
}

static inline void complex_fma_simd(fftw_complex* restrict result,
                                    const fftw_complex* restrict slice_a,
                                    const fftw_complex* restrict slice_b,
                                    size_t len) {
  float64x2_t sign = {-1.0, 1.0};
  size_t chunks_4 = len / 4;
  double* r_ptr = (double*)result;
  const double* a_ptr = (const double*)slice_a;
  const double* b_ptr = (const double*)slice_b;

  for (size_t i = 0; i < chunks_4; i++) {
    size_t off = i * 8;
    float64x2_t acc0 = vld1q_f64(r_ptr + off);
    float64x2_t acc1 = vld1q_f64(r_ptr + off + 2);
    float64x2_t acc2 = vld1q_f64(r_ptr + off + 4);
    float64x2_t acc3 = vld1q_f64(r_ptr + off + 6);

    float64x2_t a0 = vld1q_f64(a_ptr + off);
    float64x2_t b0 = vld1q_f64(b_ptr + off);
    float64x2_t a1 = vld1q_f64(a_ptr + off + 2);
    float64x2_t b1 = vld1q_f64(b_ptr + off + 2);
    float64x2_t a2 = vld1q_f64(a_ptr + off + 4);
    float64x2_t b2 = vld1q_f64(b_ptr + off + 4);
    float64x2_t a3 = vld1q_f64(a_ptr + off + 6);
    float64x2_t b3 = vld1q_f64(b_ptr + off + 6);

    float64x2_t a_re0 = vdupq_laneq_f64(a0, 0);
    float64x2_t a_im0 = vdupq_laneq_f64(a0, 1);
    float64x2_t a_re1 = vdupq_laneq_f64(a1, 0);
    float64x2_t a_im1 = vdupq_laneq_f64(a1, 1);
    float64x2_t a_re2 = vdupq_laneq_f64(a2, 0);
    float64x2_t a_im2 = vdupq_laneq_f64(a2, 1);
    float64x2_t a_re3 = vdupq_laneq_f64(a3, 0);
    float64x2_t a_im3 = vdupq_laneq_f64(a3, 1);

    float64x2_t b_sw0 = vmulq_f64(vextq_f64(b0, b0, 1), sign);
    float64x2_t b_sw1 = vmulq_f64(vextq_f64(b1, b1, 1), sign);
    float64x2_t b_sw2 = vmulq_f64(vextq_f64(b2, b2, 1), sign);
    float64x2_t b_sw3 = vmulq_f64(vextq_f64(b3, b3, 1), sign);

    float64x2_t prod0 = vfmaq_f64(vmulq_f64(a_re0, b0), a_im0, b_sw0);
    float64x2_t prod1 = vfmaq_f64(vmulq_f64(a_re1, b1), a_im1, b_sw1);
    float64x2_t prod2 = vfmaq_f64(vmulq_f64(a_re2, b2), a_im2, b_sw2);
    float64x2_t prod3 = vfmaq_f64(vmulq_f64(a_re3, b3), a_im3, b_sw3);

    vst1q_f64(r_ptr + off, vaddq_f64(acc0, prod0));
    vst1q_f64(r_ptr + off + 2, vaddq_f64(acc1, prod1));
    vst1q_f64(r_ptr + off + 4, vaddq_f64(acc2, prod2));
    vst1q_f64(r_ptr + off + 6, vaddq_f64(acc3, prod3));
  }
  for (size_t j = chunks_4 * 4; j < len; j++) {
    size_t off = j * 2;
    float64x2_t acc0 = vld1q_f64(r_ptr + off);
    float64x2_t a0 = vld1q_f64(a_ptr + off);
    float64x2_t b0 = vld1q_f64(b_ptr + off);
    float64x2_t a_re0 = vdupq_laneq_f64(a0, 0);
    float64x2_t a_im0 = vdupq_laneq_f64(a0, 1);
    float64x2_t b_sw0 = vmulq_f64(vextq_f64(b0, b0, 1), sign);
    float64x2_t prod0 = vfmaq_f64(vmulq_f64(a_re0, b0), a_im0, b_sw0);
    vst1q_f64(r_ptr + off, vaddq_f64(acc0, prod0));
  }
}

#elif (defined(__x86_64__) || defined(_M_X64)) && defined(__AVX2__) && \
    defined(__FMA__)
#include <immintrin.h>

static inline void complex_mul_simd(fftw_complex* restrict result,
                                    const fftw_complex* restrict slice_a,
                                    const fftw_complex* restrict slice_b,
                                    size_t len) {
  size_t chunks_8 = len / 8;
  double* r_ptr = (double*)result;
  const double* a_ptr = (const double*)slice_a;
  const double* b_ptr = (const double*)slice_b;

  for (size_t i = 0; i < chunks_8; i++) {
    size_t off = i * 16;
    __m256d a0 = _mm256_loadu_pd(a_ptr + off);
    __m256d b0 = _mm256_loadu_pd(b_ptr + off);
    __m256d a1 = _mm256_loadu_pd(a_ptr + off + 4);
    __m256d b1 = _mm256_loadu_pd(b_ptr + off + 4);
    __m256d a2 = _mm256_loadu_pd(a_ptr + off + 8);
    __m256d b2 = _mm256_loadu_pd(b_ptr + off + 8);
    __m256d a3 = _mm256_loadu_pd(a_ptr + off + 12);
    __m256d b3 = _mm256_loadu_pd(b_ptr + off + 12);

    __m256d a_re0 = _mm256_movedup_pd(a0);
    __m256d a_im0 = _mm256_permute_pd(a0, 0xF);
    __m256d a_re1 = _mm256_movedup_pd(a1);
    __m256d a_im1 = _mm256_permute_pd(a1, 0xF);
    __m256d a_re2 = _mm256_movedup_pd(a2);
    __m256d a_im2 = _mm256_permute_pd(a2, 0xF);
    __m256d a_re3 = _mm256_movedup_pd(a3);
    __m256d a_im3 = _mm256_permute_pd(a3, 0xF);

    __m256d b_sw0 = _mm256_permute_pd(b0, 0x5);
    __m256d b_sw1 = _mm256_permute_pd(b1, 0x5);
    __m256d b_sw2 = _mm256_permute_pd(b2, 0x5);
    __m256d b_sw3 = _mm256_permute_pd(b3, 0x5);

    _mm256_storeu_pd(r_ptr + off, _mm256_fmaddsub_pd(
                                      a_re0, b0, _mm256_mul_pd(a_im0, b_sw0)));
    _mm256_storeu_pd(
        r_ptr + off + 4,
        _mm256_fmaddsub_pd(a_re1, b1, _mm256_mul_pd(a_im1, b_sw1)));
    _mm256_storeu_pd(
        r_ptr + off + 8,
        _mm256_fmaddsub_pd(a_re2, b2, _mm256_mul_pd(a_im2, b_sw2)));
    _mm256_storeu_pd(
        r_ptr + off + 12,
        _mm256_fmaddsub_pd(a_re3, b3, _mm256_mul_pd(a_im3, b_sw3)));
  }

  size_t tail_start = chunks_8 * 8;
  for (size_t i = tail_start; i < len; i++) {
    double a_re = ((const double*)slice_a)[2 * i];
    double a_im = ((const double*)slice_a)[2 * i + 1];
    double b_re = ((const double*)slice_b)[2 * i];
    double b_im = ((const double*)slice_b)[2 * i + 1];
    ((double*)result)[2 * i] = a_re * b_re - a_im * b_im;
    ((double*)result)[2 * i + 1] = a_re * b_im + a_im * b_re;
  }
}

static inline void complex_fma_simd(fftw_complex* restrict result,
                                    const fftw_complex* restrict slice_a,
                                    const fftw_complex* restrict slice_b,
                                    size_t len) {
  size_t chunks_8 = len / 8;
  double* r_ptr = (double*)result;
  const double* a_ptr = (const double*)slice_a;
  const double* b_ptr = (const double*)slice_b;

  for (size_t i = 0; i < chunks_8; i++) {
    size_t off = i * 16;
    __m256d acc0 = _mm256_loadu_pd(r_ptr + off);
    __m256d acc1 = _mm256_loadu_pd(r_ptr + off + 4);
    __m256d acc2 = _mm256_loadu_pd(r_ptr + off + 8);
    __m256d acc3 = _mm256_loadu_pd(r_ptr + off + 12);

    __m256d a0 = _mm256_loadu_pd(a_ptr + off);
    __m256d b0 = _mm256_loadu_pd(b_ptr + off);
    __m256d a1 = _mm256_loadu_pd(a_ptr + off + 4);
    __m256d b1 = _mm256_loadu_pd(b_ptr + off + 4);
    __m256d a2 = _mm256_loadu_pd(a_ptr + off + 8);
    __m256d b2 = _mm256_loadu_pd(b_ptr + off + 8);
    __m256d a3 = _mm256_loadu_pd(a_ptr + off + 12);
    __m256d b3 = _mm256_loadu_pd(b_ptr + off + 12);

    __m256d a_re0 = _mm256_movedup_pd(a0);
    __m256d a_im0 = _mm256_permute_pd(a0, 0xF);
    __m256d a_re1 = _mm256_movedup_pd(a1);
    __m256d a_im1 = _mm256_permute_pd(a1, 0xF);
    __m256d a_re2 = _mm256_movedup_pd(a2);
    __m256d a_im2 = _mm256_permute_pd(a2, 0xF);
    __m256d a_re3 = _mm256_movedup_pd(a3);
    __m256d a_im3 = _mm256_permute_pd(a3, 0xF);

    __m256d b_sw0 = _mm256_permute_pd(b0, 0x5);
    __m256d b_sw1 = _mm256_permute_pd(b1, 0x5);
    __m256d b_sw2 = _mm256_permute_pd(b2, 0x5);
    __m256d b_sw3 = _mm256_permute_pd(b3, 0x5);

    __m256d prod0 = _mm256_fmaddsub_pd(a_re0, b0, _mm256_mul_pd(a_im0, b_sw0));
    __m256d prod1 = _mm256_fmaddsub_pd(a_re1, b1, _mm256_mul_pd(a_im1, b_sw1));
    __m256d prod2 = _mm256_fmaddsub_pd(a_re2, b2, _mm256_mul_pd(a_im2, b_sw2));
    __m256d prod3 = _mm256_fmaddsub_pd(a_re3, b3, _mm256_mul_pd(a_im3, b_sw3));

    _mm256_storeu_pd(r_ptr + off, _mm256_add_pd(acc0, prod0));
    _mm256_storeu_pd(r_ptr + off + 4, _mm256_add_pd(acc1, prod1));
    _mm256_storeu_pd(r_ptr + off + 8, _mm256_add_pd(acc2, prod2));
    _mm256_storeu_pd(r_ptr + off + 12, _mm256_add_pd(acc3, prod3));
  }

  size_t tail_start = chunks_8 * 8;
  for (size_t i = tail_start; i < len; i++) {
    double a_re = ((const double*)slice_a)[2 * i];
    double a_im = ((const double*)slice_a)[2 * i + 1];
    double b_re = ((const double*)slice_b)[2 * i];
    double b_im = ((const double*)slice_b)[2 * i + 1];
    ((double*)result)[2 * i] += a_re * b_re - a_im * b_im;
    ((double*)result)[2 * i + 1] += a_re * b_im + a_im * b_re;
  }
}

#else

static inline void complex_mul_simd(fftw_complex* restrict result,
                                    const fftw_complex* restrict slice_a,
                                    const fftw_complex* restrict slice_b,
                                    size_t len) {
  PRAGMA_VECTORIZE_LOOP
  for (size_t i = 0; i < len; i++) {
    double a_re = ((const double*)slice_a)[2 * i];
    double a_im = ((const double*)slice_a)[2 * i + 1];
    double b_re = ((const double*)slice_b)[2 * i];
    double b_im = ((const double*)slice_b)[2 * i + 1];
    ((double*)result)[2 * i] = a_re * b_re - a_im * b_im;
    ((double*)result)[2 * i + 1] = a_re * b_im + a_im * b_re;
  }
}

static inline void complex_fma_simd(fftw_complex* restrict result,
                                    const fftw_complex* restrict slice_a,
                                    const fftw_complex* restrict slice_b,
                                    size_t len) {
  PRAGMA_VECTORIZE_LOOP
  for (size_t i = 0; i < len; i++) {
    double a_re = ((const double*)slice_a)[2 * i];
    double a_im = ((const double*)slice_a)[2 * i + 1];
    double b_re = ((const double*)slice_b)[2 * i];
    double b_im = ((const double*)slice_b)[2 * i + 1];
    ((double*)result)[2 * i] += a_re * b_re - a_im * b_im;
    ((double*)result)[2 * i + 1] += a_re * b_im + a_im * b_re;
  }
}
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Config/engine_config_types.h"
// Uniform-partitioned overlap-add FIR convolution.
// Stockham-style segmented overlap-add with one 2N-point real FFT per
// chunk and an N+1-bin spectrum-domain multiply-accumulate across the
// segment history.
//
//   - Uses direct FFTW3 r2c/c2r interleaved complex transforms without
//     redundant intermediate buffers or copy loops.
//   - Frequency-domain multiply-accumulate runs through vectorized
//     ARM NEON / AVX+FMA fused multiply-add kernels.
//   - Inverse FFT produces unscaled linear convolution sum due to
//     pre-scaling of coefficients by 1 / (2 * chunkSize).

/**
 * @brief Loads a single channel from a WAV file and converts it to double.
 *
 * Supports 16-bit, 24-bit, 32-bit PCM and float, and 64-bit float formats.
 *
 * @param path Path to the WAV file.
 * @param channel The channel index to load (0-indexed).
 * @param out_count Output pointer to store the number of loaded samples.
 * @return Pointer to the allocated double array containing samples, or NULL on
 * failure.
 */
static double* load_wav_file(const char* path, int channel, size_t* out_count) {
  FILE* f = cdsp_fopen(path, "rb");
  if (!f) return NULL;

  uint16_t audio_format = 0;
  uint16_t channels = 0;
  uint16_t bits_per_sample = 0;
  bool fmt_found = false;
  bool data_found = false;
  uint32_t data_bytes = 0;

  uint8_t riff_header[12];
  if (fread(riff_header, 1, 12, f) != 12) {
    fclose(f);
    return NULL;
  }

  if ((memcmp(riff_header, "RIFF", 4) != 0 &&
       memcmp(riff_header, "RF64", 4) != 0) ||
      memcmp(riff_header + 8, "WAVE", 4) != 0) {
    fclose(f);
    return NULL;
  }

  uint8_t chunk_id[4];
  uint32_t chunk_size;
  while (fread(chunk_id, 1, 4, f) == 4) {
    if (fread(&chunk_size, 4, 1, f) != 1) break;

    if (memcmp(chunk_id, "fmt ", 4) == 0) {
      if (chunk_size < 16) {
        fclose(f);
        return NULL;
      }
      uint8_t* fmt_data = (uint8_t*)malloc(chunk_size);
      if (!fmt_data) {
        fclose(f);
        return NULL;
      }
      if (fread(fmt_data, 1, chunk_size, f) != chunk_size) {
        free(fmt_data);
        fclose(f);
        return NULL;
      }
      audio_format = fmt_data[0] | (fmt_data[1] << 8);
      channels = fmt_data[2] | (fmt_data[3] << 8);
      bits_per_sample = fmt_data[14] | (fmt_data[15] << 8);

      if (audio_format == 65534) {  // WAVE_FORMAT_EXTENSIBLE
        if (chunk_size >= 40) {
          audio_format = fmt_data[24] | (fmt_data[25] << 8);
        }
      }
      free(fmt_data);
      fmt_found = true;
      if (chunk_size % 2 != 0) {
        fseek(f, 1, SEEK_CUR);
      }
    } else if (memcmp(chunk_id, "data", 4) == 0) {
      data_bytes = chunk_size;
      data_found = true;
      break;
    } else {
      uint32_t skip_bytes = (chunk_size + 1) & ~1;
      fseek(f, skip_bytes, SEEK_CUR);
    }
  }

  if (!fmt_found || !data_found || data_bytes == 0) {
    fclose(f);
    return NULL;
  }

  if (audio_format != 1 && audio_format != 3) {
    fclose(f);
    return NULL;
  }

  if (channel < 0 || channel >= (int)channels) {
    fclose(f);
    return NULL;
  }

  size_t bytes_per_sample = bits_per_sample / 8;
  if (channels == 0 || bytes_per_sample == 0) {
    fclose(f);
    return NULL;
  }
  size_t num_frames = data_bytes / (channels * bytes_per_sample);
  if (num_frames == 0) {
    fclose(f);
    return NULL;
  }

  double* result = (double*)calloc(num_frames, sizeof(double));
  if (!result) {
    fclose(f);
    return NULL;
  }

  uint8_t* frame_buf = (uint8_t*)calloc(channels, bytes_per_sample);
  if (!frame_buf) {
    free(result);
    fclose(f);
    return NULL;
  }

  size_t read_frames = 0;
  for (size_t i = 0; i < num_frames; i++) {
    if (fread(frame_buf, 1, channels * bytes_per_sample, f) !=
        channels * bytes_per_sample) {
      break;
    }
    const uint8_t* src = frame_buf + channel * bytes_per_sample;
    double sample = 0.0;
    if (bits_per_sample == 16) {
      sample = pcm_sample_decode_s16_bytes(src);
    } else if (bits_per_sample == 24) {
      sample = pcm_sample_decode_s24_3bytes(src);
    } else if (bits_per_sample == 32) {
      if (audio_format == 3) {
        sample = pcm_sample_decode_f32_bytes(src);
      } else {
        sample = pcm_sample_decode_s32_bytes(src);
      }
    } else if (bits_per_sample == 64) {
      sample = pcm_sample_decode_f64_bytes(src);
    }
    result[read_frames++] = sample;
  }

  free(frame_buf);
  fclose(f);
  *out_count = read_frames;
  return result;
}

/**
 * @brief Loads raw PCM or text data from a file and converts it to double.
 *
 * For "TEXT" format, reads line by line. For binary formats, reads according
 * to specified sample format.
 *
 * @param path Path to the raw file.
 * @param format_str Format string (e.g., "S16_LE", "TEXT").
 * @param skip_bytes Number of bytes (or lines for TEXT) to skip at start.
 * @param read_bytes Max bytes (or lines for TEXT) to read.
 * @param out_count Output pointer to store the number of loaded samples.
 * @return Pointer to the allocated double array containing samples, or NULL on
 * failure.
 */
static double* load_raw_file(const char* path, const char* format_str,
                             int skip_bytes, int read_bytes,
                             size_t* out_count) {
  if (strcmp(format_str, "TEXT") == 0) {
    FILE* f = cdsp_fopen(path, "r");
    if (!f) return NULL;
    char line[128];
    bool skip_failed = false;
    for (int i = 0; i < skip_bytes; i++) {
      if (!fgets(line, sizeof(line), f)) {
        skip_failed = true;
        break;
      }
    }
    if (skip_failed) {
      fclose(f);
      return NULL;
    }
    size_t cap = 1024;
    double* result = (double*)calloc(cap, sizeof(double));
    if (!result) {
      fclose(f);
      return NULL;
    }
    size_t count = 0;
    while (fgets(line, sizeof(line), f)) {
      if (read_bytes > 0 && (int)count >= read_bytes) break;
      if (count >= cap) {
        cap *= 2;
        double* new_res = (double*)realloc(result, cap * sizeof(double));
        if (!new_res) {
          free(result);
          fclose(f);
          return NULL;
        }
        result = new_res;
      }
      char* endptr;
      double val = strtod(line, &endptr);
      if (endptr != line) {
        result[count++] = val;
      }
    }
    fclose(f);
    *out_count = count;
    return result;
  }

  FILE* f = cdsp_fopen(path, "rb");
  if (!f) return NULL;

  if (skip_bytes > 0) {
    fseek(f, skip_bytes, SEEK_SET);
  }

  binary_sample_format_t format = file_sample_format_from_string(format_str);
  if (format == BINARY_SAMPLE_FORMAT_INVALID) {
    fclose(f);
    return NULL;
  }

  size_t sample_size = sample_format_bytes_per_sample(format);
  if (sample_size == 0) {
    fclose(f);
    return NULL;
  }

  fseek(f, 0, SEEK_END);
  long file_size = ftell(f) - skip_bytes;
  fseek(f, skip_bytes, SEEK_SET);
  if (file_size <= 0) {
    fclose(f);
    return NULL;
  }

  long max_read = file_size;
  if (read_bytes > 0 && read_bytes < file_size) {
    max_read = read_bytes;
  }

  size_t num_samples = max_read / sample_size;
  if (num_samples == 0) {
    fclose(f);
    return NULL;
  }

  double* result = (double*)calloc(num_samples, sizeof(double));
  if (!result) {
    fclose(f);
    return NULL;
  }

  uint8_t* buf = (uint8_t*)calloc(1, sample_size);
  if (!buf) {
    free(result);
    fclose(f);
    return NULL;
  }

  size_t read_count = 0;
  for (size_t i = 0; i < num_samples; i++) {
    if (fread(buf, 1, sample_size, f) != sample_size) {
      break;
    }
    double val = 0.0;
    switch (format) {
      case BINARY_SAMPLE_FORMAT_S16_LE: {
        val = pcm_sample_decode_s16_bytes(buf);
        break;
      }
      case BINARY_SAMPLE_FORMAT_S24_3_LE: {
        val = pcm_sample_decode_s24_3bytes(buf);
        break;
      }
      case BINARY_SAMPLE_FORMAT_S24_4_RJ_LE: {
        val = pcm_sample_decode_s24_4_rj_bytes(buf);
        break;
      }
      case BINARY_SAMPLE_FORMAT_S24_4_LJ_LE: {
        val = pcm_sample_decode_s24_4_lj_bytes(buf);
        break;
      }
      case BINARY_SAMPLE_FORMAT_S32_LE: {
        val = pcm_sample_decode_s32_bytes(buf);
        break;
      }
      case BINARY_SAMPLE_FORMAT_F32_LE: {
        val = pcm_sample_decode_f32_bytes(buf);
        break;
      }
      case BINARY_SAMPLE_FORMAT_F64_LE: {
        val = pcm_sample_decode_f64_bytes(buf);
        break;
      }
      default:
        break;
    }
    result[read_count++] = val;
  }

  free(buf);
  fclose(f);
  *out_count = read_count;
  return result;
}

/**
 * @brief Free the convolution filter instance and its associated resources.
 *
 * @param filter The convolution filter instance to free.
 */
static void convolution_filter_free(void* instance) {
  convolution_filter_t* filter = (convolution_filter_t*)instance;
  if (!filter) return;
  size_t num_seg = filter->num_segments;
  for (size_t s = 0; s < num_seg; s++) {
    if (filter->coeffs_f && filter->coeffs_f[s]) fftw_free(filter->coeffs_f[s]);
    if (filter->hist_f && filter->hist_f[s]) fftw_free(filter->hist_f[s]);
  }
  if (filter->coeffs_f) free(filter->coeffs_f);
  if (filter->hist_f) free(filter->hist_f);
  if (filter->temp_buf) fftw_free(filter->temp_buf);
  if (filter->plan_forward) fftw_destroy_plan(filter->plan_forward);
  if (filter->plan_inverse) fftw_destroy_plan(filter->plan_inverse);
  if (filter->overlap_buffer) free(filter->overlap_buffer);
  if (filter->time_buf) fftw_free(filter->time_buf);
  if (filter->out_buf) fftw_free(filter->out_buf);
  if (filter->input_buffer) free(filter->input_buffer);
  if (filter->output_buffer) free(filter->output_buffer);
  free(filter);
}

/**
 * @brief Validates convolution filter parameters.
 *
 * @param config High-level filter configuration.
 * @param sample_rate The sample rate.
 * @param err Pointer to a config error struct to populate on failure.
 * @return 0 on success, -1 on failure.
 */
static int convolution_config_validate(const filter_config_t* config,
                                       int sample_rate, config_error_t* err) {
  (void)sample_rate;
  if (!config || config->type != FILTER_TYPE_CONV) return -1;
  const convolution_config_t* params = &config->parameters.conv;
  if (!params) return 0;
  switch (params->type) {
    case CONV_TYPE_VALUES:
      if (!params->values || params->values_count == 0) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "Conv 'values' must be non-empty");
        return -1;
      }
      break;
    case CONV_TYPE_WAV:
    case CONV_TYPE_RAW: {
      if (params->filename[0] == '\0') {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "Conv filter missing filename");
        return -1;
      }
      FILE* f = cdsp_fopen(params->filename, "rb");
      if (!f) {
        char msg[512];
        snprintf(msg, sizeof(msg),
                 "Conv file '%s' cannot be opened or does not exist",
                 params->filename);
        config_error_set(err, CONFIG_ERR_INVALID_FILTER, msg);
        return -1;
      }
      fseek(f, 0, SEEK_END);
      long fsize = ftell(f);
      fclose(f);
      if (fsize <= 0) {
        char msg[512];
        snprintf(msg, sizeof(msg), "Conv file '%s' is empty or invalid",
                 params->filename);
        config_error_set(err, CONFIG_ERR_INVALID_FILTER, msg);
        return -1;
      }
      break;
    }
    case CONV_TYPE_DUMMY:
      if (params->length <= 0) {
        config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                         "Conv 'dummy' length must be > 0");
        return -1;
      }
      break;
  }
  return 0;
}

/**
 * @brief Build a convolution filter from raw IR samples.
 *
 * Resolve the parameters to a flat IR buffer. Only called from the
 * control plane (filter creation / hot-swap), never from
 * convolution_filter_process.
 *
 * @param name The name of the filter.
 * @param config High-level filter configuration.
 * @param sample_rate The sample rate.
 * @param chunk_size Per-call block length N. Must match the
 *                   validFrames the pipeline will hand to process.
 * @param proc_params Processing parameters.
 * @param err Pointer to a config error struct to populate on failure.
 * @return A pointer to the created convolution filter, or NULL on failure.
 */
static void* convolution_filter_create(const char* name,
                                       const filter_config_t* config,
                                       int sample_rate, size_t chunk_size,
                                       processing_parameters_t* proc_params,
                                       config_error_t* err) {
  (void)sample_rate;
  (void)proc_params;
  if (!config || config->type != FILTER_TYPE_CONV) return NULL;
  const convolution_config_t* params = &config->parameters.conv;
  if (convolution_config_validate(config, 0, err) != 0) return NULL;
  if (chunk_size == 0) {
    config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                     "Convolution chunk_size must be positive");
    return NULL;
  }
  convolution_filter_t* filter =
      (convolution_filter_t*)calloc(1, sizeof(convolution_filter_t));
  if (!filter) {
    config_error_set(err, CONFIG_ERR_PARSE,
                     "Failed to allocate convolution filter wrapper");
    return NULL;
  }
  if (name) {
    strncpy(filter->name, name, sizeof(filter->name) - 1);
    filter->name[sizeof(filter->name) - 1] = '\0';
  } else {
    strcpy(filter->name, "convolution");
  }
  filter->chunk_size = chunk_size;
  size_t fft_len = 2 * chunk_size;
  size_t spec_len = fft_len / 2 + 1;
  filter->spec_len = spec_len;

  const double* coeffs = NULL;
  size_t coeffs_count = 0;
  double* dummy_coeffs = NULL;
  double* scratch = NULL;

  if (params->type == CONV_TYPE_VALUES) {
    coeffs = params->values;
    coeffs_count = params->values_count;
  } else if (params->type == CONV_TYPE_DUMMY) {
    size_t len = params->length > 0 ? params->length : 1;
    dummy_coeffs = (double*)calloc(len, sizeof(double));
    if (!dummy_coeffs) {
      goto fail;
    }
    dummy_coeffs[0] = 1.0;
    coeffs = dummy_coeffs;
    coeffs_count = len;
  } else if (params->type == CONV_TYPE_WAV) {
    size_t count = 0;
    dummy_coeffs = load_wav_file(params->filename, params->channel, &count);
    coeffs = dummy_coeffs;
    coeffs_count = count;
  } else if (params->type == CONV_TYPE_RAW) {
    size_t count = 0;
    dummy_coeffs = load_raw_file(params->filename, params->format,
                                 params->skip_bytes_lines,
                                 params->read_bytes_lines, &count);
    coeffs = dummy_coeffs;
    coeffs_count = count;
  }

  if (!coeffs || coeffs_count == 0) {
    goto fail;
  }

  size_t num_seg = (coeffs_count + chunk_size - 1) / chunk_size;
  filter->num_segments = num_seg;
  filter->coeffs_f = (fftw_complex**)calloc(num_seg, sizeof(fftw_complex*));
  filter->hist_f = (fftw_complex**)calloc(num_seg, sizeof(fftw_complex*));

  if (!filter->coeffs_f || !filter->hist_f) {
    goto fail;
  }

  for (size_t s = 0; s < num_seg; s++) {
    filter->coeffs_f[s] =
        (fftw_complex*)fftw_malloc(spec_len * sizeof(fftw_complex));
    filter->hist_f[s] =
        (fftw_complex*)fftw_malloc(spec_len * sizeof(fftw_complex));
    if (!filter->coeffs_f[s] || !filter->hist_f[s]) {
      goto fail;
    }
    memset(filter->hist_f[s], 0, spec_len * sizeof(fftw_complex));
  }

  filter->temp_buf =
      (fftw_complex*)fftw_malloc(spec_len * sizeof(fftw_complex));
  filter->time_buf = (double*)fftw_malloc(fft_len * sizeof(double));
  filter->out_buf = (double*)fftw_malloc(fft_len * sizeof(double));
  filter->overlap_buffer = (double*)calloc(chunk_size, sizeof(double));
  filter->input_buffer = (double*)calloc(chunk_size, sizeof(double));
  filter->output_buffer = (double*)calloc(chunk_size, sizeof(double));
  filter->buf_pos = 0;
  filter->write_idx = 0;

  if (!filter->temp_buf || !filter->time_buf || !filter->out_buf ||
      !filter->overlap_buffer || !filter->input_buffer ||
      !filter->output_buffer) {
    config_error_set(err, CONFIG_ERR_PARSE,
                     "Failed to allocate convolution scratch buffers");
    goto fail;
  }

  filter->plan_forward = fftw_plan_dft_r2c_1d((int)fft_len, filter->time_buf,
                                              filter->hist_f[0], FFTW_PATIENT);
  filter->plan_inverse = fftw_plan_dft_c2r_1d((int)fft_len, filter->temp_buf,
                                              filter->out_buf, FFTW_PATIENT);

  if (!filter->plan_forward || !filter->plan_inverse) {
    config_error_set(err, CONFIG_ERR_PARSE, "Failed to create FFTW plan");
    goto fail;
  }

  // Clear buffers that might have been overwritten during FFTW_MEASURE
  memset(filter->hist_f[0], 0, spec_len * sizeof(fftw_complex));
  memset(filter->temp_buf, 0, spec_len * sizeof(fftw_complex));
  memset(filter->time_buf, 0, fft_len * sizeof(double));
  memset(filter->out_buf, 0, fft_len * sizeof(double));

  scratch = (double*)fftw_malloc(fft_len * sizeof(double));
  if (!scratch) {
    goto fail;
  }
  double inv_scale = 1.0 / (double)fft_len;

  // Pre-scale and FFT each IR segment directly into interleaved
  // frequency-domain storage.
  for (size_t s = 0; s < num_seg; s++) {
    memset(scratch, 0, fft_len * sizeof(double));
    size_t offset = s * chunk_size;
    size_t copy_len = (coeffs_count > offset) ? (coeffs_count - offset) : 0;
    if (copy_len > chunk_size) copy_len = chunk_size;
    if (copy_len > 0) {
      for (size_t k = 0; k < copy_len; k++) {
        scratch[k] = coeffs[offset + k] * inv_scale;
      }
    }
    fftw_execute_dft_r2c(filter->plan_forward, scratch, filter->coeffs_f[s]);
  }
  fftw_free(scratch);
  scratch = NULL;
  if (dummy_coeffs) {
    free(dummy_coeffs);
    dummy_coeffs = NULL;
  }

  return filter;

fail:
  if (err && err->type == CONFIG_ERR_NONE) {
    config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                     "Failed to initialize convolution filter '%s' (check IR "
                     "values or file format/existence)",
                     name ? name : "");
  }
  if (dummy_coeffs) free(dummy_coeffs);
  if (scratch) fftw_free(scratch);
  convolution_filter_free(filter);
  return NULL;
}

/**
 * @brief Processes one chunk of audio data using partitioned overlap-add
 * convolution.
 *
 * This function performs the following steps:
 * 1. Copies the input block to the time buffer and pads with zeros.
 * 2. Computes the forward FFT of the padded block directly into the history
 * buffer.
 * 3. Performs vectorized frequency-domain multiply-accumulate with partitioned
 * IR segments.
 * 4. Computes the inverse FFT of the accumulated spectrum directly.
 * 5. Reconstructs the output block using overlap-add.
 *
 * @param filter Pointer to the convolution filter.
 * @param waveform In-place buffer containing the input block, which will be
 * overwritten with the output.
 */
static void process_chunk(convolution_filter_t* filter,
                          mutable_waveform_t waveform) {
  if (!filter || filter->num_segments == 0) return;
  size_t cs = filter->chunk_size;
  size_t spec_len = filter->spec_len;
  size_t num_seg = filter->num_segments;
  size_t widx = filter->write_idx;

  // 1. Stage the new block in the first `chunkSize` samples of
  //    `time_buf`; zero the second half (the FFT zero-pad).
  memcpy(filter->time_buf, waveform, cs * sizeof(double));
  memset(filter->time_buf + cs, 0, cs * sizeof(double));

  // 2. Advance the history index and FFT the new block directly into that slot.
  fftw_execute_dft_r2c(filter->plan_forward, filter->time_buf,
                       filter->hist_f[widx]);

  // 3. Spectrum-domain multiply-accumulate across the segment history.
  //    seg=0 pairs the newest input with coeff[0]; seg=k pairs the input from
  //    `k` blocks ago with coeff[k].
  size_t hidx0 = widx;
  complex_mul_simd(filter->temp_buf, filter->hist_f[hidx0], filter->coeffs_f[0],
                   spec_len);

  for (size_t s = 1; s < num_seg; s++) {
    size_t hidx = (widx + num_seg - s) % num_seg;
    complex_fma_simd(filter->temp_buf, filter->hist_f[hidx],
                     filter->coeffs_f[s], spec_len);
  }

  // 4. Inverse FFT directly into out_buf.
  fftw_execute_dft_c2r(filter->plan_inverse, filter->temp_buf, filter->out_buf);

  // 5. Overlap-add output: out[i] = ifft[i] + overlap_prev[i] for
  //    i in 0..<N; overlap_next = ifft[N..2N].
  for (size_t i = 0; i < cs; i++) {
    waveform[i] = filter->out_buf[i] + filter->overlap_buffer[i];
  }
  memcpy(filter->overlap_buffer, filter->out_buf + cs, cs * sizeof(double));

  filter->write_idx = (widx + 1) % num_seg;
}

/// Process one block in-place. The hot path is allocation-free in
/// steady state; everything below is pointer arithmetic over the
/// preallocated storage from `init`.
static void convolution_filter_process(void* instance,
                                       mutable_waveform_t waveform,
                                       size_t count) {
  convolution_filter_t* filter = (convolution_filter_t*)instance;
  if (!filter || !waveform || count == 0) return;
  size_t cs = filter->chunk_size;
  size_t i = 0;
  if (filter->buf_pos > 0) {
    size_t needed = cs - filter->buf_pos;
    size_t len = (count - i < needed) ? (count - i) : needed;
    memcpy(filter->input_buffer + filter->buf_pos, waveform + i,
           len * sizeof(double));
    filter->buf_pos += len;

    if (filter->buf_pos == cs) {
      process_chunk(filter, filter->input_buffer);
      memcpy(filter->output_buffer, filter->input_buffer, cs * sizeof(double));
      filter->buf_pos = 0;
      memcpy(waveform + i, filter->output_buffer + (cs - len),
             len * sizeof(double));
      i += len;
    } else {
      memcpy(waveform + i, filter->output_buffer + filter->buf_pos - len,
             len * sizeof(double));
      i += len;
      return;
    }
  }

  // 2. Process any full blocks in-place directly from/to the waveform
  while (i + cs <= count) {
    process_chunk(filter, waveform + i);
    i += cs;
  }

  // 3. Buffer any remaining partial block
  size_t rem = count - i;
  if (rem > 0) {
    memcpy(filter->input_buffer + filter->buf_pos, waveform + i,
           rem * sizeof(double));
    memcpy(waveform + i, filter->output_buffer + filter->buf_pos,
           rem * sizeof(double));
    filter->buf_pos += rem;
    i += rem;
  }
}

static void convolution_filter_transfer_state(void* dest_ptr,
                                              const void* src_ptr) {
  convolution_filter_t* dest = (convolution_filter_t*)dest_ptr;
  const convolution_filter_t* src = (const convolution_filter_t*)src_ptr;
  if (!dest || !src || dest == src) return;

  if (dest->chunk_size == src->chunk_size &&
      dest->num_segments == src->num_segments) {
    size_t num_seg = dest->num_segments;
    size_t spec_len = dest->spec_len;

    // Copy overlap buffer
    memcpy(dest->overlap_buffer, src->overlap_buffer,
           dest->chunk_size * sizeof(double));

    // Copy history segments
    for (size_t s = 0; s < num_seg; s++) {
      memcpy(dest->hist_f[s], src->hist_f[s], spec_len * sizeof(fftw_complex));
    }
    dest->write_idx = src->write_idx;
    memcpy(dest->input_buffer, src->input_buffer,
           dest->chunk_size * sizeof(double));
    memcpy(dest->output_buffer, src->output_buffer,
           dest->chunk_size * sizeof(double));
    dest->buf_pos = src->buf_pos;
  }
}

const filter_vtable_t g_convolution_vtable = {
    .validate = convolution_config_validate,
    .create = convolution_filter_create,
    .process = convolution_filter_process,
    .transfer_state = convolution_filter_transfer_state,
    .free = convolution_filter_free};
