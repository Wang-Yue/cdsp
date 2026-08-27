#!/bin/bash
set -e

CDSP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$CDSP_DIR"

CROSS_PREFIX="${CROSS_COMPILE:-x86_64-w64-mingw32-}"
CC="$(which ${CROSS_PREFIX}gcc 2>/dev/null || echo ${CROSS_PREFIX}gcc)"
AR="$(which ${CROSS_PREFIX}gcc-ar 2>/dev/null || which ${CROSS_PREFIX}ar 2>/dev/null || echo ${CROSS_PREFIX}ar)"
SYSROOT="$("$CC" -print-sysroot 2>/dev/null || true)"

BUILD_DIR="build-win"

echo "=== Cross-compiling cdsp for Windows (x86_64) with CMake ==="

cmake -B "$BUILD_DIR" \
    -DCMAKE_SYSTEM_NAME=Windows \
    -DCMAKE_C_COMPILER="$CC" \
    -DCMAKE_AR="$AR" \
    ${SYSROOT:+-DCMAKE_FIND_ROOT_PATH="$SYSROOT"} \
    -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
    -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
    -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
    -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY \
    -DENABLE_NATIVE_ARCH=OFF \
    -DBUILD_TESTING=OFF \
    -DBUILD_BENCHMARKS=OFF \
    -DCMAKE_BUILD_TYPE=Release \
    "$@"

cmake --build "$BUILD_DIR" --target cdsp -j$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

echo "✅ Windows cross-compilation complete: $BUILD_DIR/bin/cdsp.exe"
