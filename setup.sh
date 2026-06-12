#!/usr/bin/env bash
set -euo pipefail
echo "Initialising git submodules..."
git submodule update --init --recursive
echo ""
echo "Done. Build with:"
echo "  scons target=template_debug"
echo "  scons target=template_release"
