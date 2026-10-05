# CDSP Studio — Chrome Extension & WebAssembly Audio DSP

High-performance real-time parametric EQ, biquad filtering, volume control, and audio DSP engine for browser tabs, powered by **CDSP** (CamillaDSP/Rubato C core) compiled to **WebAssembly**.

---

## 1. Prerequisites

- **Google Chrome / Chromium**: Version 116+ (Manifest V3 with `chrome.offscreen` and `chrome.tabCapture` support).
- **Emscripten SDK (`emsdk`)**: exactly `3.1.50` (the version Qt 6.7.3 was built with; embind is not ABI-compatible across emsdk versions).
- **Qt 6.7.3 for WebAssembly** (`wasm_multithread`) plus the desktop host (`gcc_64`), installed using `aqtinstall`:
  ```bash
  pip install aqtinstall
  python3 -m aqt install-qt linux desktop 6.7.3 linux_gcc_64 --outputdir ~/Qt
  python3 -m aqt install-qt all_os wasm 6.7.3 wasm_multithread --outputdir ~/Qt
  ```
- **CMake**: Version 3.20 or newer.
- **Node.js**: for running WebAssembly unit tests.

Make sure the Emscripten environment is activated in your terminal:
```bash
source /path/to/emsdk/emsdk_env.sh
```

---

## 2. Building the WebAssembly Extension & Qt Studio GUI

The WebAssembly build compiles both the AudioWorklet DSP core (`cdsp_wasm.wasm`) and the embedded **Qt 6 WebAssembly Studio GUI** (`cdsp-studio.wasm` / `cdsp-studio.js`). The Qt Studio monitor is required to configure, manage, and inspect the DSP pipeline inside the browser.

Use [`tools/build_wasm.sh`](../tools/build_wasm.sh) — the same script CI runs. It checks toolchain versions, configures, builds, runs the WASM tests, and packages the extension:

```bash
source /path/to/emsdk/emsdk_env.sh
QT_ROOT=~/Qt tools/build_wasm.sh build-wasm
```

> [!NOTE]
> All core dependencies (`fftw3`, `fftw3f`, `libyaml`, `cJSON`) are automatically downloaded and compiled via CMake `FetchContent` during the build.

This builds and packages:
- `build-wasm/extension_dist/` (unpacked Chrome Extension):
  - `manifest.json`
  - `wasm/cdsp_wasm.js` & `cdsp_wasm.wasm` (AudioWorklet DSP core)
  - `worklet/cdsp-processor.js`
  - `offscreen/` (Web Audio host)
  - `background/` (Service Worker)
  - `ui/` (Popup UI and spectrum visualizer)
  - `ui/studio/` (Qt 6 WebAssembly Studio: `cdsp-studio.html`, `cdsp-studio.js`, `cdsp-studio.wasm`)
- `build-wasm/cdsp_extension.zip` (standalone packed zip bundle for distribution)
- `build-wasm/test_wasm.js` & `test_wasm.wasm` (WASM test suite).

---

## 3. Loading the Extension in Google Chrome

1. Open Google Chrome and navigate to:
   ```text
   chrome://extensions
   ```
2. Enable **Developer mode** using the toggle switch in the top-right corner.
3. Click the **Load unpacked** button in the top-left toolbar.
4. In the file picker, select the generated **`build-wasm/extension_dist/`**:
   ```text
   /path/to/cdsp/build-wasm/extension_dist
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
