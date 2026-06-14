#!/usr/bin/env bash
# Package the addon into build/tree-sitter-gd-<version>/tree_sitter_gd/
#
# Everything inside the tree_sitter_gd/ source folder is copied recursively, so
# adding new .gd files (or any addon assets) there needs no change to this script.
# LICENSE and README live at the repo root (for GitHub) and are copied in too.
# Compiled libraries are pulled from bin/.
#
# Version is read from tree_sitter_gd/plugin.cfg. A pre-existing build/ is removed first.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

ADDON_SRC="tree_sitter_gd"   # source folder that maps 1:1 to addons/tree_sitter_gd/

if [[ ! -d "$ADDON_SRC" ]]; then
    echo "package.sh: addon source folder '$ADDON_SRC/' not found" >&2
    exit 1
fi

# --- version from plugin.cfg: version="x.x.x" ---
VERSION="$(sed -n 's/^[[:space:]]*version[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p' "$ADDON_SRC/plugin.cfg" | head -n1)"
if [[ -z "$VERSION" ]]; then
    echo "package.sh: could not read version from $ADDON_SRC/plugin.cfg" >&2
    exit 1
fi

DEST="build/tree-sitter-gd-${VERSION}/tree_sitter_gd"

# --- clean & recreate ---
rm -rf build
mkdir -p "$DEST/bin"

# --- addon source (recursive: everything in the folder ships) ---
cp -R "$ADDON_SRC/." "$DEST/"
find "$DEST" -name '.DS_Store' -delete

# --- repo-root docs ---
for f in LICENSE README.md; do
    if [[ -f "$f" ]]; then
        cp "$f" "$DEST/"
    else
        echo "package.sh: warning — '$f' not found, skipping" >&2
    fi
done

# --- compiled libraries (skip cruft) ---
if compgen -G "bin/*" > /dev/null; then
    find bin -type f \
        ! -name '.DS_Store' \
        ! -name '*.os' \
        -exec cp {} "$DEST/bin/" \;
else
    echo "package.sh: warning — bin/ is empty; did you build first?" >&2
fi

echo "Packaged tree-sitter-gd v${VERSION} -> ${DEST}"
