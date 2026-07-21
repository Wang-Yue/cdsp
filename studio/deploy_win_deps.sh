#!/bin/bash

# Default build directory to current directory if not specified
BUILD_DIR="${1:-.}"

# Resolve to absolute path
if [ -d "$BUILD_DIR" ]; then
    BUILD_DIR=$(cd "$BUILD_DIR" && pwd)
else
    echo "Error: Directory $BUILD_DIR does not exist."
    exit 1
fi

echo "Scanning dependencies in $BUILD_DIR..."

# Detect MSYS2 environment prefix from environment or path
# Matches: clangarm64, clang64, clang32, ucrt64, mingw64, mingw32
ENV_PREFIX=$(echo "$MSYSTEM" | tr '[:upper:]' '[:lower:]')
if [ -z "$ENV_PREFIX" ]; then
    ENV_PATTERN="/(clangarm64|clang64|clang32|ucrt64|mingw64|mingw32)/bin/"
else
    ENV_PATTERN="/${ENV_PREFIX}/bin/"
fi

# Find dependencies and copy them
find "$BUILD_DIR" -name "*.dll" -o -name "*.exe" | while read -r f; do
    ldd "$f" | grep -E "$ENV_PATTERN" | awk '{print $3}'
done | sort -u | while read -r d; do
    if [ -f "$d" ]; then
        filename=$(basename "$d")
        if [ ! -f "$BUILD_DIR/$filename" ]; then
            echo "Copying dependency: $d"
            cp "$d" "$BUILD_DIR/"
        else
            echo "Dependency already present: $filename"
        fi
    else
        if [ -n "$d" ]; then
            echo "Dependency file not found: $d"
        fi
    fi
done

echo "Deployment finished."
