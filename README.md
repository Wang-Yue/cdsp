# CDSP Audio Suite

<p align="center">
  <strong>A high-performance, cross-platform audio DSP engine, CLI daemon, and Qt 6 desktop studio suite.</strong>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Language-C11%20%7C%20C%2B%2B17-blue.svg?style=flat-square" alt="Language" />
  <img src="https://img.shields.io/badge/GUI-Qt%206-green.svg?style=flat-square&logo=qt" alt="Qt 6" />
  <img src="https://img.shields.io/badge/Platform-macOS%20%7C%20Linux%20%7C%20Windows-lightgrey.svg?style=flat-square" alt="Platform" />
  <img src="https://img.shields.io/badge/License-GPLv3-orange.svg?style=flat-square" alt="License" />
</p>

---

## Overview

**CDSP** is a modular, high-throughput audio digital signal processing suite engineered for ultra-low latency, lock-free real-time audio routing, parametric equalization, convolution filtering, and acoustic measurement.

The repository is organized into three primary components:

1. **[Core C DSP Engine (`libcdsp`) & CLI Daemon (`cdsp`)](docs/ENGINE.md)**:
   A lightweight, drop-in replacement for CamillaDSP with hardware SIMD acceleration (Apple Accelerate / NEON / AVX2 / FFTW3), wait-free SPSC queue concurrency, driverless macOS CoreAudio loopback, and native DSD/DoP decoding and encoding.
2. **[CDSP Studio (`cdsp-studio`)](studio/README.md)**:
   A cross-platform Qt 6 / C++ desktop application providing real-time DSP signal chain visualization, interactive parametric EQ design, FIR impulse response filtering, acoustic room correction wizards, headphone AutoEQ / Oratory1990 preset databases, and floating mini-players.
3. **[ALSA Rate Notify Plugin (`plugins/`)](plugins/README.md)**:
   A native Linux ALSA `ioplug` module enabling automatic, bit-perfect sample rate and format switching over `snd-aloop` without audio drops.

---

## Screenshots

![CDSP Studio visualization dashboard](Visualization.png)

![CDSP Studio parametric equalizer diagram](EQDiagram.png)

![CDSP Studio audio device settings (Linux)](DeviceSetting.png)

![CDSP Studio dashboard (Linux)](Dashboard.png)

---

## Project Structure

```text
cdsp/
├── CMakeLists.txt              # Root CMake build configuration
├── src/                        # Core C DSP engine (audio, backend, config, dsd, engine, filters, resampler)
├── include/                    # Public C API headers (include/cdsp/cdsp.h)
├── app/                        # CLI daemon & WebSocket RPC server entry point
│   ├── main.c
│   └── server/
├── studio/                     # CDSP Studio (Qt 6 / C++ GUI application)
│   ├── CMakeLists.txt
│   ├── main.cpp                # Qt application entry point
│   ├── ui/                     # Views, plots, dialogs, visualizers, mini-player
│   ├── models/                 # Audio devices, presets, monitoring, pipeline models
│   ├── engine/                 # DSP engine controller & state bridge
│   ├── room_correction/        # AutoFit curve fitting, sweeps, subwoofer assist
│   ├── resources/              # Icons (app.icns/app.png), QRC resource bundle
│   ├── cmake/
│   └── README.md
├── plugins/                    # ALSA rate and format notification plugin for Linux
│   ├── CMakeLists.txt
│   ├── pcm_rate_notify.c
│   └── README.md
├── docs/                       # Technical specifications, audits, and architecture deep dives
│   ├── ENGINE.md               # Core engine architecture & benchmark evaluation
│   ├── INTENTIONAL_DIVERGENCES.md # Architectural enhancements & safety guarantees
│   ├── engine_state_management.md # Real-time state machine & thread concurrency model
│   ├── dsp_engine_public_api_alignment.md # Public C API dispatch contract
│   └── callgraph_audit_report.md # Real-time audio loop zero-lock/zero-alloc audit
├── tests/                      # Full unit, integration, and benchmark test suite
└── tools/                      # Code generation, schema generators, and callgraph AST audit tools
```

---

## Key Features

- ⚡ **High-Throughput Real-Time Audio**: Up to **1.8x faster** filter execution and **1.7x faster** resampling throughput with full multi-threaded dynamic scheduling (Apple GCD / OpenMP).
- 🔒 **Zero-Lock & Zero-Allocation Audio Loops**: Verified by automated AST Call Graph Auditing to ensure steady-state audio threads never acquire mutexes or invoke dynamic memory allocators.
- 🪟 **Rich Desktop Experience**: Full-featured Qt 6 GUI with interactive frequency response curves, vector scopes, waterfall spectrograms, VU meters, and AutoEQ database integration.
- 🎧 **Native DSD & DoP Support**: In-place decoding and encoding for DSD64–DSD512 and DoP carrier streams.
- 🍏 **Driverless macOS Loopback**: Native process-level and hardware-level audio capture via `CATapDescription` without third-party virtual audio cables.
- 🐧 **Bit-Perfect Linux Switching**: ALSA rate notify plugin intercepts player sample rate transitions and coordinates dynamic engine restarts.

---

## Building from Source

### Prerequisites & Dependencies

- **C/C++ Compiler**: C11 and C++17 compatible compiler (Clang, GCC, or MSVC)
- **CMake**: `3.20` or newer
- **FFTW3**: Double & single precision FFT libraries (`libfftw3`, `libfftw3f`)
- **Qt 6** *(Required for CDSP Studio GUI)*: `Core`, `Widgets`, `Network`, `Concurrent`, `Multimedia`

#### macOS (Homebrew)
```bash
brew install cmake fftw qt
```

#### Linux (Debian / Ubuntu)
```bash
sudo apt-get update && sudo apt-get install -y \
    build-essential cmake \
    libfftw3-dev \
    libasound2-dev libpipewire-0.3-dev libdbus-1-dev \
    qt6-base-dev qt6-multimedia-dev
```

#### Windows (MSYS2 UCRT64)
In the MSYS2 UCRT64 shell:
```bash
pacman -S --needed \
    mingw-w64-ucrt-x86_64-gcc \
    mingw-w64-ucrt-x86_64-cmake \
    mingw-w64-ucrt-x86_64-ninja \
    mingw-w64-ucrt-x86_64-fftw \
    mingw-w64-ucrt-x86_64-qt6-base \
    mingw-w64-ucrt-x86_64-qt6-multimedia
```

---

### Build Commands

#### 1. Full Build (Core Engine, Daemon & CDSP Studio)

```bash
cmake -B build -S .
cmake --build build -j
```

The compiled binaries will be placed in `build/bin/`:
- `build/bin/cdsp` — Core DSP CLI daemon & WebSocket RPC server
- `build/bin/CDSPStudio` (or `.app` on macOS) — Qt 6 Desktop GUI Studio

#### 2. Headless Build (Core Engine & CLI Daemon Only)

If building on headless servers, embedded devices, or minimal environments without Qt:

```bash
cmake -B build -S . -DENABLE_STUDIO=OFF
cmake --build build -j
```

#### 3. Run Test Suite

```bash
ctest --test-dir build -j --output-on-failure
```

#### 4. Code Formatting & Static Analysis

```bash
# Format all C/C++ source and header files
cmake --build build --target format

# Check formatting without modifying files
cmake --build build --target format-check

# Run Include-What-You-Use (IWYU) analysis
cmake --build build --target iwyu
```

---

## Documentation

- 📖 **[Core DSP Engine Deep Dive](docs/ENGINE.md)** — In-depth concurrency design, benchmarks, and performance evaluation.
- 🎨 **[CDSP Studio Guide](studio/README.md)** — GUI features, screenshots, and acoustic wizards.
- 🛡️ **[Intentional Divergences & Safety Enhancements](docs/INTENTIONAL_DIVERGENCES.md)** — Safe volume ramping, DSP precision, and real-time invariants.
- 🔄 **[Engine State Management Specification](docs/engine_state_management.md)** — Lock-free thread coordination and atomic state machine.
- 🔌 **[Public C API Specification](docs/dsp_engine_public_api_alignment.md)** — Direct C library embedding and FFI dispatch contract.
- 🔬 **[Static Call Graph Audit Report](docs/callgraph_audit_report.md)** — Formal verification of zero-lock and zero-allocation hot paths.

---

## License & Attribution

CDSP is licensed under the **[GNU General Public License v3.0 (GPLv3)](LICENSE)**. Full copyright notices for upstream works and contributors are preserved in the **[NOTICE](NOTICE)** file.
