#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MIDDLEWARE_DIR="$ROOT_DIR/middleware"
VERSION_FILE="$MIDDLEWARE_DIR/src/version.js"
IMAGE_NAME="maccompanion"

if [ ! -f "$VERSION_FILE" ]; then
    echo "Error: Server version file not found at $VERSION_FILE"
    exit 1
fi

VERSION=$( node -e 'console.log(require("./middleware/src/version.js").SERVER_VERSION)' )
VERSION_SLUG=$( node -e 'console.log(require("./middleware/src/version.js").SERVER_VERSION_SLUG)' )
CODENAME=$( node -e 'console.log(require("./middleware/src/version.js").SERVER_CODENAME)' )

echo "==> Building MacCompanion Docker Image ($VERSION / $VERSION_SLUG / \"$CODENAME\")..."

docker build \
    -t "${IMAGE_NAME}:${VERSION_SLUG}" \
    -t "${IMAGE_NAME}:latest" \
    "$MIDDLEWARE_DIR"

IMAGE_ID=$(docker inspect --format='{{.Id}}' "${IMAGE_NAME}:${VERSION_SLUG}")
GIT_COMMIT=$(git rev-parse HEAD 2>/dev/null || echo "unknown")
BUILD_TIME=$(date -u +"%Y-%m-%dT%H:%M:%SZ")

RELEASE_DIR="$ROOT_DIR/releases/server/$VERSION_SLUG"
mkdir -p "$RELEASE_DIR"

# 1. Snapshot Dockerfile
cp "$MIDDLEWARE_DIR/Dockerfile" "$RELEASE_DIR/Dockerfile"

# 2. Save release metadata
cat <<EOF > "$RELEASE_DIR/release-info.json"
{
  "version": "$VERSION",
  "slug": "$VERSION_SLUG",
  "codename": "$CODENAME",
  "gitCommit": "$GIT_COMMIT",
  "builtAt": "$BUILD_TIME",
  "dockerImageId": "$IMAGE_ID",
  "dockerTag": "${IMAGE_NAME}:$VERSION_SLUG"
}
EOF

# 3. Retention policy: Keep only the latest 3 versions in releases/server/
MAX_RELEASES_TO_KEEP=3
SERVER_RELEASES_DIR="$ROOT_DIR/releases/server"

EXISTING_VERSIONS=( $(ls -1d "$SERVER_RELEASES_DIR"/v* 2>/dev/null | sort -V || true) )
NUM_VERSIONS=${#EXISTING_VERSIONS[@]}

if [ "$NUM_VERSIONS" -gt "$MAX_RELEASES_TO_KEEP" ]; then
    NUM_TO_REMOVE=$((NUM_VERSIONS - MAX_RELEASES_TO_KEEP))
    echo "==> Pruning older server releases (keeping the latest $MAX_RELEASES_TO_KEEP)..."
    for (( i=0; i<NUM_TO_REMOVE; i++ )); do
        OLD_VER_DIR="${EXISTING_VERSIONS[$i]}"
        OLD_SLUG=$(basename "$OLD_VER_DIR")
        echo "    Pruning release folder: $OLD_SLUG"
        rm -rf "$OLD_VER_DIR"
        # Prune matching Docker image tags beyond the 3 latest
        docker rmi "${IMAGE_NAME}:${OLD_SLUG}" 2>/dev/null || true
        docker rmi "spotify68k-maccompanion:${OLD_SLUG}" 2>/dev/null || true
    done
fi

echo "==> Tagged Docker Images:"
docker images "$IMAGE_NAME"

echo "==> Restarting MacCompanion Container with new image..."
docker compose -f "$ROOT_DIR/docker-compose.yml" up -d

echo "==> Archived Server Release in: $RELEASE_DIR"
ls -la "$RELEASE_DIR"
