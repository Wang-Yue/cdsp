/**
 * @file cdsp_wasm.h
 * @brief WebAssembly C API and AudioWorklet bindings for CDSP engine.
 *
 * Provides a synchronous, zero-allocation real-time DSP interface for
 * WebAssembly, AudioWorkletProcessor, and browser environments.
 */

#ifndef CDSP_WASM_H
#define CDSP_WASM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#ifndef EMSCRIPTEN_KEEPALIVE
#define EMSCRIPTEN_KEEPALIVE
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque WebAssembly CDSP context.
 */
typedef struct cdsp_wasm cdsp_wasm_t;

/**
 * @brief Create a WebAssembly CDSP instance from a JSON configuration string.
 *
 * Allocates and initializes the DSP pipeline, filters, biquads, mixers,
 * faders, and pre-allocated planar input/output float staging buffers.
 *
 * @param json_config Null-terminated JSON string containing CDSP configuration.
 * @param sample_rate AudioContext sample rate (typically 48000 or 44100 Hz).
 * @param quantum_size Web Audio render quantum (default: 128 frames).
 * @return Allocated cdsp_wasm_t instance pointer, or NULL on error.
 */
EMSCRIPTEN_KEEPALIVE
cdsp_wasm_t *cdsp_wasm_create(const char *json_config, int sample_rate,
                              int quantum_size);

/**
 * @brief Destroy a WebAssembly CDSP instance and release all resources.
 *
 * @param ctx The CDSP WASM context.
 */
EMSCRIPTEN_KEEPALIVE
void cdsp_wasm_destroy(cdsp_wasm_t *ctx);

/**
 * @brief Get array of pointers to pre-allocated planar input channel buffers.
 *
 * In AudioWorklet, JS writes input audio (inputs[0][ch]) directly into these
 * float pointers in WASM linear memory:
 *   wasmHeapF32.set(inputs[0][ch], wasmInPtrs[ch] >> 2);
 *
 * @param ctx The CDSP WASM context.
 * @return Pointer to array of float* channel buffers (length = capture channels).
 */
EMSCRIPTEN_KEEPALIVE
float **cdsp_wasm_get_input_buffer_ptrs(cdsp_wasm_t *ctx);

/**
 * @brief Get array of pointers to pre-allocated planar output channel buffers.
 *
 * In AudioWorklet, JS reads processed audio directly out of these float pointers:
 *   outputs[0][ch].set(wasmHeapF32.subarray(outOffset, outOffset + 128));
 *
 * @param ctx The CDSP WASM context.
 * @return Pointer to array of float* channel buffers (length = playback channels).
 */
EMSCRIPTEN_KEEPALIVE
float **cdsp_wasm_get_output_buffer_ptrs(cdsp_wasm_t *ctx);

/**
 * @brief Get the configured number of capture (input) channels.
 */
EMSCRIPTEN_KEEPALIVE
size_t cdsp_wasm_get_input_channels(const cdsp_wasm_t *ctx);

/**
 * @brief Get the configured number of playback (output) channels.
 */
EMSCRIPTEN_KEEPALIVE
size_t cdsp_wasm_get_output_channels(const cdsp_wasm_t *ctx);

/**
 * @brief Get the configured quantum frame size (e.g. 128).
 */
EMSCRIPTEN_KEEPALIVE
size_t cdsp_wasm_get_quantum_size(const cdsp_wasm_t *ctx);

/**
 * @brief Synchronously process audio in the pre-allocated planar buffers.
 *
 * Decodes planar float -> double, executes the DSP pipeline, computes telemetry,
 * and encodes double -> planar float into the output buffers.
 * Zero dynamic heap allocations on the hot path.
 *
 * @param ctx The CDSP WASM context.
 * @param frames Number of frames to process (must match quantum_size).
 * @return true on success, false on error.
 */
EMSCRIPTEN_KEEPALIVE
bool cdsp_wasm_process(cdsp_wasm_t *ctx, size_t frames);

/**
 * @brief Update volume for a specific fader (0 = Main, 1-4 = Aux 1-4).
 *
 * @param ctx The CDSP WASM context.
 * @param fader Fader index (0 = Main).
 * @param volume_db Volume in decibels.
 * @param instant If true, skips smooth ramp and applies instantly.
 */
EMSCRIPTEN_KEEPALIVE
void cdsp_wasm_set_fader_volume(cdsp_wasm_t *ctx, int fader, double volume_db,
                                bool instant);

/**
 * @brief Query current volume for a specific fader.
 */
EMSCRIPTEN_KEEPALIVE
double cdsp_wasm_get_fader_volume(const cdsp_wasm_t *ctx, int fader);

/**
 * @brief Update mute state for a specific fader.
 */
EMSCRIPTEN_KEEPALIVE
void cdsp_wasm_set_fader_mute(cdsp_wasm_t *ctx, int fader, bool mute);

/**
 * @brief Query current mute state for a specific fader.
 */
EMSCRIPTEN_KEEPALIVE
bool cdsp_wasm_get_fader_mute(const cdsp_wasm_t *ctx, int fader);

/**
 * @brief Hot-swap DSP configuration without clicks or pops.
 *
 * Transfers filter states, biquad histories, and volume states seamlessly
 * using pipeline_transfer_state().
 *
 * @param ctx The CDSP WASM context.
 * @param json_config New JSON configuration string.
 * @return true on success, false on error.
 */
EMSCRIPTEN_KEEPALIVE
bool cdsp_wasm_set_config_json(cdsp_wasm_t *ctx, const char *json_config);

/**
 * @brief Retrieve current stereo/multi-channel VU meter levels.
 *
 * @param ctx The CDSP WASM context.
 * @param[out] in_peak Output array for input peak dBFS levels (length >= in_channels).
 * @param[out] in_rms Output array for input RMS dBFS levels (length >= in_channels).
 * @param[out] out_peak Output array for output peak dBFS levels (length >= out_channels).
 * @param[out] out_rms Output array for output RMS dBFS levels (length >= out_channels).
 */
EMSCRIPTEN_KEEPALIVE
void cdsp_wasm_get_vu_levels(const cdsp_wasm_t *ctx, float *in_peak,
                             float *in_rms, float *out_peak, float *out_rms);

/**
 * @brief Retrieve FFT spectrum magnitude bins for real-time visualization.
 *
 * @param ctx The CDSP WASM context.
 * @param is_capture true for input spectrum, false for output spectrum.
 * @param channel Channel index (e.g. 0 = Left, 1 = Right).
 * @param min_freq Minimum frequency in Hz (e.g. 20.0).
 * @param max_freq Maximum frequency in Hz (e.g. 20000.0).
 * @param n_bins Number of bins to compute.
 * @param[out] out_bins Output buffer of length n_bins to store dBFS magnitudes.
 * @return true on success, false on error.
 */
EMSCRIPTEN_KEEPALIVE
bool cdsp_wasm_get_spectrum(cdsp_wasm_t *ctx, bool is_capture, int channel,
                            double min_freq, double max_freq, size_t n_bins,
                            float *out_bins);

/**
 * @brief Retrieve latest raw audio samples for real-time oscilloscope / vectorscope.
 *
 * @param ctx The CDSP WASM context.
 * @param is_capture true for input samples, false for output samples.
 * @param n_frames Number of frames requested.
 * @param[out] out_left Buffer for channel 0 (left) samples (length >= n_frames).
 * @param[out] out_right Buffer for channel 1 (right) samples (length >= n_frames).
 * @return Number of frames actually read into out_left and out_right.
 */
EMSCRIPTEN_KEEPALIVE
size_t cdsp_wasm_get_samples(cdsp_wasm_t *ctx, bool is_capture, size_t n_frames,
                             float *out_left, float *out_right);

#ifdef __cplusplus
}
#endif

#endif // CDSP_WASM_H
