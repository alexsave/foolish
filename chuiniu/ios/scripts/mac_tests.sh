#!/usr/bin/env bash
# mac_tests.sh - Chui Niu's Mac-side gate: what needs Xcode and a simulator.
# The portable half (the kernel, its wire and the bridge smoke) is
# `make -C chuiniu/c run asan`.
#
# The body is shared/scripts/ios_mac_tests.sh, which every product runs; this
# file is the product's half of it: the schemes, the library and what it writes.
# The shared script:
#   1. rebuilds the kernel's xcframework AND the generated readers and string
#      tables beside it (`make -C chuiniu/c ios-lib`), so the tests run
#      against the kernel in this tree and readers of its layout;
#   1b. runs scripts/lint_architecture.sh, which reads the readers just made
#      (no Swift imports the kernel outside BridgeKernel.swift or reads its
#      bytes by hand);
#   2. regenerates Chuiniu.xcodeproj from project.yml (a git-ignored build
#      artifact, like uttt's);
#   3. puts every tracked entitlements file back, BYTES AND MTIME (`cp -p`).
#
# Usage:
#   chuiniu/ios/scripts/mac_tests.sh                 # lib + project + tests + the shipping build
#   chuiniu/ios/scripts/mac_tests.sh unit            # ChuiniuKitTests only
#   chuiniu/ios/scripts/mac_tests.sh app             # build the SHIPPING scheme only
#   chuiniu/ios/scripts/mac_tests.sh --no-lib unit   # skip the xcframework build
#
#   DEST='platform=iOS Simulator,id=<udid>' chuiniu/ios/scripts/mac_tests.sh
#
# At most two booted simulators on this Mac, ever (every shell dies past
# that); shut yours down when done.
set -euo pipefail

HELP_FILE="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
cd "$(dirname "$0")/../.."           # chuiniu/

export HELP_FILE
export PROJECT="ios/Chuiniu.xcodeproj"
export TEST_SCHEMES="unit=ChuiniuKitTests"
export BUILD_SCHEMES="app=ChuiniuMessagesApp"
export LIB_CMD="make -C c ios-lib"
export LIB_LABEL="kernel xcframework + generated readers (make -C chuiniu/c ios-lib)"
export XCFRAMEWORK="ios/vendor/Chuiniu.xcframework ios/Generated/ChuiniuKernel.swift"
export POST_LIB_CMD="ios/scripts/lint_architecture.sh"
export POST_LIB_LABEL="architecture lint (scripts/lint_architecture.sh)"
export SUITE_NAME="Chui Niu"

exec bash ../shared/scripts/ios_mac_tests.sh "$@"
