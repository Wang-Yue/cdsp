// FFT-based fixed-ratio sample-rate converter.
//
// Independently derived from textbook descriptions of FFT-based rate
// conversion via overlap-add convolution and spectral resampling.
//
// References
// ----------
//   * R. E. Crochiere and L. R. Rabiner (1983), "Multirate Digital
//     Signal Processing", Prentice-Hall — §3 covers the L/M
//     decimator-interpolator structure and its block-rate FFT
//     realisation.
//   * A. V. Oppenheim and R. W. Schafer, "Discrete-Time Signal
//     Processing", Prentice-Hall — §4 (sample-rate alteration), §8.7
//     ("Overlap-Save and Overlap-Add Methods" for FFT-based linear
//     convolution), §8.8 (FFT-based fast convolution).
//   * J. O. Smith, "Digital Audio Resampling Home Page", CCRMA —
//     https://ccrma.stanford.edu/~jos/resample/ — covers FFT-based
//     bandlimited interpolation and windowed-sinc filter design.
//   * F. J. Harris (1978), "On the Use of Windows for Harmonic
//     Analysis with the Discrete Fourier Transform", Proc. IEEE
//     vol. 66 no. 1 — Blackman-Harris window (used here via
//     `SincWindowFunction.swift`).
//
// Algorithm
// ---------
// Given input rate `Fᵢ`, output rate `Fₒ`, and `g = gcd(Fᵢ, Fₒ)`,
// define
//
//     L = Fᵢ / g     (input block size in samples per rational period)
//     M = Fₒ / g     (output block size in samples per rational period)
//
// Any integer multiple `N = K·L` input samples corresponds to
// exactly `K·M` output samples — the resampler is fixed-ratio. We
// round the user-supplied `chunkSize` up to the smallest valid
// `K·L`, which fixes the per-call input/output block lengths.
//
// At init, build a windowed-sinc lowpass filter `h[n]` of length `N`
// with cutoff at `min(1, Fₒ/Fᵢ) · π` rad/sample (Crochiere & Rabiner
// §3.1, Smith CCRMA §"Windowed-Sinc Filter Design"), zero-pad to
// length `2N`, and pre-FFT it once into `H`.
//
// Per chunk per channel:
//
//   1. Forward 2N-point real FFT of the zero-padded input. The
//      zero-pad to length 2N converts the otherwise cyclic FFT
//      convolution into a linear convolution — the standard
//      overlap-add structure in Oppenheim & Schafer §8.7.
//
//   2. Multiply pointwise by `H` to apply the anti-aliasing filter
//      in the frequency domain. Cost: O(N) per chunk versus O(N²)
//      for a direct time-domain convolution.
//
//   3. Build the output spectrum of length `2P` where `P = K·M`:
//        — bins 0…min(N, P) get a copy of the filtered input
//          spectrum;
//        — bins above are set to zero.
//      For upsampling (M > L), the zero-pad above input Nyquist is
//      what extends the bandwidth. For downsampling (M < L),
//      truncating to the first `P + 1` unique bins is the
//      band-limiting step. This is the "spectral resampling" of
//      Smith's CCRMA notes.
//
//   4. Inverse 2P-point real FFT recovers a length-2P time-domain
//      block.
//
//   5. Overlap-add: emit `result[0..P) + carry`, save
//      `result[P..2P)` as `carry` for the next chunk
//      (Oppenheim & Schafer §8.7).
//
// The arbitrary-length real FFTs are handled by `RealFFT`,
// which lets the block lengths be sized exactly to `L` and `M`
// rather than padded to a power of two.
//
// Allocation discipline
// ---------------------
// Every per-channel and per-call buffer is allocated once at
// `init`. `process(input:into:)` does no heap allocation and writes
// directly into the caller's pre-allocated `output` chunk.

#include "synchronous_resampler.h"

#include "Audio/audio_chunk.h"
#include "FFT/real_fft.h"
#include "audio_resampler.h"
#include "resampler_error.h"
#include "sinc_window_function.h"

typedef struct synchronous_resampler synchronous_resampler_t;

struct synchronous_resampler {
  /// Number of channels processed per call.
  size_t channels;
  /// Input frames the resampler expects on every `process` call —
  /// `K·L` for some integer `K ≥ 1`, where `L = Fᵢ / gcd(Fᵢ, Fₒ)`.
  size_t chunk_size;
  /// Output frames produced per `process` call — `K·M`, where
  /// `M = Fₒ / gcd(Fᵢ, Fₒ)`.
  size_t output_chunk_size;
  double ratio;
  /// Length of the working FFT block on the input side (`sub_fft_in`).
  size_t sub_fft_in;
  /// Length of the working FFT block on the output side (`sub_fft_out`).
  size_t sub_fft_out;
  /// Number of sub-chunks processed per call.
  size_t num_subchunks;
  /// Number of unique-bin frequencies common to the input and output
  /// spectra: `min(sub_fft_in, sub_fft_out) + 1`. Bins above
  /// this in the output spectrum are zeroed (band-limiting for
  /// downsampling, spectral zero-pad for upsampling).
  size_t shared_bins;
  // Anti-aliasing filter, pre-FFT'd at init. `sub_fft_in + 1`
  // unique bins. Stored as raw pointer to bypass overhead.
  double* filter_spec_re;
  double* filter_spec_im;
  // Real-input FFT engines. The forward engine handles the zero-padded
  // input block (length `2 · sub_fft_in`); the inverse engine
  // reconstructs the output block (length `2 · sub_fft_out`).
  real_fft_t* input_fft;
  real_fft_t* output_fft;
  // Per-channel time-domain overlap-add carry. Each entry holds the
  // tail of the previous chunk's IFFT result, length `sub_fft_out`.
  double** carries;
  // Hot-path scratch buffers reused across channels. Unified to minimize
  // cache footprint and avoid intermediate allocations.
  //   `workingTime`: holds the 2N zero-padded input block for forward FFT,
  //                  and the 2P overlap-add output block from inverse FFT.
  //   `workingSpecRe`/`Im`: holds the shared low-frequency bins during
  //   filtering.
  double* working_time;
  double* working_spec_re;
  double* working_spec_im;
};

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef ENABLE_ACCELERATE
#include <Accelerate/Accelerate.h>
#endif

/**
 * @brief Computes the greatest common divisor (GCD) of two size_t integers
 * using Euclid's algorithm.
 *
 * This helper is used to find the irreducible rational fraction L/M of the
 * sample rate conversion ratio, which determines the input and output chunk
 * size scaling.
 *
 * @param a First value.
 * @param b Second value.
 * @return Greatest common divisor of a and b.
 */
static inline size_t gcd_size(size_t a, size_t b) {
  size_t x = a, y = b;
  while (y != 0) {
    size_t t = y;
    y = x % y;
    x = t;
  }
  return x;
}

static void synchronous_resampler_free(synchronous_resampler_t* resampler) {
  if (!resampler) return;
  if (resampler->input_fft) real_fft_free(resampler->input_fft);
  if (resampler->output_fft) real_fft_free(resampler->output_fft);
  free(resampler->filter_spec_re);
  free(resampler->filter_spec_im);
  if (resampler->carries) {
    for (size_t ch = 0; ch < resampler->channels; ch++) {
      free(resampler->carries[ch]);
    }
    free(resampler->carries);
  }
  free(resampler->working_time);
  free(resampler->working_spec_re);
  free(resampler->working_spec_im);
  free(resampler);
}

/// `SynchronousResampler` runs at a fixed rational ratio fixed at
/// construction. The rate-adjust controller's relative multiplier
/// has nowhere to go here — we accept it without effect.
static void synchronous_resampler_set_relative_ratio(
    synchronous_resampler_t* resampler, double multiplier) {
  (void)resampler;
  (void)multiplier;
  // Fixed-ratio
}

static double synchronous_resampler_get_ratio(
    const synchronous_resampler_t* resampler) {
  return resampler ? resampler->ratio : 1.0;
}

static size_t synchronous_resampler_get_max_output_frames(
    const synchronous_resampler_t* resampler) {
  return resampler ? resampler->output_chunk_size : 0;
}

static size_t synchronous_resampler_get_chunk_size(
    const synchronous_resampler_t* resampler) {
  return resampler ? resampler->chunk_size : 0;
}

static size_t synchronous_resampler_get_input_frames_next(
    const synchronous_resampler_t* resampler) {
  return resampler ? resampler->chunk_size : 0;
}

static size_t synchronous_resampler_get_output_frames_next(
    const synchronous_resampler_t* resampler) {
  return resampler ? resampler->output_chunk_size : 0;
}

static size_t synchronous_resampler_get_channels(
    const synchronous_resampler_t* resampler) {
  return resampler ? resampler->channels : 0;
}

/// One channel's worth of FFT-based overlap-add convolution +
/// spectral remap. All scratch buffers are class-owned; this
/// function performs no heap allocation.
static resampler_error_t synchronous_resampler_process(
    synchronous_resampler_t* resampler, const audio_chunk_t* input,
    audio_chunk_t* output) {
  if (!resampler || !input || !output) return RESAMPLER_ERR_INVALID_PARAMETER;
  size_t valid_frames = audio_chunk_get_valid_frames(input);
  if (valid_frames > resampler->chunk_size) {
    return RESAMPLER_ERR_INPUT_SIZE_MISMATCH;
  }
  if (audio_chunk_get_channels(input) != resampler->channels) {
    return RESAMPLER_ERR_CHANNEL_COUNT_MISMATCH;
  }
  if (audio_chunk_get_channels(output) != resampler->channels) {
    return RESAMPLER_ERR_CHANNEL_COUNT_MISMATCH;
  }
  if (audio_chunk_get_frames(output) < resampler->output_chunk_size) {
    return RESAMPLER_ERR_OUTPUT_BUFFER_TOO_SMALL;
  }

  for (size_t s = 0; s < resampler->num_subchunks; s++) {
    size_t in_offset = s * resampler->sub_fft_in;
    size_t out_offset = s * resampler->sub_fft_out;
    size_t sub_valid =
        (valid_frames > in_offset) ? (valid_frames - in_offset) : 0;
    if (sub_valid > resampler->sub_fft_in) sub_valid = resampler->sub_fft_in;

    for (size_t ch = 0; ch < resampler->channels; ch++) {
      const double* src_ptr = audio_chunk_get_channel(input, ch) + in_offset;
      double* out_ptr = audio_chunk_get_channel(output, ch) + out_offset;
      double* carry_ptr = resampler->carries[ch];

      // Step 1. Place the input block at the start of a length-2N
      // buffer, with the second half zero. The zero-pad is what makes
      // the cyclic FFT convolution behave as a linear convolution
      // (Oppenheim & Schafer §8.7). The upper half is cleared each call
      // to ensure the zero-pad region for the forward FFT is clean.
      memcpy(resampler->working_time, src_ptr, sub_valid * sizeof(double));
      memset(resampler->working_time + sub_valid, 0,
             (2 * resampler->sub_fft_in - sub_valid) * sizeof(double));

      // Step 2. Forward 2N-point real FFT.
      real_fft_forward(resampler->input_fft, resampler->working_time,
                       resampler->working_spec_re, resampler->working_spec_im);

      // Step 3. Pointwise multiply input spectrum by the pre-FFT'd
      // filter. Only the `sharedBins` matter since bins above are
      // dropped on the output side; doing the multiply in place over
      // that span avoids touching the upper half.
#ifdef ENABLE_ACCELERATE
      DSPDoubleSplitComplex io_split = {resampler->working_spec_re,
                                        resampler->working_spec_im};
      DSPDoubleSplitComplex f_split = {resampler->filter_spec_re,
                                       resampler->filter_spec_im};
      vDSP_zvmulD(&io_split, 1, &f_split, 1, &io_split, 1,
                  resampler->shared_bins, 1);
#else
      for (size_t i = 0; i < resampler->shared_bins; i++) {
        double re = resampler->working_spec_re[i];
        double im = resampler->working_spec_im[i];
        double fre = resampler->filter_spec_re[i];
        double fim = resampler->filter_spec_im[i];
        resampler->working_spec_re[i] = re * fre - im * fim;
        resampler->working_spec_im[i] = re * fim + im * fre;
      }
#endif

      // Step 4. Build the output spectrum of length `2·outputBlockLen`:
      // copy the filtered low bins and zero the rest. For upsampling
      // (outputBlockLen > inputBlockLen) the zeros above input Nyquist
      // are the spectral zero-pad that extends the bandwidth. For
      // downsampling they discard everything above output Nyquist —
      // the band-limiting step.
      if (resampler->sub_fft_in > resampler->sub_fft_out) {
        resampler->working_spec_im[resampler->sub_fft_out] = 0.0;
      }
      if (resampler->sub_fft_out + 1 > resampler->shared_bins) {
        size_t zero_count = resampler->sub_fft_out + 1 - resampler->shared_bins;
        memset(resampler->working_spec_re + resampler->shared_bins, 0,
               zero_count * sizeof(double));
        memset(resampler->working_spec_im + resampler->shared_bins, 0,
               zero_count * sizeof(double));
      }

      // Step 5. Inverse 2P-point real FFT to time domain (P = outputBlockLen).
      real_fft_inverse(resampler->output_fft, resampler->working_spec_re,
                       resampler->working_spec_im, resampler->working_time);

      // Step 6. Overlap-add: write `result[0..P) + carry` as the chunk's
      // output samples, and save `result[P..2P)` for the next chunk's
      // overlap.
#ifdef ENABLE_ACCELERATE
      vDSP_vaddD(resampler->working_time, 1, carry_ptr, 1, out_ptr, 1,
                 resampler->sub_fft_out);
#else
      for (size_t i = 0; i < resampler->sub_fft_out; i++) {
        out_ptr[i] = resampler->working_time[i] + carry_ptr[i];
      }
#endif
      memcpy(carry_ptr, resampler->working_time + resampler->sub_fft_out,
             resampler->sub_fft_out * sizeof(double));
    }
  }

  size_t valid_out =
      (resampler->output_chunk_size * valid_frames) / resampler->chunk_size;
  audio_chunk_set_valid_frames(output, valid_out);
  return RESAMPLER_OK;
}

static void* synchronous_resampler_create_impl(size_t channels,
                                               size_t input_rate,
                                               size_t output_rate,
                                               size_t requested_chunk_size,
                                               config_error_t* err) {
  if (channels == 0) {
    config_error_set(err, CONFIG_ERR_VALIDATION,
                     "SynchronousResampler: channels must be positive");
    return NULL;
  }
  if (requested_chunk_size == 0) {
    config_error_set(err, CONFIG_ERR_VALIDATION,
                     "SynchronousResampler: chunk_size must be positive");
    return NULL;
  }
  if (input_rate == 0 || output_rate == 0) {
    config_error_set(err, CONFIG_ERR_VALIDATION,
                     "SynchronousResampler: rates must be positive");
    return NULL;
  }

  synchronous_resampler_t* resampler =
      (synchronous_resampler_t*)calloc(1, sizeof(synchronous_resampler_t));
  if (!resampler) {
    config_error_set(err, CONFIG_ERR_PARSE,
                     "Failed to allocate SynchronousResampler");
    return NULL;
  }

  resampler->channels = channels;
  resampler->ratio = (double)output_rate / (double)input_rate;

  size_t g = gcd_size(input_rate, output_rate);
  size_t min_chunk_in = input_rate / g;
  size_t min_chunk_out = output_rate / g;

  size_t fft_chunks =
      (size_t)ceil((double)requested_chunk_size / (double)min_chunk_in);
  if (fft_chunks < 1) fft_chunks = 1;

  size_t sub_fft_in = fft_chunks * min_chunk_in;
  size_t sub_fft_out = fft_chunks * min_chunk_out;

  size_t num_subchunks = 1;

  size_t input_block = num_subchunks * sub_fft_in;
  size_t output_block = num_subchunks * sub_fft_out;

  if (input_block > SIZE_MAX / 2 || output_block > SIZE_MAX / 2) {
    config_error_set(err, CONFIG_ERR_VALIDATION,
                     "SynchronousResampler: block size overflows SIZE_MAX / 2");
    synchronous_resampler_free(resampler);
    return NULL;
  }

  resampler->sub_fft_in = sub_fft_in;
  resampler->sub_fft_out = sub_fft_out;
  resampler->num_subchunks = num_subchunks;
  resampler->chunk_size = input_block;
  resampler->output_chunk_size = output_block;
  resampler->shared_bins =
      (sub_fft_in < sub_fft_out ? sub_fft_in : sub_fft_out) + 1;

  double n = (double)sub_fft_in;
  double margin = 13.5 / n + 50.0 / (n * n);
  double cutoff;
  if (sub_fft_in > sub_fft_out) {
    double target_nyquist = (double)sub_fft_out / (double)sub_fft_in;
    cutoff = target_nyquist - margin;
    if (cutoff < 1e-6) cutoff = 1e-6;
  } else {
    cutoff = 1.0 - margin;
    if (cutoff < 1e-6) cutoff = 1e-6;
  }
  double* kernel =
      make_sinc_table(sub_fft_in, 1, WINDOW_FUNCTION_BLACKMAN_HARRIS2, cutoff);
  if (!kernel) {
    config_error_set(
        err, CONFIG_ERR_PARSE,
        "SynchronousResampler: Failed to build anti-aliasing kernel");
    synchronous_resampler_free(resampler);
    return NULL;
  }

  size_t two_sub_in = 2 * sub_fft_in;
  double* filter_time = (double*)calloc(two_sub_in, sizeof(double));
  if (!filter_time) {
    config_error_set(
        err, CONFIG_ERR_PARSE,
        "SynchronousResampler: Failed to allocate filter time buffer");
    free(kernel);
    synchronous_resampler_free(resampler);
    return NULL;
  }
  double scale = 1.0 / (double)two_sub_in;
  for (size_t i = 0; i < sub_fft_in; i++) {
    filter_time[i] = kernel[i] * scale;
  }
  free(kernel);

  resampler->input_fft = real_fft_create(two_sub_in, err);
  resampler->output_fft = real_fft_create(2 * sub_fft_out, err);
  if (!resampler->input_fft || !resampler->output_fft) {
    free(filter_time);
    synchronous_resampler_free(resampler);
    return NULL;
  }

  resampler->filter_spec_re = (double*)calloc(sub_fft_in + 1, sizeof(double));
  resampler->filter_spec_im = (double*)calloc(sub_fft_in + 1, sizeof(double));
  if (!resampler->filter_spec_re || !resampler->filter_spec_im) {
    config_error_set(
        err, CONFIG_ERR_PARSE,
        "SynchronousResampler: Failed to allocate filter spectrum buffer");
    free(filter_time);
    synchronous_resampler_free(resampler);
    return NULL;
  }
  real_fft_forward(resampler->input_fft, filter_time, resampler->filter_spec_re,
                   resampler->filter_spec_im);
  free(filter_time);

  resampler->carries = (double**)calloc(channels, sizeof(double*));
  if (!resampler->carries) {
    config_error_set(
        err, CONFIG_ERR_PARSE,
        "SynchronousResampler: Failed to allocate channel carries array");
    synchronous_resampler_free(resampler);
    return NULL;
  }
  for (size_t ch = 0; ch < channels; ch++) {
    resampler->carries[ch] = (double*)calloc(sub_fft_out, sizeof(double));
    if (!resampler->carries[ch]) {
      config_error_set(err, CONFIG_ERR_PARSE,
                       "SynchronousResampler: Failed to allocate carry buffer "
                       "for channel %zu",
                       ch);
      synchronous_resampler_free(resampler);
      return NULL;
    }
  }

  size_t max_len = sub_fft_in > sub_fft_out ? sub_fft_in : sub_fft_out;
  resampler->working_time = (double*)calloc(2 * max_len, sizeof(double));
  resampler->working_spec_re = (double*)calloc(max_len + 1, sizeof(double));
  resampler->working_spec_im = (double*)calloc(max_len + 1, sizeof(double));
  if (!resampler->working_time || !resampler->working_spec_re ||
      !resampler->working_spec_im) {
    config_error_set(
        err, CONFIG_ERR_PARSE,
        "SynchronousResampler: Failed to allocate working buffers");
    synchronous_resampler_free(resampler);
    return NULL;
  }

  return resampler;
}

/**
 * @brief Validates synchronous resampler parameters.
 *
 * @param config Pointer to the resampler configuration to validate.
 * @param err Pointer to a config error struct to populate on failure.
 * @return 0 on success, -1 on failure.
 */
static int synchronous_resampler_config_validate(
    const resampler_config_t* config, config_error_t* err) {
  (void)err;
  if (!config || config->type != RESAMPLER_TYPE_SYNCHRONOUS) return -1;
  return 0;
}

/**
 * @brief Creates and initializes a synchronous resampler instance.
 *
 * Independently derived from textbook descriptions of FFT-based rate
 * conversion via overlap-add convolution and spectral resampling.
 *
 * References
 * ----------
 *   * R. E. Crochiere and L. R. Rabiner (1983), "Multirate Digital
 *     Signal Processing", Prentice-Hall.
 *   * A. V. Oppenheim and R. W. Schafer, "Discrete-Time Signal
 *     Processing", Prentice-Hall.
 *
 * @param config Resampler configuration parameters.
 * @param input_rate The input sample rate in Hz.
 * @param output_rate The output sample rate in Hz.
 * @param channels The number of audio channels.
 * @param chunk_size The desired size of input chunks (in frames).
 *                             The resampler will round this up to a size
 * matching the rational period.
 * @param err Pointer to a config error struct to populate on failure.
 * @return A pointer to the created audio resampler instance, or NULL on
 * failure.
 */
static void* synchronous_resampler_create(const resampler_config_t* config,
                                          size_t input_rate, size_t output_rate,
                                          size_t channels, size_t chunk_size,
                                          config_error_t* err) {
  if (!config || config->type != RESAMPLER_TYPE_SYNCHRONOUS) return NULL;
  return synchronous_resampler_create_impl(channels, input_rate, output_rate,
                                           chunk_size, err);
}

const resampler_vtable_t g_synchronous_resampler_vtable = {
    .validate = synchronous_resampler_config_validate,
    .create = synchronous_resampler_create,
    .process =
        (resampler_error_t (*)(void*, const audio_chunk_t*,
                               audio_chunk_t*))synchronous_resampler_process,
    .set_relative_ratio =
        (void (*)(void*, double))synchronous_resampler_set_relative_ratio,
    .get_ratio = (double (*)(const void*))synchronous_resampler_get_ratio,
    .get_max_output_frames =
        (size_t (*)(const void*))synchronous_resampler_get_max_output_frames,
    .get_chunk_size =
        (size_t (*)(const void*))synchronous_resampler_get_chunk_size,
    .get_input_frames_next =
        (size_t (*)(const void*))synchronous_resampler_get_input_frames_next,
    .get_output_frames_next =
        (size_t (*)(const void*))synchronous_resampler_get_output_frames_next,
    .get_channels = (size_t (*)(const void*))synchronous_resampler_get_channels,
    .free = (void (*)(void*))synchronous_resampler_free};
