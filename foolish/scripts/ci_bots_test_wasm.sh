#!/usr/bin/env bash
# CI: install the wasm toolchain and build the TEST bots module (c/Makefile
# wasm-bots-test, e2e/helpers/bots_test_wasm.ts) before a suite that loads it.
#
# The test build is the whole kernel - web.wasm's objects under every entry they
# have: the C Table, the bots, the fixture sealer, the card-list parser, the
# durable roster codec. It is the only whole-kernel wasm module (the server's,
# bots.wasm, went when the server became native), it is never committed, and
# every suite that plays a table loads it, so a job that runs them builds it
# here, once, instead of every test process discovering it is missing.
#
# The toolchain comes from shared/scripts/ci_llvm.sh - the same pinned clang and
# binaryen that build the SHIPPED modules, which is why that script raised its
# pin to 22 when they became build outputs. e2e/bots_test_build.test.ts compares
# this module's export list against web.wasm's, and two modules built by two
# compilers is a difference nobody wants to have to reason about.
set -euo pipefail
cd "$(dirname "$0")/.."
# shellcheck source=../shared/scripts/ci_llvm.sh
. ../shared/scripts/ci_llvm.sh
LLVM="$LLVM_PREFIX"
make -C c WASM_CC="$LLVM/bin/clang" LLVM_PREFIX="$LLVM" CC="$LLVM/bin/clang" wasm-bots-test
echo "built c/build/bots_test.wasm ($(wc -c < c/build/bots_test.wasm) B) for sources $(cat c/build/bots_test.stamp)"
