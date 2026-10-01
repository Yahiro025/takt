#!/usr/bin/env bash
# Installs keeby-inputd's setgid-input privilege, then grants exactly one
# desktop account permission to execute it via a POSIX ACL — that account is
# the real security boundary here: whoever can execute this helper receives
# keystrokes through its socket. Requires sudo. This is the ONLY privileged
# step KEEBY's install needs — launching keeby itself never requires sudo.
# See docs/006-step-2.5-security-permissions.md.
#
# Usage: sudo ./scripts/install-keeby-inputd.sh /path/to/build/keeby-inputd [user]
# `user` defaults to $SUDO_USER (the account that invoked sudo).
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    echo "error: must run as root (sudo ./scripts/install-keeby-inputd.sh <path>)" >&2
    exit 1
fi

SRC="${1:?usage: $0 /path/to/build/keeby-inputd [user]}"
if [ ! -f "$SRC" ]; then
    echo "error: $SRC does not exist" >&2
    exit 1
fi

# Not $USER: under sudo that's root. $SUDO_USER is the desktop account that
# actually needs to run keeby, which is what the ACL below must name.
target_user="${2:-${SUDO_USER:-}}"
if [ -z "$target_user" ] || [ "$target_user" = "root" ]; then
    echo "error: no desktop user to grant access to — run via sudo from your desktop account, or pass the user explicitly" >&2
    exit 1
fi
if ! id -u "$target_user" >/dev/null 2>&1; then
    echo "error: user '$target_user' does not exist" >&2
    exit 1
fi

if ! getent group input >/dev/null; then
    echo "error: no 'input' group exists on this system" >&2
    exit 1
fi

if ! command -v setfacl >/dev/null; then
    echo "error: setfacl not found — install the 'acl' package" >&2
    exit 1
fi

DEST="/usr/local/bin/keeby-inputd"

echo "About to run:"
echo "  install -o root -g input -m 2750 \"$SRC\" \"$DEST\""
echo "  setfacl -m \"u:${target_user}:x\" \"$DEST\""
echo "This makes $DEST owned by root:input, mode 2750 (setgid, rwxr-s---,"
echo "not world-readable/writable/executable), then grants ONLY '${target_user}'"
echo "permission to execute it via a POSIX ACL. Nothing else is changed."
read -r -p "Proceed? [y/N] " reply
case "$reply" in
    y|Y) ;;
    *) echo "aborted"; exit 1 ;;
esac

install -o root -g input -m 2750 "$SRC" "$DEST"
setfacl -m "u:${target_user}:x" "$DEST"

if ! getfacl -p "$DEST" | grep -q "user:${target_user}:--x"; then
    echo "error: ACL verification failed — expected 'user:${target_user}:--x' in getfacl output:" >&2
    getfacl -p "$DEST" >&2
    exit 1
fi
mode="$(stat -c %A "$DEST")"
if [ "${mode:6:1}" != "s" ]; then
    echo "error: setgid bit did not survive install (mode: $mode) — refusing to report success" >&2
    exit 1
fi

echo "Installed and verified."
echo
echo "'${target_user}' does NOT need to be a member of the 'input' group for"
echo "keeby to work. Execute access to $DEST is granted to that account"
echo "alone via the POSIX ACL above — that is what now controls who can run"
echo "the helper (and therefore who can receive keystrokes through its"
echo "socket). No other account or system service can execute it."
echo
echo "Verify yourself with:"
echo "  ls -l $DEST"
echo "  getfacl $DEST"
echo "Expected ls -l (note the trailing '+': an ACL is attached):"
echo "  -rwxr-s---+ 1 root input ... $DEST"
echo "Expected getfacl (among other lines):"
echo "  user:${target_user}:--x"
echo
echo "keeby's main process finds this automatically next to its own binary"
echo "only if run from the same directory; for a system install, keeby"
echo "resolves the helper via KEEBY_INPUTD_PATH if set, else next to its"
echo "own executable. If keeby itself is not also installed to"
echo "/usr/local/bin, set KEEBY_INPUTD_PATH=$DEST in its environment."
