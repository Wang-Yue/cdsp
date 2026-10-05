# CDSP Studio — Chrome Extension & WebAssembly Audio DSP

High-performance real-time parametric EQ, biquad filtering, volume control, and audio DSP engine for browser tabs, powered by **CDSP** (CamillaDSP/Rubato C core) compiled to **WebAssembly**.

---

## 1. Prerequisites

- **Google Chrome / Chromium**: Version 116+ (Manifest V3 with `chrome.offscreen` and `chrome.tabCapture` support).
- **Emscripten SDK (`emsdk`)**: Version 3.1.40 or newer.
- **CMake**: Version 3.20 or newer.
- **Node.js**: (Optional, for running WebAssembly unit tests).

Make sure the Emscripten environment is activated in your terminal:
```bash
source /path/to/emsdk/emsdk_env.sh
```

---

## 2. Building the WebAssembly Extension

The top-level [`CMakeLists.txt`](../CMakeLists.txt) automatically detects the Emscripten toolchain, fetches and configures all required dependencies (`FFTW3` with pre-generated scalar codelets for single/double precision, `libyaml`, `cJSON`), and compiles both the WebAssembly DSP engine and the complete Chrome Extension bundle into the **build directory** (`<build-dir>/extension`), keeping the source tree completely clean and unpolluted.

### Build Commands
```bash
# 1. Activate the Emscripten SDK environment
source /path/to/emsdk/emsdk_env.sh

# 2. Configure and build using Emscripten CMake wrapper (parallel multi-core build)
emcmake cmake -B build-wasm -DCMAKE_BUILD_TYPE=Release
cmake --build build-wasm -j
```

> [!NOTE]
> All build dependencies (`fftw3`, `fftw3f`, `libyaml`, `cJSON`) are automatically downloaded and compiled via CMake `FetchContent` during the Emscripten build.

This builds and packages:
- `build-wasm/extension/` (the complete self-contained unpacked Chrome Extension):
  - `build-wasm/extension/manifest.json`
  - `build-wasm/extension/wasm/cdsp_wasm.js` & `cdsp_wasm.wasm`
  - `build-wasm/extension/worklet/cdsp-processor.js`
  - `build-wasm/extension/offscreen/`
  - `build-wasm/extension/background/`
  - `build-wasm/extension/ui/`
- `build-wasm/extension/test_wasm.js` & `test_wasm.wasm` (WASM unit test suite).

---

## 3. Loading the Extension in Google Chrome

1. Open Google Chrome and navigate to:
   ```text
   chrome://extensions
   ```
2. Enable **Developer mode** using the toggle switch in the top-right corner.
3. Click the **Load unpacked** button in the top-left toolbar.
4. In the file picker, select the generated **`build-wasm/extension/`** (or your custom build folder, e.g. `build/extension/`):
   ```text
   /path/to/cdsp/build-wasm/extension
   ```
5. The **CDSP Audio Studio** extension card will appear in your installed extensions list.

---

## 4. Enabling & Using DSP on Browser Tabs

1. **Pin Extension**: Click the puzzle piece icon (Extensions menu) in the Chrome toolbar and pin **CDSP Audio Studio**.
2. **Open Audio Source**: Navigate to any tab playing audio (e.g. YouTube, Spotify Web, SoundCloud, Twitch, Apple Music).
3. **Activate Tab Capture**:
   - Click the **CDSP** toolbar icon to open the popup.
   - Click **"Enable DSP for this Tab"**.
   - Audio from the tab is redirected into the CDSP AudioWorklet DSP pipeline and routed seamlessly to your speakers/headphones with minimal latency.
4. **Live Controls & Visualization**:
   - **Real-Time Spectrum Analyzer**: View live input (Cyan) vs output (Emerald) FFT spectrum curves from 20 Hz to 20 kHz.
   - **Stereo VU Meters**: Monitor input and output peak levels (dBFS) and clipping indicators.
   - **Master Volume & Mute**: Adjust output gain with smooth fader ramps.
   - **DSP Presets**: Select presets (Flat Bypass, Bass Boost, Harman Target EQ, Vocal Clarity, Late Night Limiter).
5. **Open Full Studio GUI**:
   - Click the pop-out icon in the top right of the popup header to open the interactive parametric EQ diagram in Chrome **Side Panel** or a dedicated tab (`studio.html`).

---

## 5. Extension Architecture (Manifest V3)

```text
┌────────────────────────────────────────────────────────┐
│ Chrome Tab Audio Stream (MediaStream / 48 kHz PCM)      │
└──────────────────────────┬─────────────────────────────┘
                           │ chrome.tabCapture
                           ▼
┌────────────────────────────────────────────────────────┐
│ Offscreen Document (offscreen.html / offscreen.js)     │
│  ├─ Web Audio API AudioContext                         │
│  └─ AudioWorkletNode ('cdsp-processor')                │
│       │ Synchronous 128-frame Audio Thread Quantums    │
│       ▼                                                │
│  ┌──────────────────────────────────────────────────┐  │
│  │ WebAssembly CDSP Core Engine (cdsp_wasm.wasm)    │  │
│  │  ├─ WebAudio Lock-Free SPSC Ring Buffers         │  │
│  │  ├─ Strict IEEE 754 Double Precision DSP Pipeline│  │
│  │  ├─ Real-Time Parametric Biquad Filters & Gains  │  │
│  │  ├─ Zero-Allocation Linear Memory Transfer       │  │
│  │  └─ Live VU Levels & Spectrum Estimators         │  │
│  └──────────────────────────────────────────────────┘  │
│       │ Processed Audio                                │
│       ▼                                                │
│  audioContext.destination (Hardware Audio Output)      │
└────────────────────────────────────────────────────────┘
```
