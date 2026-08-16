#!/bin/bash
set -e

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$PROJECT_DIR"

SYSROOT="${RPI_SYSROOT:-/Users/wangyue/rpi-sysroot}"
TOOLCHAIN_FILE="${TOOLCHAIN_FILE:-$SYSROOT/rpi-toolchain.cmake}"
BUILD_DIR="${BUILD_DIR:-$PROJECT_DIR/build-rpi}"

if [ ! -d "$SYSROOT" ]; then
    echo "❌ Error: Sysroot directory not found at $SYSROOT"
    echo "Please specify the RPI_SYSROOT environment variable."
    exit 1
fi

if [ ! -f "$TOOLCHAIN_FILE" ]; then
    echo "❌ Error: Toolchain file not found at $TOOLCHAIN_FILE"
    exit 1
fi

# Matching cdsp/cross_build.sh options:
# ENABLE_ALSA=1, ENABLE_PIPEWIRE=1, ENABLE_FFTW=1, USE_LIBDISPATCH=1
ENABLE_FFTW="${ENABLE_FFTW:-ON}"
USE_LIBDISPATCH="${USE_LIBDISPATCH:-ON}"

NPROC="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)}"
export CMAKE_BUILD_PARALLEL_LEVEL="$NPROC"

echo "=== Cross-compiling Monitor-Qt for Raspberry Pi (aarch64) on Mac ==="
echo "Matching cdsp options: ALSA=1, PIPEWIRE=1, FFTW=${ENABLE_FFTW}, LIBDISPATCH=${USE_LIBDISPATCH}"
echo "Sysroot:   $SYSROOT"
echo "Toolchain: $TOOLCHAIN_FILE"
echo "Build Dir: $BUILD_DIR"
echo "Jobs:      $NPROC"

cmake -B "$BUILD_DIR" -S "$PROJECT_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_FFTW="$ENABLE_FFTW" \
    -DUSE_LIBDISPATCH="$USE_LIBDISPATCH"

cmake --build "$BUILD_DIR" --parallel "$NPROC" "$@"

echo "✅ Raspberry Pi cross-compilation complete: $BUILD_DIR/MonitorQt"
