#!/usr/bin/env bash
# regenerate_textures.sh — re-bake FoolishKit/Resources/wool-classic.jpg and
# wood-classic.jpg from the generators in FoolishKit/DesignSystem/.
#
# Run this after changing WoolTexture.render / WoodTexture.render or either
# `Palette`, then commit the regenerated images. The shipping app NEVER runs the
# generators (see the header comments in those files: a procedural render on
# launch is what took the iMessage extension down on a real phone), so the images
# are the only way a look change reaches the product.
#
#   ios/Tools/regenerate_textures.sh          # writes into FoolishKit/Resources
#   ios/Tools/regenerate_textures.sh /tmp/out # writes somewhere else (for A/B)
#
# Under a second on an M-series Mac: it is the same expensive loop, paid once
# here instead of on every user's first launch.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IOS="$(dirname "$HERE")"
OUT="${1:-$IOS/FoolishKit/Resources}"
BUILD="$(mktemp -d)"
trap 'rm -rf "$BUILD"' EXIT

# -Ounchecked, not -O: the generators are hot arithmetic loops with bounds
# checks the render already guards by hand. Deterministic either way.
swiftc -Ounchecked -whole-module-optimization \
  -o "$BUILD/gentex" \
  "$IOS/FoolishKit/DesignSystem/WoolTexture.swift" \
  "$IOS/FoolishKit/DesignSystem/WoodTexture.swift" \
  "$HERE/GenerateTextures.swift"

"$BUILD/gentex" "$OUT"
