#!/usr/bin/env bash
# setup-ssh.sh — Authorise your SSH key on the Ableton Move
#
# Run this once before using install.sh.  It copies your default public key
# (~/.ssh/id_*.pub) to the Move so subsequent ssh/scp commands don't prompt
# for a password.
#
# Usage:
#   ./scripts/setup-ssh.sh
#   MOVE_HOST=192.168.1.42 ./scripts/setup-ssh.sh

set -euo pipefail

MOVE_HOST="${MOVE_HOST:-move.local}"
MOVE_USER="${MOVE_USER:-ableton}"

echo "=== Authorising SSH key on ${MOVE_USER}@${MOVE_HOST} ==="
echo ""
echo "You will be prompted for the Move's SSH password."
echo "(Default Move password is printed on the underside of the device.)"
echo ""

ssh-copy-id "${MOVE_USER}@${MOVE_HOST}"

echo ""
echo "Done. Test with:"
echo "  ssh ${MOVE_USER}@${MOVE_HOST} echo ok"
