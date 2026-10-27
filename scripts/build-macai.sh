#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLCHAIN_FILE="${RETRO68_TOOLCHAIN_FILE:-$ROOT_DIR/toolchain/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake}"
MACAI_DIR="$ROOT_DIR/macai"
BUILD_DIR="$MACAI_DIR/build"
VERSION_HEADER="$MACAI_DIR/src/version.h"
DRIVESHARE_DIR="${BASILISK_DRIVESHARE:-/Applications/Basillisk/driveshare/MacCompanion}"
APP_NAME="MacAI"

if [ ! -f "$TOOLCHAIN_FILE" ]; then
    echo "Error: Retro68 toolchain not found at $TOOLCHAIN_FILE"
    exit 1
fi

VERSION_SHORT=$( sed -n 's/^#define APP_VERSION_SHORT[[:space:]]*"\(.*\)"/\1/p' "$VERSION_HEADER" )
VERSION_SLUG=$(  sed -n 's/^#define APP_VERSION_TAG[[:space:]]*"\(.*\)"/\1/p'   "$VERSION_HEADER" )
VERSION_SHORT="${VERSION_SHORT:-0.1.0}"
VERSION_SLUG="${VERSION_SLUG:-v0.1.0}"

echo "==> Configuring & building $APP_NAME ($VERSION_SHORT / $VERSION_SLUG)..."
mkdir -p "$BUILD_DIR"
if [ -f "$BUILD_DIR/resources.r.rsrc.bin" ] && [ "$VERSION_HEADER" -nt "$BUILD_DIR/resources.r.rsrc.bin" ]; then
    touch "$MACAI_DIR/src/resources.r"
fi
cmake -S "$MACAI_DIR" -B "$BUILD_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -G Ninja
ninja -C "$BUILD_DIR"

echo "==> Built artifacts in $BUILD_DIR:"
ls -lh "$BUILD_DIR/$APP_NAME.bin" "$BUILD_DIR/$APP_NAME.dsk"

# Archive versioned .bin and .dsk into releases/macai/<version_slug>/
# Never overwrite an existing release directory; append b, c, d... if same version exists
EFFECTIVE_SLUG="$VERSION_SLUG"
if [ -d "$ROOT_DIR/releases/macai/$EFFECTIVE_SLUG" ]; then
    for suffix in b c d e f g h; do
        CANDIDATE="${VERSION_SLUG}${suffix}"
        if [ ! -d "$ROOT_DIR/releases/macai/$CANDIDATE" ]; then
            EFFECTIVE_SLUG="$CANDIDATE"
            break
        fi
    done
fi
RELEASE_DIR="$ROOT_DIR/releases/macai/$EFFECTIVE_SLUG"
mkdir -p "$RELEASE_DIR"
cp "$BUILD_DIR/$APP_NAME.bin" "$RELEASE_DIR/${APP_NAME}-${EFFECTIVE_SLUG}.bin"
cp "$BUILD_DIR/$APP_NAME.dsk" "$RELEASE_DIR/${APP_NAME}-${EFFECTIVE_SLUG}.dsk"

echo "==> Archived MacAI release artifacts in $RELEASE_DIR:"
ls -lh "$RELEASE_DIR/${APP_NAME}-${EFFECTIVE_SLUG}.bin" "$RELEASE_DIR/${APP_NAME}-${EFFECTIVE_SLUG}.dsk"

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
finfo = ("APPL" + creator[:4]).encode("ascii") + b"\x20\x00" + (b"\x00" * 22)
hex_finfo = finfo.hex()
with open(finf_file, "wb") as f:
    f.write(finfo)
for attr in ("com.apple.FinderInfo", "org.BasiliskII.FinderInfo"):
    subprocess.run(["xattr", "-wx", attr, hex_finfo, target_appl], check=False)
PYEOF
}

if [ -d "/Applications/Basillisk/driveshare" ]; then
    echo "==> Deploying $APP_NAME to Basilisk II Unix Shared Drive: $DRIVESHARE_DIR"
    OLDER_RELEASES_DIR="$DRIVESHARE_DIR/Older releases"
    CURRENT_BUILD_RELEASES_DIR="$DRIVESHARE_DIR/current build releases"
    mkdir -p "$OLDER_RELEASES_DIR" "$CURRENT_BUILD_RELEASES_DIR"

    cp "$BUILD_DIR/$APP_NAME.dsk" "$CURRENT_BUILD_RELEASES_DIR/${APP_NAME}-${EFFECTIVE_SLUG}.dsk"
    cp "$BUILD_DIR/$APP_NAME.bin" "$CURRENT_BUILD_RELEASES_DIR/${APP_NAME}-${EFFECTIVE_SLUG}.bin"

    deploy_appl_to_folder "$BUILD_DIR/$APP_NAME.APPL" "$DRIVESHARE_DIR" "$APP_NAME" "MCAI"

    VERSIONED_APP_NAME="$APP_NAME $VERSION_SHORT"
    if [ -e "$OLDER_RELEASES_DIR/$VERSIONED_APP_NAME" ] || [ -e "$OLDER_RELEASES_DIR/.rsrc/$VERSIONED_APP_NAME" ]; then
        for suffix in b c d e f g h; do
            CANDIDATE="${APP_NAME} ${VERSION_SHORT}${suffix}"
            if [ ! -e "$OLDER_RELEASES_DIR/$CANDIDATE" ] && [ ! -e "$OLDER_RELEASES_DIR/.rsrc/$CANDIDATE" ]; then
                VERSIONED_APP_NAME="$CANDIDATE"
                break
            fi
        done
    fi
    deploy_appl_to_folder "$BUILD_DIR/$APP_NAME.APPL" "$OLDER_RELEASES_DIR" "$VERSIONED_APP_NAME" "MCAI"

    echo "==> Deployed $APP_NAME to Basilisk II:"
    echo "    - Latest build:           $DRIVESHARE_DIR/$APP_NAME"
    echo "    - Current build releases: $CURRENT_BUILD_RELEASES_DIR/${APP_NAME}-${EFFECTIVE_SLUG}.{dsk,bin}"
    echo "    - Older releases:         $OLDER_RELEASES_DIR/$VERSIONED_APP_NAME"
fi
