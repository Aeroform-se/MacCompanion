#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLCHAIN_FILE="${RETRO68_TOOLCHAIN_FILE:-$ROOT_DIR/toolchain/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake}"
CLIENT_DIR="$ROOT_DIR/client"
BUILD_DIR="$CLIENT_DIR/build"
VERSION_HEADER="$CLIENT_DIR/src/version.h"
APP_NAME="SpotiMac"

if [ ! -f "$TOOLCHAIN_FILE" ]; then
    echo "Error: Retro68 toolchain not found at $TOOLCHAIN_FILE"
    echo "Set RETRO68_TOOLCHAIN_FILE to your m68k-apple-macos/cmake/retro68.toolchain.cmake path."
    exit 1
fi

VERSION_SHORT=$( sed -n 's/^#define APP_VERSION_SHORT[[:space:]]*"\(.*\)"/\1/p' "$VERSION_HEADER" )
VERSION_SLUG=$(  sed -n 's/^#define APP_VERSION_SLUG[[:space:]]*"\(.*\)"/\1/p'  "$VERSION_HEADER" )
VERSION_SHORT="${VERSION_SHORT:-0.3.6 RC}"
VERSION_SLUG="${VERSION_SLUG:-v0.3.6-rc}"

echo "==> Configuring & building $APP_NAME ($VERSION_SHORT / $VERSION_SLUG)..."
mkdir -p "$BUILD_DIR"
if [ -f "$BUILD_DIR/resources.r.rsrc.bin" ] && [ "$VERSION_HEADER" -nt "$BUILD_DIR/resources.r.rsrc.bin" ]; then
    touch "$CLIENT_DIR/src/resources.r"
fi
cmake -S "$CLIENT_DIR" -B "$BUILD_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -G Ninja
ninja -C "$BUILD_DIR"

echo "==> Built artifacts in $BUILD_DIR:"
ls -lh "$BUILD_DIR/$APP_NAME.bin" "$BUILD_DIR/$APP_NAME.dsk"

if [ -x "$ROOT_DIR/scripts/deploy-basilisk.sh" ]; then
    "$ROOT_DIR/scripts/deploy-basilisk.sh"
fi
