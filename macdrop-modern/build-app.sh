#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

APP_NAME="MacDrop"
BUILD_DIR="$SCRIPT_DIR/build"
APP_BUNDLE="$BUILD_DIR/$APP_NAME.app"
CONTENTS_DIR="$APP_BUNDLE/Contents"
MACOS_DIR="$CONTENTS_DIR/MacOS"
RESOURCES_DIR="$CONTENTS_DIR/Resources"

echo "==> Building $APP_NAME (Universal Binary: arm64 + x86_64, macOS 11.0+)..."
swift build -c release --triple arm64-apple-macosx11.0
swift build -c release --triple x86_64-apple-macosx11.0

ARM_BIN="$SCRIPT_DIR/.build/arm64-apple-macosx/release/$APP_NAME"
X86_BIN="$SCRIPT_DIR/.build/x86_64-apple-macosx/release/$APP_NAME"
if [ ! -f "$ARM_BIN" ] || [ ! -f "$X86_BIN" ]; then
    echo "Error: Expected both arm64 and x86_64 binaries:"
    echo "  $ARM_BIN"
    echo "  $X86_BIN"
    exit 1
fi

echo "==> Creating macOS App Bundle..."
rm -rf "$APP_BUNDLE"
mkdir -p "$MACOS_DIR" "$RESOURCES_DIR"

# Combine arm64 + x86_64 slices into a single Universal Mach-O binary
echo "==> Creating Universal Binary with lipo..."
lipo -create "$ARM_BIN" "$X86_BIN" -output "$MACOS_DIR/$APP_NAME"
chmod +x "$MACOS_DIR/$APP_NAME"

# Copy Info.plist
cp "$SCRIPT_DIR/Info.plist" "$CONTENTS_DIR/Info.plist"

# Copy Resources
if [ -d "$SCRIPT_DIR/Resources" ]; then
    cp -R "$SCRIPT_DIR/Resources/"* "$RESOURCES_DIR/" || true
fi

# Generate AppIcon.icns and Assets.car from Resources/AppIcon.png if available
if [ -f "$SCRIPT_DIR/Resources/AppIcon.png" ]; then
    echo "==> Generating AppIcon.icns & Assets.car..."
    ICONSET_DIR="/tmp/$APP_NAME.iconset"
    ASSET_DIR="/tmp/$APP_NAME.xcassets"
    rm -rf "$ICONSET_DIR" "$ASSET_DIR"
    mkdir -p "$ICONSET_DIR" "$ASSET_DIR/AppIcon.appiconset"

    cat << 'EOF' > "$ASSET_DIR/AppIcon.appiconset/Contents.json"
{
  "images" : [
    { "size" : "16x16",   "idiom" : "mac", "filename" : "icon_16x16.png",    "scale" : "1x" },
    { "size" : "16x16",   "idiom" : "mac", "filename" : "icon_16x16@2x.png", "scale" : "2x" },
    { "size" : "32x32",   "idiom" : "mac", "filename" : "icon_32x32.png",    "scale" : "1x" },
    { "size" : "32x32",   "idiom" : "mac", "filename" : "icon_32x32@2x.png", "scale" : "2x" },
    { "size" : "128x128", "idiom" : "mac", "filename" : "icon_128x128.png",   "scale" : "1x" },
    { "size" : "128x128", "idiom" : "mac", "filename" : "icon_128x128@2x.png", "scale" : "2x" },
    { "size" : "256x256", "idiom" : "mac", "filename" : "icon_256x256.png",   "scale" : "1x" },
    { "size" : "256x256", "idiom" : "mac", "filename" : "icon_256x256@2x.png", "scale" : "2x" },
    { "size" : "512x512", "idiom" : "mac", "filename" : "icon_512x512.png",   "scale" : "1x" },
    { "size" : "512x512", "idiom" : "mac", "filename" : "icon_512x512@2x.png", "scale" : "2x" }
  ],
  "info" : { "version" : 1, "author" : "xcode" }
}
EOF

    sips -z 16 16     "$SCRIPT_DIR/Resources/AppIcon.png" --out "$ICONSET_DIR/icon_16x16.png" >/dev/null 2>&1 || true
    sips -z 32 32     "$SCRIPT_DIR/Resources/AppIcon.png" --out "$ICONSET_DIR/icon_16x16@2x.png" >/dev/null 2>&1 || true
    sips -z 32 32     "$SCRIPT_DIR/Resources/AppIcon.png" --out "$ICONSET_DIR/icon_32x32.png" >/dev/null 2>&1 || true
    sips -z 64 64     "$SCRIPT_DIR/Resources/AppIcon.png" --out "$ICONSET_DIR/icon_32x32@2x.png" >/dev/null 2>&1 || true
    sips -z 128 128   "$SCRIPT_DIR/Resources/AppIcon.png" --out "$ICONSET_DIR/icon_128x128.png" >/dev/null 2>&1 || true
    sips -z 256 256   "$SCRIPT_DIR/Resources/AppIcon.png" --out "$ICONSET_DIR/icon_128x128@2x.png" >/dev/null 2>&1 || true
    sips -z 256 256   "$SCRIPT_DIR/Resources/AppIcon.png" --out "$ICONSET_DIR/icon_256x256.png" >/dev/null 2>&1 || true
    sips -z 512 512   "$SCRIPT_DIR/Resources/AppIcon.png" --out "$ICONSET_DIR/icon_256x256@2x.png" >/dev/null 2>&1 || true
    sips -z 512 512   "$SCRIPT_DIR/Resources/AppIcon.png" --out "$ICONSET_DIR/icon_512x512.png" >/dev/null 2>&1 || true
    sips -z 1024 1024 "$SCRIPT_DIR/Resources/AppIcon.png" --out "$ICONSET_DIR/icon_512x512@2x.png" >/dev/null 2>&1 || true

    cp "$ICONSET_DIR/"*.png "$ASSET_DIR/AppIcon.appiconset/"

    # Compile Assets.car for macOS Notification Center banner app icon
    if command -v actool >/dev/null 2>&1 || xcrun -find actool >/dev/null 2>&1; then
        xcrun actool "$ASSET_DIR" --compile "$RESOURCES_DIR" --platform macosx --minimum-deployment-target 11.0 --app-icon AppIcon --output-partial-info-plist /tmp/appicon_partial.plist >/dev/null 2>&1 || true
        if [ -f "$RESOURCES_DIR/Assets.car" ]; then
            cp "$RESOURCES_DIR/Assets.car" "$SCRIPT_DIR/Resources/Assets.car" || true
        fi
        rm -f /tmp/appicon_partial.plist
    fi

    # Compile AppIcon.icns for Finder & Dock
    if command -v iconutil >/dev/null 2>&1; then
        iconutil -c icns "$ICONSET_DIR" -o "$RESOURCES_DIR/AppIcon.icns" || true
        cp "$RESOURCES_DIR/AppIcon.icns" "$SCRIPT_DIR/Resources/AppIcon.icns" || true
    fi
    rm -rf "$ICONSET_DIR" "$ASSET_DIR"
fi

echo "==> Stripping extended attributes before signing..."
xattr -cr "$APP_BUNDLE"

echo "==> Ad-hoc code signing $APP_NAME.app..."
codesign -s - --force --deep "$APP_BUNDLE"

echo "==> Registering with LaunchServices database & resetting notification daemon cache..."
touch "$APP_BUNDLE"
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -u "$APP_BUNDLE" || true
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "$APP_BUNDLE" || true
killall usernoted NotificationCenter 2>/dev/null || true

echo "==> Successfully created $APP_BUNDLE!"
echo "    You can run it with: open \"$APP_BUNDLE\""
