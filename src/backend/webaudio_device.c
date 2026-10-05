/**
 * @file webaudio_device.c
 * @brief Browser audio device for the WebAudio backend (Emscripten only).
 *
 * Creates an AudioContext running a Wasm AudioWorklet node (Emscripten
 * -sAUDIO_WORKLET). The node's render callback runs on the browser's real-time
 * audio thread, shares the module's memory, and calls
 * webaudio_device_process() directly, so the WebAudio backend behaves like a
 * native callback backend (CoreAudio, ASIO, PipeWire) with the engine running
 * on its usual capture, processing and playback pthreads.
 */

#if defined(ENABLE_WEBAUDIO) && defined(__EMSCRIPTEN__)

#include <emscripten.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <emscripten/webaudio.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "backend/webaudio_backend.h"
#include "logging/app_logger.h"

static const logger_t g_logger = {"dsp.backend.webaudio"};

/** Frames per Web Audio render quantum. */
#define WEBAUDIO_QUANTUM_FRAMES 128

/** Initial AudioContext rate, used until a backend opens at another rate. */
#define WEBAUDIO_DEVICE_SAMPLE_RATE 48000

/** Initial node input/output channel count, until a backend asks for another. */
#define WEBAUDIO_DEVICE_CHANNELS 2

/** Stack of the audio worklet thread. */
static uint8_t g_worklet_stack[64 * 1024] __attribute__((aligned(16)));

static EM_BOOL webaudio_device_render(int num_inputs,
                                      const AudioSampleFrame *inputs,
                                      int num_outputs,
                                      AudioSampleFrame *outputs, int num_params,
                                      const AudioParamFrame *params,
                                      void *user_data) {
  (void)num_params;
  (void)params;
  (void)user_data;
  const float *in[WEBAUDIO_MAX_CHANNELS];
  float *out[WEBAUDIO_MAX_CHANNELS];
  size_t in_channels = 0;
  size_t out_channels = 0;
  if (num_inputs > 0) {
    in_channels = (size_t)inputs[0].numberOfChannels;
    if (in_channels > WEBAUDIO_MAX_CHANNELS)
      in_channels = WEBAUDIO_MAX_CHANNELS;
    for (size_t c = 0; c < in_channels; c++)
      in[c] = inputs[0].data + c * WEBAUDIO_QUANTUM_FRAMES;
  }
  if (num_outputs > 0) {
    out_channels = (size_t)outputs[0].numberOfChannels;
    if (out_channels > WEBAUDIO_MAX_CHANNELS)
      out_channels = WEBAUDIO_MAX_CHANNELS;
    for (size_t c = 0; c < out_channels; c++)
      out[c] = outputs[0].data + c * WEBAUDIO_QUANTUM_FRAMES;
  }
  webaudio_device_process(in, in_channels, out, out_channels,
                          WEBAUDIO_QUANTUM_FRAMES);
  return EM_TRUE;
}

/** Tells the page when the engine attaches/detaches capture (engine thread). */
static void webaudio_device_capture_state_changed(bool attached) {
  MAIN_THREAD_ASYNC_EM_ASM(
      {
        var bridge = globalThis.cdspBridge;
        if (bridge && bridge.onCaptureAttached)
          bridge.onCaptureAttached(!!$0);
      },
      attached ? 1 : 0);
}

// Device state. Touched only on the browser main thread, except the g_wanted_*
// format, which backends set from engine threads.
static EMSCRIPTEN_WEBAUDIO_T g_context;
static EMSCRIPTEN_AUDIO_WORKLET_NODE_T g_node;
static int g_context_rate;
static int g_context_output_channels;
static bool g_closing;
static atomic_int g_wanted_rate = WEBAUDIO_DEVICE_SAMPLE_RATE;
static atomic_int g_wanted_input_channels = WEBAUDIO_DEVICE_CHANNELS;
static atomic_int g_wanted_output_channels = WEBAUDIO_DEVICE_CHANNELS;

static void webaudio_device_processor_created(EMSCRIPTEN_WEBAUDIO_T context,
                                              EM_BOOL success,
                                              void *user_data) {
  (void)user_data;
  if (context != g_context)
    return; // Superseded by a rate switch.
  if (!success) {
    logger_error(&g_logger, "Failed to register the WebAudio worklet processor");
    return;
  }
  // The output channel count is fixed for the node's lifetime. The input is
  // mixed down/up ('speakers') to the capture channel count, which can change
  // at any time.
  int output_channels[1] = {g_context_output_channels};
  EmscriptenAudioWorkletNodeCreateOptions options = {
      .numberOfInputs = 1,
      .numberOfOutputs = 1,
      .outputChannelCounts = output_channels,
  };
  g_node = emscripten_create_wasm_audio_worklet_node(
      context, "cdsp-device", &options, webaudio_device_render, NULL);
  EM_ASM(
      {
        var context = emscriptenGetAudioObject($0);
        var node = emscriptenGetAudioObject($1);
        node.channelCount = $2;
        node.channelCountMode = 'explicit';
        node.channelInterpretation = 'speakers';
        // Beyond stereo, route each channel to the matching hardware output
        // instead of down-mixing to the default stereo destination.
        if ($3 > 2) {
          context.destination.channelCount = $3;
          context.destination.channelCountMode = 'explicit';
          context.destination.channelInterpretation = 'discrete';
        }
        node.connect(context.destination);
        globalThis.cdspAudio = ({context : context, node : node});
        globalThis.dispatchEvent(new Event('cdsp-audio-ready'));
      },
      context, g_node, atomic_load(&g_wanted_input_channels),
      g_context_output_channels);
  logger_info(&g_logger, "WebAudio device ready at %d Hz, %d output channels",
              g_context_rate, g_context_output_channels);
}

static void webaudio_device_worklet_started(EMSCRIPTEN_WEBAUDIO_T context,
                                            EM_BOOL success, void *user_data) {
  (void)user_data;
  if (context != g_context)
    return; // Superseded by a rate switch.
  if (!success) {
    logger_error(&g_logger, "Failed to start the WebAudio worklet thread");
    return;
  }
  WebAudioWorkletProcessorCreateOptions options = {
      .name = "cdsp-device",
      .numAudioParams = 0,
      .audioParamDescriptors = NULL,
  };
  emscripten_create_wasm_audio_worklet_processor_async(
      context, &options, webaudio_device_processor_created, NULL);
}

/** Creates the AudioContext and its worklet in the wanted format. */
static void webaudio_device_open_context(void) {
  int rate = atomic_load(&g_wanted_rate);
  int output_channels = atomic_load(&g_wanted_output_channels);
  EmscriptenWebAudioCreateAttributes attrs = {
      .latencyHint = "interactive",
      .sampleRate = (uint32_t)rate,
  };
  EMSCRIPTEN_WEBAUDIO_T context = emscripten_create_audio_context(&attrs);
  if (context <= 0) {
    logger_error(&g_logger, "Failed to create the AudioContext at %d Hz", rate);
    return;
  }
  g_context = context;
  g_context_rate =
      EM_ASM_INT({ return emscriptenGetAudioObject($0).sampleRate; }, context);
  int max_channels = EM_ASM_INT(
      { return emscriptenGetAudioObject($0).destination.maxChannelCount; },
      context);
  if (max_channels > WEBAUDIO_MAX_CHANNELS)
    max_channels = WEBAUDIO_MAX_CHANNELS;
  if (max_channels > 0 && output_channels > max_channels) {
    logger_warn(&g_logger,
                "WebAudio output supports %d channels; %d were requested",
                max_channels, output_channels);
    output_channels = max_channels;
  }
  g_context_output_channels = output_channels;
  // Backends in this format are served once the worklet starts rendering.
  webaudio_device_set_output_channels((size_t)output_channels,
                                      max_channels > 0 ? (size_t)max_channels
                                                       : 0);
  webaudio_device_set_sample_rate(g_context_rate);
  // The previous context is closed, so its worklet no longer uses the stack.
  emscripten_start_wasm_audio_worklet_thread_async(
      context, g_worklet_stack, sizeof(g_worklet_stack),
      webaudio_device_worklet_started, NULL);
  logger_info(&g_logger, "Starting WebAudio device at %d Hz", g_context_rate);
}

static void webaudio_device_switch(void *arg);

/** Called from JS once the previous AudioContext has closed. */
EMSCRIPTEN_KEEPALIVE void webaudio_device_context_closed(void) {
  g_closing = false;
  webaudio_device_switch(NULL);
}

/**
 * Brings the device to the wanted format (main thread). A new rate or output
 * channel count needs a new context: the old one is closed first and the new
 * one created only once it has, so the two worklets never share the stack.
 * Until then the device format stays at the old value, which keeps backends in
 * the new format silent. A new input channel count only re-mixes the node's
 * input.
 */
static void webaudio_device_switch(void *arg) {
  (void)arg;
  if (g_closing)
    return; // webaudio_device_context_closed() picks up the latest format.
  int rate = atomic_load(&g_wanted_rate);
  int output_channels = atomic_load(&g_wanted_output_channels);
  if (!g_context) {
    webaudio_device_open_context();
    return;
  }
  if (g_context_rate == rate &&
      g_context_output_channels == output_channels) {
    if (g_node)
      EM_ASM({ emscriptenGetAudioObject($0).channelCount = $1; }, g_node,
             atomic_load(&g_wanted_input_channels));
    return;
  }
  logger_info(&g_logger,
              "Switching WebAudio device from %d Hz/%d ch to %d Hz/%d ch",
              g_context_rate, g_context_output_channels, rate,
              output_channels);
  g_closing = true;
  EMSCRIPTEN_WEBAUDIO_T old_context = g_context;
  EMSCRIPTEN_AUDIO_WORKLET_NODE_T old_node = g_node;
  g_context = 0;
  g_node = 0;
  g_context_rate = 0;
  g_context_output_channels = 0;
  EM_ASM(
      {
        var context = emscriptenGetAudioObject($0);
        delete EmAudio[$0];
        if ($1)
          delete EmAudio[$1];
        globalThis.cdspAudio = null;
        context.close().finally(function() { _webaudio_device_context_closed(); });
      },
      old_context, old_node);
}

/** Format-request hook: called from a backend's open() on an engine thread. */
static void webaudio_device_request_format(int sample_rate,
                                           size_t input_channels,
                                           size_t output_channels) {
  atomic_store(&g_wanted_rate, sample_rate);
  if (input_channels != 0)
    atomic_store(&g_wanted_input_channels, (int)input_channels);
  if (output_channels != 0)
    atomic_store(&g_wanted_output_channels, (int)output_channels);
  emscripten_proxy_async(emscripten_proxy_get_system_queue(),
                         emscripten_main_runtime_thread_id(),
                         webaudio_device_switch, NULL);
}

void webaudio_device_start(void) {
  // The AudioContext can only be created on the browser main thread, and not
  // at all without Web Audio (e.g. the Node test runner).
  if (!emscripten_is_main_browser_thread() ||
      !EM_ASM_INT({ return typeof AudioContext !== 'undefined'; }))
    return;
  static atomic_bool started = false;
  if (atomic_exchange(&started, true))
    return;

  webaudio_device_set_capture_state_hook(webaudio_device_capture_state_changed);
  webaudio_device_set_format_request_hook(webaudio_device_request_format);
  webaudio_device_open_context();
}

#endif // ENABLE_WEBAUDIO && __EMSCRIPTEN__
