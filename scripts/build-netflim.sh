#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NETFLIM_DIR="$ROOT_DIR/netflim/netflim-client"
BUILD_DIR="$NETFLIM_DIR/build"
VERSION_HEADER="$NETFLIM_DIR/src/version.h"
TOOLCHAIN="${RETRO68_TOOLCHAIN_FILE:-$ROOT_DIR/toolchain/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake}"
TOOLS_BIN="$ROOT_DIR/toolchain/Retro68-build/toolchain/bin"
DIST_DIR="$ROOT_DIR/dist"
DRIVESHARE_DIR="${BASILISK_DRIVESHARE:-/Applications/Basillisk/driveshare/MacCompanion}"
APP_NAME="NetFlim"

if [ ! -f "$TOOLCHAIN" ]; then
    echo "ERROR: Retro68 toolchain not found at $TOOLCHAIN"
    exit 1
fi

export PATH="$TOOLS_BIN:$PATH"

VERSION_SHORT=$(sed -n 's/^#define[[:space:]]\{1,\}APP_VERSION_SHORT[[:space:]]*"\(.*\)"/\1/p' "$VERSION_HEADER")
VERSION_SLUG=$(sed -n 's/^#define[[:space:]]\{1,\}APP_VERSION_SLUG[[:space:]]*"\(.*\)"/\1/p' "$VERSION_HEADER")
VERSION_CODENAME=$(sed -n 's/^#define[[:space:]]\{1,\}APP_VERSION_CODENAME[[:space:]]*"\(.*\)"/\1/p' "$VERSION_HEADER")
VERSION_SHORT="${VERSION_SHORT:-0.1.0}"
VERSION_SLUG="${VERSION_SLUG:-v0.1.0}"
VERSION_CODENAME="${VERSION_CODENAME:-First Reel}"

echo "==> Configuring & building $APP_NAME ($VERSION_SHORT \"$VERSION_CODENAME\" / $VERSION_SLUG)..."
mkdir -p "$BUILD_DIR" "$DIST_DIR"
if [ -f "$BUILD_DIR/resources.r.rsrc.bin" ] && [ "$VERSION_HEADER" -nt "$BUILD_DIR/resources.r.rsrc.bin" ]; then
    touch "$NETFLIM_DIR/src/resources.r"
fi

cmake -S "$NETFLIM_DIR" -B "$BUILD_DIR" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN"
ninja -C "$BUILD_DIR"

# Set HasBundle (0x2000) | HasCustomIcon (0x0400) = 0x2400 so System 7 Finder displays NetFlim icon
python3 - "$BUILD_DIR/$APP_NAME.bin" "$BUILD_DIR/$APP_NAME.dsk" <<'PYEOF'
import binascii, pathlib, struct, sys

bin_path = pathlib.Path(sys.argv[1])
if bin_path.exists():
    bdata = bytearray(bin_path.read_bytes())
    if len(bdata) >= 128 and bdata[65:73] == b"APPLNFLM":
        bdata[73] |= 0x24
        bdata[74] = 0x00
        crc = binascii.crc_hqx(bytes(bdata[:124]), 0)
        struct.pack_into(">H", bdata, 124, crc)
        bin_path.write_bytes(bdata)

dsk_path = pathlib.Path(sys.argv[2])
if dsk_path.exists():
    dsk = bytearray(dsk_path.read_bytes())
    sig = b"\x02\x00\x00\x00APPLNFLM\x00\x00"
    idx = dsk.find(sig)
    if idx != -1:
        f_off = idx + 12
        d_flags, = struct.unpack(">H", dsk[f_off:f_off + 2])
        struct.pack_into(">H", dsk, f_off, d_flags | 0x2400)
        dsk_path.write_bytes(dsk)
        print("Patched DSK HasBundle/HasCustomIcon flags.")
PYEOF

if [ -f "$BUILD_DIR/$APP_NAME.dsk" ] && [ -f "$NETFLIM_DIR/ReadMe - NetFlim.txt" ]; then
    bash "$ROOT_DIR/scripts/inject-dsk-readme.sh" "$BUILD_DIR/$APP_NAME.dsk" "$NETFLIM_DIR/ReadMe - NetFlim.txt" "ReadMe - NetFlim"
fi

# Mirror latest build outputs to dist/
cp -f "$BUILD_DIR/$APP_NAME.bin" "$DIST_DIR/$APP_NAME.bin"
cp -f "$BUILD_DIR/$APP_NAME.dsk" "$DIST_DIR/$APP_NAME.dsk"
if [ -f "$BUILD_DIR/$APP_NAME.APPL" ]; then
    cp -f "$BUILD_DIR/$APP_NAME.APPL" "$DIST_DIR/$APP_NAME.APPL"
fi

# Archive versioned artifacts into releases/netflim/<version_slug>/
# Never overwrite an existing release directory; append b, c, d... if same version exists
EFFECTIVE_SLUG="$VERSION_SLUG"
if [ -d "$ROOT_DIR/releases/netflim/$EFFECTIVE_SLUG" ]; then
    for suffix in b c d e f g h; do
        CANDIDATE="${VERSION_SLUG}${suffix}"
        if [ ! -d "$ROOT_DIR/releases/netflim/$CANDIDATE" ]; then
            EFFECTIVE_SLUG="$CANDIDATE"
            break
        fi
    done
fi
RELEASE_DIR="$ROOT_DIR/releases/netflim/$EFFECTIVE_SLUG"
mkdir -p "$RELEASE_DIR"
cp -f "$BUILD_DIR/$APP_NAME.bin" "$RELEASE_DIR/${APP_NAME}-${EFFECTIVE_SLUG}.bin"
cp -f "$BUILD_DIR/$APP_NAME.dsk" "$RELEASE_DIR/${APP_NAME}-${EFFECTIVE_SLUG}.dsk"
if [ -f "$BUILD_DIR/$APP_NAME.APPL" ]; then
    cp -f "$BUILD_DIR/$APP_NAME.APPL" "$RELEASE_DIR/${APP_NAME}-${EFFECTIVE_SLUG}.APPL"
fi

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
finfo = ("APPL" + creator[:4]).encode("ascii") + b"\x24\x00" + (b"\x00" * 22)
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

    rm -f "$CURRENT_BUILD_RELEASES_DIR/${APP_NAME}-"*
    cp "$BUILD_DIR/$APP_NAME.dsk" "$CURRENT_BUILD_RELEASES_DIR/${APP_NAME}-${EFFECTIVE_SLUG}.dsk"
    cp "$BUILD_DIR/$APP_NAME.bin" "$CURRENT_BUILD_RELEASES_DIR/${APP_NAME}-${EFFECTIVE_SLUG}.bin"

    deploy_appl_to_folder "$BUILD_DIR/$APP_NAME.APPL" "$DRIVESHARE_DIR" "$APP_NAME" "NFLM"

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
    deploy_appl_to_folder "$BUILD_DIR/$APP_NAME.APPL" "$OLDER_RELEASES_DIR" "$VERSIONED_APP_NAME" "NFLM"

    echo "==> Deployed $APP_NAME to Basilisk II:"
    echo "    - Latest build:           $DRIVESHARE_DIR/$APP_NAME"
    echo "    - Current build releases: $CURRENT_BUILD_RELEASES_DIR/${APP_NAME}-${EFFECTIVE_SLUG}.{dsk,bin}"
    echo "    - Older releases:         $OLDER_RELEASES_DIR/$VERSIONED_APP_NAME"
fi

echo ""
echo "=================================================="
echo "  NetFlim ${EFFECTIVE_SLUG} \"${VERSION_CODENAME}\" Built!"
echo "=================================================="
echo "  BinHex/MacBinary: $DIST_DIR/NetFlim.bin"
echo "  Disk Image:       $DIST_DIR/NetFlim.dsk"
echo "  Release Archive:  $RELEASE_DIR/"
echo ""
