#!/usr/bin/env bash
# mac_tests.sh - Pick 'Em Up's Mac-side gate: what needs Xcode and a simulator.
# The portable half (the kernel, its wire and the bridge smoke) is
# `make -C pickemup/c run asan`, which CI runs on Linux.
#
# The body is shared/scripts/ios_mac_tests.sh, which every product runs; this
# file is the product's half of it: the schemes, the library and what it writes.
# The shared script:
#   1. rebuilds the kernel's xcframework AND the generated readers and string
#      tables beside it (`make -C pickemup/c ios-lib`), so the tests run
#      against the kernel in this tree and readers of its layout;
#   2. regenerates Pickemup.xcodeproj from project.yml (a git-ignored build
#      artifact, like uttt's), also when a .swift file is newer than it;
#   3. puts every tracked entitlements file back, BYTES AND MTIME (`cp -p`).
#      This product's only entitlements file is set per-config by hand, so
#      xcodegen does not know it, but the guard stays: it costs nothing.
#
# Usage:
#   pickemup/ios/scripts/mac_tests.sh                 # lib + project + tests + the shipping build
#   pickemup/ios/scripts/mac_tests.sh unit            # PickemupKitTests only
#   pickemup/ios/scripts/mac_tests.sh app             # build the SHIPPING scheme only
#   pickemup/ios/scripts/mac_tests.sh --no-lib unit   # skip the xcframework build
#   pickemup/ios/scripts/mac_tests.sh --regen         # force xcodegen
#
#   DEST='platform=iOS Simulator,id=<udid>' pickemup/ios/scripts/mac_tests.sh
#
# At most two booted simulators on this Mac, ever (every shell dies past
# that); shut yours down when done.
set -euo pipefail

HELP_FILE="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
cd "$(dirname "$0")/../.."           # pickemup/

export HELP_FILE
export PROJECT="ios/Pickemup.xcodeproj"
export TEST_SCHEMES="unit=PickemupKitTests"
export BUILD_SCHEMES="app=PickemupMessagesApp"
export LIB_CMD="make -C c ios-lib"
export LIB_LABEL="kernel xcframework + generated readers (make -C pickemup/c ios-lib)"
export XCFRAMEWORK="ios/vendor/Pickemup.xcframework ios/Generated/PickemupKernel.swift"
export REGEN_WATCH="ios/PickemupKit ios/PickemupKitTests ios/PickemupMessages"
export SUITE_NAME="Pick 'Em Up"

exec bash ../shared/scripts/ios_mac_tests.sh "$@"
