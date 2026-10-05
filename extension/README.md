# CDSP Studio — Chrome Extension & WebAssembly Audio DSP

High-performance real-time parametric EQ, biquad filtering, volume control, and audio DSP engine for browser tabs, powered by **CDSP** (CamillaDSP/Rubato C core) compiled to **WebAssembly**.

---

## 1. Prerequisites

- **Google Chrome / Chromium**: Version 116+ (Manifest V3 with `chrome.tabCapture` and cross-origin isolated extension pages for `SharedArrayBuffer`).
- **Emscripten SDK (`emsdk`)**: exactly `3.1.50` (the version Qt 6.7.3 was built with; embind is not ABI-compatible across emsdk versions).
- **Qt 6.7.3 for WebAssembly** (`wasm_multithread`) plus the desktop host (`gcc_64`), installed using `aqtinstall`:
  ```bash
  pip install aqtinstall
  python3 -m aqt install-qt linux desktop 6.7.3 linux_gcc_64 --outputdir ~/Qt
  python3 -m aqt install-qt all_os wasm 6.7.3 wasm_multithread -m qtmultimedia --outputdir ~/Qt
  ```
- **CMake**: Version 3.20 or newer.
- **Node.js**: for running WebAssembly unit tests.

Make sure the Emscripten environment is activated in your terminal:
```bash
source /path/to/emsdk/emsdk_env.sh
```

---

## 2. Building the WebAssembly Extension & Qt Studio GUI

The WebAssembly build compiles the **Qt 6 WebAssembly Studio** (`cdsp-studio.wasm` / `cdsp-studio.js`), which contains both the GUI and the multithreaded CDSP engine — the same engine and `CDSPEngine` code path as the desktop Studio.

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
  - `background/` (Service Worker: opens Studio, picks the tab to capture, badge)
  - `ui/icons/`
  - `ui/studio/` (Qt 6 WebAssembly Studio: `cdsp-studio.html`, `cdsp-studio.js`, `cdsp-studio.wasm`, `cdsp-studio.worker.js` (pthreads), `cdsp-studio.aw.js` (AudioWorklet), `studio-bootstrap.js`)
- `build-wasm/cdsp_extension.zip` (standalone packed zip bundle for distribution)
- `build-wasm/test_wasm.js` & `test_wasm.wasm` (WASM test suite: runs the threaded engine with the WebAudio backend under node and checks bit-exact output).

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
3. **Open CDSP Studio**: Click the **CDSP** toolbar icon. The Studio opens in its own app-style window (no tabs or address bar); its size and position are remembered. Clicking the icon again focuses it.
4. **Start the DSP engine** in Studio. When the engine opens its WebAudio capture backend, Studio captures the audible (or most recently active) tab; its audio runs through the CDSP pipeline and out to your speakers/headphones. Closing the Studio window stops processing and releases the capture.

> [!NOTE]
> Chrome keeps an AudioContext suspended until the page receives a user gesture; click anywhere in Studio once if audio does not start.

---

## 5. Extension Architecture (Manifest V3)

```text
┌────────────────────────────────────────────────────────────┐
│ Captured tab audio (chrome.tabCapture → MediaStream)       │
└──────────────────────────┬─────────────────────────────────┘
                           │ getUserMedia (Studio page)
                           ▼
┌────────────────────────────────────────────────────────────┐
│ CDSP Studio tab (cdsp-studio.html, Qt for WebAssembly)     │
│                                                            │
│  AudioContext ─ MediaStreamSource ─▶ Wasm AudioWorklet node │
│                                       │ real-time audio    │
│                                       │ thread, 128 frames │
│                                       ▼                    │
│                     webaudio_device_process()              │
│                       │ lock-free SPSC rings ▲             │
│                       ▼                      │             │
│  Engine pthreads: capture ─▶ processing ─▶ playback        │
│  (same multithreaded engine & CDSPEngine as desktop)       │
│                                                            │
│  Worklet node ─▶ audioContext.destination (speakers)       │
└────────────────────────────────────────────────────────────┘
```
