#!/bin/bash
set -euo pipefail
kiln_root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$kiln_root"
kiln_logs="${KILN_OUTPUT:-/tmp/kiln-build}"
mkdir -p "$kiln_logs"
git merge-base --is-ancestor ed1daf0bf001b61586d9930840f2f1394092c079 HEAD
python3 kiln/tools/build_gi_shaders.py
scons platform=macos target=editor arch=arm64 vulkan=no metal=yes -j8 > "$kiln_logs/editor.log" 2>&1
if [[ "${1:-}" == "--templates" ]]; then
  scons platform=macos target=template_release arch=arm64 vulkan=no metal=yes -j8 > "$kiln_logs/template.log" 2>&1
fi
printf 'Build complete. Logs: %s\n' "$kiln_logs"
