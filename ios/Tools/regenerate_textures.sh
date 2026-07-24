#!/usr/bin/env bash
# regenerate_textures.sh — re-bake every entry of WoolTexture.bakes and
# WoodTexture.bakes into FoolishKit/Resources/, from the generators in
# FoolishKit/DesignSystem/. Today that is five files:
#
#   wool-classic.jpg  wool-dark-green.jpg  wool-dark-navy.jpg
#   wood-classic.jpg  wood-dark.jpg
#
# Run this after changing WoolTexture.render / WoodTexture.render or any
# `Palette`, then commit the regenerated images. The shipping app NEVER runs the
# generators (see the header comments in those files: a procedural render on
# launch is what took the iMessage extension down on a real phone), so the images
# are the only way a look change reaches the product.
#
# NOTE this does NOT need re-running to switch the dark board between green and
# navy: both are already baked, and `WoolTexture.darkAccent` picks which one the
# app loads. Re-bake only when a palette's NUMBERS change.
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
  "$IOS/FoolishKit/DesignSystem/FernCardBack.swift" \
  "$HERE/GenerateTextures.swift"

"$BUILD/gentex" "$OUT"
