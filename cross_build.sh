#!/bin/bash
set -e

CDSP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$CDSP_DIR"

SYSROOT="/Users/wangyue/rpi-sysroot"
CC="/opt/homebrew/bin/aarch64-linux-gnu-gcc"
AR="/opt/homebrew/bin/aarch64-linux-gnu-ar"

echo "=== Cross-compiling cdsp for Raspberry Pi (aarch64) on Mac ==="
make CC="$CC" \
     AR="$AR" \
     ENABLE_ALSA=1 \
     ENABLE_PIPEWIRE=1 \
     ENABLE_FFTW=1 \
     IS_DARWIN=0 \
     CFLAGS="--sysroot=$SYSROOT -isystem $SYSROOT/usr/include/aarch64-linux-gnu -isystem $SYSROOT/usr/include/dbus-1.0 -isystem $SYSROOT/usr/lib/aarch64-linux-gnu/dbus-1.0/include -isystem $SYSROOT/usr/include/pipewire-0.3 -isystem $SYSROOT/usr/include/spa-0.2 -D_REENTRANT -O3 -std=c11 -DCDSP_BUILD_SHARED -Wall -Wextra -DENABLE_ALSA -DENABLE_PIPEWIRE -DENABLE_FFTW -D_GNU_SOURCE -I. -I./Filters -I./Audio -I./Config -I./FFT -I./Mixer -I./Resampler -I./Processors -I./DoP -I./Pipeline -I./Engine -I./Server -I./Backend -I./Logging -I./Utils" \
     LDFLAGS="--sysroot=$SYSROOT -B$SYSROOT/usr/lib/aarch64-linux-gnu -L$SYSROOT/usr/lib/aarch64-linux-gnu -L$SYSROOT/lib/aarch64-linux-gnu -Wl,-rpath-link,$SYSROOT/usr/lib/aarch64-linux-gnu -Wl,-rpath-link,$SYSROOT/lib/aarch64-linux-gnu -lm -lpthread -ldbus-1 -lasound -lpipewire-0.3 -lfftw3 -lfftw3f -lrt" \
     -j8 "$@"

echo "✅ Cross-compilation complete: bin/dsp-cli"
