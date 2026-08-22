#!/bin/bash
set -e

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$PROJECT_DIR"

CROSS_PREFIX="${CROSS_COMPILE:-x86_64-w64-mingw32-}"
BUILD_DIR="${BUILD_DIR:-$PROJECT_DIR/build-windows}"
TOOLCHAIN_FILE="${TOOLCHAIN_FILE:-$PROJECT_DIR/cmake/windows-toolchain.cmake}"
ZIP_NAME="${ZIP_NAME:-MonitorQt-Windows-x86_64.zip}"
ZIP_OUT="${ZIP_OUT:-$BUILD_DIR/$ZIP_NAME}"

# MinGW compiler check
if ! command -v "${CROSS_PREFIX}g++" >/dev/null 2>&1; then
    echo "❌ Error: Cross-compiler ${CROSS_PREFIX}g++ not found in PATH."
    echo "On macOS, you can install MinGW with: brew install mingw-w64"
    exit 1
fi

# Windows Qt prefix and macOS Host Qt path
WIN_QT_DIR="${WIN_QT_DIR:-/Users/wangyue/Qt6-Win/6.5.2/mingw_64}"
QT_HOST_PATH="${QT_HOST_PATH:-/Users/wangyue/Qt6.8.2/6.8.2/macos}"

# Options matching cdsp (ASIO, WASAPI, FFTW & LIBDISPATCH)
ENABLE_FFTW="${ENABLE_FFTW:-ON}"
USE_LIBDISPATCH="${USE_LIBDISPATCH:-ON}"

NPROC="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)}"
export CMAKE_BUILD_PARALLEL_LEVEL="$NPROC"

echo "=== Cross-compiling Monitor-Qt for Windows (x86_64 MinGW) on Mac ==="
echo "Options:      WASAPI=1, ASIO=1, FFTW=${ENABLE_FFTW}, LIBDISPATCH=${USE_LIBDISPATCH}"
echo "Compiler:     ${CROSS_PREFIX}gcc / ${CROSS_PREFIX}g++"
echo "Toolchain:    $TOOLCHAIN_FILE"
echo "Build Dir:    $BUILD_DIR"
echo "Jobs:         $NPROC"

CMAKE_ARGS=(
    -B "$BUILD_DIR"
    -S "$PROJECT_DIR"
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE"
    -DCMAKE_BUILD_TYPE=Release
    -DENABLE_FFTW="$ENABLE_FFTW"
    -DUSE_LIBDISPATCH="$USE_LIBDISPATCH"
)

if [ -d "$WIN_QT_DIR" ]; then
    echo "Windows Qt:   $WIN_QT_DIR"
    CMAKE_ARGS+=("-DCMAKE_PREFIX_PATH=$WIN_QT_DIR")
fi

if [ -d "$QT_HOST_PATH" ]; then
    echo "Host Qt:      $QT_HOST_PATH"
    CMAKE_ARGS+=("-DQT_HOST_PATH=$QT_HOST_PATH")
fi

cmake "${CMAKE_ARGS[@]}"

cmake --build "$BUILD_DIR" --parallel "$NPROC" "$@"

echo "✅ Windows compilation complete: $BUILD_DIR/MonitorQt.exe"

# ==============================================================================
# Windows Runtime & Qt Dependencies Deployment
# ==============================================================================
echo "=== Deploying Windows Dependencies in $BUILD_DIR ==="

SEARCH_DIRS=()

# Prioritize active MinGW toolchain directories (contains modern libwinpthread with clock_gettime64, libdispatch, libfftw, etc.)
for p in \
    /opt/homebrew/Cellar/mingw-w64/*/toolchain-x86_64/x86_64-w64-mingw32/bin \
    /opt/homebrew/Cellar/mingw-w64/*/toolchain-x86_64/x86_64-w64-mingw32/lib \
    /opt/homebrew/Cellar/mingw-w64/*/toolchain-x86_64/bin \
    /usr/x86_64-w64-mingw32/bin \
    /usr/lib/gcc/x86_64-w64-mingw32/* \
    /$MSYSTEM/bin \
    /mingw64/bin \
    /ucrt64/bin \
    /clang64/bin
do
    if [ -d "$p" ]; then
        SEARCH_DIRS+=("$p")
    fi
done

if [ -d "$WIN_QT_DIR/bin" ]; then
    SEARCH_DIRS+=("$WIN_QT_DIR/bin")
fi

is_system_dll() {
    local name=$(echo "$1" | tr '[:upper:]' '[:lower:]')
    case "$name" in
        advapi32.dll|avrt.dll|comctl32.dll|comdlg32.dll|crypt32.dll|d3d9.dll|d3d11.dll|dxgi.dll|\
        dbghelp.dll|dwmapi.dll|gdi32.dll|imm32.dll|iphlpapi.dll|kernel32.dll|ksuser.dll|ksguid.dll|\
        mpr.dll|msvcrt.dll|netapi32.dll|ntdll.dll|ole32.dll|oleaut32.dll|psapi.dll|rpcrt4.dll|\
        secur32.dll|setupapi.dll|shell32.dll|shlwapi.dll|user32.dll|userenv.dll|uxtheme.dll|\
        version.dll|winhttp.dll|wininet.dll|winmm.dll|ws2_32.dll|wldap32.dll|wtsapi32.dll|\
        api-ms-win-*.dll|ext-ms-*.dll)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

get_imports() {
    local target="$1"
    if command -v x86_64-w64-mingw32-objdump >/dev/null 2>&1; then
        x86_64-w64-mingw32-objdump -p "$target" 2>/dev/null | grep -i "DLL Name:" | awk '{print $3}'
    elif command -v llvm-objdump >/dev/null 2>&1; then
        llvm-objdump -p "$target" 2>/dev/null | grep -i "DLL Name:" | awk '{print $3}'
    elif command -v objdump >/dev/null 2>&1; then
        objdump -p "$target" 2>/dev/null | grep -i "DLL Name:" | awk '{print $3}'
    elif command -v ldd >/dev/null 2>&1; then
        ldd "$target" 2>/dev/null | awk '{print $1}'
    fi
}

# Deploy essential Qt plugins
if [ -d "$WIN_QT_DIR/plugins" ]; then
    if [ -f "$WIN_QT_DIR/plugins/platforms/qwindows.dll" ]; then
        mkdir -p "$BUILD_DIR/platforms"
        if [ ! -f "$BUILD_DIR/platforms/qwindows.dll" ]; then
            echo "📦 Copying Qt platform plugin: platforms/qwindows.dll"
            cp "$WIN_QT_DIR/plugins/platforms/qwindows.dll" "$BUILD_DIR/platforms/"
        fi
    fi
    if [ -d "$WIN_QT_DIR/plugins/styles" ]; then
        mkdir -p "$BUILD_DIR/styles"
        cp -u "$WIN_QT_DIR/plugins/styles/"*.dll "$BUILD_DIR/styles/" 2>/dev/null || cp "$WIN_QT_DIR/plugins/styles/"*.dll "$BUILD_DIR/styles/" 2>/dev/null || true
    fi
    if [ -d "$WIN_QT_DIR/plugins/tls" ]; then
        mkdir -p "$BUILD_DIR/tls"
        cp -u "$WIN_QT_DIR/plugins/tls/"*.dll "$BUILD_DIR/tls/" 2>/dev/null || cp "$WIN_QT_DIR/plugins/tls/"*.dll "$BUILD_DIR/tls/" 2>/dev/null || true
    fi
fi

# Iteratively copy missing dynamic dependencies
COPIED_NEW=1
while [ "$COPIED_NEW" -eq 1 ]; do
    COPIED_NEW=0
    ALL_BINARIES=$(find "$BUILD_DIR" -type f \( -name "*.exe" -o -name "*.dll" \))
    
    for bin in $ALL_BINARIES; do
        for dep in $(get_imports "$bin"); do
            if is_system_dll "$dep"; then
                continue
            fi
            
            if [ -f "$BUILD_DIR/$dep" ]; then
                continue
            fi
            
            FOUND=""
            for dir in "${SEARCH_DIRS[@]}"; do
                if [ -f "$dir/$dep" ]; then
                    FOUND="$dir/$dep"
                    break
                fi
            done
            
            if [ -n "$FOUND" ]; then
                echo "📦 Copying dependency: $dep"
                cp "$FOUND" "$BUILD_DIR/"
                COPIED_NEW=1
            fi
        done
    done
done

# ==============================================================================
# Create Standalone Windows Distribution Zip Package
# ==============================================================================
echo "=== Packaging Standalone Windows Release ==="

STAGE_DIR="$BUILD_DIR/dist/MonitorQt"
rm -rf "$BUILD_DIR/dist"
mkdir -p "$STAGE_DIR"

# Copy executable and all runtime DLLs
cp "$BUILD_DIR/MonitorQt.exe" "$STAGE_DIR/"
cp "$BUILD_DIR"/*.dll "$STAGE_DIR/" 2>/dev/null || true

# Copy Qt plugins
if [ -d "$BUILD_DIR/platforms" ]; then
    cp -r "$BUILD_DIR/platforms" "$STAGE_DIR/"
fi
if [ -d "$BUILD_DIR/styles" ]; then
    cp -r "$BUILD_DIR/styles" "$STAGE_DIR/"
fi
if [ -d "$BUILD_DIR/tls" ]; then
    cp -r "$BUILD_DIR/tls" "$STAGE_DIR/"
fi

# Build clean zip archive
rm -f "$ZIP_OUT"
(cd "$BUILD_DIR/dist" && zip -r -q -9 "$ZIP_OUT" MonitorQt)

ZIP_SIZE=$(du -h "$ZIP_OUT" | awk '{print $1}')
echo "=============================================================================="
echo "🎉 Windows Release ZIP Package Ready!"
echo "📦 Zip Archive: $ZIP_OUT ($ZIP_SIZE)"
echo "=============================================================================="

# ==============================================================================
# Build Windows NSIS Setup Installer (.exe)
# ==============================================================================
INSTALLER_NSI="$PROJECT_DIR/cmake/installer.nsi"
INSTALLER_OUT="${INSTALLER_OUT:-$BUILD_DIR/MonitorQt-Setup-1.0.0.exe}"

if command -v makensis >/dev/null 2>&1; then
    echo "=== Building Windows Setup Installer with NSIS ==="
    makensis -DAPP_NAME="CDSP Monitor" \
             -DAPP_VERSION="1.0.0" \
             -DAPP_PUBLISHER="DSPMonitor" \
             -DAPP_EXE="MonitorQt.exe" \
             -DSOURCE_DIR="$PROJECT_DIR" \
             -DSTAGE_DIR="$STAGE_DIR" \
             -DOUTPUT_EXE="$INSTALLER_OUT" \
             "$INSTALLER_NSI"
    
    INSTALLER_SIZE=$(du -h "$INSTALLER_OUT" | awk '{print $1}')
    echo "=============================================================================="
    echo "🎉 Windows Setup Installer Ready!"
    echo "💿 Installer:   $INSTALLER_OUT ($INSTALLER_SIZE)"
    echo "🚀 Run 'MonitorQt-Setup-1.0.0.exe' on any 64-bit Windows machine to install."
    echo "=============================================================================="
else
    echo "ℹ️  NSIS (makensis) not found. To build a Windows setup installer (.exe), run:"
    echo "   brew install makensis"
fi

