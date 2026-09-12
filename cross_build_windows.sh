#!/bin/bash
set -e

CDSP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$CDSP_DIR"

CROSS_PREFIX="${CROSS_COMPILE:-x86_64-w64-mingw32-}"
CC="$(which ${CROSS_PREFIX}gcc 2>/dev/null || echo ${CROSS_PREFIX}gcc)"
AR="$(which ${CROSS_PREFIX}gcc-ar 2>/dev/null || which ${CROSS_PREFIX}ar 2>/dev/null || echo ${CROSS_PREFIX}ar)"
DLLTOOL="$(which ${CROSS_PREFIX}dlltool 2>/dev/null || echo ${CROSS_PREFIX}dlltool)"
SYSROOT="$("$CC" -print-sysroot 2>/dev/null || true)"

BUILD_DIR="build-win"
DEPS_DIR="$CDSP_DIR/build-win/deps"
FFTW_DIR="$DEPS_DIR/fftw-win64"

echo "=== Cross-compiling cdsp for Windows (x86_64) with CMake ==="

# Set up FFTW3 Windows libraries if not already present
if [ ! -f "$FFTW_DIR/libfftw3.dll.a" ] || [ ! -f "$FFTW_DIR/libfftw3f.dll.a" ]; then
    echo "--- Setting up FFTW3 Windows x64 dependencies in $FFTW_DIR ---"
    mkdir -p "$FFTW_DIR"
    if [ ! -f "$FFTW_DIR/fftw-3.3.5-dll64.zip" ]; then
        curl -fsSL https://fftw.org/pub/fftw/fftw-3.3.5-dll64.zip -o "$FFTW_DIR/fftw-3.3.5-dll64.zip"
    fi
    unzip -q -o "$FFTW_DIR/fftw-3.3.5-dll64.zip" -d "$FFTW_DIR"
    "$DLLTOOL" -d "$FFTW_DIR/libfftw3-3.def" -l "$FFTW_DIR/libfftw3.dll.a" -D libfftw3-3.dll
    "$DLLTOOL" -d "$FFTW_DIR/libfftw3f-3.def" -l "$FFTW_DIR/libfftw3f.dll.a" -D libfftw3f-3.dll
fi

cmake -B "$BUILD_DIR" \
    -DCMAKE_SYSTEM_NAME=Windows \
    -DCMAKE_C_COMPILER="$CC" \
    -DCMAKE_AR="$AR" \
    -DFFTW3_INCLUDE_DIR="$FFTW_DIR" \
    -DFFTW3_LIB="$FFTW_DIR/libfftw3.dll.a" \
    -DFFTW3F_LIB="$FFTW_DIR/libfftw3f.dll.a" \
    -DENABLE_NATIVE_ARCH=OFF \
    -DBUILD_TESTING=ON \
    -DBUILD_BENCHMARKS=ON \
    -DCMAKE_BUILD_TYPE=Release \
    "$@"

cmake --build "$BUILD_DIR" -j$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

# Copy runtime DLLs next to executable
cp "$FFTW_DIR"/libfftw3*.dll "$BUILD_DIR/bin/" 2>/dev/null || true

echo "✅ Windows cross-compilation complete:"
echo "   - Main binary: $BUILD_DIR/bin/cdsp.exe"
echo "   - Test runner: $BUILD_DIR/bin/test_runner.exe"
echo "   - Benchmarks:  $BUILD_DIR/bin/test_*_benchmark.exe"
