#!/usr/bin/env bash
# felt_variations.sh - DEV ONLY. Bake the candidate baizes from
# ios/Tools/FeltVariations.swift, using the REAL FeltTexture generator
# (shared/swift/Textures).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IOS="$(dirname "$HERE")"
TEX="$IOS/../../shared/swift/Textures"
OUT="${1:?usage: felt_variations.sh <out-dir>}"
BUILD="$(mktemp -d)"; trap 'rm -rf "$BUILD"' EXIT
swiftc -Ounchecked -whole-module-optimization -D TEXTURE_BAKE -o "$BUILD/feltvar" \
  "$TEX/WoolTexture.swift" \
  "$TEX/FeltTexture.swift" \
  "$HERE/FeltVariations.swift"
"$BUILD/feltvar" "$OUT"
