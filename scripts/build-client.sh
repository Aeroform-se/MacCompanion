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

# Patch Finder flags (0x2400 = kHasBundle | kHasCustomIcon) into SpotiMac.bin and SpotiMac.dsk
python3 - "$BUILD_DIR/$APP_NAME.bin" "$BUILD_DIR/$APP_NAME.dsk" <<'PYEOF'
import binascii, struct, sys

bin_path, dsk_path = sys.argv[1], sys.argv[2]

with open(bin_path, "rb") as f:
    bdata = bytearray(f.read())
if len(bdata) >= 128 and bdata[65:73] == b"APPLMCSP":
    bdata[73] = 0x24   # fdFlags high byte: kHasBundle (0x20) | kHasCustomIcon (0x04)
    bdata[101] = 0x00  # fdFlags low byte
    crc = binascii.crc_hqx(bytes(bdata[:124]), 0)
    struct.pack_into(">H", bdata, 124, crc)
    with open(bin_path, "wb") as f:
        f.write(bdata)

with open(dsk_path, "rb") as f:
    ddata = bytearray(f.read())
pat = b"\x02\x00\x00\x00APPLMCSP\x00\x00"
idx = ddata.find(pat)
if idx != -1:
    ddata[idx + 12] = 0x24
    ddata[idx + 13] = 0x00
    with open(dsk_path, "wb") as f:
        f.write(ddata)
PYEOF

if [ -f "$CLIENT_DIR/ReadMe - SpotiMac.txt" ]; then
    bash "$ROOT_DIR/scripts/inject-dsk-readme.sh" "$BUILD_DIR/$APP_NAME.dsk" "$CLIENT_DIR/ReadMe - SpotiMac.txt" "ReadMe - SpotiMac"
fi

echo "==> Built artifacts in $BUILD_DIR:"
ls -lh "$BUILD_DIR/$APP_NAME.bin" "$BUILD_DIR/$APP_NAME.dsk"

if [ -x "$ROOT_DIR/scripts/deploy-basilisk.sh" ]; then
    "$ROOT_DIR/scripts/deploy-basilisk.sh"
fi
