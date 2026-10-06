/**
 * @file webaudio_device.c
 * @brief Browser audio device for the WebAudio backend (Emscripten only).
 *
 * Creates two AudioContexts, capture and playback, each with its own clock and
 * rate, running a Wasm AudioWorklet node (Emscripten -sAUDIO_WORKLET). Each
 * node's render callback runs on the browser's real-time
 * audio thread, shares the module's memory, and calls
 * webaudio_device_process() directly, so the WebAudio backend behaves like a
 * native callback backend (CoreAudio, ASIO, PipeWire) with the engine running
 * on its usual capture, processing and playback pthreads.
 */

#if defined(ENABLE_WEBAUDIO) && defined(__EMSCRIPTEN__)

#include <emscripten.h>
#include <emscripten/threading.h>
#include <emscripten/webaudio.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "backend/webaudio_backend.h"
#include "logging/app_logger.h"

static const logger_t g_logger = {"dsp.backend.webaudio"};

/** Frames per Web Audio render quantum. */
#define WEBAUDIO_QUANTUM_FRAMES 128

/** Context and worklet of one direction. Touched only on the main thread. */
typedef struct webaudio_slot {
  bool is_capture;
  EMSCRIPTEN_WEBAUDIO_T context;
  EMSCRIPTEN_AUDIO_WORKLET_NODE_T node;
  int rate;
  int channels; /* Output channels (playback) or node input channels. */
  bool closing;
  /** Stack of this context's audio worklet thread. */
  uint8_t stack[64 * 1024] __attribute__((aligned(16)));
} webaudio_slot_t;

static webaudio_slot_t g_capture_slot = {.is_capture = true};
static webaudio_slot_t g_playback_slot = {.is_capture = false};

/** Captured stream's format, 0 until the page reports it. */
static int g_input_rate;
static int g_input_channels;

static EM_BOOL webaudio_device_render_capture(
    int num_inputs, const AudioSampleFrame *inputs, int num_outputs,
    AudioSampleFrame *outputs, int num_params, const AudioParamFrame *params,
    void *user_data) {
  (void)num_params;
  (void)params;
  (void)user_data;
  const float *in[WEBAUDIO_MAX_CHANNELS];
  size_t in_channels = 0;
  if (num_inputs > 0) {
    in_channels = (size_t)inputs[0].numberOfChannels;
    if (in_channels > WEBAUDIO_MAX_CHANNELS)
      in_channels = WEBAUDIO_MAX_CHANNELS;
    for (size_t c = 0; c < in_channels; c++)
      in[c] = inputs[0].data + c * WEBAUDIO_QUANTUM_FRAMES;
  }
  // The output only keeps the node pulled (the context has no audible sink).
  for (int o = 0; o < num_outputs; o++)
    memset(outputs[o].data, 0,
           (size_t)outputs[o].numberOfChannels * WEBAUDIO_QUANTUM_FRAMES *
               sizeof(float));
  static const float *const no_input[1] = {NULL};
  webaudio_device_process(in_channels ? in : no_input, in_channels, NULL, 0,
                          WEBAUDIO_QUANTUM_FRAMES);
  return EM_TRUE;
}

static EM_BOOL webaudio_device_render_playback(
    int num_inputs, const AudioSampleFrame *inputs, int num_outputs,
    AudioSampleFrame *outputs, int num_params, const AudioParamFrame *params,
    void *user_data) {
  (void)num_inputs;
  (void)inputs;
  (void)num_params;
  (void)params;
  (void)user_data;
  float *out[WEBAUDIO_MAX_CHANNELS];
  size_t out_channels = 0;
  if (num_outputs > 0) {
    out_channels = (size_t)outputs[0].numberOfChannels;
    if (out_channels > WEBAUDIO_MAX_CHANNELS)
      out_channels = WEBAUDIO_MAX_CHANNELS;
    for (size_t c = 0; c < out_channels; c++)
      out[c] = outputs[0].data + c * WEBAUDIO_QUANTUM_FRAMES;
  }
  webaudio_device_process(NULL, 0, out, out_channels, WEBAUDIO_QUANTUM_FRAMES);
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

/** Publishes a slot's current native format to the backends. */
static void webaudio_device_publish(webaudio_slot_t *slot) {
  if (slot->is_capture)
    webaudio_device_set_capture_format(g_capture_slot.rate,
                                       (size_t)g_input_channels);
  else
    webaudio_device_set_playback_format(g_playback_slot.rate,
                                        (size_t)g_playback_slot.channels);
}

static void webaudio_device_processor_created(EMSCRIPTEN_WEBAUDIO_T context,
                                              EM_BOOL success,
                                              void *user_data) {
  webaudio_slot_t *slot = (webaudio_slot_t *)user_data;
  if (context != slot->context)
    return; // Superseded by a device change.
  if (!success) {
    logger_error(&g_logger, "Failed to register the WebAudio worklet processor");
    return;
  }
  if (slot->is_capture) {
    // The input is mixed down/up ('speakers') to the capture channel count,
    // which can change at any time. One silent output keeps the node pulled.
    int output_channels[1] = {1};
    EmscriptenAudioWorkletNodeCreateOptions options = {
        .numberOfInputs = 1,
        .numberOfOutputs = 1,
        .outputChannelCounts = output_channels,
    };
    slot->node = emscripten_create_wasm_audio_worklet_node(
        context, "cdsp-device", &options, webaudio_device_render_capture, NULL);
    EM_ASM(
        {
          var context = emscriptenGetAudioObject($0);
          var node = emscriptenGetAudioObject($1);
          node.channelCount = $2;
          node.channelCountMode = 'explicit';
          node.channelInterpretation = 'speakers';
          node.connect(context.destination);
          globalThis.cdspAudio = ({context : context, node : node});
          globalThis.dispatchEvent(new Event('cdsp-audio-ready'));
        },
        context, slot->node,
        slot->channels > 0 ? slot->channels : WEBAUDIO_DEFAULT_CHANNELS);
  } else {
    // The output channel count is fixed for the node's lifetime.
    int output_channels[1] = {slot->channels};
    EmscriptenAudioWorkletNodeCreateOptions options = {
        .numberOfInputs = 0,
        .numberOfOutputs = 1,
        .outputChannelCounts = output_channels,
    };
    slot->node = emscripten_create_wasm_audio_worklet_node(
        context, "cdsp-device", &options, webaudio_device_render_playback,
        NULL);
    EM_ASM(
        {
          var context = emscriptenGetAudioObject($0);
          var node = emscriptenGetAudioObject($1);
          // Beyond stereo, route each channel to the matching hardware output
          // instead of down-mixing to the default stereo destination.
          if ($2 > 2) {
            context.destination.channelCount = $2;
            context.destination.channelCountMode = 'explicit';
            context.destination.channelInterpretation = 'discrete';
          }
          node.connect(context.destination);
          globalThis.cdspPlaybackAudio = ({context : context, node : node});
          globalThis.dispatchEvent(new Event('cdsp-audio-ready'));
        },
        context, slot->node, slot->channels);
  }
  logger_info(&g_logger, "WebAudio %s device ready at %d Hz, %d channels",
              slot->is_capture ? "capture" : "playback", slot->rate,
              slot->channels);
}

static void webaudio_device_worklet_started(EMSCRIPTEN_WEBAUDIO_T context,
                                            EM_BOOL success, void *user_data) {
  webaudio_slot_t *slot = (webaudio_slot_t *)user_data;
  if (context != slot->context)
    return; // Superseded by a device change.
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
      context, &options, webaudio_device_processor_created, slot);
}

/** Clamps a destination.maxChannelCount to a usable output channel count. */
static int webaudio_device_clamp_channels(int channels) {
  if (channels <= 0)
    return WEBAUDIO_DEFAULT_CHANNELS;
  return channels > WEBAUDIO_MAX_CHANNELS ? WEBAUDIO_MAX_CHANNELS : channels;
}

/**
 * Creates a slot's AudioContext and worklet in its native format. Playback:
 * the browser's default rate (the output hardware's) and all of the hardware's
 * channels. Capture: the captured stream's rate (the browser's default until
 * known), so the stream is not resampled, rendering to no output device so its
 * clock is independent of playback's.
 */
static void webaudio_device_open_context(webaudio_slot_t *slot) {
  EmscriptenWebAudioCreateAttributes attrs = {
      .latencyHint = "interactive",
      .sampleRate = slot->is_capture ? (uint32_t)g_input_rate : 0,
  };
  EMSCRIPTEN_WEBAUDIO_T context = emscripten_create_audio_context(&attrs);
  if (context <= 0) {
    logger_error(&g_logger, "Failed to create the %s AudioContext",
                 slot->is_capture ? "capture" : "playback");
    return;
  }
  slot->context = context;
  slot->rate =
      EM_ASM_INT({ return emscriptenGetAudioObject($0).sampleRate; }, context);
  if (slot->is_capture) {
    slot->channels = g_input_channels;
    EM_ASM(
        {
          var context = emscriptenGetAudioObject($0);
          if (context.setSinkId)
            context.setSinkId({type : 'none'}).catch(function() {});
        },
        context);
  } else {
    slot->channels = webaudio_device_clamp_channels(EM_ASM_INT(
        { return emscriptenGetAudioObject($0).destination.maxChannelCount; },
        context));
  }
  // Backends in this format are served once the worklet starts rendering; a
  // change from the previous context's format stops the engine.
  webaudio_device_publish(slot);
  // The previous context is closed, so its worklet no longer uses the stack.
  emscripten_start_wasm_audio_worklet_thread_async(
      context, slot->stack, sizeof(slot->stack),
      webaudio_device_worklet_started, slot);
  logger_info(&g_logger, "Starting WebAudio %s device at %d Hz",
              slot->is_capture ? "capture" : "playback", slot->rate);
}

/** Called from JS once a slot's previous AudioContext has closed. */
EMSCRIPTEN_KEEPALIVE void webaudio_device_context_closed(int is_capture) {
  webaudio_slot_t *slot = is_capture ? &g_capture_slot : &g_playback_slot;
  slot->closing = false;
  webaudio_device_open_context(slot);
}

/**
 * Recreates a slot's context: a context's rate (and a worklet node's output
 * count) is fixed for its lifetime. The old one is closed first and the new
 * one created only once it has, so the two worklets never share the stack.
 */
static void webaudio_device_reopen(webaudio_slot_t *slot) {
  if (slot->closing || !slot->context)
    return; // Already being (re)created in the latest native format.
  slot->closing = true;
  EMSCRIPTEN_WEBAUDIO_T old_context = slot->context;
  EMSCRIPTEN_AUDIO_WORKLET_NODE_T old_node = slot->node;
  slot->context = 0;
  slot->node = 0;
  EM_ASM(
      {
        var context = emscriptenGetAudioObject($0);
        delete EmAudio[$0];
        if ($1)
          delete EmAudio[$1];
        if ($2)
          globalThis.cdspAudio = null;
        else
          globalThis.cdspPlaybackAudio = null;
        context.close().finally(function() {
          _webaudio_device_context_closed($2);
        });
      },
      old_context, old_node, slot->is_capture ? 1 : 0);
}

/**
 * Called from JS when the output device may have changed, with the format a
 * fresh AudioContext gets; the playback device follows it.
 */
EMSCRIPTEN_KEEPALIVE void webaudio_device_output_probed(int rate,
                                                        int max_channels) {
  webaudio_slot_t *slot = &g_playback_slot;
  int channels = webaudio_device_clamp_channels(max_channels);
  if (slot->closing || !slot->context ||
      (rate == slot->rate && channels == slot->channels))
    return;
  logger_info(&g_logger, "WebAudio output changed from %d Hz to %d Hz",
              slot->rate, rate);
  logger_info(&g_logger, "WebAudio output channels: %d -> %d", slot->channels,
              channels);
  webaudio_device_reopen(slot);
}

/**
 * Called from JS with the captured stream's format. A new rate recreates the
 * capture context at it; a new channel count re-mixes the node's input. A
 * change of a known value is reported as a format change.
 */
EMSCRIPTEN_KEEPALIVE void webaudio_device_set_input_format(int rate,
                                                           int channels) {
  webaudio_slot_t *slot = &g_capture_slot;
  if (channels > WEBAUDIO_MAX_CHANNELS)
    channels = WEBAUDIO_MAX_CHANNELS;
  if (channels > 0 && channels != g_input_channels) {
    g_input_channels = channels;
    slot->channels = channels;
    if (slot->node)
      EM_ASM({ emscriptenGetAudioObject($0).channelCount = $1; }, slot->node,
             channels);
    if (slot->context && !slot->closing)
      webaudio_device_publish(slot);
  }
  if (rate > 0 && rate != g_input_rate) {
    g_input_rate = rate;
    if (slot->context && rate != slot->rate) {
      logger_info(&g_logger, "WebAudio capture stream at %d Hz (device %d Hz)",
                  rate, slot->rate);
      webaudio_device_reopen(slot);
    }
  }
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
  webaudio_device_open_context(&g_playback_slot);
  webaudio_device_open_context(&g_capture_slot);
  EM_ASM({
    globalThis.cdspAudioDevice = ({
      setInputFormat : function(rate, channels) {
        _webaudio_device_set_input_format(rate | 0, channels | 0);
      }
    });
    // Probe the output's native format with a throwaway context whenever the
    // devices change; the playback device follows it when it differs.
    var md = globalThis.navigator && navigator.mediaDevices;
    if (md && md.addEventListener) {
      md.addEventListener('devicechange', function() {
        var probe;
        try {
          probe = new AudioContext();
        } catch (e) {
          return;
        }
        var rate = probe.sampleRate;
        var channels = probe.destination.maxChannelCount;
        probe.close().finally(function() {
          _webaudio_device_output_probed(rate | 0, channels | 0);
        });
      });
    }
  });
}

#endif // ENABLE_WEBAUDIO && __EMSCRIPTEN__
