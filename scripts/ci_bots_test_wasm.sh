#!/usr/bin/env bash
# CI: install the wasm toolchain and build the TEST bots module (c/Makefile
# wasm-bots-test, e2e/helpers/bots_test_wasm.ts) before a suite that loads it.
#
# The test build is the shipped bots.wasm plus the exports only tests call (the
# fixture sealer, the card-list parser, the durable roster codec). It is never
# committed, so a job that runs table_fixture/roster_kernel suites builds it
# here, once, instead of every test process discovering it is missing.
#
# The toolchain comes from scripts/ci_wasm_toolchain.sh - the SAME pinned clang
# and binaryen that build the shipped modules. It used to come from
# scripts/ci_llvm.sh, which installs clang 18 for libclang; that was fine while
# the shipped modules were committed and this was the only wasm anything built,
# and it is not fine now that a lane holds both. e2e/bots_test_build.test.ts
# compares this module's export list against the shipped one, and two modules
# built by two compilers is a difference nobody wants to have to reason about.
set -euo pipefail
cd "$(dirname "$0")/.."
# shellcheck source=scripts/ci_wasm_toolchain.sh
. scripts/ci_wasm_toolchain.sh
LLVM="$LLVM_PREFIX"
make -C c WASM_CC="$LLVM/bin/clang" LLVM_PREFIX="$LLVM" CC="$LLVM/bin/clang" wasm-bots-test
echo "built c/build/bots_test.wasm ($(wc -c < c/build/bots_test.wasm) B) for sources $(cat c/build/bots_test.stamp)"
