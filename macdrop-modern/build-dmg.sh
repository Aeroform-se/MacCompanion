#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Build the Universal .app bundle first
"$SCRIPT_DIR/build-app.sh"

SRC_APP="$SCRIPT_DIR/build/MacDrop.app"
if [ ! -d "$SRC_APP" ]; then
    echo "Error: Built app bundle not found at $SRC_APP"
    exit 1
fi

VERSION=$(/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" "$SRC_APP/Contents/Info.plist")
DMG_NAME="MacDrop-X-${VERSION}-Universal.dmg"
DMG_PATH="$SCRIPT_DIR/build/$DMG_NAME"
VOL_NAME="MacDrop X"

STAGE_DIR="$(mktemp -d /tmp/macdrop_dmg_stage.XXXXXX)"
trap 'rm -rf "$STAGE_DIR"' EXIT

echo "==> Staging DMG contents..."
# Preserve symlinks, resource forks, and code signature when copying bundle
ditto "$SRC_APP" "$STAGE_DIR/MacDrop X.app"
ln -s /Applications "$STAGE_DIR/Applications"
cp "$SCRIPT_DIR/README-INSTALL.txt" "$STAGE_DIR/README - How to Open.txt"

echo "==> Creating compressed drag-to-Applications DMG ($DMG_NAME)..."
rm -f "$DMG_PATH"
hdiutil create \
    -volname "$VOL_NAME" \
    -srcfolder "$STAGE_DIR" \
    -ov \
    -format UDZO \
    "$DMG_PATH"

echo "==> Created distribution DMG at:"
echo "    $DMG_PATH"
