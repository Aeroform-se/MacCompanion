#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MIDDLEWARE_DIR="$ROOT_DIR/middleware"
VERSION_FILE="$MIDDLEWARE_DIR/src/version.js"

if [ ! -f "$VERSION_FILE" ]; then
    echo "Error: Server version file not found at $VERSION_FILE"
    exit 1
fi

VERSION=$( node -e 'console.log(require("./middleware/src/version.js").SERVER_VERSION)' )
VERSION_SLUG=$( node -e 'console.log(require("./middleware/src/version.js").SERVER_VERSION_SLUG)' )
CODENAME=$( node -e 'console.log(require("./middleware/src/version.js").SERVER_CODENAME)' )

echo "==> Building MacCompanion Docker Image ($VERSION / $VERSION_SLUG / \"$CODENAME\")..."

docker build \
    -t "spotify68k-maccompanion:${VERSION_SLUG}" \
    -t "spotify68k-maccompanion:latest" \
    "$MIDDLEWARE_DIR"

IMAGE_ID=$(docker inspect --format='{{.Id}}' "spotify68k-maccompanion:${VERSION_SLUG}")
GIT_COMMIT=$(git rev-parse HEAD 2>/dev/null || echo "unknown")
BUILD_TIME=$(date -u +"%Y-%m-%dT%H:%M:%SZ")

RELEASE_DIR="$ROOT_DIR/releases/server/$VERSION_SLUG"
mkdir -p "$RELEASE_DIR"

cat <<EOF > "$RELEASE_DIR/release-info.json"
{
  "version": "$VERSION",
  "slug": "$VERSION_SLUG",
  "codename": "$CODENAME",
  "gitCommit": "$GIT_COMMIT",
  "builtAt": "$BUILD_TIME",
  "dockerImageId": "$IMAGE_ID",
  "dockerTag": "spotify68k-maccompanion:$VERSION_SLUG"
}
EOF

echo "==> Tagged Docker Images:"
docker images "spotify68k-maccompanion"

echo "==> Restarting MacCompanion Container with new image..."
docker compose -f "$ROOT_DIR/docker-compose.yml" up -d

echo "==> Archived Server Release Metadata in: $RELEASE_DIR/release-info.json"
cat "$RELEASE_DIR/release-info.json"
