#!/bin/bash
set -e

CDSP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$CDSP_DIR"

SYSROOT="${SYSROOT:-/Users/wangyue/rpi-sysroot}"
CC="${CC:-/opt/homebrew/bin/aarch64-linux-gnu-gcc}"
AR="${AR:-/opt/homebrew/bin/aarch64-linux-gnu-ar}"

BUILD_DIR="build-rpi"

echo "=== Cross-compiling cdsp for Raspberry Pi (aarch64) with CMake ==="

cmake -B "$BUILD_DIR" \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    -DCMAKE_C_COMPILER="$CC" \
    -DCMAKE_AR="$AR" \
    -DCMAKE_SYSROOT="$SYSROOT" \
    -DENABLE_ALSA=ON \
    -DENABLE_PIPEWIRE=ON \
    -DENABLE_FFTW=ON \
    -DENABLE_LIBDISPATCH=ON \
    -DENABLE_NATIVE_ARCH=OFF \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS="--sysroot=$SYSROOT -isystem $SYSROOT/usr/include/aarch64-linux-gnu -isystem $SYSROOT/usr/include/dbus-1.0 -isystem $SYSROOT/usr/lib/aarch64-linux-gnu/dbus-1.0/include -isystem $SYSROOT/usr/include/pipewire-0.3 -isystem $SYSROOT/usr/include/spa-0.2 -isystem $SYSROOT/usr/libexec/swift/lib/swift -D_REENTRANT" \
    -DCMAKE_EXE_LINKER_FLAGS="--sysroot=$SYSROOT -B$SYSROOT/usr/lib/aarch64-linux-gnu -L$SYSROOT/usr/lib/aarch64-linux-gnu -L$SYSROOT/lib/aarch64-linux-gnu -L$SYSROOT/usr/libexec/swift/lib/swift/linux -Wl,-rpath-link,$SYSROOT/usr/lib/aarch64-linux-gnu -Wl,-rpath-link,$SYSROOT/lib/aarch64-linux-gnu -Wl,-rpath-link,$SYSROOT/usr/libexec/swift/lib/swift/linux -Wl,-rpath,/usr/libexec/swift/lib/swift/linux -ldbus-1 -lasound -lpipewire-0.3 -lfftw3 -lfftw3f -ldispatch -lBlocksRuntime -lrt" \
    "$@"

cmake --build "$BUILD_DIR" -j$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

echo "✅ Raspberry Pi cross-compilation complete: $BUILD_DIR/bin/cdsp"
