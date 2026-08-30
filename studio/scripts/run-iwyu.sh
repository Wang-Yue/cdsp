#!/usr/bin/env bash
# ==============================================================================
# run-iwyu.sh - Run Include-What-You-Use (IWYU) on the Monitor-Qt project
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
MAPPING_FILE="${PROJECT_ROOT}/cmake/iwyu.imp"

# Detect IWYU tool binaries
IWYU_TOOL=""
if command -v iwyu_tool.py >/dev/null 2>&1; then
    IWYU_TOOL="$(command -v iwyu_tool.py)"
elif command -v iwyu_tool >/dev/null 2>&1; then
    IWYU_TOOL="$(command -v iwyu_tool)"
elif [ -f "/opt/homebrew/bin/iwyu_tool.py" ]; then
    IWYU_TOOL="/opt/homebrew/bin/iwyu_tool.py"
fi

IWYU_BIN=""
if command -v include-what-you-use >/dev/null 2>&1; then
    IWYU_BIN="$(command -v include-what-you-use)"
elif [ -f "/opt/homebrew/bin/include-what-you-use" ]; then
    IWYU_BIN="/opt/homebrew/bin/include-what-you-use"
fi

FIX_INCLUDES=""
if command -v fix_includes.py >/dev/null 2>&1; then
    FIX_INCLUDES="$(command -v fix_includes.py)"
elif command -v fix_includes >/dev/null 2>&1; then
    FIX_INCLUDES="$(command -v fix_includes)"
elif [ -f "/opt/homebrew/bin/fix_includes.py" ]; then
    FIX_INCLUDES="/opt/homebrew/bin/fix_includes.py"
fi

if [ -z "$IWYU_TOOL" ] && [ -z "$IWYU_BIN" ]; then
    echo "Error: include-what-you-use / iwyu_tool.py not found in PATH." >&2
    echo "Install it via:" >&2
    echo "  macOS:  brew install include-what-you-use" >&2
    echo "  Ubuntu/Debian: sudo apt-get install iwyu" >&2
    exit 1
fi

# Detect CPU cores for parallel processing
DEFAULT_JOBS=4
if command -v sysctl >/dev/null 2>&1; then
    DEFAULT_JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
elif command -v nproc >/dev/null 2>&1; then
    DEFAULT_JOBS="$(nproc 2>/dev/null || echo 4)"
fi

# Parse CLI arguments
MODE="check"
JOBS="$DEFAULT_JOBS"
SAFE_HEADERS=false
DRY_RUN=false
TARGET_FILES=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --fix)
            MODE="fix"
            shift
            ;;
        --check)
            MODE="check"
            shift
            ;;
        --dry-run)
            MODE="fix"
            DRY_RUN=true
            shift
            ;;
        --safe-headers)
            SAFE_HEADERS=true
            shift
            ;;
        --nosafe-headers)
            SAFE_HEADERS=false
            shift
            ;;
        -j|--jobs)
            JOBS="$2"
            shift 2
            ;;
        --build-dir)
            BUILD_DIR="$2"
            shift 2
            ;;
        -h|--help)
            echo "Usage: $0 [options] [files...]"
            echo ""
            echo "Options:"
            echo "  --check          Analyze and display IWYU recommendations (default)"
            echo "  --fix            Automatically apply IWYU suggestions (add needed & remove unused)"
            echo "  --dry-run        Preview the modifications that --fix would make without editing files"
            echo "  --safe-headers   Preserve unused includes in header files (.h)"
            echo "  --nosafe-headers Remove unused includes from header files as well (default in --fix)"
            echo "  -j, --jobs <N>   Run N jobs in parallel (default: auto = $DEFAULT_JOBS)"
            echo "  --build-dir <D>  Specify build directory (default: build/)"
            echo "  -h, --help       Show this help message"
            exit 0
            ;;
        *)
            TARGET_FILES+=("$1")
            shift
            ;;
    esac
done

# Ensure compile_commands.json is present
COMPILE_COMMANDS="${BUILD_DIR}/compile_commands.json"
if [ ! -f "$COMPILE_COMMANDS" ]; then
    echo "Notice: compile_commands.json not found in ${BUILD_DIR}. Configuring CMake..."
    cmake -B "${BUILD_DIR}" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null
fi

# Extra compiler arguments (sysroot for macOS)
EXTRA_IWYU_ARGS=()
EXTRA_IWYU_ARGS+=("-Xiwyu" "--mapping_file=${MAPPING_FILE}")
EXTRA_IWYU_ARGS+=("-Xiwyu" "--no_fwd_decls")
EXTRA_IWYU_ARGS+=("-Xiwyu" "--max_line_length=120")

if [[ "$(uname)" == "Darwin" ]]; then
    SDK_PATH="$(xcrun --show-sdk-path 2>/dev/null || true)"
    if [ -n "$SDK_PATH" ]; then
        EXTRA_IWYU_ARGS+=("-isysroot" "$SDK_PATH")
    fi
fi

# Determine source files to check if none specified
if [ ${#TARGET_FILES[@]} -eq 0 ]; then
    # Collect all project .cpp files in src/, excluding ObjC++ .mm
    while IFS= read -r file; do
        TARGET_FILES+=("$file")
    done < <(find "${PROJECT_ROOT}/src" -name "*.cpp" -not -path "*/build/*" | sort)
fi

echo "Running Include-What-You-Use on ${#TARGET_FILES[@]} file(s) with ${JOBS} worker(s)..."

if [ "$MODE" == "fix" ]; then
    if [ -z "$FIX_INCLUDES" ]; then
        echo "Error: fix_includes.py not found. Cannot apply automated fixes." >&2
        exit 1
    fi
    FIX_FLAGS=(--comments --reorder)
    if [ "$SAFE_HEADERS" = true ]; then
        FIX_FLAGS+=(--safe_headers)
    else
        FIX_FLAGS+=(--nosafe_headers)
    fi
    if [ "$DRY_RUN" = true ]; then
        FIX_FLAGS+=(--dry_run)
        echo "Running in DRY-RUN mode: showing diffs without modifying files."
    else
        echo "Running in FIX mode: adding needed includes and removing unused includes."
    fi

    set +e
    python3 "$IWYU_TOOL" -p "$COMPILE_COMMANDS" -j "$JOBS" "${TARGET_FILES[@]}" -- "${EXTRA_IWYU_ARGS[@]}" | python3 "$FIX_INCLUDES" "${FIX_FLAGS[@]}"
    EXIT_CODE=$?
    set -e

    if [ "$DRY_RUN" = false ]; then
        echo "Done. You can run 'git diff' to review the changes."
    fi
    exit $EXIT_CODE
else
    python3 "$IWYU_TOOL" -p "$COMPILE_COMMANDS" -j "$JOBS" "${TARGET_FILES[@]}" -- "${EXTRA_IWYU_ARGS[@]}"
fi
