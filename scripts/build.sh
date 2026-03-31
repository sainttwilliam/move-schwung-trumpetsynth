#!/usr/bin/env bash
# build.sh — Cross-compile trumpet-synth for aarch64 (Ableton Move)
#
# Usage (from repo root or scripts/):
#   ./scripts/build.sh          # local: wraps compilation in Docker
#
# When invoked from inside the Docker container (e.g. by CI) the script
# detects /.dockerenv and runs the compilation directly without re-entering
# Docker.
#
# Output: dist/trumpet-synth/{module.json,ui.js,ui_chain.js,help.json,dsp.so}

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$REPO_ROOT"

MODULE_ID="trumpet-synth"
DOCKER_IMAGE="${DOCKER_IMAGE:-trumpet-synth-builder}"

# ---------------------------------------------------------------------------
# compile() — runs inside Docker (or any aarch64 cross-compile environment)
# ---------------------------------------------------------------------------
compile() {
    CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"
    AUBIO_STATIC="${AUBIO_STATIC:-/usr/local/lib/libaubio-aarch64.a}"
    AUBIO_INCLUDE="${AUBIO_INCLUDE:-/usr/local/include}"

    mkdir -p build

    echo "  Compiling dsp.so..."
    ${CROSS_PREFIX}gcc \
        -g -O3 -shared -fPIC \
        -march=armv8-a -mtune=cortex-a72 \
        -fomit-frame-pointer \
        -DNDEBUG \
        dsp/trumpet_synth.c \
        -Iinclude \
        -I"${AUBIO_INCLUDE}" \
        -o build/trumpet-synth.so \
        "${AUBIO_STATIC}" \
        -lm

    echo "  Verifying binary..."
    file build/trumpet-synth.so
    ${CROSS_PREFIX}readelf -d build/trumpet-synth.so | grep NEEDED || true

    echo "  Assembling dist/..."
    rm -rf "dist/${MODULE_ID}"
    mkdir -p "dist/${MODULE_ID}"
    cp module.json              "dist/${MODULE_ID}/"
    cp ui.js                    "dist/${MODULE_ID}/"
    cp ui_chain.js              "dist/${MODULE_ID}/"
    cp help.json                "dist/${MODULE_ID}/"
    cp build/trumpet-synth.so  "dist/${MODULE_ID}/"
}

# ---------------------------------------------------------------------------
# Inside Docker — compile directly
# ---------------------------------------------------------------------------
if [ -f /.dockerenv ]; then
    echo "=== Trumpet Synth — cross-compile (inside Docker) ==="
    compile
    echo ""
    echo "=== Done ==="
    ls -lh "dist/${MODULE_ID}/"
    exit 0
fi

# ---------------------------------------------------------------------------
# On host machine — build Docker image (uses layer cache), then compile
# ---------------------------------------------------------------------------
echo "=== Trumpet Synth Build ==="
echo "Target : aarch64-linux-gnu (Ableton Move / Cortex-A72)"
echo "Image  : ${DOCKER_IMAGE}"
echo ""

# Build (or refresh from cache) the builder image
docker build \
    --tag "${DOCKER_IMAGE}" \
    --file scripts/Dockerfile \
    scripts/

echo ""
echo "Running cross-compilation in Docker..."
docker run --rm \
    --volume "${REPO_ROOT}:/build" \
    --workdir /build \
    "${DOCKER_IMAGE}" \
    bash scripts/build.sh   # re-enters this script inside Docker

echo ""
echo "=== Build complete ==="
echo "Output: dist/${MODULE_ID}/"
ls -lh "dist/${MODULE_ID}/"
