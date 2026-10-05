/**
 * @file cdsp-processor.js
 * @brief Web Audio AudioWorkletProcessor running CDSP DSP engine via WebAssembly.
 *
 * Real-time audio quantum processing (128 frames) delegating 100% of DSP
 * filtering, volume/mute fader management, true peak/RMS metering, and FFT spectrum
 * analysis to the compiled CDSP C engine core (`cdsp_wasm.wasm`).
 */

// Polyfill performance.now if running in AudioWorkletGlobalScope without performance object
if (typeof performance === 'undefined') {
  globalThis.performance = {
    now: () => Date.now()
  };
} else if (typeof performance.now !== 'function') {
  performance.now = () => Date.now();
}

class CdspProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super(options);

    this.quantumSize = 128;
    this.inChannels = 2;
    this.outChannels = 2;
    this.telemetryCounter = 0;

    const procOpts = options?.processorOptions || {};

    // Initial parameters & config
    this.volumeDb = procOpts.volumeDb ?? 0.0;
    this.muted = !!procOpts.muted;
    this.activeConfig = procOpts.initialConfig || null;

    // Metering state (in dBFS)
    this.inPeak = [-120.0, -120.0];
    this.inRms = [-120.0, -120.0];
    this.outPeak = [-120.0, -120.0];
    this.outRms = [-120.0, -120.0];

    // Spectrum state (48 logarithmic bands)
    this.nSpectrumBins = 48;
    this.inSpectrum = new Float32Array(this.nSpectrumBins).fill(-120.0);
    this.outSpectrum = new Float32Array(this.nSpectrumBins).fill(-120.0);

    // WASM instance handle & linear memory pointers
    this.wasmInstance = null;
    this.wasmCtx = null;
    this.wasmInPtrs = null;
    this.wasmOutPtrs = null;
    this.wasmVuPtrs = null;
    this.wasmSpecInPtr = null;
    this.wasmSpecOutPtr = null;

    // Telemetry generation is disabled by default to minimize CPU when popup is closed
    this.telemetryActive = false;

    // Listen for control commands from offscreen/popup
    this.port.onmessage = (e) => this.handleMessage(e.data);

    // Initialize WebAssembly engine
    this.initWasm(procOpts.wasmBytes);
  }

  async initWasm(wasmBytes) {
    const loader = (typeof loadCdspWasm !== 'undefined')
      ? loadCdspWasm
      : (typeof globalThis !== 'undefined' && globalThis.loadCdspWasm
          ? globalThis.loadCdspWasm
          : (typeof self !== 'undefined' ? self.loadCdspWasm : undefined));

    if (loader) {
      try {
        const opts = {};
        if (wasmBytes) {
          opts.wasmBinary = wasmBytes;
        }
        const module = await loader(opts);
        this.setupWasmContext(module);
      } catch (err) {
        console.error('[CDSP Worklet] WASM initialization failed:', err);
      }
    } else {
      console.warn('[CDSP Worklet] loadCdspWasm loader not found in worklet scope');
    }
  }

  updateBufferPointers() {
    if (!this.wasmInstance || !this.wasmCtx) return;
    const inPtrArray = this.wasmInstance._cdsp_wasm_get_input_buffer_ptrs(this.wasmCtx);
    const outPtrArray = this.wasmInstance._cdsp_wasm_get_output_buffer_ptrs(this.wasmCtx);

    const heapU32 = this.wasmInstance.HEAPU32;
    this.wasmInPtrs = [
      heapU32[(inPtrArray >> 2) + 0],
      heapU32[(inPtrArray >> 2) + 1]
    ];
    this.wasmOutPtrs = [
      heapU32[(outPtrArray >> 2) + 0],
      heapU32[(outPtrArray >> 2) + 1]
    ];
  }

  setupWasmContext(module) {
    this.wasmInstance = module;
    const configStr = this.activeConfig
      ? (typeof this.activeConfig === 'string' ? this.activeConfig : JSON.stringify(this.activeConfig))
      : '';

    const strPtr = module.allocateUTF8(configStr);
    this.wasmCtx = module._cdsp_wasm_create(
      strPtr,
      sampleRate || 48000,
      this.quantumSize
    );
    module._free(strPtr);

    if (this.wasmCtx) {
      // Set initial volume and mute on CDSP engine fader
      module._cdsp_wasm_set_fader_volume(this.wasmCtx, 0, this.volumeDb, true);
      module._cdsp_wasm_set_fader_mute(this.wasmCtx, 0, this.muted);

      this.updateBufferPointers();

      this.wasmVuPtrs = {
        inPeak: module._malloc(8),
        inRms: module._malloc(8),
        outPeak: module._malloc(8),
        outRms: module._malloc(8)
      };
      this.wasmSpecInPtr = module._malloc(this.nSpectrumBins * 4);
      this.wasmSpecOutPtr = module._malloc(this.nSpectrumBins * 4);

      console.log('[CDSP Worklet] CDSP WebAssembly engine initialized successfully.');
    }
  }

  handleMessage(data) {
    if (!data) return;

    if (data.type === 'SET_TELEMETRY_ACTIVE') {
      this.telemetryActive = !!data.active;
      return;
    }

    if (data.type === 'SET_VOLUME') {
      this.volumeDb = data.db ?? 0.0;
      if (this.wasmInstance && this.wasmCtx) {
        this.wasmInstance._cdsp_wasm_set_fader_volume(
          this.wasmCtx,
          data.fader ?? 0,
          this.volumeDb,
          data.instant ?? false
        );
      }
    }

    if (data.type === 'SET_MUTE') {
      this.muted = !!data.mute;
      if (this.wasmInstance && this.wasmCtx) {
        this.wasmInstance._cdsp_wasm_set_fader_mute(
          this.wasmCtx,
          data.fader ?? 0,
          this.muted
        );
      }
    }

    if (data.type === 'SET_CONFIG') {
      this.activeConfig = data.configJson;
      if (this.wasmInstance && this.wasmCtx) {
        const jsonStr = typeof data.configJson === 'string'
          ? data.configJson
          : JSON.stringify(data.configJson);
        const strPtr = this.wasmInstance.allocateUTF8(jsonStr);
        this.wasmInstance._cdsp_wasm_set_config_json(this.wasmCtx, strPtr);
        this.wasmInstance._free(strPtr);
        this.updateBufferPointers();
      }
    }
  }

  process(inputs, outputs, parameters) {
    const input = inputs[0];
    const output = outputs[0];

    if (!input || input.length === 0 || !output || output.length === 0) {
      return true;
    }

    const numChannels = Math.min(input.length, output.length, 2);
    const frames = input[0].length; // 128 frames

    // CDSP WebAssembly Core Processing
    if (this.wasmInstance && this.wasmCtx && this.wasmInPtrs && this.wasmOutPtrs) {
      const heapF32 = this.wasmInstance.HEAPF32;

      // 1. Copy incoming Web Audio float samples into WASM planar input buffers
      for (let ch = 0; ch < numChannels; ch++) {
        heapF32.set(input[ch], this.wasmInPtrs[ch] >> 2);
      }

      // 2. Execute full CDSP C pipeline (Filters, Mixers, Processors, Faders, History Ring Buffers)
      const success = this.wasmInstance._cdsp_wasm_process(this.wasmCtx, frames);

      // 3. Copy processed output float samples out to Web Audio output buffers
      if (success) {
        for (let ch = 0; ch < numChannels; ch++) {
          const outOffset = this.wasmOutPtrs[ch] >> 2;
          output[ch].set(heapF32.subarray(outOffset, outOffset + frames));
        }
      } else {
        // Fallback pass-through if engine inactive
        for (let ch = 0; ch < numChannels; ch++) {
          output[ch].set(input[ch]);
        }
      }

      // 4. Query True Peak & RMS VU levels and FFT Spectrum ONLY when popup UI is actively open
      if (this.telemetryActive) {
        if (this.wasmVuPtrs) {
          this.wasmInstance._cdsp_wasm_get_vu_levels(
            this.wasmCtx,
            this.wasmVuPtrs.inPeak,
            this.wasmVuPtrs.inRms,
            this.wasmVuPtrs.outPeak,
            this.wasmVuPtrs.outRms
          );
          const inPeakOff = this.wasmVuPtrs.inPeak >> 2;
          const inRmsOff = this.wasmVuPtrs.inRms >> 2;
          const outPeakOff = this.wasmVuPtrs.outPeak >> 2;
          const outRmsOff = this.wasmVuPtrs.outRms >> 2;

          this.inPeak[0] = heapF32[inPeakOff + 0];
          this.inPeak[1] = heapF32[inPeakOff + 1];
          this.inRms[0] = heapF32[inRmsOff + 0];
          this.inRms[1] = heapF32[inRmsOff + 1];
          this.outPeak[0] = heapF32[outPeakOff + 0];
          this.outPeak[1] = heapF32[outPeakOff + 1];
          this.outRms[0] = heapF32[outRmsOff + 0];
          this.outRms[1] = heapF32[outRmsOff + 1];
        }

        // 5. Query 48-band Logarithmic Spectrum from CDSP C Spectrum Analyzer (~25 Hz)
        if (this.telemetryCounter >= 14 && this.wasmSpecInPtr && this.wasmSpecOutPtr) {
          const nBins = this.nSpectrumBins;

          const okIn = this.wasmInstance._cdsp_wasm_get_spectrum(
            this.wasmCtx,
            1 /* is_capture */, 0 /* channel */, 20.0, 20000.0, nBins, this.wasmSpecInPtr
          );
          if (okIn) {
            const inOff = this.wasmSpecInPtr >> 2;
            this.inSpectrum.set(heapF32.subarray(inOff, inOff + nBins));
          } else {
            this.inSpectrum.fill(-120.0);
          }

          const okOut = this.wasmInstance._cdsp_wasm_get_spectrum(
            this.wasmCtx,
            0 /* is_playback */, 0 /* channel */, 20.0, 20000.0, nBins, this.wasmSpecOutPtr
          );
          if (okOut) {
            const outOff = this.wasmSpecOutPtr >> 2;
            this.outSpectrum.set(heapF32.subarray(outOff, outOff + nBins));
          } else {
            this.outSpectrum.fill(-120.0);
          }
        }

        // Telemetry dispatch throttled to ~25 Hz (every 15 quantums of 128 frames = ~40 ms)
        if (++this.telemetryCounter >= 15) {
          this.telemetryCounter = 0;
          this.port.postMessage({
            type: 'TELEMETRY',
            telemetry: {
              inPeak: [...this.inPeak],
              inRms: [...this.inRms],
              outPeak: [...this.outPeak],
              outRms: [...this.outRms],
              inSpectrum: Array.from(this.inSpectrum),
              outSpectrum: Array.from(this.outSpectrum)
            }
          });
        }
      }
    } else {
      // Direct pass-through before WASM core is ready
      for (let ch = 0; ch < numChannels; ch++) {
        output[ch].set(input[ch]);
      }

      if (this.telemetryActive) {
        for (let ch = 0; ch < numChannels; ch++) {
          const chData = input[ch];
          let peak = 0.0;
          let sumSq = 0.0;
          for (let i = 0; i < frames; i++) {
            const s = Math.abs(chData[i]);
            if (s > peak) peak = s;
            sumSq += s * s;
          }
          const peakDb = peak > 1e-6 ? 20.0 * Math.log10(peak) : -120.0;
          const rmsDb = sumSq > 1e-12 ? 10.0 * Math.log10(sumSq / frames) : -120.0;

          this.inPeak[ch] = peakDb;
          this.inRms[ch] = rmsDb;
          this.outPeak[ch] = peakDb;
          this.outRms[ch] = rmsDb;
        }

        if (++this.telemetryCounter >= 15) {
          this.telemetryCounter = 0;
          this.port.postMessage({
            type: 'TELEMETRY',
            telemetry: {
              inPeak: [...this.inPeak],
              inRms: [...this.inRms],
              outPeak: [...this.outPeak],
              outRms: [...this.outRms],
              inSpectrum: Array.from(this.inSpectrum),
              outSpectrum: Array.from(this.outSpectrum)
            }
          });
        }
      }
    }

    return true;
  }
}

registerProcessor('cdsp-processor', CdspProcessor);
