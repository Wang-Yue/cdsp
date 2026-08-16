#!/bin/bash
set -e

CDSP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$CDSP_DIR"

CROSS_PREFIX="${CROSS_COMPILE:-x86_64-w64-mingw32-}"
CC="${CROSS_PREFIX}gcc"
AR="${CROSS_PREFIX}gcc-ar"
if ! command -v "$AR" >/dev/null 2>&1; then
    AR="${CROSS_PREFIX}ar"
fi

echo "=== Cross-compiling cdsp for Windows (x86_64) with ASIO, WASAPI, FFTW & libdispatch ==="
make clean
make IS_WINDOWS=1 \
     CROSS_COMPILE="$CROSS_PREFIX" \
     CC="$CC" \
     AR="$AR" \
     ENABLE_ASIO=1 \
     ENABLE_WASAPI=1 \
     ENABLE_WEBSOCKET=1 \
     ENABLE_FFTW=1 \
     USE_LIBDISPATCH=1 \
     -j$(sysctl -n hw.ncpu 2>/dev/null || echo 4) \
     "$@"

echo "✅ Windows cross-compilation complete: bin/dsp-cli.exe and libdsp.a"
