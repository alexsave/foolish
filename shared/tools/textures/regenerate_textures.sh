#!/usr/bin/env bash
# regenerate_textures.sh - re-bake every entry of WoolTexture.bakes,
# FeltTexture.bakes, WoodTexture.bakes and FernCardBack.bakes into <out-dir>,
# from the generators in shared/swift/Textures/. Today that is seven files:
#
#   wool-classic.jpg  wool-dark.jpg
#   felt-classic.jpg  felt-dark.jpg
#   wood-classic.jpg  wood-dark.jpg
#   fern-back.jpg
#
# Run this after changing a `render` or any `Palette`, then commit the
# regenerated images in the product that ships them. A shipping app NEVER runs
# the generators (see the header comments in those files: a procedural render on
# launch is what took an iMessage extension down on a real phone), so the images
# are the only way a look change reaches a product.
#
# NOTE re-bake only when a palette's NUMBERS change. Which baked image an app
# loads is its texture loader's decision at runtime, not this script's.
#
#   shared/tools/textures/regenerate_textures.sh <out-dir>
#
# The output directory is required: each product keeps its own baked images in
# its own resources, and this script does not know which product called it.
#
# Under a second on an M-series Mac: it is the same expensive loop, paid once
# here instead of on every user's first launch.
set -euo pipefail

OUT="${1:?usage: regenerate_textures.sh <out-dir>}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$(cd "$HERE/../../swift/Textures" && pwd)"
BUILD="$(mktemp -d)"
trap 'rm -rf "$BUILD"' EXIT

# -Ounchecked, not -O: the generators are hot arithmetic loops with bounds
# checks the render already guards by hand. Deterministic either way.
swiftc -Ounchecked -whole-module-optimization -D TEXTURE_BAKE \
  -o "$BUILD/gentex" \
  "$SRC/WoolTexture.swift" \
  "$SRC/FeltTexture.swift" \
  "$SRC/WoodTexture.swift" \
  "$SRC/FernCardBack.swift" \
  "$HERE/GenerateTextures.swift"

"$BUILD/gentex" "$OUT"
