#!/usr/bin/env bash
# ==============================================================================
# tools/build_wasm.sh - Single source of truth for the WebAssembly build.
#
# Builds the cdsp WASM core, the Qt-for-WebAssembly CDSP Studio, runs the WASM
# bit-correctness test, and packages the Chrome extension (cdsp_extension.zip).
# Used verbatim by CI (.github/workflows/ci.yml, job wasm-test-and-package) and
# for local builds, so both produce identical artifacts.
#
# Requirements:
#   - emsdk ${EMSDK_VERSION} activated (emcc on PATH)
#   - Qt ${QT_VERSION} for WebAssembly (wasm_multithread) and its desktop host
#     (gcc_64) installed under ${QT_ROOT}, e.g. via:
#       python3 -m aqt install-qt linux desktop ${QT_VERSION} linux_gcc_64 --outputdir "$QT_ROOT"
#       python3 -m aqt install-qt all_os wasm ${QT_VERSION} wasm_multithread -m qtmultimedia --outputdir "$QT_ROOT"
#
# Usage: QT_ROOT=/path/to/Qt tools/build_wasm.sh [build_dir]
# ==============================================================================
set -euo pipefail

QT_VERSION="${QT_VERSION:-6.7.3}"
# Must be the exact emsdk Qt ${QT_VERSION} was built with. embind inlines special value handles
# (e.g. val::isNull() compares against _EMVAL_NULL, which is 2 in 3.1.50 but 4 in 3.1.56) into
# Qt's prebuilt static libs, so a different emsdk runtime silently breaks Qt (e.g. QSettings
# throws "Cannot pass non-string to std::string" on a missing localStorage key).
EMSDK_VERSION="${EMSDK_VERSION:-3.1.50}"
QT_ROOT="${QT_ROOT:-$HOME/Qt}"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-$REPO_ROOT/build-wasm}"

QT_WASM_DIR="$QT_ROOT/$QT_VERSION/wasm_multithread"
QT_HOST_DIR="$QT_ROOT/$QT_VERSION/gcc_64"

die() { echo "error: $*" >&2; exit 1; }

command -v emcc >/dev/null || die "emcc not found; activate emsdk $EMSDK_VERSION (source emsdk_env.sh)"
EMCC_VERSION="$(emcc --version | head -n1 | sed -E 's/.* ([0-9]+\.[0-9]+\.[0-9]+).*/\1/')"
[[ "$EMCC_VERSION" == "$EMSDK_VERSION" ]] || die "emcc $EMCC_VERSION found, expected $EMSDK_VERSION"
[[ -f "$QT_WASM_DIR/lib/cmake/Qt6/qt.toolchain.cmake" ]] || die "Qt WASM not found at $QT_WASM_DIR"
[[ -d "$QT_HOST_DIR" ]] || die "Qt host not found at $QT_HOST_DIR"
command -v node >/dev/null || die "node not found"

echo "==> Qt $QT_VERSION (wasm_multithread) + emsdk $EMCC_VERSION -> $BUILD_DIR"
echo "    cmake $(cmake --version | head -n1 | awk '{print $3}'), node $(node --version), QT_ROOT=$QT_ROOT"

cmake -S "$REPO_ROOT" -B "$BUILD_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$QT_WASM_DIR/lib/cmake/Qt6/qt.toolchain.cmake" \
    -DQT_HOST_PATH="$QT_HOST_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=ON \
    -DENABLE_WEBAUDIO=ON \
    -DENABLE_STUDIO=ON \
    -DENABLE_NATIVE_ARCH=OFF

cmake --build "$BUILD_DIR" -j"$(nproc)"

echo "==> Running WebAssembly tests"
node "$BUILD_DIR/test_wasm.js"

[[ -f "$BUILD_DIR/cdsp_extension.zip" ]] || die "cdsp_extension.zip was not produced"
echo "==> Done. Unpacked extension: $BUILD_DIR/extension_dist"
echo "    Packaged extension:  $BUILD_DIR/cdsp_extension.zip"
