#!/usr/bin/env bash
# regenerate_textures.sh - re-bake this app's wool, felt, wood and fern-back
# JPEGs into FoolishKit/Resources/. The generators and the bake tool live in
# shared/ (shared/swift/Textures, shared/tools/textures); this only says where
# the images go. Read shared/tools/textures/regenerate_textures.sh first.
#
#   ios/Tools/regenerate_textures.sh          # writes into FoolishKit/Resources
#   ios/Tools/regenerate_textures.sh /tmp/out # writes somewhere else (for A/B)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IOS="$(dirname "$HERE")"
exec "$IOS/../../shared/tools/textures/regenerate_textures.sh" "${1:-$IOS/FoolishKit/Resources}"
