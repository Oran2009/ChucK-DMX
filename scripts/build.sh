#!/usr/bin/env bash
# Build script for DMX.chug (Linux/macOS)
set -euo pipefail

CONFIG="Release"
CLEAN=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --debug)   CONFIG="Debug"; shift ;;
        --release) CONFIG="Release"; shift ;;
        --clean)   CLEAN=true; shift ;;
        -h|--help)
            echo "Usage: $0 [--debug|--release] [--clean]"
            exit 0
            ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"

if [ "$CLEAN" = true ] && [ -d "$BUILD_DIR" ]; then
    echo "Cleaning build directory..."
    rm -rf "$BUILD_DIR"
fi

mkdir -p "$BUILD_DIR"

echo "Configuring CMake ($CONFIG)..."
cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$CONFIG"

echo "Building..."
cmake --build "$BUILD_DIR" --config "$CONFIG"

echo "Build complete: $BUILD_DIR/DMX.chug"
