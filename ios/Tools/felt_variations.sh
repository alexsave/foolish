#!/usr/bin/env bash
# felt_variations.sh — DEV ONLY. Bake the candidate baizes from
# ios/Tools/FeltVariations.swift, using the REAL FeltTexture generator.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IOS="$(dirname "$HERE")"
OUT="${1:?usage: felt_variations.sh <out-dir>}"
BUILD="$(mktemp -d)"; trap 'rm -rf "$BUILD"' EXIT
swiftc -Ounchecked -whole-module-optimization -o "$BUILD/feltvar" \
  "$IOS/FoolishKit/DesignSystem/WoolTexture.swift" \
  "$IOS/FoolishKit/DesignSystem/FeltTexture.swift" \
  "$HERE/FeltVariations.swift"
"$BUILD/feltvar" "$OUT"
