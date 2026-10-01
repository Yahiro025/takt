#!/usr/bin/env bash
# Convenience fetcher for the classic Mechvibes community sound packs.
# User-run only -- never invoked by keeby/keeby-inputd, no sudo, nothing here
# is committed (see docs/008-step-2.7-sound-packs.md). Verified against the
# live repo: packs live under src/audio/<pack-id>/ (default branch: main).
set -euo pipefail

REPO_URL="https://github.com/hainguyents13/mechvibes.git"
BRANCH="main"
PACKS_SUBDIR="src/audio"
DEST_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/keeby/packs"

echo "keeby: fetch classic Mechvibes sound packs"
echo
echo "This will:"
echo "  1. Create a temporary directory and shallow sparse-clone"
echo "       $REPO_URL (branch: $BRANCH, path: $PACKS_SUBDIR/ only)"
echo "     -- no full history, and only that one subdirectory is fetched"
echo "  2. Copy every pack directory under $PACKS_SUBDIR/ that contains a config.json into"
echo "       $DEST_DIR/<pack-id>/"
echo "  3. Skip 'default' (reserved for keeby's built-in pack) and any pack already"
echo "     present at the destination -- nothing already installed is overwritten"
echo "  4. Delete the temporary directory afterwards"
echo
echo "Licensing note: these are community-contributed sound packs bundled by the"
echo "Mechvibes project (https://github.com/hainguyents13/mechvibes). Most carry no"
echo "explicit license of their own. They are fetched here for your personal use only --"
echo "review the upstream repository's LICENSE and each pack's files yourself before any"
echo "other use, such as redistribution."
echo

read -r -p "Proceed? [y/N] " reply
case "$reply" in
    [yY]|[yY][eE][sS]) ;;
    *) echo "keeby: aborted, nothing changed"; exit 0 ;;
esac

if ! command -v git >/dev/null 2>&1; then
    echo "keeby: git is required but was not found in PATH" >&2
    exit 1
fi

tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT

echo "keeby: cloning $PACKS_SUBDIR/ only (shallow, sparse)..."
git -C "$tmp_dir" init -q
git -C "$tmp_dir" remote add origin "$REPO_URL"
git -C "$tmp_dir" sparse-checkout init --no-cone
echo "$PACKS_SUBDIR/*" > "$tmp_dir/.git/info/sparse-checkout"
# --filter=blob:none makes the fetch itself lazy: without it, a sparse
# checkout only limits which blobs are CHECKED OUT afterwards, but "git
# fetch --depth 1" alone still downloads every blob that exists in that one
# commit, sparse path or not. With the filter, only the tree/commit objects
# come down up front, and blobs are then fetched on demand for the sparse
# path checked out below -- so only $PACKS_SUBDIR/ audio is ever downloaded.
git -C "$tmp_dir" fetch -q --depth 1 --filter=blob:none origin "$BRANCH"
git -C "$tmp_dir" checkout -q "$BRANCH"

src_dir="$tmp_dir/$PACKS_SUBDIR"
if [ ! -d "$src_dir" ]; then
    echo "keeby: expected directory '$PACKS_SUBDIR' was not found after cloning;" >&2
    echo "       the upstream repository layout may have changed -- check" >&2
    echo "       https://github.com/hainguyents13/mechvibes" >&2
    exit 1
fi

mkdir -p "$DEST_DIR"
installed=0
skipped=0
for pack_path in "$src_dir"/*/; do
    pack_path="${pack_path%/}"
    pack_id="$(basename "$pack_path")"

    if [ ! -f "$pack_path/config.json" ]; then
        continue
    fi
    if [ "$pack_id" = "default" ]; then
        echo "keeby: skipping '$pack_id' (reserved for keeby's built-in pack)"
        skipped=$((skipped + 1))
        continue
    fi
    if [ -e "$DEST_DIR/$pack_id" ]; then
        echo "keeby: skipping '$pack_id' (already exists at $DEST_DIR/$pack_id)"
        skipped=$((skipped + 1))
        continue
    fi

    cp -r "$pack_path" "$DEST_DIR/$pack_id"
    echo "keeby: installed '$pack_id' -> $DEST_DIR/$pack_id"
    installed=$((installed + 1))
done

echo "keeby: done -- $installed pack(s) installed, $skipped skipped"
echo "keeby: run 'keeby --list-profiles' to confirm, then 'keeby --profile <id>' to use one"
