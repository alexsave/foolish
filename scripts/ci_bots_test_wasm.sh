#!/usr/bin/env bash
# CI: install the wasm toolchain and build the TEST bots module (c/Makefile
# wasm-bots-test, e2e/helpers/bots_test_wasm.ts) before a suite that loads it.
#
# The test build is the shipped bots.wasm plus the exports only tests call (the
# fixture sealer, the card-list parser, the durable roster codec). It is never
# committed, so a job that runs table_fixture/roster_kernel suites builds it
# here, once, instead of every test process discovering it is missing.
#
# Explicit LLVM 18 paths, not `clang`: the swift:6.2.4-noble image already has
# Swift's own clang at /usr/bin/clang, and plain ubuntu-latest has gcc as cc.
# The layout hash does not depend on the libclang version
# (tools/structgen/test/hash.sh), so the distro LLVM agrees with the committed
# module's LAYOUT_HASH.
set -euo pipefail
cd "$(dirname "$0")/.."
SUDO="$(command -v sudo || true)"
$SUDO apt-get update -qq
$SUDO apt-get install -y --no-install-recommends clang-18 lld-18 libclang-18-dev binaryen make
LLVM=/usr/lib/llvm-18
export PATH="$LLVM/bin:$PATH"
make -C c WASM_CC="$LLVM/bin/clang" LLVM_PREFIX="$LLVM" CC="$LLVM/bin/clang" wasm-bots-test
if [ -n "${GITHUB_ENV:-}" ]; then
  echo "WASM_CC=$LLVM/bin/clang" >> "$GITHUB_ENV"
  echo "LLVM_PREFIX=$LLVM" >> "$GITHUB_ENV"
fi
if [ -n "${GITHUB_PATH:-}" ]; then echo "$LLVM/bin" >> "$GITHUB_PATH"; fi
echo "built c/build/bots_test.wasm ($(wc -c < c/build/bots_test.wasm) B) for sources $(cat c/build/bots_test.stamp)"
