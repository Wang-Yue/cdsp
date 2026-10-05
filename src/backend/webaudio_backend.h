/**
 * @file webaudio_backend.h
 * @brief WebAudio backend interface and vtable declarations.
 *
 * The WebAudio backend is a callback-driven backend, like CoreAudio, ASIO and
 * PipeWire: the engine's capture, processing and playback threads exchange
 * audio with the device through lock-free planar ring buffers
 * (backend_buffer_t), and the "device" is a Web Audio AudioWorklet render
 * quantum that calls webaudio_device_process() on the browser's real-time audio
 * thread.
 *
 * There is a single WebAudio device per process. The engine's capture and
 * playback backends attach to it in open() and detach in close();
 * webaudio_device_process() feeds the attached capture backend and renders from
 * the attached playback backend, producing silence when nothing is attached.
 * In the browser the device is created by webaudio_device_start()
 * (webaudio_device.c).
 */

#ifndef CLIB_BACKEND_WEBAUDIO_BACKEND_H
#define CLIB_BACKEND_WEBAUDIO_BACKEND_H

#if defined(ENABLE_WEBAUDIO)

#include <stdbool.h>
#include <stddef.h>

#include "backend/audio_backend.h"
#include "backend/backend_error.h"

/**
 * Maximum channel count of a WebAudio capture or playback backend: Web Audio's
 * per-node limit (AudioNode.channelCount and AudioWorkletNode outputs).
 */
#define WEBAUDIO_MAX_CHANNELS 32

/**
 * @brief Global virtual method table for WebAudio capture backend.
 */
extern const capture_backend_vtable_t g_webaudio_capture_vtable;

/**
 * @brief Global virtual method table for WebAudio playback backend.
 */
extern const playback_backend_vtable_t g_webaudio_playback_vtable;

/**
 * @brief Device callback: exchanges one block of audio with the engine.
 *
 * Called from the real-time audio thread (the AudioWorklet render quantum).
 * Lock-free and allocation-free. Pushes @p inputs into the attached capture
 * backend and renders the attached playback backend into @p outputs. Missing
 * input channels are treated as silence; outputs are silent when no playback
 * backend is attached or on underrun.
 *
 * @param inputs Planar input channels (may be NULL when @p input_channels is 0).
 * @param input_channels Number of input channels.
 * @param outputs Planar output channels.
 * @param output_channels Number of output channels.
 * @param frames Number of frames per channel.
 */
void webaudio_device_process(const float *const *inputs, size_t input_channels,
                             float *const *outputs, size_t output_channels,
                             size_t frames);

#if defined(__EMSCRIPTEN__)
/**
 * @brief Creates the browser audio device behind the WebAudio backends: an
 * AudioContext whose Wasm AudioWorklet render callback calls
 * webaudio_device_process().
 *
 * Called by dsp_engine_create(). Idempotent; a no-op off the browser main
 * thread or where Web Audio is unavailable (e.g. Node). The device starts at
 * 48 kHz stereo and follows the configured format: when a backend opens at
 * another rate or playback channel count, the AudioContext is recreated with
 * it (an AudioContext's rate and a worklet node's output count are fixed for
 * their lifetime); the capture channel count only changes how the node mixes
 * its input. Each time the device is ready,
 * `globalThis.cdspAudio = {context, node}` is set and a `cdsp-audio-ready`
 * event is dispatched; the page connects its sources to `node`. The
 * executable must be linked with -sAUDIO_WORKLET=1 -sWASM_WORKERS=1.
 */
void webaudio_device_start(void);
#endif

/** Lowest and highest AudioContext rates Chrome accepts. */
#define WEBAUDIO_MIN_SAMPLE_RATE 3000
#define WEBAUDIO_MAX_SAMPLE_RATE 768000

/**
 * @brief Publishes the device sample rate (the AudioContext rate).
 *
 * Backends at any other rate stay silent (their ring buffers are neither fed
 * nor drained), and the rate is reported by webaudio_describe(). 0 (the
 * default) means unknown: backends at any rate are served.
 *
 * @param sample_rate Device sample rate in Hz, or 0.
 */
void webaudio_device_set_sample_rate(int sample_rate);

/**
 * @brief Publishes the device's output channel count and the most output
 * channels the hardware accepts (AudioDestinationNode.maxChannelCount).
 *
 * A playback backend is only served while the device renders exactly its
 * channel count; webaudio_describe() reports @p max_channels, and playback
 * backends with more channels than that are refused. 0 means unknown.
 *
 * @param channels Current device output channel count, or 0.
 * @param max_channels Most output channels the hardware accepts, or 0.
 */
void webaudio_device_set_output_channels(size_t channels, size_t max_channels);

/**
 * @brief Callback invoked (on the engine thread, from a backend's open()) to
 * ask the device for a format: @p sample_rate, plus the capture backend's
 * channel count (@p input_channels, from capture) or the playback backend's
 * (@p output_channels, from playback). A channel count of 0 leaves that side
 * unchanged. Must not block.
 */
typedef void (*webaudio_format_request_hook_t)(int sample_rate,
                                               size_t input_channels,
                                               size_t output_channels);

/**
 * @brief Installs the format-request hook (NULL to remove). Without one, the
 * device is fixed: backends refuse to open at a rate or (playback) channel
 * count other than the published one. With one, they request the format and
 * open, staying silent until the device runs at their rate and channel count.
 */
void webaudio_device_set_format_request_hook(webaudio_format_request_hook_t hook);

/**
 * @brief Callback invoked (on the engine thread) when a capture backend
 * attaches to (true) or detaches from (false) the device.
 */
typedef void (*webaudio_capture_state_hook_t)(bool attached);

/**
 * @brief Installs the capture attach/detach hook (NULL to remove). The host
 * uses it to start and stop its audio source together with the engine.
 */
void webaudio_device_set_capture_state_hook(webaudio_capture_state_hook_t hook);

/**
 * @brief Lists WebAudio devices (a single "default" device).
 *
 * @param input True for capture devices.
 * @param out_devices Output array (may be NULL to count).
 * @param max_devices Capacity of @p out_devices.
 * @return Number of devices.
 */
int webaudio_get_available_devices(bool input, audio_device_t *out_devices,
                                   int max_devices);

/**
 * @brief Describes the capabilities of the WebAudio device.
 *
 * Lists one capability per supported channel count: capture 1 to
 * WEBAUDIO_MAX_CHANNELS, playback 1 to the hardware maximum (only the current
 * count when the device cannot switch).
 *
 * @param device Device name: "default", or NULL/empty for it.
 * @param is_capture True for the capture side.
 * @param err Optional error output.
 * @return Descriptor to free with free_audio_device_descriptor(), or NULL.
 */
audio_device_descriptor_t *webaudio_describe(const char *device,
                                             bool is_capture,
                                             device_error_t *err);


#endif // ENABLE_WEBAUDIO

#endif // CLIB_BACKEND_WEBAUDIO_BACKEND_H
