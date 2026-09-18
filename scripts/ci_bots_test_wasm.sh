#!/usr/bin/env bash
# CI: install the wasm toolchain and build the TEST bots module (c/Makefile
# wasm-bots-test, e2e/helpers/bots_test_wasm.ts) before a suite that loads it.
#
# The test build is the shipped bots.wasm plus the exports only tests call (the
# fixture sealer, the card-list parser, the durable roster codec). It is never
# committed, so a job that runs table_fixture/roster_kernel suites builds it
# here, once, instead of every test process discovering it is missing.
#
# The toolchain itself comes from scripts/ci_llvm.sh - which lane needs a
# compiler is a bigger question than which lane needs this module, and other
# lanes now need the one without the other (generation is part of every build).
# That script also exports WASM_CC, LLVM_PREFIX and PATH for the rest of the job.
set -euo pipefail
cd "$(dirname "$0")/.."
# shellcheck source=scripts/ci_llvm.sh
. scripts/ci_llvm.sh
LLVM="$LLVM_PREFIX"
make -C c WASM_CC="$LLVM/bin/clang" LLVM_PREFIX="$LLVM" CC="$LLVM/bin/clang" wasm-bots-test
echo "built c/build/bots_test.wasm ($(wc -c < c/build/bots_test.wasm) B) for sources $(cat c/build/bots_test.stamp)"
