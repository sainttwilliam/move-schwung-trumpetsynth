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
MOVE_MODULES_DIR="/data/UserData/schwung/modules"
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
scp -p "${DIST_DIR}/module.json"  "${MOVE_USER}@${MOVE_HOST}:${MOVE_MODULES_DIR}/${MODULE_ID}/"
scp -p "${DIST_DIR}/ui.js"        "${MOVE_USER}@${MOVE_HOST}:${MOVE_MODULES_DIR}/${MODULE_ID}/"
scp -p "${DIST_DIR}/ui_chain.js"  "${MOVE_USER}@${MOVE_HOST}:${MOVE_MODULES_DIR}/${MODULE_ID}/"
scp -p "${DIST_DIR}/help.json"    "${MOVE_USER}@${MOVE_HOST}:${MOVE_MODULES_DIR}/${MODULE_ID}/"
scp -p "${DIST_DIR}/trumpet-synth.so" "${MOVE_USER}@${MOVE_HOST}:${MOVE_MODULES_DIR}/${MODULE_ID}/"

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
