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
# ENABLE_ALSA=ON, ENABLE_PIPEWIRE=ON, ENABLE_FFTW=ON, ENABLE_LIBDISPATCH=ON, ENABLE_NATIVE_ARCH=OFF
ENABLE_FFTW="${ENABLE_FFTW:-ON}"
ENABLE_LIBDISPATCH="${ENABLE_LIBDISPATCH:-ON}"
ENABLE_NATIVE_ARCH="${ENABLE_NATIVE_ARCH:-OFF}"

NPROC="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)}"
export CMAKE_BUILD_PARALLEL_LEVEL="$NPROC"

echo "=== Cross-compiling Monitor-Qt for Raspberry Pi (aarch64) on Mac ==="
echo "Matching cdsp options: ALSA=ON, PIPEWIRE=ON, FFTW=${ENABLE_FFTW}, LIBDISPATCH=${ENABLE_LIBDISPATCH}"
echo "Sysroot:   $SYSROOT"
echo "Toolchain: $TOOLCHAIN_FILE"
echo "Build Dir: $BUILD_DIR"
echo "Jobs:      $NPROC"

cmake -B "$BUILD_DIR" -S "$PROJECT_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_FFTW="$ENABLE_FFTW" \
    -DENABLE_LIBDISPATCH="$ENABLE_LIBDISPATCH" \
    -DENABLE_NATIVE_ARCH="$ENABLE_NATIVE_ARCH" \
    "$@"

cmake --build "$BUILD_DIR" --parallel "$NPROC"

echo "✅ Raspberry Pi cross-compilation complete: $BUILD_DIR/MonitorQt"
