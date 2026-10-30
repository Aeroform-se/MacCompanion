#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE_NAME="maccompanion"

if [ $# -lt 1 ]; then
    echo "Usage: $0 <version_slug>"
    echo "Example: $0 v0.4.8"
    echo ""
    echo "Available tagged server images:"
    docker images "$IMAGE_NAME" --format "table {{.Repository}}:{{.Tag}}\t{{.ID}}\t{{.CreatedAt}}"
    echo ""
    echo "Available archived server releases:"
    ls -1 "$ROOT_DIR/releases/server" 2>/dev/null || echo "None found yet."
    exit 1
fi

TARGET_SLUG="$1"
TARGET_IMAGE="${IMAGE_NAME}:$TARGET_SLUG"

# Also check for legacy tag if target not found
if ! docker image inspect "$TARGET_IMAGE" >/dev/null 2>&1; then
    LEGACY_IMAGE="spotify68k-maccompanion:$TARGET_SLUG"
    if docker image inspect "$LEGACY_IMAGE" >/dev/null 2>&1; then
        echo "Found legacy image $LEGACY_IMAGE, retagging as $TARGET_IMAGE..."
        docker tag "$LEGACY_IMAGE" "$TARGET_IMAGE"
    fi
fi

if ! docker image inspect "$TARGET_IMAGE" >/dev/null 2>&1; then
    echo "Error: Docker image $TARGET_IMAGE not found locally."
    echo "Checking if release metadata exists in $ROOT_DIR/releases/server/$TARGET_SLUG..."
    if [ -f "$ROOT_DIR/releases/server/$TARGET_SLUG/release-info.json" ]; then
        GIT_COMMIT=$(node -e "console.log(require('$ROOT_DIR/releases/server/$TARGET_SLUG/release-info.json').gitCommit)")
        echo "Found release metadata referencing git commit $GIT_COMMIT."
        echo "To rebuild this exact version, you can run:"
        echo "  git checkout $GIT_COMMIT -- middleware/"
        echo "  ./scripts/build-server.sh"
    fi
    exit 1
fi

echo "==> Rolling back MacCompanion Server to: $TARGET_IMAGE..."

# Retag target version as latest
docker tag "$TARGET_IMAGE" "${IMAGE_NAME}:latest"

echo "==> Restarting container with rolled back image..."
docker compose -f "$ROOT_DIR/docker-compose.yml" up -d --no-build

echo "==> Rollback complete! Running container:"
docker ps --filter "name=maccompanion"
