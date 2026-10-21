#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLCHAIN_FILE="$ROOT_DIR/toolchain/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake"
CLIENT_DIR="$ROOT_DIR/client"
BUILD_DIR="$CLIENT_DIR/build"
DRIVESHARE_DIR="${BASILISK_DRIVESHARE:-/Applications/Basillisk/driveshare/MacCompanion}"
APP_NAME="MacCompanionSpotify"

if [ ! -f "$TOOLCHAIN_FILE" ]; then
    echo "Error: Retro68 toolchain not found at $TOOLCHAIN_FILE"
    exit 1
fi

echo "==> Configuring & building $APP_NAME (m68k-apple-macos)..."
mkdir -p "$BUILD_DIR"
cmake -S "$CLIENT_DIR" -B "$BUILD_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -G Ninja
ninja -C "$BUILD_DIR"

echo "==> Built artifacts in $BUILD_DIR:"
ls -lh "$BUILD_DIR/$APP_NAME.bin" "$BUILD_DIR/$APP_NAME.dsk" "$BUILD_DIR/$APP_NAME.APPL"

if [ -d "/Applications/Basillisk/driveshare" ]; then
    echo "==> Deploying to Basilisk II Unix Shared Drive: $DRIVESHARE_DIR"
    mkdir -p "$DRIVESHARE_DIR/.rsrc" "$DRIVESHARE_DIR/.finf" "$DRIVESHARE_DIR/launch"

    TARGET_APPL="$DRIVESHARE_DIR/$APP_NAME"

    # 1. Copy data fork
    cp "$BUILD_DIR/$APP_NAME.APPL" "$TARGET_APPL"

    # 2. Copy resource fork to both macOS namedfork (extfs_macosx) and .rsrc/ (extfs_unix)
    if [ -f "$BUILD_DIR/$APP_NAME.APPL/..namedfork/rsrc" ]; then
        cat "$BUILD_DIR/$APP_NAME.APPL/..namedfork/rsrc" > "$TARGET_APPL/..namedfork/rsrc"
        cat "$BUILD_DIR/$APP_NAME.APPL/..namedfork/rsrc" > "$DRIVESHARE_DIR/.rsrc/$APP_NAME"
    fi

    # 3. Write 32-byte FinderInfo ('APPL' + 'MCSP' + 24 zero bytes) to:
    #    - .finf/<name>
    #    - com.apple.FinderInfo xattr
    #    - org.BasiliskII.FinderInfo xattr (used by BasiliskII.app on macOS!)
    python3 - "$TARGET_APPL" "$DRIVESHARE_DIR/.finf/$APP_NAME" <<'PYEOF'
import os, subprocess, sys

target_appl, finf_file = sys.argv[1], sys.argv[2]
finfo = b"APPLMCSP" + (b"\x00" * 24)
hex_finfo = finfo.hex()

with open(finf_file, "wb") as f:
    f.write(finfo)

for attr in ("com.apple.FinderInfo", "org.BasiliskII.FinderInfo"):
    subprocess.run(["xattr", "-wx", attr, hex_finfo, target_appl], check=False)
PYEOF

    # 4. Also copy the MacBinary (.bin) and HFS floppy image (.dsk) for convenience
    cp "$BUILD_DIR/$APP_NAME.bin" "$DRIVESHARE_DIR/$APP_NAME.bin"
    cp "$BUILD_DIR/$APP_NAME.dsk" "$DRIVESHARE_DIR/$APP_NAME.dsk"

    # 5. Also deploy Retro68's LaunchAPPLServer if built
    LAUNCH_SERVER="$ROOT_DIR/toolchain/Retro68-build/build-target/LaunchAPPL/Server/LaunchAPPLServer.APPL"
    if [ -f "$LAUNCH_SERVER" ]; then
        cp "$LAUNCH_SERVER" "$DRIVESHARE_DIR/LaunchAPPLServer"
        if [ -f "$LAUNCH_SERVER/..namedfork/rsrc" ]; then
            cat "$LAUNCH_SERVER/..namedfork/rsrc" > "$DRIVESHARE_DIR/LaunchAPPLServer/..namedfork/rsrc"
            cat "$LAUNCH_SERVER/..namedfork/rsrc" > "$DRIVESHARE_DIR/.rsrc/LaunchAPPLServer"
        fi
        python3 - "$DRIVESHARE_DIR/LaunchAPPLServer" "$DRIVESHARE_DIR/.finf/LaunchAPPLServer" <<'PYEOF'
import subprocess, sys
target_appl, finf_file = sys.argv[1], sys.argv[2]
finfo = b"APPLLAPL" + (b"\x00" * 24)
hex_finfo = finfo.hex()
with open(finf_file, "wb") as f:
    f.write(finfo)
for attr in ("com.apple.FinderInfo", "org.BasiliskII.FinderInfo"):
    subprocess.run(["xattr", "-wx", attr, hex_finfo, target_appl], check=False)
PYEOF
    fi

    echo "==> Deployed successfully to $DRIVESHARE_DIR!"
    echo "    In Basilisk II, open the 'Unix' disk on the desktop -> 'MacCompanion' -> double-click '$APP_NAME'."
fi
