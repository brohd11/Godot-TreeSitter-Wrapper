#!/usr/bin/env bash
# Build the GDExtension for all platforms, then package the addon.
#
# Intended to run on a macOS host with >=16 GB RAM:
#   macOS   — native (universal)
#   Windows — cross-compiled via MinGW-w64   (brew install mingw-w64)
#   Linux   — built natively inside an Ubuntu container (Docker Desktop running)
#
# Builds are sequential on purpose: SCons keys object signatures on the
# platform/flags, so switching platform= forces a clean rebuild — no stale
# cross-platform objects. Each platform's library has a distinct suffix, so all
# six (debug+release x 3 OSes) coexist in bin/.
#
# Usage:
#   ./build-cross.sh                 # all three platforms, then package
#   ./build-cross.sh --no-windows    # skip Windows
#   ./build-cross.sh --no-linux      # skip Linux
#   ./build-cross.sh --no-package    # build only, don't run package.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

DO_MACOS=1
DO_WINDOWS=1
DO_LINUX=1
DO_PACKAGE=1
for arg in "$@"; do
    case "$arg" in
        --no-macos)   DO_MACOS=0 ;;
        --no-windows) DO_WINDOWS=0 ;;
        --no-linux)   DO_LINUX=0 ;;
        --no-package) DO_PACKAGE=0 ;;
        -h|--help)
            # Print the top comment block only (stop at the first non-comment line).
            awk 'NR==1{next} /^#/{sub(/^# ?/,""); print; next} {exit}' "$0"
            exit 0 ;;
        *)
            echo "build-cross.sh: unknown option '$arg' (try --help)" >&2
            exit 2 ;;
    esac
done

# --- prerequisite checks -----------------------------------------------------
if ! command -v scons >/dev/null 2>&1; then
    echo "build-cross.sh: 'scons' not found. Install with: brew install scons" >&2
    exit 1
fi

if [[ $DO_WINDOWS -eq 1 ]] && ! command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1; then
    echo "build-cross.sh: MinGW not found (x86_64-w64-mingw32-g++)." >&2
    echo "  Install with: brew install mingw-w64   (or rerun with --no-windows)" >&2
    exit 1
fi

if [[ $DO_LINUX -eq 1 ]]; then
    if ! command -v docker >/dev/null 2>&1; then
        echo "build-cross.sh: 'docker' not found. Install Docker Desktop (or rerun with --no-linux)." >&2
        exit 1
    fi
    if ! docker info >/dev/null 2>&1; then
        echo "build-cross.sh: Docker daemon not responding. Start Docker Desktop (or rerun with --no-linux)." >&2
        exit 1
    fi
fi

# --- builds ------------------------------------------------------------------
if [[ $DO_MACOS -eq 1 ]]; then
    echo "==> macOS (native, universal)"
    scons platform=macos target=template_debug
    scons platform=macos target=template_release
fi

if [[ $DO_WINDOWS -eq 1 ]]; then
    echo "==> Windows x86_64 (MinGW cross)"
    scons platform=windows arch=x86_64 use_mingw=yes target=template_debug
    scons platform=windows arch=x86_64 use_mingw=yes target=template_release
fi

if [[ $DO_LINUX -eq 1 ]]; then
    echo "==> Linux x86_64 (Docker / Ubuntu)"
    docker run --rm -v "$ROOT":/src -w /src ubuntu:22.04 bash -c '
        set -e
        apt-get update
        apt-get install -y build-essential python3 python3-pip scons git
        scons platform=linux arch=x86_64 target=template_debug
        scons platform=linux arch=x86_64 target=template_release'
    # Docker Desktop for macOS usually maps bind-mount ownership to the host
    # user; reclaim anything left root-owned just in case.
    if find bin -user 0 -print -quit 2>/dev/null | grep -q .; then
        echo "    (reclaiming root-owned files written by the container)"
        sudo chown -R "$(id -u):$(id -g)" bin
    fi
fi

# --- summary -----------------------------------------------------------------
echo ""
echo "Binaries in bin/:"
expected=(
    "libtree_sitter_gd.macos.template_debug.universal.dylib"
    "libtree_sitter_gd.macos.template_release.universal.dylib"
    "libtree_sitter_gd.linux.template_debug.x86_64.so"
    "libtree_sitter_gd.linux.template_release.x86_64.so"
    "libtree_sitter_gd.windows.template_debug.x86_64.dll"
    "libtree_sitter_gd.windows.template_release.x86_64.dll"
)
for f in "${expected[@]}"; do
    if [[ -f "bin/$f" ]]; then echo "  [x] $f"; else echo "  [ ] $f (missing)"; fi
done

# --- package -----------------------------------------------------------------
if [[ $DO_PACKAGE -eq 1 ]]; then
    echo ""
    ./package.sh
fi
