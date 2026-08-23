#!/bin/bash
set -eo pipefail

CDSP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$CDSP_DIR"

# -----------------------------------------------------------------------------
# Compiler Selection
# -----------------------------------------------------------------------------
OS="$(uname -s)"
if [ "$OS" = "Darwin" ]; then
    if [ -z "$CC" ]; then
        if [ -x "/opt/homebrew/opt/llvm/bin/clang" ]; then
            export CC="/opt/homebrew/opt/llvm/bin/clang"
            export AR="/opt/homebrew/opt/llvm/bin/llvm-ar"
        elif [ -x "/usr/local/opt/llvm/bin/clang" ]; then
            export CC="/usr/local/opt/llvm/bin/clang"
            export AR="/usr/local/opt/llvm/bin/llvm-ar"
        else
            echo "⚠️ Warning: Homebrew LLVM not found at /opt/homebrew/opt/llvm or /usr/local/opt/llvm."
            echo "   Xcode clang may fail with sanitizers on macOS. Install via: brew install llvm"
        fi
    fi
elif [ "$OS" = "Linux" ]; then
    if [ -z "$CC" ]; then
        export CC="clang"
    fi
    if [ -z "$AR" ] && command -v llvm-ar >/dev/null 2>&1; then
        export AR="llvm-ar"
    fi
fi

JOBS=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

# Sanitizers to run (defaults to all supported sanitizers for the current OS)
if [ "$#" -gt 0 ]; then
    SANITIZERS=("$@")
else
    if [ "$OS" = "Darwin" ]; then
        SANITIZERS=("address" "thread" "undefined" "intsan" "leak" "cfi")
    else
        SANITIZERS=("address" "thread" "undefined" "intsan" "leak" "cfi" "memory")
    fi
fi

echo "================================================================="
echo "CDSP Sanitizer Test Suite Runner"
echo "OS:       $OS"
echo "Compiler: ${CC:-default cc}"
echo "Jobs:     $JOBS"
echo "Targets:  ${SANITIZERS[*]}"
echo "================================================================="

FAILED_SANITIZERS=()
PASSED_SANITIZERS=()

for san in "${SANITIZERS[@]}"; do
    BUILD_DIR="build-san-$san"
    echo ""
    echo "================================================================="
    echo ">>> Running Sanitizer: $san (build dir: $BUILD_DIR)"
    echo "================================================================="

    if cmake -B "$BUILD_DIR" -DENABLE_SANITIZER="$san" -DCMAKE_BUILD_TYPE=Debug && \
       cmake --build "$BUILD_DIR" -j"$JOBS" && \
       ctest --test-dir "$BUILD_DIR" --output-on-failure -j"$JOBS"; then
        echo "✅ Sanitizer [$san] PASSED"
        PASSED_SANITIZERS+=("$san")
    else
        echo "❌ Sanitizer [$san] FAILED"
        FAILED_SANITIZERS+=("$san")
    fi
done

echo ""
echo "================================================================="
echo "Sanitizer Summary Report:"
for p in "${PASSED_SANITIZERS[@]}"; do
    echo "  [PASS] $p"
done
for f in "${FAILED_SANITIZERS[@]}"; do
    echo "  [FAIL] $f"
done
echo "================================================================="

if [ "${#FAILED_SANITIZERS[@]}" -gt 0 ]; then
    exit 1
fi
