# CDSP Studio

<p align="center">
  <strong>A high-performance, cross-platform audio DSP monitoring, equalization, and pipeline control suite.</strong>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/C%2B%2B-17-blue.svg?style=flat-square&logo=c%2B%2B" alt="C++17" />
  <img src="https://img.shields.io/badge/Qt-6-green.svg?style=flat-square&logo=qt" alt="Qt 6" />
  <img src="https://img.shields.io/badge/Platform-macOS%20%7C%20Windows%20%7C%20Linux-lightgrey.svg?style=flat-square" alt="Platform" />
  <img src="https://img.shields.io/badge/Engine-cdsp-orange.svg?style=flat-square" alt="Engine" />
</p>

---

## Overview

**CDSP Studio** is the cross-platform Qt 6 / C++ port and evolution of [**CamillaDSP-Monitor**](https://github.com/Wang-Yue/CamillaDSP-Monitor). It brings real-time audio DSP monitoring, interactive pipeline editing, acoustic analysis, room correction, and headphone equalization to **macOS**, **Windows**, and **Linux**.

The application is powered directly by [**cdsp**](https://github.com/Wang-Yue/cdsp) — a lightweight, high-performance C implementation of CamillaDSP with multi-threaded pipeline execution and hardware SIMD acceleration.

---

## Key Features

### 🎛 Real-Time DSP Signal Chain & Pipeline Control
- **Interactive Signal Graph**: Inspect and manage full DSP pipelines including input, capture resampler, mixer matrices, filters, and output stages.
- **Parametric Equalizer (PEQ)**: Real-time interactive frequency response curve visualizer supporting Peaking, Low-Shelf, High-Shelf, Butterworth, Linkwitz-Riley, and custom biquad filters.
- **Convolution Engine**: FIR impulse response (IR) filtering with support for WAV and raw float impulse responses, complete with time-domain and frequency-domain IR visualizers.
- **Multichannel Routing & Mixers**: Flexible matrix mixer configurations, gain/phase trimming, channel muting, and dither control.

### 📊 Comprehensive Audio Visualizers
- **Spectrum Analyzer**: Real-time magnitude spectrum display with configurable FFT resolution, windowing functions, and frequency smoothing.
- **Spectrogram Waterfall**: 2D and 3D waterfall spectrogram views for tracking frequency content and energy over time.
- **Stereo Vector Scope**: Lissajous oscilloscope and stereo correlation meter with particle decay effects.
- **Analog VU Meter**: Classic ballistics VU meters calibrated for accurate average loudness indication.
- **Level Meters**: Multichannel peak, RMS, and true-peak metering with hold indicators and clip alerts.

### 🎧 Headphone EQ & Profile Integration
- **AutoEQ Database Integration**: Search, preview, and load thousands of headphone frequency response correction profiles directly from AutoEQ.
- **Oratory1990 Presets**: One-click import for Harman-target headphone EQ presets.

### 🏠 Acoustic Wizards & Room Correction
- **Room Correction Assistant**: Automated PEQ curve fitting algorithms (`PEQAutoFit`) that calculate optimal filter parameters from measurement sweeps.
- **Subwoofer Integration Assist**: Crossover frequency matching, time-alignment delay estimation, and phase alignment tools.

### 🪟 Sleek Floating Mini Player
- **Frameless Floating HUD**: Compact, translucent floating player with quick volume, mute, playback toggles, and visualizer cycling.
- **Edge Resizing**: Resizable border handles with persistent geometry across sessions.

### 🔔 System Tray & Background Operation
- **System Tray Integration**: Background audio processing with options to minimize to tray or close window to tray without interrupting playback.
- **Multi-Resolution App Icon**: Vector-rendered icons optimized for OS taskbars and system tray menus.

---

## Audio Backends & Acceleration

CDSP Studio automatically leverages optimal platform audio APIs and hardware SIMD acceleration:

| Platform | Audio Backend | Hardware Acceleration | Concurrency |
| :--- | :--- | :--- | :--- |
| **macOS** | CoreAudio | Apple Accelerate Framework (`vDSP`) | Grand Central Dispatch (`libdispatch`) |
| **Windows** | WASAPI (Exclusive / Shared), ASIO | FFTW3 (`libfftw3` / `libfftw3f` SIMD: AVX2, SSE2) | `libdispatch` / C++17 Threads |
| **Linux** | ALSA, PipeWire | FFTW3 (`libfftw3` / `libfftw3f`) | `libdispatch` / C++17 Threads |

---

## Building

CDSP Studio is built directly as part of the unified CDSP build tree. See the top-level [**README.md**](../README.md#building-from-source) for prerequisites and build instructions.

```bash
# From repository root:
cmake -B build -S .
cmake --build build -j
```

---

## Project Structure

```text
studio/
├── CMakeLists.txt              # CMake build configuration and platform dependencies
├── main.cpp                    # Application entrypoint & high-DPI initialization
├── config/                     # Biquad coefficients, DSP types, and configuration models
├── engine/                     # CDSPEngine bridge to native cdsp processing core
├── models/                     # AudioDeviceManager, AudioSettings, PipelineStore, LogManager
├── room_correction/            # AutoEQ parsing, PEQ auto-fitting, measurement filters
├── ui/                         # Qt Widgets: Dashboard, MiniPlayer, Visualizers, Dialogs
├── utils/                      # App icon rendering, Math utilities, Helpers
└── resources/                  # Qt resource bundles (.qrc) and icons
```

---

## Acknowledgments & Related Projects

- [**CamillaDSP-Monitor**](https://github.com/Wang-Yue/CamillaDSP-Monitor): The original macOS / SwiftUI audio monitor and DSP controller interface upon which this project is based.
- [**cdsp**](https://github.com/Wang-Yue/cdsp): High-performance C implementation of the CamillaDSP audio processing engine.
- [**CamillaDSP**](https://github.com/HEnquist/camilladsp): The original cross-platform IIR and FIR audio processing engine by Henrik Enquist.
- [**AutoEq**](https://github.com/jaakkopasanen/AutoEq): Automatic headphone equalization dataset and tooling by Jaakko Pasanen.
- [**oratory1990**](https://www.reddit.com/r/oratory1990/): Accurate acoustic headphone measurement database.

---

## License

This project is licensed under the terms of the project's repository license.
