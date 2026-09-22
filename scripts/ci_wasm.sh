#!/usr/bin/env bash
# CI: build the three shipped wasm modules with the pinned toolchain.
#
# The toolchain and the reasoning behind the pin are in
# scripts/ci_wasm_toolchain.sh; what the modules are and why none of them is
# committed is in scripts/wasm_build.sh. This is just the two, in order, for a
# lane to call in one step.
#
# Idempotent, and safe to run after ci_llvm.sh or ci_bots_test_wasm.sh.
#
# Usage:
#   bash scripts/ci_wasm.sh          # every group
#   bash scripts/ci_wasm.sh bots     # just the kernel every host loads
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.."

# shellcheck source=scripts/ci_wasm_toolchain.sh
. scripts/ci_wasm_toolchain.sh

bash scripts/wasm_build.sh "$@"

echo "--- what the shipped-module build produced ---"
# Reported, never compared: a size is a fact a human can see move. The gate on
# it is e2e/mem/wasm_memory.test.ts, which measures the module rather than a
# committed copy of it.
while IFS= read -r p; do printf '  %8d B  %s\n' "$(wc -c < "$p")" "$p"; done \
  < <(bash scripts/wasm_build.sh --print-paths "$@")
