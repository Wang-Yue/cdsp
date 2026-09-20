#!/bin/bash
set -e

CDSP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$CDSP_DIR"

TOOLCHAIN_FILE="${TOOLCHAIN_FILE:-$CDSP_DIR/cmake/x86_64-w64-mingw32.cmake}"
BUILD_DIR="build-win"

echo "=== Cross-compiling cdsp for Windows (x86_64) with CMake ==="

cmake -B "$BUILD_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -DENABLE_NATIVE_ARCH=OFF \
    -DBUILD_TESTING=ON \
    -DBUILD_BENCHMARKS=ON \
    -DCMAKE_BUILD_TYPE=Release \
    "$@"

cmake --build "$BUILD_DIR" -j$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

echo "✅ Windows cross-compilation complete:"
echo "   - Main binary: $BUILD_DIR/bin/cdsp.exe"
[ -f "$BUILD_DIR/bin/CDSPStudio.exe" ] && echo "   - Studio GUI:  $BUILD_DIR/bin/CDSPStudio.exe"
echo "   - Test runner: $BUILD_DIR/bin/test_runner.exe"
echo "   - Benchmarks:  $BUILD_DIR/bin/test_*_benchmark.exe"
