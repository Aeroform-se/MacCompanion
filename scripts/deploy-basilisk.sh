#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLCHAIN_FILE="$ROOT_DIR/toolchain/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake"
CLIENT_DIR="$ROOT_DIR/client"
BUILD_DIR="$CLIENT_DIR/build"
VERSION_HEADER="$CLIENT_DIR/src/version.h"
DRIVESHARE_DIR="${BASILISK_DRIVESHARE:-/Applications/Basillisk/driveshare/MacCompanion}"
APP_NAME="MaciSpot"

if [ ! -f "$TOOLCHAIN_FILE" ]; then
    echo "Error: Retro68 toolchain not found at $TOOLCHAIN_FILE"
    exit 1
fi

# Extract version strings from client/src/version.h
VERSION_SHORT=$( sed -n 's/^#define APP_VERSION_SHORT[[:space:]]*"\(.*\)"/\1/p' "$VERSION_HEADER" )
VERSION_SLUG=$(  sed -n 's/^#define APP_VERSION_SLUG[[:space:]]*"\(.*\)"/\1/p'  "$VERSION_HEADER" )
VERSION_SHORT="${VERSION_SHORT:-0.2 Beta}"
VERSION_SLUG="${VERSION_SLUG:-v0.2-beta}"

echo "==> Configuring & building $APP_NAME ($VERSION_SHORT / $VERSION_SLUG)..."
mkdir -p "$BUILD_DIR"
if [ -f "$BUILD_DIR/resources.r.rsrc.bin" ] && [ "$VERSION_HEADER" -nt "$BUILD_DIR/resources.r.rsrc.bin" ]; then
    touch "$CLIENT_DIR/src/resources.r"
fi
cmake -S "$CLIENT_DIR" -B "$BUILD_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -G Ninja
ninja -C "$BUILD_DIR"

# 1. Archive versioned .bin and .dsk into releases/spotify/<version_slug>/
RELEASE_DIR="$ROOT_DIR/releases/spotify/$VERSION_SLUG"
mkdir -p "$RELEASE_DIR"
cp "$BUILD_DIR/$APP_NAME.bin" "$RELEASE_DIR/${APP_NAME}-${VERSION_SLUG}.bin"
cp "$BUILD_DIR/$APP_NAME.dsk" "$RELEASE_DIR/${APP_NAME}-${VERSION_SLUG}.dsk"

echo "==> Archived release artifacts in $RELEASE_DIR:"
ls -lh "$RELEASE_DIR/${APP_NAME}-${VERSION_SLUG}.bin" "$RELEASE_DIR/${APP_NAME}-${VERSION_SLUG}.dsk"

# Helper to deploy a Classic Mac APPL with both macOS namedfork/xattr, .rsrc/.finf metadata, and hasBundle flag (0x2000)
deploy_appl_to_folder() {
    local src_appl="$1"
    local dst_dir="$2"
    local dst_name="$3"
    local creator="$4"

    mkdir -p "$dst_dir/.rsrc" "$dst_dir/.finf"
    local target_appl="$dst_dir/$dst_name"

    cp "$src_appl" "$target_appl"
    if [ -f "$src_appl/..namedfork/rsrc" ]; then
        cat "$src_appl/..namedfork/rsrc" > "$target_appl/..namedfork/rsrc"
        cat "$src_appl/..namedfork/rsrc" > "$dst_dir/.rsrc/$dst_name"
    fi

    python3 - "$target_appl" "$dst_dir/.finf/$dst_name" "$creator" <<'PYEOF'
import subprocess, sys
target_appl, finf_file, creator = sys.argv[1], sys.argv[2], sys.argv[3]
# FinderInfo: 4B Type ('APPL') + 4B Creator ('MCSP') + 2B fdFlags (0x2000 = hasBundle) + 22B zeros
finfo = ("APPL" + creator[:4]).encode("ascii") + b"\x20\x00" + (b"\x00" * 22)
hex_finfo = finfo.hex()
with open(finf_file, "wb") as f:
    f.write(finfo)
for attr in ("com.apple.FinderInfo", "org.BasiliskII.FinderInfo"):
    subprocess.run(["xattr", "-wx", attr, hex_finfo, target_appl], check=False)
PYEOF
}

if [ -d "/Applications/Basillisk/driveshare" ]; then
    echo "==> Deploying to Basilisk II Unix Shared Drive: $DRIVESHARE_DIR"
    mkdir -p "$DRIVESHARE_DIR/launch" "$DRIVESHARE_DIR/Releases"

    # Deploy latest active build as MaciSpot
    deploy_appl_to_folder "$BUILD_DIR/$APP_NAME.APPL" "$DRIVESHARE_DIR" "$APP_NAME" "MCSP"

    # Deploy version-numbered copy into Unix -> MacCompanion -> Releases
    VERSIONED_APP_NAME="$APP_NAME $VERSION_SHORT"
    deploy_appl_to_folder "$BUILD_DIR/$APP_NAME.APPL" "$DRIVESHARE_DIR/Releases" "$VERSIONED_APP_NAME" "MCSP"

    # Also copy MacBinary (.bin) and HFS floppy (.dsk)
    cp "$BUILD_DIR/$APP_NAME.bin" "$DRIVESHARE_DIR/$APP_NAME.bin"
    cp "$BUILD_DIR/$APP_NAME.dsk" "$DRIVESHARE_DIR/$APP_NAME.dsk"

    # Deploy LaunchAPPLServer if present
    LAUNCH_SERVER="$ROOT_DIR/toolchain/Retro68-build/build-target/LaunchAPPL/Server/LaunchAPPLServer.APPL"
    if [ -f "$LAUNCH_SERVER" ]; then
        deploy_appl_to_folder "$LAUNCH_SERVER" "$DRIVESHARE_DIR" "LaunchAPPLServer" "LAPL"
    fi

    echo "==> Deployed to Basilisk II:"
    echo "    - Latest build:   $DRIVESHARE_DIR/$APP_NAME"
    echo "    - Versioned copy: $DRIVESHARE_DIR/Releases/$VERSIONED_APP_NAME"
fi
