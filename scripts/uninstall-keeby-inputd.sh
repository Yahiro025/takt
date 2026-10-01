#!/usr/bin/env bash
# Reverts scripts/install-keeby-inputd.sh. Requires sudo.
#
# Usage: sudo ./scripts/uninstall-keeby-inputd.sh
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    echo "error: must run as root (sudo ./scripts/uninstall-keeby-inputd.sh)" >&2
    exit 1
fi

DEST="/usr/local/bin/keeby-inputd"
if [ ! -e "$DEST" ]; then
    echo "$DEST does not exist; nothing to remove."
    exit 0
fi

echo "About to run: rm -f \"$DEST\""
read -r -p "Proceed? [y/N] " reply
case "$reply" in
    y|Y) ;;
    *) echo "aborted"; exit 1 ;;
esac
rm -f "$DEST"
echo "Removed. The POSIX ACL granting one account execute access lived on"
echo "this file's inode, not anywhere else, so it is gone with it — nothing"
echo "separate to clean up. keeby will no longer be able to launch the"
echo "helper unless KEEBY_INPUTD_PATH points somewhere else, or a copy"
echo "exists next to keeby's own binary."
echo
echo "LAST-RESORT RECOVERY ONLY (not the normal fix): normal use never"
echo "requires 'input' group membership — reinstalling via"
echo "scripts/install-keeby-inputd.sh, which re-grants your account execute"
echo "access via ACL, is the correct way back. Only if you need keyboard"
echo "access some other way right now, you can temporarily re-add yourself"
echo "to 'input' — NOT done by this script:"
echo "  sudo usermod -aG input \$USER"
echo "then log out and back in for it to take effect."
