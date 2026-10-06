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
 * thread or where Web Audio is unavailable (e.g. Node). The device runs in its
 * native format only, with separate AudioContexts (and clocks) per direction:
 * playback at the browser's default rate (the output hardware's, so the
 * browser does not resample) rendering the hardware's channel count, and
 * capture, rendering to no output device, at the captured stream's rate and
 * channel count. When the output device or the captured stream changes format,
 * the device follows it and reports a format change
 * (webaudio_device_set_capture_format() / _playback_format()). Each time the
 * capture side is ready, `globalThis.cdspAudio = {context, node}` is set to the
 * capture context and node and a `cdsp-audio-ready` event is dispatched; the
 * page connects its sources to `node` and reports the captured stream's format
 * with `globalThis.cdspAudioDevice.setInputFormat(rate, channels)`. The executable must be
 * linked with -sAUDIO_WORKLET=1 -sWASM_WORKERS=1.
 */
void webaudio_device_start(void);
#endif

/** Format reported while the device's (or captured stream's) is unknown. */
#define WEBAUDIO_DEFAULT_SAMPLE_RATE 48000
#define WEBAUDIO_DEFAULT_CHANNELS 2

/**
 * @brief Publishes the capture device's native format: the captured stream's
 * rate (its AudioContext's) and channel count (0 = unknown).
 *
 * Capture and playback run on separate AudioContexts with independent clocks,
 * so each direction has its own format, and the engine's resampler and rate
 * adjust can bridge them like any two devices. webaudio_describe() reports
 * exactly this format, and capture backends in any other format are refused.
 * When a known value changes, the attached backends get a pending format
 * change, so the engine stops with a CAPTURE/PLAYBACK FORMAT_CHANGE reason
 * (carrying the new rate) and the host restarts it in the new format, as with
 * the other backends. Backends left in an old rate are not served meanwhile.
 */
void webaudio_device_set_capture_format(int sample_rate, size_t channels);

/**
 * @brief Publishes the playback device's native format: the output
 * AudioContext's rate and channel count (0 = unknown). As
 * webaudio_device_set_capture_format(), for playback backends.
 */
void webaudio_device_set_playback_format(int sample_rate, size_t channels);

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
 * Reports only the active stream's format: one capability with the device's
 * native rate and the captured stream's (capture) or output's (playback)
 * channel count; WEBAUDIO_DEFAULT_SAMPLE_RATE / WEBAUDIO_DEFAULT_CHANNELS
 * while unknown.
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
