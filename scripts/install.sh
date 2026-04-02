#!/usr/bin/env bash
# install.sh — Deploy trumpet-synth to an Ableton Move over WiFi
#
# Usage:
#   ./scripts/install.sh                    # deploy to move.local
#   MOVE_HOST=192.168.1.42 ./scripts/install.sh   # custom IP
#
# Prerequisites:
#   • Move is reachable at $MOVE_HOST (default: move.local)
#   • SSH key accepted by the Move (run scripts/setup-ssh.sh if needed)
#   • dist/trumpet-synth/ exists (run ./scripts/build.sh first)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$REPO_ROOT"

MODULE_ID="trumpet-synth"
MOVE_HOST="${MOVE_HOST:-move.local}"
MOVE_USER="${MOVE_USER:-ableton}"
MOVE_MODULES_DIR="/data/UserData/schwung/modules/audio_fx"
DIST_DIR="dist/${MODULE_ID}"

# ---------------------------------------------------------------------------
# Build if dist is missing
# ---------------------------------------------------------------------------
if [ ! -d "${DIST_DIR}" ]; then
    echo "dist/${MODULE_ID} not found — running build first..."
    echo ""
    "${SCRIPT_DIR}/build.sh"
fi

# ---------------------------------------------------------------------------
# Verify the Move is reachable before doing any work
# ---------------------------------------------------------------------------
echo "=== Installing ${MODULE_ID} ==="
echo "Target : ${MOVE_USER}@${MOVE_HOST}:${MOVE_MODULES_DIR}/${MODULE_ID}/"
echo ""

if ! ssh -o ConnectTimeout=5 -o BatchMode=yes \
         "${MOVE_USER}@${MOVE_HOST}" true 2>/dev/null; then
    echo "ERROR: Cannot reach ${MOVE_USER}@${MOVE_HOST}"
    echo ""
    echo "Make sure:"
    echo "  1. The Move is powered on and connected to the same network"
    echo "  2. Your SSH key is authorised — run: scripts/setup-ssh.sh"
    echo "  3. Or set MOVE_HOST to the Move's IP address"
    exit 1
fi

# ---------------------------------------------------------------------------
# Upload
# ---------------------------------------------------------------------------
echo "Creating remote module directory..."
ssh "${MOVE_USER}@${MOVE_HOST}" \
    "mkdir -p '${MOVE_MODULES_DIR}/${MODULE_ID}'"

echo "Copying module files..."
# Pack the entire dist dir and unpack on the device in one connection.
# This avoids partial deploys from individual scp calls failing silently.
tar -C "${DIST_DIR}" -cf - . \
    | ssh "${MOVE_USER}@${MOVE_HOST}" \
          "tar -C '${MOVE_MODULES_DIR}/${MODULE_ID}' -xf -"

echo "Enforcing component_type=audio_fx in module.json..."
ssh "${MOVE_USER}@${MOVE_HOST}" \
    "sed -i 's/\"component_type\"[[:space:]]*:[[:space:]]*\"[^\"]*\"/\"component_type\": \"audio_fx\"/' \
     '${MOVE_MODULES_DIR}/${MODULE_ID}/module.json'"

echo "Setting permissions..."
ssh "${MOVE_USER}@${MOVE_HOST}" \
    "chmod -R a+rw '${MOVE_MODULES_DIR}/${MODULE_ID}'"

# ---------------------------------------------------------------------------
# Done
# ---------------------------------------------------------------------------
echo ""
echo "=== Install complete ==="
echo "Installed: ${MOVE_MODULES_DIR}/${MODULE_ID}/"
echo ""
echo "To activate: open Schwung on the Move and run 'Rescan Modules',"
echo "or restart Schwung."
