#!/bin/bash
set -euo pipefail

kiln_scene="$(cd "$(dirname "$0")" && pwd)"
kiln_root="$(cd "$kiln_scene/../.." && pwd)"
kiln_engine="$kiln_root/bin/godot.macos.editor.arm64"

if [[ ! -x "$kiln_engine" ]]; then
  printf 'Engine missing. Build it with: bash "%s/kiln/tools/build_macos.sh"\n' "$kiln_root" >&2
  exit 1
fi
if [[ ! -f "$kiln_scene/assets/sponza.obj" ]]; then
  printf 'Sponza assets missing. Run: python3 "%s/kiln/tools/fetch_sponza.py"\n' "$kiln_root" >&2
  exit 1
fi

kiln_method=kiln_deferred
if [[ "${1:-}" == "--forward-plus" ]]; then
  kiln_method=forward_plus
  shift
fi

cd "$kiln_root"
exec "$kiln_engine" --path "$kiln_scene" --rendering-driver metal \
  --rendering-method "$kiln_method" -- "$@"
