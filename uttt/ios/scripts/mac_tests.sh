#!/usr/bin/env bash
# mac_tests.sh - Ultimate Tic-Tac-Toe's Mac-side gate: what needs Xcode.
# The portable half (the kernel, its wire and the bridge smoke) is
# `make -C uttt/c run asan ios-smoke`.
#
# The body is shared/scripts/ios_mac_tests.sh, which every product runs; this
# file is the product's half of it. uttt has no XCTest target, so the gate is
# the kernel's xcframework (`make -C uttt/c ios-lib`), a regenerated project
# with the entitlements put back BYTES AND MTIME, and a build of both schemes:
# the preview harness and the shipping container.
#
# Usage:
#   uttt/ios/scripts/mac_tests.sh                   # lib + project + both builds
#   uttt/ios/scripts/mac_tests.sh preview           # build UtttPreview only
#   uttt/ios/scripts/mac_tests.sh app               # build the SHIPPING scheme only
#   uttt/ios/scripts/mac_tests.sh --no-lib app      # skip the xcframework build
#
#   DEST='platform=iOS Simulator,id=<udid>' uttt/ios/scripts/mac_tests.sh
#
# At most two booted simulators on this Mac, ever (every shell dies past
# that); shut yours down when done.
set -euo pipefail

HELP_FILE="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
cd "$(dirname "$0")/../.."           # uttt/

export HELP_FILE
export PROJECT="ios/Uttt.xcodeproj"
export BUILD_SCHEMES="preview=UtttPreview app=UtttMessagesApp"
export LIB_CMD="make -C c ios-lib"
export XCFRAMEWORK="ios/vendor/Uttt.xcframework"
export SUITE_NAME="UTTT"

exec bash ../shared/scripts/ios_mac_tests.sh "$@"
